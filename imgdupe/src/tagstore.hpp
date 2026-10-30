// タグの置き場所（版 12）。タグの名前・種類・出どころを番号で持ち（tag_keys・tag_sources）、画像ごとのタグは tag_rows に番号で持つ。
// 読む側は、前と同じ列のビュー tags（image_id, tag, kind, confidence, source, manual）を使う。
// 書く側は、ここの関数を使う（前の版の DB を変換している間は、まだ tags が表なので、そちらに書く）。
// 前の版の DB は convertTagsStep で少しずつ変換する（途中で止めても続きから。変換の間に書き換わった画像は入れ替える前に移し直す）。
// OS に依存しない。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "db.hpp"

namespace imgdupe {

// 新しい形か（tags がビュー）。まだ変換していなければ false
bool newTagFormat(Db& db);

// 同じタグ（画像・タグ・種類・出どころ）が既にある時
enum class TagConflict {
    Replace,     // 置き換える（手動の印も）
    Ignore,      // そのまま
    KeepHigher,  // 自動のものなら、確度の高い方
    KeepManual,  // 自動のものなら置き換える（手動で確定したものは残す）
};

// まとめて書く（同じ画像・出どころの何行もを速く書く。呼ぶ側のトランザクションの中で）
class TagWriter {
public:
    TagWriter(Db& db, TagConflict how);
    ~TagWriter();
    TagWriter(const TagWriter&) = delete;
    TagWriter& operator=(const TagWriter&) = delete;
    void put(int64_t imageId, const std::string& tag, const std::string& kind, double confidence, const std::string& source,
             bool manual);

private:
    Db& db_;
    bool newFormat_;
    Stmt* st_ = nullptr;
};

// タグの名前と種類・出どころの番号（新しい形の時。無ければ作る）
int64_t tagKeyId(Db& db, const std::string& tag, const std::string& kind);
int64_t tagSourceId(Db& db, const std::string& source);

// 出どころ source の自動のタグを全部消す（手動で確定したものは残す）
void deleteSourceTags(Db& db, const std::string& source);
// 画像の、sources の自動のタグを消す
void deleteImageTags(Db& db, int64_t imageId, const std::vector<std::string>& sources);
// 画像の、手動で確定したタグのうち、名前が names のどれか（大文字小文字を区別しない）のものを消す
void deleteManualTags(Db& db, int64_t imageId, const std::vector<std::string>& names);
// 画像 from の手動で確定したタグを、画像 to に写す（to に同じものがあればそちら）
void copyManualTags(Db& db, int64_t from, int64_t to);

// 出どころの行の数（新しい形なら数えておいた数なので速い）
int64_t tagRowCount(Db& db, const std::string& source);
int64_t tagRowCountExcept(Db& db, const std::string& source);
std::vector<std::pair<std::string, int64_t>> tagSourceCounts(Db& db);  // 多い順
// タグの名前（大文字小文字だけが違うものは 1 つ。行のあるものだけ）
std::vector<std::string> tagNames(Db& db);

// 前の版の DB の変換
struct TagConversionState {
    bool pending = false;     // まだ変換が済んでいない
    int64_t done = 0, total = 0;  // 画像の ID で、ここまで移した / 一番大きい ID
};
TagConversionState tagConversionState(Db& db);
// 次の maxImages 枚分（画像の ID の幅）を移す。全部移したら、索引を作って入れ替える（数分かかることがある）。
// 入れ替えまで済んだら true。step には今していることを入れる
bool convertTagsStep(Db& db, int64_t maxImages, std::string* step = nullptr);
// 新しく作った DB を新しい形にする（Db::migrate のトランザクションの中で。中身は空）
bool finishNewTagFormat(Db& db);
// v2.18 で変換した DB の索引を、探すのに要る列が全部入った索引に作り直す（数分かかることがある。済んでいれば何もしない）
bool tagIndexUpgradePending(Db& db);
void upgradeTagIndex(Db& db);

// DB を詰めて小さくする（VACUUM。並べ替えは一時ファイルで。数分かかることがある）。失敗したら false と理由
struct DbSpace {
    int64_t fileBytes = 0, freeBytes = 0;  // ファイルの大きさ（ページの数 × 大きさ）、そのうち使っていない分
};
DbSpace dbSpace(Db& db);
bool compactDb(Db& db, std::string& error);

}  // namespace imgdupe
