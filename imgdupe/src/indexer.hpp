// indexer の処理（仕様「解析層」）: フォルダを走査して DB に登録し、軽い段階（ハッシュ・サムネイル）を順に進める。
// 画像の読み込みなど OS に依存する部分は呼び出し側が渡す（Windows では WIC、テストでは偽物）。
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "analyzer.hpp"
#include "meta.hpp"
#include "pack.hpp"

namespace imgdupe {

// サムネイル（段階 "thumb"）の版と大きさ
constexpr int kThumbVersion = 1;
constexpr int kThumbSide = 256;

struct IndexOptions {
    std::vector<std::string> roots;
    std::string dbFile;
    std::string packDir;   // サムネイルのパックを置くフォルダ
    unsigned threads = 0;
    bool thumbnails = true;
};

struct IndexCallbacks {
    std::function<void(const std::string& root, std::vector<ScannedFile>& out)> scan;
    std::function<Features(const std::string& path)> decode;
    // サムネイルを作る（JPEG）。失敗したら false
    std::function<bool(const std::string& path, int maxSide, std::vector<uint8_t>& bytes, int& w, int& h)> encodeThumb;
    // メタデータを読む（無ければ段階 meta は飛ばす）
    std::function<bool(const std::string& path, FileMeta& out)> readMeta;
    std::function<void()> threadInit, threadExit;
    // 段階 tagger（無ければ飛ばす）。版を先に聞き、済んでいない画像がある時だけ makeTagger でモデルを読む
    std::function<int()> taggerVersion;
    std::function<std::shared_ptr<Analyzer>(std::string& error)> makeTagger;
    // 段階 clip（同じく。prepareClip は版を聞いた後、済んでいない画像を調べる前に呼ぶ）
    std::function<int()> clipVersion;
    std::function<void(Db&)> prepareClip;
    std::function<std::shared_ptr<Analyzer>(std::string& error)> makeClip;
    // 7. 品質（段階 quality。無いか false を返せば飛ばす）
    std::function<bool()> qualityEnabled;
    std::function<bool(const std::string& path, int maxSide, std::vector<uint8_t>& bgra, int& w, int& h, int& origW, int& origH)>
        decodeQuality;
    // CLIP の後（差分グループ）
    std::function<void(Db&, unsigned threads, const std::atomic<bool>& cancel, Progress*)> afterClip;
    // 8. 外部照会（投稿サイトのタグ。通信するので、設定で有効にした時だけ）
    std::function<void(Db&, const std::atomic<bool>& cancel, Progress*)> lookup;
    // 9. 統合（行動・アイテム辞書を当てる。変わっていなければ何もしない）
    std::function<void(Db&)> merge;
};

enum class IndexPhase { Idle, Scan, Hash, Thumb, Meta, Names, Tagger, Clip, Variants, Quality, Lookup, Merge, Done };

struct IndexProgress {
    std::atomic<int> phase{static_cast<int>(IndexPhase::Idle)};
    Progress prog;
};

struct IndexReport {
    size_t files = 0, added = 0, removed = 0, hashed = 0, thumbs = 0, metas = 0, names = 0, tagged = 0, clipped = 0, quality = 0, failed = 0;
    std::string taggerError;  // モデルを読めなかった時の理由
    std::string clipError;
    bool cancelled = false;
};

// 段階 2: 見た目の特徴量（images の列に書く）
class HashAnalyzer : public Analyzer {
public:
    explicit HashAnalyzer(const IndexCallbacks& cb) : cb_(cb) {}
    std::string stageName() const override { return stage::kHash; }
    int version() const override { return kHashVersion; }
    AnalyzerOutput analyze(const AnalyzerInput& in) override;
    bool store(Db& db, int64_t imageId, AnalyzerOutput& out) override;
    void threadBegin() override;
    void threadEnd() override;

private:
    const IndexCallbacks& cb_;
};

// 段階 3: サムネイル（パックファイルに追記する）
class ThumbAnalyzer : public Analyzer {
public:
    ThumbAnalyzer(const IndexCallbacks& cb, ThumbPack& pack) : cb_(cb), pack_(pack) {}
    std::string stageName() const override { return stage::kThumb; }
    int version() const override { return kThumbVersion; }
    AnalyzerOutput analyze(const AnalyzerInput& in) override;
    bool store(Db& db, int64_t imageId, AnalyzerOutput& out) override;
    void threadBegin() override;
    void threadEnd() override;

private:
    const IndexCallbacks& cb_;
    ThumbPack& pack_;
};

// 1 回分: 走査 → 登録（消えたものは消す）→ ハッシュ → サムネイル → メタデータ
IndexReport indexOnce(const IndexOptions& opt, const IndexCallbacks& cb, IndexProgress& progress,
                      const std::atomic<bool>& cancel);

}  // namespace imgdupe
