// 解析器の作法（仕様「解析層」）: 画像を受け取り、タグや情報を返す同じ形にそろえる。
// 段階ごとの処理状態は stages テーブルに「段階の名前・版」で記録し、版が変わったらその段階だけやり直す。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "db.hpp"

namespace imgdupe {

// 処理の段階（仕様の 1〜9）。stages テーブルにはこの名前で残す
namespace stage {
constexpr const char* kHash = "hash";        // 2. dHash（同じ大きさのファイルだけ SHA は run の中で）
constexpr const char* kThumb = "thumb";      // 3. サムネイル
constexpr const char* kMeta = "meta";        // 4. メタデータ・名前解析
constexpr const char* kTagger = "tagger";    // 5. WD14
constexpr const char* kClip = "clip";        // 6. CLIP
constexpr const char* kExtra = "extra";      // 7. OCR・顔・画風など
constexpr const char* kExternal = "external";  // 8. 外部照会
constexpr const char* kMerge = "merge";      // 9. 統合
}  // namespace stage

// 見た目の特徴量（段階 "hash"）の版。作り方を変えたら上げる（全件の hash だけやり直しになる）
constexpr int kHashVersion = 1;

struct AnalyzerInput {
    int64_t imageId = 0;
    std::string path;
};

struct AnalyzerOutput {
    bool ok = true;              // false なら段階を「失敗」にする（同じ版では再挑戦しない）
    std::vector<TagRow> tags;    // 出どころ（source）は空なら解析器の段階の名前
};

class Analyzer {
public:
    virtual ~Analyzer() = default;
    virtual std::string stageName() const = 0;
    virtual int version() const = 0;
    // 複数のスレッドから同時に呼ばれる
    virtual AnalyzerOutput analyze(const AnalyzerInput& in) = 0;
    // 各スレッドの始めと終わり（COM の初期化など）
    virtual void threadBegin() {}
    virtual void threadEnd() {}
};

struct StageReport {
    size_t processed = 0;
    size_t failed = 0;
    bool cancelled = false;
};

// 呼んだ時にその段階が今の版で済んでいない画像を、新しく追加されたものから順に batch 枚ずつ処理する。
// 結果（タグと段階の状態）はひとまとまりごとに書くので、いつ止めても続きから再開できる
StageReport runStage(Db& db, Analyzer& analyzer, unsigned threads, const std::atomic<bool>& cancel,
                     Progress* progress = nullptr, size_t batch = 256);

}  // namespace imgdupe
