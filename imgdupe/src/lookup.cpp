#include "lookup.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>

#include "json.hpp"

namespace imgdupe {

namespace {

std::string scalar(const Json* v) {
    if (!v) return {};
    if (v->type == Json::String || v->type == Json::Number) return v->str;
    return {};
}

std::string lowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool allDigits(const std::string& s) {
    if (s.empty() || s.size() > 20) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

bool isMd5(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

void addTag(LookupPost& out, std::set<std::string>& seen, const std::string& tag, const std::string& kind) {
    if (tag.empty() || tag.size() > 200) return;
    if (!seen.insert(kind + '\n' + lowerAscii(tag)).second) return;
    out.tags.push_back({tag, kind, "", 0, false});
}

void addSpaceList(LookupPost& out, std::set<std::string>& seen, const std::string& s, const std::string& kind) {
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find(' ', a);
        if (b == std::string::npos) b = s.size();
        if (b > a) addTag(out, seen, s.substr(a, b - a), kind);
        a = b + 1;
    }
}

std::string urlEncode(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

const char* const kKnownSourcesSql = "('meta','id_lookup','reverse')";

void insertTags(Db& db, int64_t imageId, const std::vector<TagRow>& tags, const char* source, double conf) {
    Stmt ins(db.handle(),
             "INSERT INTO tags(image_id,tag,kind,confidence,source,manual) VALUES(?,?,?,?,?,0) "
             "ON CONFLICT(image_id,kind,tag,source) DO UPDATE SET confidence=max(confidence, excluded.confidence) WHERE manual=0");
    for (const auto& t : tags) {
        ins.bind(1, imageId).bind(2, t.tag).bind(3, t.kind).bind(4, conf).bind(5, std::string(source));
        ins.step();
        ins.reset();
    }
}

void insertSource(Db& db, int64_t imageId, const std::string& site, const std::string& postId, const std::string& authorId,
                  const std::string& url, int64_t now) {
    Stmt ins(db.handle(),
             "INSERT INTO sources(image_id,site,post_id,author_id,url,checked) VALUES(?,?,?,?,?,?) "
             "ON CONFLICT(image_id,site,post_id) DO UPDATE SET checked=excluded.checked, "
             "author_id=coalesce(nullif(excluded.author_id,''), author_id), url=coalesce(nullif(url,''), excluded.url)");
    ins.bind(1, imageId).bind(2, site).bind(3, postId).bind(4, authorId).bind(5, url).bind(6, now);
    ins.step();
}

// 中止を確かめながら待つ
bool pause(const LookupNet& net, int ms, const std::atomic<bool>& cancel) {
    while (ms > 0 && !cancel) {
        const int step = std::min(ms, 200);
        if (net.sleep) net.sleep(step);
        ms -= step;
    }
    return !cancel;
}

// サイトごとの間隔
struct Pacer {
    int intervalMs;
    bool used = false;
    bool wait(const LookupNet& net, const std::atomic<bool>& cancel) {
        if (used && !pause(net, intervalMs, cancel)) return false;
        used = true;
        return true;
    }
};

std::string dayKey(int64_t now) { return std::to_string(now / 86400); }

void addSauceSent(Db& db, int64_t now) {
    const int n = sauceSentToday(db, now) + 1;
    db.setSetting("saucenao:day", dayKey(now) + ":" + std::to_string(n));
}

}  // namespace

bool parseDanbooruPost(const std::string& json, LookupPost& out) {
    out = LookupPost{};
    Json j;
    if (!parseJson(json, j)) return false;
    const Json* p = &j;
    if (j.type == Json::Array) {
        if (j.arr.empty()) return true;
        p = &j.arr[0];
    }
    if (p->type != Json::Object) return false;
    out.site = "danbooru";
    out.postId = scalar(p->get("id"));
    if (out.postId.empty()) return true;  // 消された投稿など
    out.found = true;
    out.url = "https://danbooru.donmai.us/posts/" + out.postId;
    std::set<std::string> seen;
    for (const char* kind : {"artist", "character", "copyright", "general"})
        addSpaceList(out, seen, p->getString(std::string("tag_string_") + kind), kind);
    return true;
}

bool parsePixivIllust(const std::string& json, LookupPost& out) {
    out = LookupPost{};
    Json j;
    if (!parseJson(json, j) || j.type != Json::Object) return false;
    out.site = "pixiv";
    const Json* err = j.get("error");
    const Json* body = j.get("body");
    if ((err && err->type == Json::Bool && err->str == "true") || !body || body->type != Json::Object) return true;
    out.postId = scalar(body->get("illustId"));
    if (out.postId.empty()) out.postId = scalar(body->get("id"));
    if (out.postId.empty()) return true;
    out.found = true;
    out.url = "https://www.pixiv.net/artworks/" + out.postId;
    out.authorId = scalar(body->get("userId"));
    out.title = body->getString("illustTitle");
    if (out.title.empty()) out.title = body->getString("title");
    std::set<std::string> seen;
    addTag(out, seen, body->getString("userName"), "artist");
    if (const Json* tags = body->get("tags")) {
        const Json* list = tags->type == Json::Object ? tags->get("tags") : tags;
        if (list && list->type == Json::Array)
            for (const Json& t : list->arr) {
                if (t.type == Json::String) {
                    addTag(out, seen, t.str, "general");
                    continue;
                }
                if (t.type != Json::Object) continue;
                addTag(out, seen, t.getString("tag"), "general");
                if (const Json* tr = t.get("translation"))
                    if (tr->type == Json::Object) addTag(out, seen, tr->getString("en"), "general");
            }
    }
    return true;
}

bool parseSauceNao(const std::string& json, SauceResult& out, double minSimilarity) {
    out = SauceResult{};
    Json j;
    if (!parseJson(json, j) || j.type != Json::Object) return false;
    const Json* h = j.get("header");
    if (!h || h->type != Json::Object) return false;
    auto num = [](const Json* v, int def) {
        const std::string s = scalar(v);
        return s.empty() ? def : std::atoi(s.c_str());
    };
    out.status = num(h->get("status"), 0);
    out.shortRemaining = num(h->get("short_remaining"), -1);
    out.longRemaining = num(h->get("long_remaining"), -1);
    out.message = h->getString("message");
    const Json* results = j.get("results");
    if (!results || results->type != Json::Array) return true;
    bool bestHasId = false;
    for (const Json& r : results->arr) {
        const Json* rh = r.get("header");
        const Json* d = r.get("data");
        if (!rh || !d || d->type != Json::Object) continue;
        SauceMatch m;
        m.similarity = std::atof(scalar(rh->get("similarity")).c_str());
        if (m.similarity < minSimilarity) continue;
        m.pixivId = scalar(d->get("pixiv_id"));
        m.danbooruId = scalar(d->get("danbooru_id"));
        if (!allDigits(m.pixivId)) m.pixivId.clear();
        if (!allDigits(m.danbooruId)) m.danbooruId.clear();
        m.author = d->getString("member_name");
        if (m.author.empty()) {
            const Json* c = d->get("creator");
            if (c && c->type == Json::String) m.author = c->str;
            else if (c && c->type == Json::Array && !c->arr.empty() && c->arr[0].type == Json::String) m.author = c->arr[0].str;
        }
        m.title = d->getString("title");
        if (const Json* urls = d->get("ext_urls"))
            if (urls->type == Json::Array && !urls->arr.empty() && urls->arr[0].type == Json::String) m.url = urls->arr[0].str;
        const bool hasId = !m.pixivId.empty() || !m.danbooruId.empty();
        if (!out.found || (hasId && !bestHasId) || (hasId == bestHasId && m.similarity > out.best.similarity)) {
            out.best = m;
            out.found = true;
            bestHasId = hasId;
        }
    }
    return true;
}

std::string buildMultipart(const std::string& field, const std::string& fileName, const std::string& fileType,
                           const std::string& data, std::string& contentType) {
    // 本文に出てこない区切りを選ぶ
    std::string boundary = "----imgdupe7d1a4f";
    for (int i = 0; data.find(boundary) != std::string::npos; ++i) boundary += static_cast<char>('a' + i % 26);
    contentType = "multipart/form-data; boundary=" + boundary;
    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"" + field + "\"; filename=\"" + fileName + "\"\r\n";
    body += "Content-Type: " + fileType + "\r\n\r\n";
    body += data;
    body += "\r\n--" + boundary + "--\r\n";
    return body;
}

std::vector<LookupTask> pendingIdLookups(Db& db, size_t limit) {
    std::vector<LookupTask> out;
    const std::string sql = std::string(
                                "SELECT s.image_id, s.site, s.post_id FROM sources s "
                                "WHERE s.site IN ('pixiv','md5') AND s.checked IS NULL AND s.post_id IS NOT NULL AND s.post_id<>'' "
                                "AND NOT EXISTS (SELECT 1 FROM tags t WHERE t.image_id=s.image_id AND t.source IN ") +
                            kKnownSourcesSql + ") ORDER BY s.image_id DESC LIMIT ?";
    Stmt st(db.handle(), sql.c_str());
    st.bind(1, static_cast<int64_t>(limit));
    while (st.step()) out.push_back({st.i64(0), st.text(1), st.text(2)});
    return out;
}

std::vector<int64_t> pendingReverseLookups(Db& db, size_t limit) {
    std::vector<int64_t> out;
    if (!limit) return out;
    // 出典が無い（Danbooru に無かった MD5 だけなら、無いのと同じ）、タグの分かる json も無い、写真でもない
    const std::string sql = std::string(
                                "SELECT i.id FROM images i JOIN thumbs th ON th.image_id=i.id "
                                "LEFT JOIN file_meta f ON f.image_id=i.id "
                                "WHERE (f.camera IS NULL OR f.camera='') "
                                "AND NOT EXISTS (SELECT 1 FROM sources s WHERE s.image_id=i.id AND NOT (s.site='md5' AND s.checked IS NOT NULL)) "
                                "AND NOT EXISTS (SELECT 1 FROM tags t WHERE t.image_id=i.id AND t.source IN ") +
                            kKnownSourcesSql + ") ORDER BY i.id DESC LIMIT ?";
    Stmt st(db.handle(), sql.c_str());
    st.bind(1, static_cast<int64_t>(limit));
    while (st.step()) out.push_back(st.i64(0));
    return out;
}

void storeIdLookup(Db& db, const LookupTask& task, const LookupPost& post, int64_t now) {
    db.begin();
    try {
        if (post.found) insertTags(db, task.imageId, post.tags, kSourceIdLookup, kIdLookupConfidence);
        Stmt up(db.handle(),
                "UPDATE sources SET checked=?, author_id=coalesce(nullif(?,''), author_id), url=coalesce(nullif(url,''), nullif(?,'')) "
                "WHERE image_id=? AND site=? AND post_id=?");
        up.bind(1, now).bind(2, post.found && task.site == post.site ? post.authorId : std::string())
            .bind(3, post.found && task.site == post.site ? post.url : std::string())
            .bind(4, task.imageId).bind(5, task.site).bind(6, task.postId);
        up.step();
        // MD5 から分かった Danbooru の投稿
        if (post.found && post.site != task.site && !post.postId.empty())
            insertSource(db, task.imageId, post.site, post.postId, post.authorId, post.url, now);
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}

void storeReverseLookup(Db& db, int64_t imageId, const SauceResult& r, const std::vector<LookupPost>& posts, int64_t now) {
    db.begin();
    try {
        insertSource(db, imageId, "saucenao", "", "", r.found ? r.best.url : std::string(), now);
        if (r.found) {
            bool any = false;
            for (const auto& p : posts) {
                if (!p.found) continue;
                any = true;
                insertTags(db, imageId, p.tags, kSourceReverse, kReverseConfidence);
                insertSource(db, imageId, p.site, p.postId, p.authorId, p.url, now);
            }
            // 照会できなくても、作者の名前だけは分かる
            if (!any && !r.best.author.empty())
                insertTags(db, imageId, {TagRow{r.best.author, "artist", "", 0, false}}, kSourceReverse, kReverseConfidence);
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}

void resetLookups(Db& db, const std::vector<int64_t>& ids) {
    db.begin();
    try {
        Stmt delTags(db.handle(), "DELETE FROM tags WHERE image_id=? AND source IN ('id_lookup','reverse') AND manual=0");
        Stmt delSrc(db.handle(), "DELETE FROM sources WHERE image_id=? AND (site='saucenao' OR (site='danbooru' AND checked IS NOT NULL))");
        Stmt up(db.handle(), "UPDATE sources SET checked=NULL WHERE image_id=? AND site IN ('pixiv','md5')");
        for (int64_t id : ids) {
            for (Stmt* s : {&delTags, &delSrc, &up}) {
                s->bind(1, id);
                s->step();
                s->reset();
            }
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}

int sauceSentToday(Db& db, int64_t now) {
    const std::string v = db.setting("saucenao:day");
    const size_t colon = v.find(':');
    if (colon == std::string::npos || v.substr(0, colon) != dayKey(now)) return 0;
    return std::atoi(v.c_str() + colon + 1);
}

LookupReport runLookups(Db& db, const LookupOptions& opt, const LookupNet& net, const std::atomic<bool>& cancel,
                        Progress* progress) {
    LookupReport rep;
    auto now = [&] { return net.now ? net.now() : int64_t(0); };
    Pacer pixivPace{1000}, danbooruPace{1000}, saucePace{8000};
    bool pixivOff = false, danbooruOff = false;
    const std::vector<std::string> pixivHeaders = {"Referer: https://www.pixiv.net/", "Accept: application/json"};
    const std::vector<std::string> jsonHeaders = {"Accept: application/json"};

    auto stop = [&](bool& off, const std::string& name, const std::string& why) {
        off = true;
        if (rep.error.empty()) rep.error = name + ": " + why;
    };
    auto httpWhy = [](const HttpResult& r) {
        return !r.ok ? r.error : r.status == 429 ? std::string("回数の上限に達しました") : "HTTP " + std::to_string(r.status);
    };
    // 投稿を 1 つ照会する。通信の問題で調べられなかったら false（そのサイトはこの回はもう使わない）
    auto fetchPost = [&](const std::string& site, const std::string& id, LookupPost& post) -> bool {
        const bool pixiv = site == "pixiv";
        bool& off = pixiv ? pixivOff : danbooruOff;
        const char* name = pixiv ? "pixiv" : "Danbooru";
        if (off) return false;
        std::string url;
        if (pixiv) {
            if (!allDigits(id)) return true;
            url = opt.pixivUrl + "/ajax/illust/" + id;
        } else if (site == "md5") {
            if (!isMd5(id)) return true;
            url = opt.danbooruUrl + "/posts.json?tags=md5%3A" + id + "&limit=1";
        } else {
            if (!allDigits(id)) return true;
            url = opt.danbooruUrl + "/posts/" + id + ".json";
        }
        if (!(pixiv ? pixivPace : danbooruPace).wait(net, cancel)) return false;
        const HttpResult r = net.get(url, pixiv ? pixivHeaders : jsonHeaders);
        if (!r.ok || r.status == 429 || r.status >= 500) {
            stop(off, name, httpWhy(r));
            return false;
        }
        if (r.status != 200) return true;  // 消された・見られない投稿（調べたことにする）
        const bool ok = pixiv ? parsePixivIllust(r.body, post) : parseDanbooruPost(r.body, post);
        if (!ok) {
            stop(off, name, "応答を読めません");
            return false;
        }
        return true;
    };

    std::vector<LookupTask> tasks;
    if (opt.idLookup && net.get) tasks = pendingIdLookups(db, opt.maxIdPerRun);
    if (progress) {
        progress->done = 0;
        progress->total = tasks.size();
    }

    // ID 照会（同じ投稿の別のページは 1 回だけ）
    std::map<std::string, LookupPost> cache;
    for (const auto& t : tasks) {
        if (cancel) break;
        const std::string key = t.site + '\n' + t.postId;
        auto it = cache.find(key);
        if (it == cache.end()) {
            LookupPost post;
            if (!fetchPost(t.site, t.postId, post)) {
                if (progress) ++progress->done;
                continue;
            }
            it = cache.emplace(key, std::move(post)).first;
        }
        storeIdLookup(db, t, it->second, now());
        ++rep.looked;
        if (it->second.found) ++rep.found;
        if (progress) ++progress->done;
    }

    // 逆画像検索（ID 照会で見つからなかった画像も対象になるので、その後で選ぶ）
    int sauceLeft = 0;
    std::vector<int64_t> reverseIds;
    if (!cancel && opt.reverse && !opt.sauceKey.empty() && net.post && net.thumbnail) {
        sauceLeft = std::max(0, opt.sauceDailyLimit - sauceSentToday(db, now()));
        reverseIds = pendingReverseLookups(db, std::min<size_t>(opt.maxReversePerRun, static_cast<size_t>(sauceLeft)));
        if (progress) progress->total += reverseIds.size();
    }
    for (int64_t id : reverseIds) {
        if (cancel || sauceLeft <= 0) break;
        std::string jpeg;
        if (!net.thumbnail(id, jpeg) || jpeg.empty()) {
            if (progress) ++progress->done;
            continue;
        }
        if (!saucePace.wait(net, cancel)) break;
        std::string type;
        const std::string body = buildMultipart("file", "image.jpg", "image/jpeg", jpeg, type);
        const HttpResult r = net.post(
            opt.sauceUrl + "/search.php?output_type=2&numres=5&db=999&api_key=" + urlEncode(opt.sauceKey), {"Content-Type: " + type}, body);
        if (!r.ok) {
            if (rep.error.empty()) rep.error = "SauceNAO: " + r.error;
            break;
        }
        addSauceSent(db, now());
        --sauceLeft;
        SauceResult s;
        const bool parsed = parseSauceNao(r.body, s);
        if (r.status == 403 || (parsed && s.status < 0)) {
            rep.error = "SauceNAO: API キーが正しくありません" + (s.message.empty() ? std::string() : "（" + s.message + "）");
            break;
        }
        if (r.status == 429) {
            if (rep.error.empty()) rep.error = "SauceNAO: 回数の上限に達しました";
            break;
        }
        if (r.status != 200 || !parsed || s.status > 0) {
            if (rep.error.empty()) rep.error = "SauceNAO: " + (r.status != 200 ? "HTTP " + std::to_string(r.status) : std::string("応答を読めません"));
            break;
        }
        ++rep.reverseSent;
        std::vector<LookupPost> posts;
        if (s.found) {
            ++rep.reverseFound;
            if (!s.best.pixivId.empty()) {
                LookupPost p;
                if (fetchPost("pixiv", s.best.pixivId, p)) posts.push_back(std::move(p));
            }
            if (!s.best.danbooruId.empty()) {
                LookupPost p;
                if (fetchPost("danbooru", s.best.danbooruId, p)) posts.push_back(std::move(p));
            }
        }
        storeReverseLookup(db, id, s, posts, now());
        if (progress) ++progress->done;
        if (s.longRemaining == 0) break;
        if (s.shortRemaining == 0 && !pause(net, 30000, cancel)) break;
    }
    return rep;
}

}  // namespace imgdupe
