#include "names.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <set>

#include "json.hpp"
#include "platform.hpp"

namespace imgdupe {

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 前後の空白（folder なら _ も）を除く
std::string trim(const std::string& s, bool underscore = false) {
    size_t a = 0, b = s.size();
    auto blank = [&](char c) { return c == ' ' || c == '\t' || (underscore && c == '_'); };
    auto space = [&](size_t i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (blank(s[i])) return 1;
        // 全角空白（E3 80 80）
        if (c == 0xE3 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
            static_cast<unsigned char>(s[i + 2]) == 0x80)
            return 3;
        return 0;
    };
    while (a < b) {
        const int n = space(a);
        if (!n) break;
        a += n;
    }
    while (b > a) {
        if (blank(s[b - 1])) {
            --b;
        } else if (b >= a + 3 && static_cast<unsigned char>(s[b - 3]) == 0xE3 && static_cast<unsigned char>(s[b - 2]) == 0x80 &&
                   static_cast<unsigned char>(s[b - 1]) == 0x80) {
            b -= 3;
        } else {
            break;
        }
    }
    return s.substr(a, b - a);
}

// 文字列か数値（gallery-dl の ID は数値のことが多い）
std::string scalar(const Json* v) {
    if (!v) return {};
    if (v->type == Json::String || v->type == Json::Number) return v->str;
    return {};
}

void addTag(SidecarInfo& out, std::set<std::string>& seen, const std::string& raw, const std::string& kind) {
    const std::string t = trim(raw);
    if (t.empty() || t.size() > 200) return;
    if (!seen.insert(kind + '\n' + lowerAscii(t)).second) return;
    out.tags.push_back({t, kind, kSourceMeta, kMetaTagConfidence, false});
}

// 空白区切りの文字列か、文字列（または {"name": ...}）の配列
void addTagList(SidecarInfo& out, std::set<std::string>& seen, const Json* v, const std::string& kind) {
    if (!v) return;
    if (v->type == Json::String) {
        size_t a = 0;
        const std::string& s = v->str;
        while (a < s.size()) {
            size_t b = s.find(' ', a);
            if (b == std::string::npos) b = s.size();
            if (b > a) addTag(out, seen, s.substr(a, b - a), kind);
            a = b + 1;
        }
    } else if (v->type == Json::Array) {
        for (const Json& e : v->arr) {
            if (e.type == Json::String) addTag(out, seen, e.str, kind);
            else if (e.type == Json::Object) addTag(out, seen, e.getString("name"), kind);
        }
    } else if (v->type == Json::Object) {
        // {"artist": [...], "character": [...], ...}
        for (const auto& [k, sub] : v->obj) {
            const std::string kk = lowerAscii(k);
            if (kk == "artist" || kk == "character" || kk == "copyright" || kk == "general") addTagList(out, seen, &sub, kk);
        }
    }
}

const char* const kFolderStopWords[] = {
    "downloads", "download", "pictures", "picture", "pics", "images", "image", "img", "imgs", "photos", "photo",
    "desktop", "documents", "screenshots", "screenshot", "camera roll", "saved pictures", "dcim", "new folder",
    "temp", "tmp", "misc", "other", "others", "backup", "old", "new", "wallpaper", "wallpapers", "twitter", "pixiv",
    "x", "danbooru", "gallery-dl", "users", "onedrive", "dropbox", "_vfolders", "新しいフォルダー", "新しいフォルダ",
    "ダウンロード", "ピクチャ", "画像", "写真", "デスクトップ", "その他", "未分類", "壁紙", "保存", "バックアップ"};

bool isStopName(const std::string& name) {
    const std::string l = lowerAscii(name);
    for (const char* w : kFolderStopWords)
        if (l == w) return true;
    // 数字・日付だけ（2024、2024-05、20240501、100MEDIA の数字部分など）
    bool onlyDigits = true;
    for (unsigned char c : name)
        if (!(std::isdigit(c) || c == '-' || c == '_' || c == '.' || c == ' ')) onlyDigits = false;
    if (onlyDigits) return true;
    // DCIM の "100MEDIA"、"100_PANA"、"100CANON" など
    if (name.size() >= 4 && std::isdigit(static_cast<unsigned char>(name[0])) && std::isdigit(static_cast<unsigned char>(name[1])) &&
        std::isdigit(static_cast<unsigned char>(name[2])))
        return true;
    return false;
}

std::string parentName(const std::string& path) {
    const size_t e = path.find_last_of("\\/");
    if (e == std::string::npos || e == 0) return {};
    const size_t s = path.find_last_of("\\/", e - 1);
    const std::string name = path.substr(s == std::string::npos ? 0 : s + 1, e - (s == std::string::npos ? 0 : s + 1));
    if (name.size() == 2 && name[1] == ':') return {};  // ドライブの直下
    return name;
}

struct NamesPayload {
    bool haveSidecar = false;
    SidecarInfo sidecar;
    std::vector<TagRow> folder;
};

}  // namespace

