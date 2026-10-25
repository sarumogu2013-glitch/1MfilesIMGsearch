// 品質スコア（仕様「品質スコア」: ぼやけ・低解像度・アップスケール画像の検出、見栄えの数値化）。OS に依存しない。
// - 細かさ（detail）: 画像の真ん中など（大きければ 5 か所）の 512×512 の灰色を FFT し、周波数ごとの強さ（軸の上を除いた中央値）を
//   低い周波数（0.03〜0.12 サイクル/画素）の傾きから伸ばした予測と比べ、予測の 1/10 を下回る最初の周波数 × 2（0〜1）。
//   画素の細かさまで中身があれば 1、2 倍に引き伸ばした画像やぼやけた画像は 0.5 前後。最近傍法で拡大した（ギザギザの）画像は見分けられない
// - 実質の解像度: 長い辺 × 細かさ
// - くっきり度: 見る大きさ（長い辺 1024 以下）に縮めた画像での細かさ。大きな画像を引き伸ばしたものは縮めると細かさが戻るので、
//   「細かさ」は低くても「くっきり度」は高い。ピンぼけ・手ぶれは両方とも低い。平らな画像は測れない（-1）
// - 見栄え（aesthetic）: CLIP の画像のベクトルに LAION の aesthetic predictor（線形）を掛けた値（1〜10 くらい）
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "analyzer.hpp"
#include "db.hpp"
#include "vectors.hpp"

namespace imgdupe {

constexpr const char* kStageQuality = "quality";
constexpr int kQualityVersion = 1;
constexpr int kQualityDecodeSide = 4096;  // これより大きい画像は縮めて測る（拡大の倍率はその大きさで見る）
constexpr double kBlurryBelow = 0.5;      // くっきり度がこれ未満なら「ぼやけている」
constexpr double kSoftBelow = 0.75;       // 細かさがこれ未満なら「拡大された・ぼやけた（画素ほどの細かさが無い）」
constexpr int kLowResBelow = 800;         // 実質の解像度（長い辺）がこれ未満なら「低解像度」

struct GrayImage {
    int width = 0, height = 0;
    std::vector<uint8_t> px;  // 行の順
};
// 32bpp の BGRA（PBGRA）から灰色に（透明な部分は白）
GrayImage grayFromBgra(const std::vector<uint8_t>& bgra, int w, int h);

struct QualityResult {
    double sharpness = -1;  // くっきり度（測れなければ -1）
    double detail = -1;     // 細かさ（0〜1。測れなければ -1）
    int effectiveSide = 0;  // 実質の解像度（長い辺、元の画像の画素で。測れなければ長い辺）
};
// scale: 元の画像に対する img の大きさ（縮めて読んだなら 1 未満。細かさはその大きさで見る）
QualityResult measureQuality(const GrayImage& img, double scale = 1.0);

// CLIP の画像のベクトル（512 次元、長さ 1）→ 見栄えの点数
double aestheticScore(const float* clipVec);

// 段階 quality（画素を読むのは呼ぶ側）
class QualityAnalyzer : public Analyzer {
public:
    // decode: 一辺 maxSide 以内に縮めた BGRA と、元の大きさ
    using Decode = std::function<bool(const std::string& path, int maxSide, std::vector<uint8_t>& bgra, int& w, int& h, int& origW,
                                      int& origH)>;
    QualityAnalyzer(Decode decode, std::function<void()> init, std::function<void()> exit)
        : decode_(std::move(decode)), init_(std::move(init)), exit_(std::move(exit)) {}
    std::string stageName() const override { return kStageQuality; }
    int version() const override { return kQualityVersion; }
    AnalyzerOutput analyze(const AnalyzerInput& in) override;
    bool store(Db& db, int64_t imageId, AnalyzerOutput& out) override;
    void threadBegin() override {
        if (init_) init_();
    }
    void threadEnd() override {
        if (exit_) exit_();
    }

private:
    Decode decode_;
    std::function<void()> init_, exit_;
};

// CLIP のベクトル（512 次元）があって見栄えの点数がまだ無い画像に点数を付ける（付けた数を返す）
size_t applyAesthetic(Db& db, const VectorIndex& vecs);

}  // namespace imgdupe
