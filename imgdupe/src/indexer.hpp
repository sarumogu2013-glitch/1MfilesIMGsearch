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
    // false なら、スキャンタスクだけを済ませて終える（roots 全体は調べ直さない）
    bool fullPass = true;
    // 立てると、今の段階をひとまとまりの切れ目で止め、スキャンタスクの並びを読み直す（上に別のタスクが来た時）。
    // indexOnce が下ろす
    std::atomic<bool>* preempt = nullptr;
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

enum class IndexPhase { Idle, Scan, Hash, Thumb, Meta, Names, Tagger, Clip, Variants, Quality, Lookup, Merge, Convert, Done };

// 「総合」の進み具合（タスクトレイの状況の窓）: DB の画像の数と、段階ごとにその版で済んだ数
struct StageCount {
    int64_t done = 0;    // 済んだ
    int64_t failed = 0;  // 読めなかった・失敗（もう一度はしない）
};
int64_t imageCount(Db& db);
StageCount stageCount(Db& db, const std::string& stage, int version);

struct IndexProgress {
    std::atomic<int> phase{static_cast<int>(IndexPhase::Idle)};  // CPU の側（Idle = 済んで GPU の側を待っている）
    Progress prog;
    std::atomic<int> gpuPhase{static_cast<int>(IndexPhase::Idle)};  // 同時に進める GPU の側（タガー・CLIP。Idle = していない）
    Progress gpuProg;
    std::atomic<unsigned> doneMask{0};  // 今の対象で済んだ段階（1 << IndexPhase）
    std::atomic<int64_t> task{-1};  // 今のスキャンタスクの ID（0 = フォルダ全体の確認、-1 = なし）
};

struct IndexReport {
    size_t tasks = 0;  // 済ませたスキャンタスクの数
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

// path が roots のどれかの中なら、その root の書き方（大文字・小文字、/ と \）にそろえる。どれの中でもなければそのまま
std::string spellLikeRoot(const std::string& path, const std::vector<std::string>& roots);
// 調べるフォルダの中なのに書き方が違うパスの画像のうち、正しい書き方の同じ画像が DB にあるもの（二重の登録）を消す。
// 手動で確定したタグは正しい方へ移す。消した数を返す
size_t removeMisspelled(Db& db, const std::vector<std::string>& roots);

// 1 回分: スキャンタスクを上から 1 つずつ（その中の画像だけ、全部の段階）→ フォルダ全体:
// 走査 → 登録（消えたものは消す）→ ハッシュ → サムネイル → メタデータ → …
IndexReport indexOnce(const IndexOptions& opt, const IndexCallbacks& cb, IndexProgress& progress,
                      const std::atomic<bool>& cancel);

}  // namespace imgdupe