bool parseSidecarJson(const std::string& text, SidecarInfo& out) {
    Json j;
    if (!parseJson(text, j) || j.type != Json::Object) return false;
    out = SidecarInfo{};
    std::set<std::string> seen;
    out.site = lowerAscii(j.getString("category"));
    out.title = trim(j.getString("title"));
    out.postId = scalar(j.get("tweet_id"));
    if (out.postId.empty()) out.postId = scalar(j.get("id"));

    // 作者: user / author（{"id", "name", "nick", "account"}）か、artist の文字列
    std::string authorName, authorAccount;
    for (const char* key : {"author", "user", "uploader", "owner"}) {
        const Json* a = j.get(key);
        if (!a) continue;
        if (a->type == Json::Object) {
            authorName = a->getString("nick");
            if (authorName.empty()) authorName = a->getString("name");
            authorAccount = a->getString("account");
            if (authorAccount.empty()) authorAccount = a->getString("name");
            out.authorId = scalar(a->get("id"));
        } else if (a->type == Json::String) {
            authorName = a->str;
        }
        if (!authorName.empty()) break;
    }
    if (authorName.empty()) authorName = j.getString("artist");
    if (!authorName.empty()) addTag(out, seen, authorName, "artist");

    // タグ
    addTagList(out, seen, j.get("tags"), "general");
    addTagList(out, seen, j.get("hashtags"), "general");
    for (const char* kind : {"artist", "character", "copyright", "general"}) {
        addTagList(out, seen, j.get(std::string("tags_") + kind), kind);
        addTagList(out, seen, j.get(std::string("tag_string_") + kind), kind);
    }

    // 投稿の URL
    if (out.site == "pixiv" && !out.postId.empty()) out.url = "https://www.pixiv.net/artworks/" + out.postId;
    else if (out.site == "twitter" && !out.postId.empty())
        out.url = "https://x.com/" + (authorAccount.empty() ? std::string("i") : authorAccount) + "/status/" + out.postId;
    else if (out.site == "danbooru" && !out.postId.empty()) out.url = "https://danbooru.donmai.us/posts/" + out.postId;
    else {
        const std::string u = j.getString("post_url");
        out.url = !u.empty() ? u : j.getString("url");
        if (out.url.compare(0, 4, "http") != 0) out.url.clear();
    }
    return true;
}

std::string findSidecar(const std::string& imagePath) {
    if (pathExists(imagePath + ".json")) return imagePath + ".json";
    const size_t dot = imagePath.find_last_of('.');
    const size_t sep = imagePath.find_last_of("\\/");
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) {
        const std::string p = imagePath.substr(0, dot) + ".json";
        if (pathExists(p)) return p;
    }
    return {};
}

std::vector<TagRow> folderTags(const std::string& imagePath) {
    std::vector<TagRow> out;
    std::string name = trim(parentName(imagePath), true);
    if (name.empty() || name[0] == '.' || name.size() > 120 || isStopName(name)) return out;
    auto add = [&](const std::string& t, const char* kind) {
        const std::string s = trim(t, true);
        if (s.empty() || isStopName(s)) return;
        for (const auto& o : out)
            if (lowerAscii(o.tag) == lowerAscii(s)) return;
        out.push_back({s, kind, kSourceFolder, kFolderTagConfidence, false});
    };
    // "[作者] 作品名"、"【作者】作品名"（同人誌などでよくある形）
    auto bracket = [&](const std::string& open, const std::string& close) {
        if (name.compare(0, open.size(), open) != 0) return false;
        const size_t e = name.find(close, open.size());
        if (e == std::string::npos) return false;
        add(name.substr(open.size(), e - open.size()), "artist");
        add(name.substr(e + close.size()), "general");
        return true;
    };
    if (bracket("[", "]") || bracket("【", "】")) return out;
    add(name, "general");
    return out;
}

AnalyzerOutput NamesAnalyzer::analyze(const AnalyzerInput& in) {
    AnalyzerOutput out;
    auto p = std::make_shared<NamesPayload>();
    const std::string json = findSidecar(in.path);
    if (!json.empty()) {
        const int64_t size = fileSize(json);
        if (size > 0 && size < 8 * 1024 * 1024) {
            if (FILE* f = openFile(json, "rb")) {
                std::string text(static_cast<size_t>(size), '\0');
                const size_t n = std::fread(&text[0], 1, text.size(), f);
                std::fclose(f);
                text.resize(n);
                p->haveSidecar = parseSidecarJson(text, p->sidecar);
            }
        }
    }
    p->folder = folderTags(in.path);
    out.payload = p;
    return out;
}

bool NamesAnalyzer::store(Db& db, int64_t imageId, AnalyzerOutput& out) {
    auto p = std::static_pointer_cast<NamesPayload>(out.payload);
    if (!p) return false;
    replaceTags(db, imageId, kSourceMeta, p->haveSidecar ? p->sidecar.tags : std::vector<TagRow>{});
    replaceTags(db, imageId, kSourceFolder, p->folder);
    if (!p->haveSidecar) return true;
    const SidecarInfo& s = p->sidecar;
    if (!s.site.empty()) {
        Stmt ins(db.handle(),
                 "INSERT INTO sources(image_id,site,post_id,author_id,url) VALUES(?,?,?,?,?) "
                 "ON CONFLICT(image_id,site,post_id) DO UPDATE SET author_id=excluded.author_id, url=excluded.url");
        ins.bind(1, imageId).bind(2, s.site).bind(3, s.postId).bind(4, s.authorId).bind(5, s.url);
        ins.step();
    }
    if (!s.title.empty()) {
        // 画像に埋め込まれたタイトルが無い時だけ使う
        Stmt up(db.handle(), "UPDATE file_meta SET title=? WHERE image_id=? AND (title IS NULL OR title='')");
        up.bind(1, s.title).bind(2, imageId);
        up.step();
        if (db.changes()) {
            // 全文検索の索引も作り直す
            Stmt del(db.handle(), "DELETE FROM meta_fts WHERE rowid=?");
            del.bind(1, imageId);
            del.step();
            Stmt ins(db.handle(),
                     "INSERT INTO meta_fts(rowid,title,subject,comment,keywords,png_text) SELECT image_id,title,subject,comment,"
                     "keywords,png_text FROM file_meta WHERE image_id=?");
            ins.bind(1, imageId);
            ins.step();
        }
    }
    return true;
}

}  // namespace imgdupe
