// フォルダ名から作者・サークル・作者のアカウント（pixiv の ID、X のアカウントなど）を推し量る（AI を使わない）。
// 1. 名前だけで分かるもの: "pixiv_12345"・"(pixiv 12345)" のような名札つきの ID、"@account"、
//    サイト名のフォルダ（pixiv・twitter・fanbox など）の下の gallery-dl の形（"12345 account" など）
// 2. 形をそろえた兄弟のフォルダ: 同じ親の下で同じ形（"数字 名前" や "名前 (pixiv 数字)" など）のフォルダが並んでいれば、
//    中の画像から分かっていること（json・照会で分かった作者と作者の ID、pixiv の画像が多いか）と照らして、それぞれの部分の役目を決める
// 3. 教えた形: 画面で 1 つのフォルダの名前の部分ごとに役目を教えると、同じ形のフォルダに当てはめる
// 4. 作者の名前とアカウントの対応（同じフォルダ名・json・照会から）で、片方しか分からないフォルダにもう片方を足す
// 結果は画像のあるフォルダごとのタグ（出どころ infer）。下のフォルダは上のフォルダの作者・アカウントを受け継ぐ。OS に依存しない。
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "db.hpp"

namespace imgdupe {

constexpr const char* kSourceInfer = "infer";
constexpr double kInferLabeledConfidence = 0.90;  // 名札つきの ID・教えた形
constexpr double kInferEvidenceConfidence = 0.85;  // 中の画像から確かめられたもの
constexpr double kInferGroupConfidence = 0.75;     // 兄弟の形から
constexpr double kInferDictConfidence = 0.75;      // 名前とアカウントの対応から

// 役目（教える時の選択肢。保存する名前でもある）
// "" = おまかせ、"ignore" = 使わない、"artist" / "circle" / "title" / "event"、
// アカウント: "pixiv" / "x" / "fanbox" / "fantia" / "skeb" / "nijie" / "misskey" / "bluesky" / "booth" / "patreon" / "ci-en"
const std::vector<std::string>& inferRoles();
bool isAccountRole(const std::string& role);
// アカウントのタグの文字（"pixiv:12345"・"x:@name"）と、そのページの URL（分からなければ空）
std::string accountTag(const std::string& site, const std::string& value);
std::string accountUrl(const std::string& tag);

// フォルダ名の形: 値を持つ部分（数字・文字・@つき）と、その間のかっこ・記号・サイト名
struct NameSlot {
    std::string value;
    char cls = 'S';       // 'D' = 数字、'S' = 文字、'@' = @ のついたもの
    std::string label;    // 直前のサイト名（"pixiv" など。"id" も）
};
struct NameShape {
    std::string key;              // 形の名前（例 "D S"、"S(pixiv D)"、"[S]S"）
    std::vector<NameSlot> slots;
};
NameShape nameShape(const std::string& folderName);

// 教えた形（scope: その親フォルダの下だけ。空ならどこでも）
struct InferRule {
    int64_t id = 0;
    std::string scope, shape;
    std::vector<std::string> roles;  // slots と同じ数
    std::string example;             // 教えた時のフォルダ名
};
std::vector<InferRule> loadInferRules(Db& db);
int64_t saveInferRule(Db& db, const InferRule& rule);  // 同じ scope・shape のものは置き換える
void deleteInferRule(Db& db, int64_t id);

// 推し量った 1 つ
struct InferredTag {
    std::string tag, kind;  // kind: artist / circle / title / event / account
    double confidence = 0;
    bool operator<(const InferredTag& o) const { return kind != o.kind ? kind < o.kind : tag < o.tag; }
};

// 中の画像から分かっていること（フォルダの直下の画像の分）
struct DirEvidence {
    int images = 0;
    std::map<std::string, int> sites;     // 投稿の出どころ（pixiv / x / md5 …）の枚数
    std::set<std::string> accounts;       // 作者のアカウント（"pixiv:12345"）
    std::set<std::string> artists;        // 作者の名前（小文字にしたもの）
    std::map<std::string, std::string> artistNames;  // 小文字 → 元の書き方
};

struct InferInput {
    std::map<std::string, DirEvidence> dirs;  // 画像のあるフォルダ（区切りは \ か /。末尾に区切りなし）→ 分かっていること
    std::vector<InferRule> rules;
    std::set<std::string> knownArtists;   // 小文字。json・照会・同人誌の形のフォルダから
    std::set<std::string> knownCircles;   // 小文字
    std::vector<std::pair<std::string, std::string>> pairs;  // (アカウント, 作者名)。json・照会から
};

// 画像のあるフォルダごとの推し量ったタグ（上のフォルダの分も含む）
std::map<std::string, std::vector<InferredTag>> inferFolders(const InferInput& in);
// 1 つのフォルダ名を（兄弟や中の画像を見ずに）解いたもの（説明・テスト用）。site はサイト名のフォルダの下ならそのサイト
std::vector<InferredTag> inferFromName(const std::string& folderName, const std::string& site);
// 形を当てはめた結果（教える画面の見本用）
std::vector<InferredTag> applyRoles(const NameShape& shape, const std::vector<std::string>& roles, double confidence);

// DB の画像・タグ・出典から InferInput を作り、推し量って、変わったフォルダの画像のタグ（出どころ infer）を書き直す。
// 画像・作者の情報・教えた形が前回から変わっていなければ何もしない（force で必ず）。書き直したフォルダの数を返す
struct InferSummary {
    size_t dirs = 0, changedDirs = 0, images = 0, tagged = 0;
    bool skipped = false;
    bool cancelled = false;
};
// cancel が立ったら、そこまでの分を残して止める（次の回で続きから）
InferSummary runFolderInference(Db& db, bool force = false, const std::atomic<bool>* cancel = nullptr);
// 1 つの画像の分（段階 names で新しい画像に付ける。前回の結果のフォルダから）
std::vector<TagRow> storedInferTags(Db& db, const std::string& imagePath);

}  // namespace imgdupe
