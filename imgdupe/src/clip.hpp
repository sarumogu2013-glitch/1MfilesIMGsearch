// 段階 clip と、言葉・画像での検索（仕様「6. CLIP」）。OS に依存しない部分。
// 画像: OpenAI CLIP ViT-B/32 の画像側（224×224、RGB を CLIP の平均と分散で正規化、NCHW）→ 512 次元
// 言葉: clip-ViT-B-32-multilingual-v1（多言語の DistilBERT。日本語も可）→ 単語ごとのベクトルの平均 → Dense（768→512）
// どちらも長さ 1 にして、内積を似ている度合いにする。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "analyzer.hpp"
#include "search.hpp"
#include "vectors.hpp"

namespace imgdupe {

constexpr const char* kStageClip = "clip";
constexpr int kClipCodeVersion = 1;  // 前処理などを変えたら上げる

// BERT の WordPiece（大文字小文字はそのまま、漢字は 1 文字ずつ）
class WordPiece {
public:
    bool load(const std::string& vocabTxt);  // 1 行 1 語。行番号が ID
    // [CLS] … [SEP]（maxLen を超える分は切る）
    std::vector<int64_t> encode(const std::string& text, size_t maxLen = 128) const;
    std::vector<std::string> tokens(const std::string& text) const;  // 確認用（[CLS] などは付けない）

private:
    std::unordered_map<std::string, int64_t> vocab_;
    int64_t cls_ = 101, sep_ = 102, unk_ = 100;
};

// safetensors の中の 1 つの F32 の行列（Dense の重み [出力, 入力]）
struct DenseLayer {
    int out = 0, in = 0;
    std::vector<float> w;
    bool load(const std::string& safetensors);  // ファイルの中身
    std::vector<float> apply(const float* x) const;
};

// 32bpp の PBGRA（白を背景に合成）w×h から、短い辺を side に合わせて中央を side×side に切り抜いた、
// CLIP の正規化をした RGB の float（NCHW: 3×side×side）
void prepareClipInput(const uint8_t* bgra, int w, int h, int side, float* out);

// 単語ごとのベクトル [L, dim] を、mask（1 = 使う）で平均する
std::vector<float> meanPool(const float* tokens, size_t len, size_t dim, const std::vector<int64_t>& mask);

int clipStageVersion(const std::string& modelVersion);

// q の条件（タグなど）に合う画像を、qv（長さ 1）に似ている順に上位 k 件。条件が無ければ全部から。
// total は条件に合ってベクトルのある画像の数。scores は ids と同じ並びの似ている度合い（内積）
struct RankResult {
    std::vector<int64_t> ids;
    std::vector<float> scores;
    size_t total = 0;
};
RankResult rankByVector(Db& db, const SearchQuery& q, const VectorIndex& index, const float* qv, size_t k, int64_t exclude = 0);

// 推論をする部品（Windows では ONNX Runtime）
class ClipModel {
public:
    virtual ~ClipModel() = default;
    virtual int imageSide() const = 0;  // 224
    virtual size_t dim() const = 0;     // 512
    virtual bool encodeImage(const std::vector<float>& pixels, std::vector<float>& out, std::string& error) = 0;
    virtual bool encodeText(const std::string& text, std::vector<float>& out, std::string& error) = 0;
};

// 段階 clip: 画像のベクトルを作って、ベクトルのファイルに足す
class ClipAnalyzer : public Analyzer {
public:
    using Decoder = std::function<bool(const std::string& path, int box, std::vector<uint8_t>& bgra, int& w, int& h)>;
    ClipAnalyzer(std::shared_ptr<ClipModel> model, std::shared_ptr<VectorWriter> writer, int version, Decoder decode,
                 std::function<void()> threadInit = {}, std::function<void()> threadExit = {})
        : model_(std::move(model)), writer_(std::move(writer)), version_(version), decode_(std::move(decode)),
          init_(std::move(threadInit)), exit_(std::move(threadExit)) {}
    std::string stageName() const override { return kStageClip; }
    int version() const override { return version_; }
    AnalyzerOutput analyze(const AnalyzerInput& in) override;
    bool store(Db& db, int64_t imageId, AnalyzerOutput& out) override;
    void threadBegin() override {
        if (init_) init_();
    }
    void threadEnd() override {
        if (exit_) exit_();
    }

private:
    std::shared_ptr<ClipModel> model_;
    std::shared_ptr<VectorWriter> writer_;
    int version_;
    Decoder decode_;
    std::function<void()> init_, exit_;
};

}  // namespace imgdupe
