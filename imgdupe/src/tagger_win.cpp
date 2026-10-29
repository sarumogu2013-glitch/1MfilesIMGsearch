#include "tagger_win.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <vector>

#include "image_win.hpp"
#include "log.hpp"
#include "models.hpp"
#include "models_win.hpp"
#include "onnx_win.hpp"
#include "tagger.hpp"
#include "win_util.hpp"

namespace tagger {

using winutil::narrow;
using winutil::widen;

namespace {

constexpr size_t kGpuBatch = 8;  // GPU で一度に流す枚数

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

// WD14 タガーのモデル（入力 [N, 大きさ, 大きさ, 3] の BGR、出力は各タグの確率）
class OnnxTagger : public imgdupe::TaggerModel {
public:
    std::wstring open(const std::wstring& dir, const std::wstring& modelPath, bool useGpu, int cpuThreads) {
        if (std::wstring e = session_.open(dir, modelPath, useGpu, cpuThreads); !e.empty()) return e;
        const auto shape = session_.inputShape(0);
        if (session_.inputNames().empty() || session_.outputNames().empty() || shape.size() != 4 || shape[3] != 3 ||
            shape[1] != shape[2] || shape[1] <= 0)
            return L"モデルの入力の形が WD14 タガーと違います（[N, 大きさ, 大きさ, 3] のはず）";
        side_ = static_cast<int>(shape[1]);
        return {};
    }
    int inputSide() const override { return side_; }
    bool gpu() const { return session_.gpu(); }
    bool run(const std::vector<float>& input, std::vector<float>& probs, std::string& error) override {
        return runBatch(input, 1, probs, error);
    }
    // GPU では 8 枚ずつ流す（モデルの入力の枚数が決まっていない時だけ）。CPU は 1 枚ずつでも速さが変わらないので分けない
    size_t maxBatch() const override { return batch_; }
    bool runBatch(const std::vector<float>& input, size_t n, std::vector<float>& probs, std::string& error) override {
        std::vector<int64_t> shape;
        if (session_.run({{session_.inputNames()[0], {static_cast<int64_t>(n), side_, side_, 3}, input.data(), nullptr}},
                         session_.outputNames()[0], probs, shape, error))
            return true;
        if (n == 1) return false;
        // まとめて流せなかった（GPU のメモリが足りないなど）: これからは 1 枚ずつにして、今の分も 1 枚ずつやり直す
        imgdupe::logs::warn("タグ付け: " + std::to_string(n) + " 枚まとめて GPU に流せなかったので、これからは 1 枚ずつにする（" + error + "）");
        batch_ = 1;
        const size_t one = input.size() / n;
        probs.clear();
        for (size_t k = 0; k < n; ++k) {
            std::vector<float> in(input.begin() + static_cast<std::ptrdiff_t>(one * k),
                                  input.begin() + static_cast<std::ptrdiff_t>(one * (k + 1))),
                out;
            if (!runBatch(in, 1, out, error)) return false;
            probs.insert(probs.end(), out.begin(), out.end());
        }
        return true;
    }
    void setBatch(size_t b) { batch_ = b; }
    bool dynamicBatch() const { return session_.inputShape(0).size() == 4 && session_.inputShape(0)[0] < 0; }

private:
    onnx::Session session_;
    int side_ = 0;
    std::atomic<size_t> batch_{1};
};

}  // namespace

int stageVersion(const std::wstring& dataDir) {
    // 設定で切ってあれば使わない（imgdupe.ini の [imgdupe] taggerEnabled=0）
    if (!GetPrivateProfileIntW(L"imgdupe", L"taggerEnabled", 1, (dataDir + L"\\imgdupe.ini").c_str())) return 0;
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
    auto model = std::make_shared<OnnxTagger>();
    if (std::wstring e = model->open(dir, modelPath, !cpuOnly, cpuThreads); !e.empty()) {
        r.error = e;
        return r;
    }
    if (model->gpu() && model->dynamicBatch()) model->setBatch(kGpuBatch);
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
