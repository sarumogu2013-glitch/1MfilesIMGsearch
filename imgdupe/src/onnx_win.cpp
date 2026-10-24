#include "onnx_win.hpp"

#include <windows.h>

#include <algorithm>
#include <thread>

#include "onnxruntime_c_api.h"
#include "win_util.hpp"

namespace onnx {

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

}  // namespace

struct Session::Impl {
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memory = nullptr;

    std::wstring check(OrtStatus* st) const {
        if (!st) return {};
        std::wstring msg = widen(api->GetErrorMessage(st));
        api->ReleaseStatus(st);
        return msg.empty() ? L"ONNX Runtime のエラー" : msg;
    }
    ~Impl() {
        if (!api) return;
        if (session) api->ReleaseSession(session);
        if (memory) api->ReleaseMemoryInfo(memory);
        if (env) api->ReleaseEnv(env);
        // dll は外さない（プロセスの終わりまで置く）
    }
};

Session::Session() : impl_(new Impl) {}
Session::~Session() = default;

std::wstring Session::open(const std::wstring& dllDir, const std::wstring& modelPath, bool useGpu, int cpuThreads) {
    Impl& m = *impl_;
    HMODULE dll = LoadLibraryExW((dllDir + L"\\onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) return L"onnxruntime.dll を読み込めません（エラー " + std::to_wstring(GetLastError()) + L"）";
    auto getBase = procAddress<GetApiBase>(dll, "OrtGetApiBase");
    if (!getBase) return L"onnxruntime.dll が想定と違います（OrtGetApiBase がありません）";
    m.api = getBase()->GetApi(ORT_API_VERSION);
    if (!m.api) return L"onnxruntime.dll の版が古すぎます（" + widen(getBase()->GetVersionString()) + L"）";
    std::wstring err;
    // 同じプロセスで環境は 1 つだけにする（ONNX Runtime の決まり）
    static OrtEnv* sharedEnv = nullptr;
    static std::mutex envMutex;
    {
        std::lock_guard<std::mutex> lock(envMutex);
        if (!sharedEnv && !(err = m.check(m.api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "imgdupe", &sharedEnv))).empty()) return err;
    }
    if (!(err = m.check(m.api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m.memory))).empty()) return err;

    auto tryOpen = [&](bool gpu, std::wstring& e) -> bool {
        OrtSessionOptions* opt = nullptr;
        if (!(e = m.check(m.api->CreateSessionOptions(&opt))).empty()) return false;
        m.api->SetSessionGraphOptimizationLevel(opt, ORT_ENABLE_ALL);
        if (gpu) {
            // DirectML は「メモリの使い回しの事前計画なし・順に実行」が条件
            m.api->DisableMemPattern(opt);
            m.api->SetSessionExecutionMode(opt, ORT_SEQUENTIAL);
            auto append = procAddress<DmlAppend>(dll, "OrtSessionOptionsAppendExecutionProvider_DML");
            if (!append || !(e = m.check(append(opt, 0))).empty()) {
                if (e.empty()) e = L"DirectML が使えません";
                m.api->ReleaseSessionOptions(opt);
                return false;
            }
        } else {
            m.api->SetIntraOpNumThreads(
                opt, cpuThreads > 0 ? cpuThreads : static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
        }
        e = m.check(m.api->CreateSession(sharedEnv, modelPath.c_str(), opt, &m.session));
        m.api->ReleaseSessionOptions(opt);
        return e.empty();
    };
    gpu_ = useGpu && tryOpen(true, err);
    if (!gpu_ && !tryOpen(false, err)) return L"モデルを読み込めません: " + err;

    OrtAllocator* alloc = nullptr;
    m.api->GetAllocatorWithDefaultOptions(&alloc);
    size_t n = 0;
    m.api->SessionGetInputCount(m.session, &n);
    for (size_t i = 0; i < n; ++i) {
        char* name = nullptr;
        if (!(err = m.check(m.api->SessionGetInputName(m.session, i, alloc, &name))).empty()) return err;
        inputs_.push_back(name);
        alloc->Free(alloc, name);
    }
    m.api->SessionGetOutputCount(m.session, &n);
    for (size_t i = 0; i < n; ++i) {
        char* name = nullptr;
        if (!(err = m.check(m.api->SessionGetOutputName(m.session, i, alloc, &name))).empty()) return err;
        outputs_.push_back(name);
        alloc->Free(alloc, name);
    }
    return {};
}

std::vector<int64_t> Session::inputShape(size_t i) const {
    const Impl& m = *impl_;
    std::vector<int64_t> shape;
    OrtTypeInfo* type = nullptr;
    if (m.api->SessionGetInputTypeInfo(m.session, i, &type)) return shape;
    const OrtTensorTypeAndShapeInfo* tensor = nullptr;
    m.api->CastTypeInfoToTensorInfo(type, &tensor);
    size_t dims = 0;
    if (tensor) m.api->GetDimensionsCount(tensor, &dims);
    shape.resize(dims);
    if (dims) m.api->GetDimensions(tensor, shape.data(), dims);
    m.api->ReleaseTypeInfo(type);
    return shape;
}

bool Session::run(const std::vector<Input>& inputs, const std::string& output, std::vector<float>& out, std::vector<int64_t>& shape,
                  std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    Impl& m = *impl_;
    std::vector<OrtValue*> values;
    std::vector<const char*> names;
    OrtValue* result = nullptr;
    std::wstring err;
    for (const auto& in : inputs) {
        size_t count = 1;
        for (int64_t d : in.shape) count *= static_cast<size_t>(d);
        OrtValue* v = nullptr;
        err = in.f32 ? m.check(m.api->CreateTensorWithDataAsOrtValue(m.memory, const_cast<float*>(in.f32), count * sizeof(float),
                                                                       in.shape.data(), in.shape.size(),
                                                                       ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &v))
                     : m.check(m.api->CreateTensorWithDataAsOrtValue(m.memory, const_cast<int64_t*>(in.i64), count * sizeof(int64_t),
                                                                       in.shape.data(), in.shape.size(),
                                                                       ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &v));
        if (!err.empty()) break;
        values.push_back(v);
        names.push_back(in.name.c_str());
    }
    if (err.empty()) {
        const char* outNames[] = {output.c_str()};
        err = m.check(m.api->Run(m.session, nullptr, names.data(), values.data(), values.size(), outNames, 1, &result));
    }
    if (err.empty()) {
        float* data = nullptr;
        OrtTensorTypeAndShapeInfo* info = nullptr;
        size_t count = 0, dims = 0;
        err = m.check(m.api->GetTensorMutableData(result, reinterpret_cast<void**>(&data)));
        if (err.empty()) err = m.check(m.api->GetTensorTypeAndShape(result, &info));
        if (err.empty()) err = m.check(m.api->GetTensorShapeElementCount(info, &count));
        if (err.empty()) err = m.check(m.api->GetDimensionsCount(info, &dims));
        if (err.empty()) {
            shape.resize(dims);
            if (dims) err = m.check(m.api->GetDimensions(info, shape.data(), dims));
        }
        if (info) m.api->ReleaseTensorTypeAndShapeInfo(info);
        if (err.empty()) out.assign(data, data + count);
    }
    for (OrtValue* v : values) m.api->ReleaseValue(v);
    if (result) m.api->ReleaseValue(result);
    if (!err.empty()) error = narrow(err);
    return err.empty();
}

}  // namespace onnx
