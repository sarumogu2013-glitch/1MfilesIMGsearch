// 段階 tagger（仕様「5. タガー」）: WD14 タガーで画像にタグを付ける。推論そのもの（ONNX Runtime）は TaggerModel の外。
// 前処理は SmilingWolf の wd-tagger と同じ: 透明は白で埋め、白で正方形に広げ、入力の大きさに縮め、BGR の 0〜255 の float（NHWC）。
// OS に依存しない。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "analyzer.hpp"

namespace imgdupe {

constexpr const char* kSourceTagger = "tagger";
constexpr double kTaggerMinConfidence = 0.2;  // これ未満のタグは残さない（仕様「一般タグは確度 0.2 以上だけ保存」）
constexpr int kTaggerCodeVersion = 1;         // 前処理や保存のしかたを変えたら上げる（モデルの版と合わせて段階の版になる）

struct TaggerLabel {
    std::string name;
    int category = 0;  // 0 一般、1 作者、3 作品、4 キャラ、5 メタ、9 レーティング
};
// selected_tags.csv（tag_id,name,category,count）を読む
bool parseSelectedTags(const std::string& csv, std::vector<TaggerLabel>& out);

// 32bpp の BGRA（乗算済みアルファ。WIC の PBGRA）w×h（どちらも side 以下）から、side×side×3 の BGR の float を作る
void prepareTaggerInput(const uint8_t* bgra, int w, int h, int side, float* out);

// モデルの出力（各タグの確率）からタグを作る。レーティングは一番高いもの 1 つだけ、メタタグは使わない
std::vector<TagRow> taggerTags(const float* probs, size_t n, const std::vector<TaggerLabel>& labels,
                               double minConfidence = kTaggerMinConfidence);

// 段階の版（モデルの版の文字列と kTaggerCodeVersion から作る正の整数）
int taggerStageVersion(const std::string& modelVersion);

// 推論をする部品（Windows では ONNX Runtime）。run は複数のスレッドから呼ばれる
class TaggerModel {
public:
    virtual ~TaggerModel() = default;
    virtual int inputSide() const = 0;  // 448 など
    virtual bool run(const std::vector<float>& input, std::vector<float>& probs, std::string& error) = 0;
};

class TaggerAnalyzer : public Analyzer {
public:
    // decode(パス, 一辺の上限, BGRA, 幅, 高さ): 向きを直し、一辺の上限に収まるよう縮めた画素
    using Decoder = std::function<bool(const std::string& path, int box, std::vector<uint8_t>& bgra, int& w, int& h)>;
    TaggerAnalyzer(std::shared_ptr<TaggerModel> model, std::vector<TaggerLabel> labels, int version, Decoder decode,
                   std::function<void()> threadInit = {}, std::function<void()> threadExit = {})
        : model_(std::move(model)), labels_(std::move(labels)), version_(version), decode_(std::move(decode)),
          init_(std::move(threadInit)), exit_(std::move(threadExit)) {}
    std::string stageName() const override { return stage::kTagger; }
    int version() const override { return version_; }
    AnalyzerOutput analyze(const AnalyzerInput& in) override;
    void threadBegin() override {
        if (init_) init_();
    }
    void threadEnd() override {
        if (exit_) exit_();
    }
    std::string lastError() const {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return lastError_;
    }

private:
    std::shared_ptr<TaggerModel> model_;
    std::vector<TaggerLabel> labels_;
    int version_;
    Decoder decode_;
    std::function<void()> init_, exit_;
    mutable std::mutex errorMutex_;
    std::string lastError_;
};

}  // namespace imgdupe
