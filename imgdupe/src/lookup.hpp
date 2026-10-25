// 外部照会（仕様「外部照会」）: ファイル名などから分かった投稿 ID で、投稿サイトのタグと作者を調べる。
// - ID 照会: pixiv（12345678_p0）と Danbooru（ファイル名の MD5）。出どころ id_lookup（0.95）
// - 逆画像検索: 出典の分からない画像のサムネイルを SauceNAO に送る（API キーが要る）。似ている度合い 80 以上なら、
//   見つかった pixiv / Danbooru の投稿を照会してタグを付ける。出どころ reverse（0.85）
// 調べた結果は sources に残し（checked = 調べた時刻）、同じものは二度と調べない。
// 通信は呼ぶ側が渡す（Windows では WinHTTP、テストでは偽物）。OS に依存しない。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core.hpp"
#include "db.hpp"

namespace imgdupe {

constexpr const char* kSourceIdLookup = "id_lookup";
constexpr const char* kSourceReverse = "reverse";
constexpr double kIdLookupConfidence = 0.95;
constexpr double kReverseConfidence = 0.85;
constexpr double kSauceNaoMinSimilarity = 80.0;
constexpr int kSauceNaoDefaultDailyLimit = 100;  // 無料の API キーの 1 日の上限

// 投稿を照会した結果
struct LookupPost {
    bool found = false;
    std::string site, postId, authorId, url, title;
    std::vector<TagRow> tags;  // 出どころ・確度は保存する時に決める
};

// Danbooru の /posts/<id>.json か /posts.json?tags=md5:... の応答（配列なら最初の投稿。空なら found = false）
bool parseDanbooruPost(const std::string& json, LookupPost& out);
// pixiv の /ajax/illust/<id> の応答（"error": true なら found = false）
bool parsePixivIllust(const std::string& json, LookupPost& out);

struct SauceMatch {
    double similarity = 0;
    std::string pixivId, danbooruId, author, url, title;
};
struct SauceResult {
    int status = 0;  // header.status（0 = 成功、負 = こちらの誤り（API キーなど）、正 = 向こうの都合）
    int shortRemaining = -1, longRemaining = -1;
    std::string message;
    bool found = false;  // 似ている度合いが基準以上のものがあった
    SauceMatch best;     // pixiv か Danbooru の ID があるものを優先し、その中で一番似ているもの
};
bool parseSauceNao(const std::string& json, SauceResult& out, double minSimilarity = kSauceNaoMinSimilarity);

// ファイルを 1 つ送る multipart/form-data の本文。contentType に Content-Type の値を返す
std::string buildMultipart(const std::string& field, const std::string& fileName, const std::string& fileType,
                           const std::string& data, std::string& contentType);

struct LookupTask {
    int64_t imageId = 0;
    std::string site, postId;  // site: "pixiv" / "md5"
};
// まだ調べていない ID（json などで既にタグが分かっている画像は除く）
std::vector<LookupTask> pendingIdLookups(Db& db, size_t limit);
// 出典が何も分からない画像（サムネイルがあり、カメラで撮った写真ではないもの）
std::vector<int64_t> pendingReverseLookups(Db& db, size_t limit);

// 照会の結果を書く（タグ・作者・URL。見つからなくても調べたことは残す）
void storeIdLookup(Db& db, const LookupTask& task, const LookupPost& post, int64_t now);
// 逆画像検索の結果を書く（posts: 見つかった投稿を照会した結果。sources に saucenao の行を残す）
void storeReverseLookup(Db& db, int64_t imageId, const SauceResult& r, const std::vector<LookupPost>& posts, int64_t now);
// 選んだ画像を、もう一度調べるようにする
void resetLookups(Db& db, const std::vector<int64_t>& ids);

struct HttpResult {
    bool ok = false;  // 応答があった（status を見る）
    int status = 0;
    std::string body, error;
};

struct LookupOptions {
    bool idLookup = false, reverse = false;
    std::string sauceKey;
    int sauceDailyLimit = kSauceNaoDefaultDailyLimit;
    size_t maxIdPerRun = 300;     // 1 回の解析で照会する数の上限（1 秒に 1 回なので、おおよそ 5 分）
    size_t maxReversePerRun = 30;
    std::string danbooruUrl = "https://danbooru.donmai.us";
    std::string pixivUrl = "https://www.pixiv.net";
    std::string sauceUrl = "https://saucenao.com";
};

struct LookupNet {
    std::function<HttpResult(const std::string& url, const std::vector<std::string>& headers)> get;
    std::function<HttpResult(const std::string& url, const std::vector<std::string>& headers, const std::string& body)> post;
    std::function<bool(int64_t imageId, std::string& jpeg)> thumbnail;
    std::function<void(int ms)> sleep;
    std::function<int64_t()> now;  // UNIX 時刻（秒）
};

struct LookupReport {
    size_t looked = 0, found = 0, reverseSent = 0, reverseFound = 0;
    std::string error;  // 途中でやめた理由（次の回にまた試す）
};

// 間隔: 同じサイトへは 1 秒に 1 回、SauceNAO へは 8 秒に 1 回（30 秒に 4 回まで）
LookupReport runLookups(Db& db, const LookupOptions& opt, const LookupNet& net, const std::atomic<bool>& cancel,
                        Progress* progress = nullptr);

// SauceNAO に今日送った数（settings の "saucenao:day" = "<日>:<数>"。日は UTC）
int sauceSentToday(Db& db, int64_t now);

}  // namespace imgdupe
