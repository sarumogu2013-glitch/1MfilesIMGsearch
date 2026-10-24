#include "clip_win.hpp"

#include <windows.h>

#include <cstdio>
#include <unordered_set>

#include "image_win.hpp"
#include "models.hpp"
#include "models_win.hpp"
#include "onnx_win.hpp"
#include "vectors.hpp"
#include "win_util.hpp"

namespace clip {

using winutil::narrow;
using winutil::widen;

namespace {

constexpr const char* kComponent = "clip";

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

struct Files {
    std::wstring vision, text, vocab, dense;
    std::string version;
    bool ok = false;
};

// installed.json の clip の部品から、ファイルを名前で見つける
Files findFiles(const std::wstring& dataDir) {
    Files f;
    const imgdupe::Installed installed = models::loadInstalled(dataDir);
    auto it = installed.find(kComponent);
    if (it == installed.end() || !installed.count("onnxruntime")) return f;
    const std::wstring dir = models::modelsDir(dataDir);
    for (const auto& rel : it->second.files) {
        std::wstring p = dir + L"\\" + widen(rel);
        for (auto& c : p)
            if (c == L'/') c = L'\\';
        const std::string name = rel.substr(rel.find_last_of('/') + 1);
        if (name.find("vision") != std::string::npos && name.size() > 5 && name.compare(name.size() - 5, 5, ".onnx") == 0) f.vision = p;
        else if (name.size() > 5 && name.compare(name.size() - 5, 5, ".onnx") == 0) f.text = p;
        else if (name == "vocab.txt") f.vocab = p;
        else if (name.find(".safetensors") != std::string::npos) f.dense = p;
    }
    f.version = it->second.version;
    f.ok = !f.vision.empty() && !f.text.empty() && !f.vocab.empty() && !f.dense.empty();
    return f;
}

bool enabled(const std::wstring& dataDir) {
    return GetPrivateProfileIntW(L"imgdupe", L"clipEnabled", 1, (dataDir + L"\\imgdupe.ini").c_str()) != 0;
}

bool cpuOnly(const std::wstring& dataDir) {
    wchar_t env[8] = L"";
    return (GetEnvironmentVariableW(L"IMGDUPE_TAGGER_CPU", env, 8) && env[0] == L'1') ||
           GetPrivateProfileIntW(L"imgdupe", L"taggerCpu", 0, (dataDir + L"\\imgdupe.ini").c_str()) != 0;
}

class OnnxClip : public imgdupe::ClipModel {
public:
    // 画像側（vision が空でなければ）と言葉側（text が空でなければ）
    std::wstring open(const std::wstring& dir, const Files& f, bool vision, bool text, bool useGpu, int cpuThreads) {
        if (vision) {
            visionSession_ = std::make_unique<onnx::Session>();
            if (std::wstring e = visionSession_->open(dir, f.vision, useGpu, cpuThreads); !e.empty()) return e;
            if (visionSession_->inputNames().empty() || visionSession_->outputNames().empty())
                return L"CLIP の画像側のモデルの形が違います";
            const auto shape = visionSession_->inputShape(0);
            if (shape.size() == 4 && shape[2] > 0) side_ = static_cast<int>(shape[2]);
            visionOut_ = visionSession_->outputNames()[0];
            for (const auto& n : visionSession_->outputNames())
                if (n == "image_embeds") visionOut_ = n;
        }
        if (text) {
            std::string vocab, dense;
            if (!readFile(f.vocab, vocab) || !tokenizer_.load(vocab)) return L"CLIP の単語の一覧（vocab.txt）を読めません";
            if (!readFile(f.dense, dense) || !dense_.load(dense)) return L"CLIP の言葉側の重み（safetensors）を読めません";
            textSession_ = std::make_unique<onnx::Session>();
            // 言葉側は 1 回が軽いので CPU で十分（GPU の量子化モデルの相性も避ける）
            if (std::wstring e = textSession_->open(dir, f.text, false, 2); !e.empty()) return e;
            if (textSession_->outputNames().empty()) return L"CLIP の言葉側のモデルの形が違います";
        }
        return {};
    }
    bool gpu() const { return visionSession_ && visionSession_->gpu(); }
    int imageSide() const override { return side_; }
    size_t dim() const override { return 512; }

    bool encodeImage(const std::vector<float>& pixels, std::vector<float>& out, std::string& error) override {
        if (!visionSession_) return false;
        std::vector<int64_t> shape;
        return visionSession_->run({{visionSession_->inputNames()[0], {1, 3, side_, side_}, pixels.data(), nullptr}}, visionOut_, out,
                                   shape, error);
    }

