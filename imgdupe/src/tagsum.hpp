// 画像のタグをまとめる（プレビュー・タグの手動編集）。いくつもの画像のタグを 1 つの一覧に。
// 画面のスレッドでも、裏のスレッド（自分の Db で）でも使える。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "db.hpp"

namespace imgdupe {

// 手動編集でさわれるタグの由来（AI が付けるもの: タガー・辞書・CLIP・見本画像との類似）
bool aiTagSource(const std::string& source);

struct TagSummary {
    std::string tag, kind;
    double confidence = 0;  // 並べる確度（普段は付いている画像の中で一番高いもの、手動編集では一番低いもの）
    bool file = false;      // 埋め込みタグ（ファイルに書き込まれているタグ）
    int count = 0;          // 付いている画像の数
    int total = 0;          // まとめた画像の数
    int state = 1;          // 手動編集の状態: 0 否定・1 そのまま・2 確定（いくつもなら低いほう）
    bool mixed = false;     // 画像ごとに状態が違う
    bool editable = false;  // 手動編集でさわれる（AI か手で付いた画像がある）
};

// ids の画像のタグをまとめる（埋め込みタグが先、あとは確度の高い順）。
// 普段は確度 0.35 以上の管理タグだけ、手で否定したタグは出さない。edit なら否定したタグも出す。
// cancel が立ったら途中でやめて空を返す
std::vector<TagSummary> summarizeTags(Db& db, const std::vector<int64_t>& ids, bool edit, const std::atomic<bool>* cancel = nullptr);

}  // namespace imgdupe
