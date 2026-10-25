// 行動・アイテム辞書（仕様「行動・アイテム辞書」）: WD14 のタグと CLIP の文章を、自分の分類にまとめる。
// 統合の段階で当てるので、辞書を変えると数秒〜数十秒で反映される（画像を読み直さない）。
//
//   # コメント
//   [テニスをしている]
//   種類: action                       （省略すると action。kind: でも可）
//   wd14: tennis_racket, tennis_ball   （どれかが付いていれば。確度はその中で一番高いもの。出どころ dict）
//   clip: "a person playing tennis"    （画像のベクトルとの似ている度合いを確度にする。出どころ clip）
//
// OS に依存しない（CLIP の言葉側のモデルは呼ぶ側が渡す）。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "db.hpp"
#include "vectors.hpp"

namespace imgdupe {

constexpr const char* kSourceDict = "dict";
constexpr const char* kSourceClip = "clip";
constexpr const char* kDictionaryFile = "dictionary.txt";

struct DictEntry {
    std::string name;
    std::string kind = "action";
    std::vector<std::string> wd14;
    std::vector<std::string> clip;
};

// 読めない行があれば false と、その行番号つきの理由
bool parseDictionary(const std::string& text, std::vector<DictEntry>& out, std::string& error);

// 初めての時に置く辞書（例はコメントにしてあるので、そのままでは何もしない）
std::string defaultDictionary();

// 辞書の中身が変わったかを見るための値
std::string dictionaryHash(const std::string& text);

// wd14 の行: 出どころ dict のタグを全部入れ替える
void applyDictionaryTags(Db& db, const std::vector<DictEntry>& entries);

// CLIP の似ている度合い（内積）→ 確度（0.27 で 0.5。ゼロショットの値はおおよそ 0.2〜0.35 に集まる）
double clipProbability(double sim);

// clip の行: 文章のベクトル（長さ 1。いくつか書けばそれぞれ）と全部の画像の内積から確度を出し、minP 以上を返す（画像 ID の順）
std::vector<std::pair<int64_t, float>> clipMatches(const VectorIndex& index, const std::vector<std::vector<float>>& textVecs,
                                                   double minP, unsigned threads = 0);

// 出どころ clip（辞書の clip の行から）のタグを全部入れ替える。matches は entries と同じ並び（clip の行が無ければ空）
void replaceClipDictTags(Db& db, const std::vector<DictEntry>& entries,
                         const std::vector<std::vector<std::pair<int64_t, float>>>& matches);

}  // namespace imgdupe
