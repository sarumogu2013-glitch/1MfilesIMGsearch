#include "tagger_win.hpp"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "image_win.hpp"
#include "models.hpp"
#include "models_win.hpp"
#include "onnxruntime_c_api.h"
#include "tagger.hpp"
#include "win_util.hpp"

namespace tagger {

using winutil::narrow;
using winutil::widen;

namespace {

using DmlAppend = OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*, int);
using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();

// GetProcAddress の結果を関数の型にする（いったん汎用の関数ポインタを通す）
template <typename F>
F procAddress(HMODULE dll, const char* name) {
    return reinterpret_cast<F>(reinterpret_cast<void (*)()>(GetProcAddress(dll, name)));
}

bool readFile(const std::wstring& path, std::string& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// ONNX Runtime のセッション（C API）
class OnnxModel : public imgdupe::TaggerModel {
public:
    ~OnnxModel() override {
        if (!api_) return;
        if (session_) api_->ReleaseSession(session_);
        if (memory_) api_->ReleaseMemoryInfo(memory_);
        if (env_) api_->ReleaseEnv(env_);
        // dll は外さない（ほかの所がまだ関数を持っているかもしれないので、プロセスの終わりまで置く）
    }

    // 失敗したら理由を返す
    std::wstring open(const std::wstring& dir, const std::wstring& modelPath, bool useGpu, int cpuThreads) {
        HMODULE dll = LoadLibraryExW((dir + L"\\onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) return L"onnxruntime.dll を読み込めません（エラー " + std::to_wstring(GetLastError()) + L"）";
        auto getBase = procAddress<GetApiBase>(dll, "OrtGetApiBase");
        if (!getBase) return L"onnxruntime.dll が想定と違います（OrtGetApiBase がありません）";
        api_ = getBase()->GetApi(ORT_API_VERSION);
        if (!api_) return L"onnxruntime.dll の版が古すぎます（" + widen(getBase()->GetVersionString()) + L"）";
        if (auto e = check(api_->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "imgdupe", &env_)); !e.empty()) return e;
        if (auto e = check(api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_)); !e.empty()) return e;

        auto tryOpen = [&](bool gpu, std::wstring& err) -> bool {
            OrtSessionOptions* opt = nullptr;
            if (!(err = check(api_->CreateSessionOptions(&opt))).empty()) return false;
            api_->SetSessionGraphOptimizationLevel(opt, ORT_ENABLE_ALL);
            if (gpu) {
                // DirectML は「メモリの使い回しの事前計画なし・順に実行」が条件
                api_->DisableMemPattern(opt);
                api_->SetSessionExecutionMode(opt, ORT_SEQUENTIAL);
                auto append = procAddress<DmlAppend>(dll, "OrtSessionOptionsAppendExecutionProvider_DML");
                if (!append || !(err = check(append(opt, 0))).empty()) {
                    if (err.empty()) err = L"DirectML が使えません";
                    api_->ReleaseSessionOptions(opt);
                    return false;
                }
            } else {
                api_->SetIntraOpNumThreads(opt, cpuThreads > 0 ? cpuThreads
                                                               : static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
            }
            err = check(api_->CreateSession(env_, modelPath.c_str(), opt, &session_));
            api_->ReleaseSessionOptions(opt);
            return err.empty();
        };
        std::wstring err;
        gpu_ = useGpu && tryOpen(true, err);
        if (!gpu_ && !tryOpen(false, err)) return L"モデルを読み込めません: " + err;

        // 入力と出力の名前、入力の大きさ（[N, 448, 448, 3] の 448）
        OrtAllocator* alloc = nullptr;
        api_->GetAllocatorWithDefaultOptions(&alloc);
        char* name = nullptr;
        if (!(err = check(api_->SessionGetInputName(session_, 0, alloc, &name))).empty()) return err;
        inputName_ = name;
        alloc->Free(alloc, name);
        if (!(err = check(api_->SessionGetOutputName(session_, 0, alloc, &name))).empty()) return err;
        outputName_ = name;
        alloc->Free(alloc, name);
        OrtTypeInfo* type = nullptr;
        if (!(err = check(api_->SessionGetInputTypeInfo(session_, 0, &type))).empty()) return err;
        const OrtTensorTypeAndShapeInfo* tensor = nullptr;
        api_->CastTypeInfoToTensorInfo(type, &tensor);
        size_t dims = 0;
        if (tensor) api_->GetDimensionsCount(tensor, &dims);
        std::vector<int64_t> shape(dims);
        if (dims) api_->GetDimensions(tensor, shape.data(), dims);
        api_->ReleaseTypeInfo(type);
        if (dims != 4 || shape[3] != 3 || shape[1] != shape[2] || shape[1] <= 0)
            return L"モデルの入力の形が WD14 タガーと違います（[N, 大きさ, 大きさ, 3] のはず）";
        side_ = static_cast<int>(shape[1]);
        return {};
    }

    int inputSide() const override { return side_; }
    bool gpu() const { return gpu_; }

    bool run(const std::vector<float>& input, std::vector<float>& probs, std::string& error) override {
        std::lock_guard<std::mutex> lock(runMutex_);  // DirectML は同時に 1 つずつ。CPU も中で全部のコアを使う
        const int64_t shape[4] = {1, side_, side_, 3};
        OrtValue* in = nullptr;
        OrtValue* out = nullptr;
        std::wstring err = check(api_->CreateTensorWithDataAsOrtValue(memory_, const_cast<float*>(input.data()),
                                                                       input.size() * sizeof(float), shape, 4,
                                                                       ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in));
        if (err.empty()) {
            const char* inNames[] = {inputName_.c_str()};
            const char* outNames[] = {outputName_.c_str()};
            err = check(api_->Run(session_, nullptr, inNames, &in, 1, outNames, 1, &out));
        }
        if (err.empty()) {
            float* data = nullptr;
            OrtTensorTypeAndShapeInfo* info = nullptr;
            size_t count = 0;
            err = check(api_->GetTensorMutableData(out, reinterpret_cast<void**>(&data)));
            if (err.empty()) err = check(api_->GetTensorTypeAndShape(out, &info));
            if (err.empty()) err = check(api_->GetTensorShapeElementCount(info, &count));
            if (info) api_->ReleaseTensorTypeAndShapeInfo(info);
            if (err.empty()) probs.assign(data, data + count);
        }
        if (in) api_->ReleaseValue(in);
        if (out) api_->ReleaseValue(out);
        if (!err.empty()) error = narrow(err);
        return err.empty();
    }

private:
    std::wstring check(OrtStatus* st) {
        if (!st) return {};
        std::wstring msg = widen(api_->GetErrorMessage(st));
        api_->ReleaseStatus(st);
        return msg.empty() ? L"ONNX Runtime のエラー" : msg;
    }

    const OrtApi* api_ = nullptr;
    OrtEnv* env_ = nullptr;
    OrtSession* session_ = nullptr;
    OrtMemoryInfo* memory_ = nullptr;
    std::string inputName_, outputName_;
    int side_ = 0;
    bool gpu_ = false;
    std::mutex runMutex_;
};

}  // namespace

int stageVersion(const std::wstring& dataDir) {
    const imgdupe::Installed installed = models::loadInstalled(dataDir);
    auto wd = installed.find("wd14");
    if (wd == installed.end() || !installed.count("onnxruntime")) return 0;
    return imgdupe::taggerStageVersion(wd->second.version);
}

LoadResult load(const std::wstring& dataDir, int cpuThreads, std::function<void()> threadInit, std::function<void()> threadExit) {
    LoadResult r;
    const imgdupe::Installed installed = models::loadInstalled(dataDir);
    auto wd = installed.find("wd14");
    if (wd == installed.end() || !installed.count("onnxruntime")) return r;  // 入っていない
    const std::wstring dir = models::modelsDir(dataDir);
    std::wstring modelPath, tagsPath;
    for (const auto& f : wd->second.files) {
        std::wstring p = dir + L"\\" + widen(f);
        for (auto& c : p)
            if (c == L'/') c = L'\\';
        if (p.size() > 5 && _wcsicmp(p.c_str() + p.size() - 5, L".onnx") == 0) modelPath = p;
        if (p.size() > 4 && _wcsicmp(p.c_str() + p.size() - 4, L".csv") == 0) tagsPath = p;
    }
    if (modelPath.empty() || tagsPath.empty() || !exists(modelPath) || !exists(tagsPath)) {
        r.error = L"WD14 タガーのファイルが足りません（「その他 → AI 解析の部品…」で入れ直してください）";
        return r;
    }
    std::string csv;
    std::vector<imgdupe::TaggerLabel> labels;
    if (!readFile(tagsPath, csv) || !imgdupe::parseSelectedTags(csv, labels)) {
        r.error = L"selected_tags.csv を読めません";
        return r;
    }
    wchar_t env[8] = L"";
    const bool cpuOnly = (GetEnvironmentVariableW(L"IMGDUPE_TAGGER_CPU", env, 8) && env[0] == L'1') ||
                         GetPrivateProfileIntW(L"imgdupe", L"taggerCpu", 0, (dataDir + L"\\imgdupe.ini").c_str()) != 0;
    auto model = std::make_shared<OnnxModel>();
    if (std::wstring e = model->open(dir, modelPath, !cpuOnly, cpuThreads); !e.empty()) {
        r.error = e;
        return r;
    }
    r.device = model->gpu() ? L"GPU（DirectML）" : L"CPU";
    r.analyzer = std::make_shared<imgdupe::TaggerAnalyzer>(
        model, std::move(labels), imgdupe::taggerStageVersion(wd->second.version),
        [](const std::string& path, int box, std::vector<uint8_t>& bgra, int& w, int& h) {
            return imgwin::decodeBgra(widen(path), box, bgra, w, h);
        },
        threadInit ? threadInit : std::function<void()>(imgwin::threadBegin),
        threadExit ? threadExit : std::function<void()>(imgwin::threadEnd));
    return r;
}

}  // namespace tagger
