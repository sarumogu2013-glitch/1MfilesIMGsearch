// 検索（仕様「検索と確認」「メタデータでの検索・ソート」）。条件から SQL を組み立て、DB 側で絞り込みと並べ替えを行う。
// OS に依存しない。
#pragma once

#include <cstdint>
#include <set>
#include <utility>
#include <string>
#include <vector>

#include "db.hpp"

namespace imgdupe {

enum class SortKey { Path, Created, Modified, Taken, Resolution, Size, Rating, Title, Confidence, Aesthetic, Effective, FileName, Relevance };

// フリーワードの範囲: すべて（タグ・タイトルなどの文字・ファイル名）、タグに限る、タイトルに限る
enum class FreeScope { All, Tags, Title };
// 日付で絞り込む時の日付
enum class DateField { Taken, Modified, Created };

// 品質での絞り込み（quality.hpp の段階 quality が済んだ画像だけ）
enum class QualityFilter { Any, Good, Blurry, Soft, LowRes };

struct SearchQuery {
    // フリーワード（空白で区切る。どの語にも合う画像。"-語" は合うものを除く）。関連度 = 語ごとの合い方の和
    // （タグがそのまま合う = 確度、タグの頭が合う = 確度 × 0.7、タイトル = 1、ほかの文字 = 0.6、ファイル名 = 0.5）
    std::string freeText;
    FreeScope freeScope = FreeScope::All;
    // タグ（別名は tag_dict でまとめる）。tags は全部を持つもの、excludeTags はどれも持たないもの
    std::vector<std::string> tags;
    std::vector<std::string> excludeTags;
    double minConfidence = 0.5;          // 確度スライダー（0〜1）
    std::vector<std::string> sources;    // 使う出どころ（空 = すべて）。確度はこの出どころだけで合成する
    std::vector<std::string> excludeSources;  // 使わない出どころ（後から増えた出どころは使う）
    // タイトル・件名・コメント・キーワード・PNG のテキストの部分一致
    std::string text;
    // メタデータ（0 / -1 / 空 = 条件なし）
    int minRating = -1;
    bool noRating = false;               // 星なし
    int64_t takenFrom = 0, takenTo = 0;  // dateField の日時（Unix 秒、takenTo はその秒を含む）
    DateField dateField = DateField::Taken;
    bool noTaken = false;                // 撮影日時なし（EXIF なし）
    int minWidth = 0, minHeight = 0;
    int64_t minSize = 0, maxSize = 0;    // バイト
    std::string format;                  // "JPEG" など
    bool animatedOnly = false;
    bool unclassified = false;           // 未分類: 確度 minConfidence 以上のタグ（出どころで絞った合成）が 1 つも無い画像
    std::string pathContains;            // パス（フォルダ名・ファイル名）の部分一致
    std::string downloadContains;        // ダウンロード元の URL の部分一致（"pixiv" など）
    bool deadLinks = false;              // 出典の投稿が消えている（リンク切れ）画像
    // Good: くっきりしていて画素ほどの細かさがある、Blurry: ぼやけている、Soft: 拡大された・ぼやけた（細かさが足りない）、
    // LowRes: 実質の解像度（長い辺）が kLowResBelow 未満
    QualityFilter quality = QualityFilter::Any;
    // 確認キュー: 1 つ目のタグをまだ手動で確定していない画像のうち、確度が kReviewMax 未満のものを、
    // 迷っている順（確度が 0.5 に近い順）に並べる（sort は使わない）
    bool review = false;
    // 内容（CLIP）: 言葉で探す / この画像に似た画像。どちらかがあれば、似ている順に並べる（sort は使わない）。
    // SQL では扱わない（rankByVector で、ほかの条件に合う画像を似ている順にする）
    std::string clipText;
    int64_t similarTo = 0;
    // 選んだ画像（空でなければ、この画像のうちほかの条件にも合うものだけ。「選んだ画像をスマートフォルダに保存」）
    std::vector<int64_t> ids;
    // 並べ替え
    SortKey sort = SortKey::Relevance;  // 関連度（フリーワードが無ければパス順）
    bool descending = false;
    size_t limit = 0;                    // 0 = すべて
};

struct SearchResult {
    std::vector<int64_t> ids;  // 並べ替えた画像 ID
    size_t total = 0;          // 条件に合う数（limit を超えていても全部の数）
};

// タグの文字列（空白区切り。"-タグ" は除外）を分ける
void parseTagQuery(const std::string& text, std::vector<std::string>& tags, std::vector<std::string>& exclude);

SearchResult search(Db& db, const SearchQuery& q);

// 一覧に出す 1 件分
struct ImageInfo {
    int64_t id = 0;
    std::string path;
    int64_t size = 0, mtime = 0, taken = 0;
    int width = 0, height = 0, rating = -1;
    std::string title, format;
    double sharpness = -1, detail = -1, aesthetic = -1;  // 品質（無ければ -1）
    int effective = 0;
};
bool imageInfo(Db& db, int64_t id, ImageInfo& out);

// 画像の出典（投稿のページの URL が分かるもの。サイトの名前の順）
struct SourceLink {
    std::string site, postId, url;
    int status = -1;  // 1 = ある、0 = 消えている（リンク切れ）、-1 = 確かめていない・分からない
};
std::vector<SourceLink> sourceLinks(Db& db, int64_t imageId);
// 出典の投稿のページの URL（sources.url が無ければ ID から作る）。作れなければ空
std::string sourcePageUrl(const std::string& site, const std::string& postId, const std::string& url);

constexpr double kReviewMax = 0.9;

// 手動で正誤を確定する（正しい = 1.0、違う = 0）。別名を含め、同じタグの前の確定は置き換える
void setManualTag(Db& db, int64_t imageId, const std::string& tag, bool correct);
void clearManualTag(Db& db, int64_t imageId, const std::string& tag);

// タグの手動の確定を取り消せるように、変える前の状態を history に残す（仕様「操作の取り消し」）。
// 使い方: 変える前に touch(画像, タグ) → setManualTag など → 最後に save(説明)
class TagHistory {
public:
    explicit TagHistory(Db& db) : db_(db) {}
    void touch(int64_t imageId, const std::string& tag);
    int64_t save(const std::string& description);  // 何も touch していなければ 0
private:
    Db& db_;
    std::string before_;  // JSON の配列の中身
    std::set<std::pair<int64_t, std::string>> seen_;
};
// 最後のタグの操作を元に戻す（説明を返す）。戻すものが無ければ false
bool undoLastTagEdit(Db& db, std::string& description);

// 条件（並べ替え・件数・CLIP 以外）が 1 つでもあるか
bool hasConditions(const SearchQuery& q);

// 条件を JSON にする / JSON から戻す（スマートフォルダの保存用。知らない項目は無視、無い項目は既定値）
std::string queryToJson(const SearchQuery& q);
bool queryFromJson(const std::string& json, SearchQuery& q);

// スマートフォルダ（検索条件に名前を付けて DB に保存したもの。中身は毎回 DB に問い合わせるので常に最新）
struct SmartFolder {
    int64_t id = 0;
    std::string name;
    SearchQuery query;
};
std::vector<SmartFolder> smartFolders(Db& db);  // 並べた順（並べていないものは後ろに名前順）
// 同じ名前（大文字小文字は区別しない）があれば条件を上書きする。保存した ID を返す
int64_t saveSmartFolder(Db& db, const std::string& name, const SearchQuery& q);
bool renameSmartFolder(Db& db, int64_t id, const std::string& name);  // 同じ名前が別にあれば false
void deleteSmartFolder(Db& db, int64_t id);
void setSmartFolderOrder(Db& db, const std::vector<int64_t>& ids);  // この並びにする
// 書き出し（JSON。{"queries": [{"name", "query"}]}）と読み込み（読めなければ false。壊れた項目は飛ばす）
std::string exportSmartFolders(const std::vector<SmartFolder>& folders);
bool importSmartFolders(const std::string& text, std::vector<SmartFolder>& out);

// 出どころの一覧（タグの数の多い順）
std::vector<std::pair<std::string, int64_t>> tagSources(Db& db);

// 確度の合成: p = 1 - (1 - p1)(1 - p2)...（仕様「確度の合成」）
double combineConfidence(const std::vector<double>& ps);

}  // namespace imgdupe