    bool encodeText(const std::string& text, std::vector<float>& out, std::string& error) override {
        if (!textSession_) return false;
        const std::vector<int64_t> ids = tokenizer_.encode(text, 128);
        const std::vector<int64_t> mask(ids.size(), 1);
        const int64_t len = static_cast<int64_t>(ids.size());
        std::vector<onnx::Input> in;
        for (const auto& name : textSession_->inputNames()) {
            if (name == "input_ids") in.push_back({name, {1, len}, nullptr, ids.data()});
            else if (name == "attention_mask") in.push_back({name, {1, len}, nullptr, mask.data()});
        }
        std::vector<float> hidden;
        std::vector<int64_t> shape;
        if (!textSession_->run(in, textSession_->outputNames()[0], hidden, shape, error)) return false;
        if (shape.size() != 3 || shape[2] != dense_.in) {
            error = "CLIP の言葉側の出力の形が違います";
            return false;
        }
        const auto pooled = imgdupe::meanPool(hidden.data(), static_cast<size_t>(shape[1]), static_cast<size_t>(shape[2]), mask);
        out = dense_.apply(pooled.data());
        imgdupe::normalizeVector(out);
        return true;
    }

private:
    std::unique_ptr<onnx::Session> visionSession_, textSession_;
    std::string visionOut_;
    int side_ = 224;
    imgdupe::WordPiece tokenizer_;
    imgdupe::DenseLayer dense_;
};

}  // namespace

int stageVersion(const std::wstring& dataDir) {
    if (!enabled(dataDir)) return 0;
    const Files f = findFiles(dataDir);
    return f.ok ? imgdupe::clipStageVersion(f.version) : 0;
}

std::wstring vectorPath(const std::wstring& dataDir) { return dataDir + L"\\vectors_clip.bin"; }

void prepare(const std::wstring& dataDir, imgdupe::Db& db) {
    const Files f = findFiles(dataDir);
    if (!f.ok) return;
    imgdupe::VectorIndex idx;
    const std::string path = narrow(vectorPath(dataDir));
    if (!idx.load(path) || idx.version() != f.version) {
        // ベクトルが無い（消された・モデルが変わった）のに済んだ記録があれば、やり直させる
        imgdupe::Stmt st(db.handle(), "DELETE FROM stages WHERE stage='clip'");
        st.step();
        return;
    }
    // 古いレコード（作り直した画像）と消えた画像が 2 割を超えたら作り直す
    std::unordered_set<int64_t> live;
    {
        imgdupe::Stmt st(db.handle(), "SELECT id FROM images");
        while (st.step()) live.insert(st.i64(0));
    }
    size_t dead = idx.records() - idx.size();
    for (int64_t id : idx.ids())
        if (!live.count(id)) ++dead;
    if (dead * 5 > idx.records() && dead > 100) {
        idx.close();
        imgdupe::compactVectors(path, [&](int64_t id) { return live.count(id) != 0; });  // 開いている画面があれば失敗する（次の回に）
    }
}

LoadResult loadAnalyzer(const std::wstring& dataDir, int cpuThreads, std::function<void()> threadInit, std::function<void()> threadExit) {
    LoadResult r;
    const Files f = findFiles(dataDir);
    if (!f.ok) return r;
    auto model = std::make_shared<OnnxClip>();
    if (std::wstring e = model->open(models::modelsDir(dataDir), f, true, false, !cpuOnly(dataDir), cpuThreads); !e.empty()) {
        r.error = e;
        return r;
    }
    auto writer = std::make_shared<imgdupe::VectorWriter>();
    if (!writer->open(narrow(vectorPath(dataDir)), static_cast<uint32_t>(model->dim()), f.version)) {
        r.error = L"ベクトルのファイルに書けません（画面で検索中なら、後でもう一度試します）";
        return r;
    }
    r.device = model->gpu() ? L"GPU（DirectML）" : L"CPU";
    r.analyzer = std::make_shared<imgdupe::ClipAnalyzer>(
        model, writer, imgdupe::clipStageVersion(f.version),
        [](const std::string& path, int box, std::vector<uint8_t>& bgra, int& w, int& h) {
            return imgwin::decodeBgra(widen(path), box, bgra, w, h);
        },
        threadInit ? threadInit : std::function<void()>(imgwin::threadBegin),
        threadExit ? threadExit : std::function<void()>(imgwin::threadEnd));
    return r;
}

std::shared_ptr<imgdupe::ClipModel> loadText(const std::wstring& dataDir, std::wstring& error) {
    const Files f = findFiles(dataDir);
    if (!f.ok) {
        error = L"CLIP（内容で探す部品）が入っていません。「その他 → AI 解析の部品…」で入れられます";
        return nullptr;
    }
    auto model = std::make_shared<OnnxClip>();
    if (std::wstring e = model->open(models::modelsDir(dataDir), f, false, true, false, 2); !e.empty()) {
        error = e;
        return nullptr;
    }
    return model;
}

}  // namespace clip
