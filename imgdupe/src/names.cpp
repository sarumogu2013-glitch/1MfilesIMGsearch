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
    std::string bookDir;  // 本の形のフォルダの直下の画像なら、そのフォルダ（ページの役目は store で同じフォルダの画像から決める）
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

namespace {

// 全角のかっこを半角に（【】［］→ []、（）→ ()）
std::string asciiBrackets(std::string s) {
    const std::pair<const char*, const char*> table[] = {{"【", "["}, {"】", "]"}, {"［", "["}, {"］", "]"}, {"（", "("}, {"）", ")"}};
    for (const auto& [from, to] : table) {
        const std::string f = from;
        for (size_t p = 0; (p = s.find(f, p)) != std::string::npos;) {
            s.replace(p, f.size(), to);
            p += 1;
        }
    }
    return s;
}

// s[open] のかっこに合う閉じかっこの位置（入れ子も数える）。無ければ npos
size_t matchClose(const std::string& s, size_t open) {
    const char o = s[open], c = o == '(' ? ')' : ']';
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i) {
        if (s[i] == o) ++depth;
        else if (s[i] == c && --depth == 0) return i;
    }
    return std::string::npos;
}

// s[close] の閉じかっこに合う開きかっこの位置
size_t matchOpen(const std::string& s, size_t close) {
    const char c = s[close], o = c == ')' ? '(' : '[';
    int depth = 0;
    for (size_t i = close + 1; i-- > 0;) {
        if (s[i] == c) ++depth;
        else if (s[i] == o && --depth == 0) return i;
    }
    return std::string::npos;
}

// 作者の並び（、 , ， & ＆ / ／ で区切る）
std::vector<std::string> splitAuthors(const std::string& s) {
    std::vector<std::string> out;
    std::string cur = s;
    for (const char* sep : {"、", "，", "＆", "／"})
        for (size_t p = 0; (p = cur.find(sep, p)) != std::string::npos;) cur.replace(p, std::string(sep).size(), ",");
    for (char& c : cur)
        if (c == '&' || c == '/') c = ',';
    size_t a = 0;
    while (a <= cur.size()) {
        size_t b = cur.find(',', a);
        if (b == std::string::npos) b = cur.size();
        const std::string t = trim(cur.substr(a, b - a), false);
        if (!t.empty()) out.push_back(t);
        a = b + 1;
    }
    return out;
}

}  // namespace

bool looksLikeEvent(const std::string& text) {
    const std::string t = lowerAscii(trim(text, false));
    if (t.empty()) return false;
    // C102・c97（コミックマーケット）
    if (t[0] == 'c' && t.size() >= 2 && std::isdigit(static_cast<unsigned char>(t[1]))) return true;
    for (const char* w : {"コミケ", "コミックマーケット", "comic market", "comiket", "例大祭", "コミティア", "comitia", "comic1", "サンクリ",
                          "サンシャインクリエイション", "紅楼夢", "こみトレ", "こみっくトレジャー", "comic treasure", "歌姫庭園", "スパコミ",
                          "super comic city", "haru comic city", "comic city", "コミックシティ", "超同人祭", "ボーマス", "ふたけっと",
                          "みみけっと", "コミトレ", "夏コミ", "冬コミ", "エアコミケ", "即売会", "オンリー", "only", "イベント", "m3-",
                          "ゲームレジェンド", "僕らのラブライブ", "cotton", "gcc", "good comic city", "ガタケット", "コミッククリエイション",
                          "コミコミ", "サンシャイン", "ゲムマ", "ゲームマーケット", "砲雷撃戦", "我、夜戦に突入す", "神戸かわさき"})
        if (t.find(w) != std::string::npos) return true;
    // 年の数字（2023、2023冬 など）
    for (size_t i = 0; i + 4 <= t.size(); ++i) {
        bool four = true;
        for (size_t k = 0; k < 4; ++k) four = four && std::isdigit(static_cast<unsigned char>(t[i + k]));
        if (four && (t[i] == '1' || t[i] == '2') && (i + 4 == t.size() || !std::isdigit(static_cast<unsigned char>(t[i + 4])))) return true;
    }
    return false;
}

bool parseDoujinName(const std::string& raw, DoujinName& out) {
    out = DoujinName{};
    std::string s = trim(asciiBrackets(raw), false);
    // 先頭の (イベント)（何個でも）
    while (!s.empty() && s[0] == '(') {
        const size_t e = matchClose(s, 0);
        if (e == std::string::npos) return false;
        const std::string inner = trim(s.substr(1, e - 1), false);
        if (!inner.empty()) (looksLikeEvent(inner) ? out.events : out.parodies).push_back(inner);
        s = trim(s.substr(e + 1), false);
    }
    // [サークル(作者)] か [作者]
    if (s.empty() || s[0] != '[') return false;
    const size_t close = matchClose(s, 0);
    if (close == std::string::npos) return false;
    std::string head = trim(s.substr(1, close - 1), false);
    std::string rest = trim(s.substr(close + 1), false);
    if (head.empty()) return false;
    if (head.back() == ')') {
        const size_t o = matchOpen(head, head.size() - 1);
        if (o != std::string::npos && o > 0) {
            out.circle = trim(head.substr(0, o), false);
            out.authors = splitAuthors(head.substr(o + 1, head.size() - o - 2));
        } else {
            out.authors = splitAuthors(head);
        }
    } else {
        out.authors = splitAuthors(head);
    }
    // 後ろの (イベント・原作) と [DL版] など（何個でも）
    std::vector<std::string> tail;
    while (!rest.empty() && (rest.back() == ')' || rest.back() == ']')) {
        const size_t o = matchOpen(rest, rest.size() - 1);
        if (o == std::string::npos) break;
        if (rest.back() == ')') tail.push_back(trim(rest.substr(o + 1, rest.size() - o - 2), false));
        rest = trim(rest.substr(0, o), false);
    }
    for (auto it = tail.rbegin(); it != tail.rend(); ++it)
        if (!it->empty()) (looksLikeEvent(*it) ? out.events : out.parodies).push_back(*it);
    out.title = rest;
    return !out.authors.empty() || !out.circle.empty();
}

int naturalCompare(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]), cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            // 続いた数字: 頭の 0 を除いて、桁の数、それから並びで比べる
            size_t ea = i, eb = j;
            while (ea < a.size() && std::isdigit(static_cast<unsigned char>(a[ea]))) ++ea;
            while (eb < b.size() && std::isdigit(static_cast<unsigned char>(b[eb]))) ++eb;
            size_t sa = i, sb = j;
            while (sa + 1 < ea && a[sa] == '0') ++sa;
            while (sb + 1 < eb && b[sb] == '0') ++sb;
            if (ea - sa != eb - sb) return ea - sa < eb - sb ? -1 : 1;
            const int c = a.compare(sa, ea - sa, b, sb, eb - sb);
            if (c) return c < 0 ? -1 : 1;
            i = ea;
            j = eb;
            continue;
        }
        const int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return a < b ? -1 : a > b ? 1 : 0;
}

std::string pageRole(const std::string& name, const std::vector<std::string>& siblings) {
    if (std::find(siblings.begin(), siblings.end(), name) == siblings.end()) return {};
    const std::string* first = nullptr;
    const std::string* last = nullptr;
    for (const auto& n : siblings) {
        if (!first || naturalCompare(n, *first) < 0) first = &n;
        if (!last || naturalCompare(n, *last) > 0) last = &n;
    }
    if (name == *first) return "表紙";
    if (name == *last) return "背表紙";
    return "本文";
}

std::vector<TagRow> folderTags(const std::string& imagePath) {
    std::vector<TagRow> out;
    auto add = [&](const std::string& t, const char* kind, double confidence) {
        const std::string s = trim(t, true);
        if (s.empty() || isStopName(s)) return;
        for (const auto& o : out)
            if (lowerAscii(o.tag) == lowerAscii(s) && o.kind == kind) return;
        out.push_back({s, kind, kSourceFolder, confidence, false});
    };
    // 画像のあるフォルダから上へ（4 段まで）たどり、最初に見つかった "[サークル(作者)]タイトル(イベント)" の形のフォルダを使う
    std::string dir = imagePath;
    for (int level = 0; level < 4; ++level) {
        const std::string name = trim(parentName(dir), true);
        if (name.empty()) break;
        DoujinName d;
        if (name[0] != '.' && name.size() <= 240 && parseDoujinName(name, d)) {
            add(d.circle, "circle", kDoujinTagConfidence);
            for (const auto& a : d.authors) add(a, "artist", kDoujinTagConfidence);
            add(d.title, "title", kDoujinTagConfidence);
            for (const auto& e : d.events) add(e, "event", kDoujinTagConfidence);
            for (const auto& p : d.parodies) add(p, "copyright", kDoujinTagConfidence);
            if (level == 0) return out;
            break;
        }
        const size_t sep = dir.find_last_of("\\/");
        if (sep == std::string::npos) break;
        dir = dir.substr(0, sep);
    }
    // 画像のあるフォルダの名前そのもの（よくある名前・日付・数字だけのものは使わない）
    const std::string name = trim(parentName(imagePath), true);
    if (!name.empty() && name[0] != '.' && name.size() <= 120 && !isStopName(name)) add(name, "general", kFolderTagConfidence);
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
    {
        DoujinName d;
        const size_t sep = in.path.find_last_of("\\/");
        if (sep != std::string::npos && parseDoujinName(trim(parentName(in.path), true), d)) p->bookDir = in.path.substr(0, sep + 1);
    }
    out.payload = p;
    return out;
}

bool NamesAnalyzer::store(Db& db, int64_t imageId, AnalyzerOutput& out) {
    auto p = std::static_pointer_cast<NamesPayload>(out.payload);
    if (!p) return false;
    replaceTags(db, imageId, kSourceMeta, p->haveSidecar ? p->sidecar.tags : std::vector<TagRow>{});
    if (!p->bookDir.empty()) {
        // 本のページの役目: 同じフォルダの直下の画像（DB にあるもの。走査で先に登録してある）の名前順で決める
        std::vector<std::string> names;
        std::string self;
        Stmt st(db.handle(), "SELECT id, path FROM images WHERE path > ? AND path < ?");
        st.bind(1, p->bookDir).bind(2, p->bookDir + "\xff");
        while (st.step()) {
            const std::string rest = st.text(1).substr(p->bookDir.size());
            if (rest.find_first_of("\\/") != std::string::npos) continue;  // 中のフォルダ
            if (st.i64(0) == imageId) self = rest;
            names.push_back(rest);
        }
        const std::string role = pageRole(self, names);
        if (!role.empty()) p->folder.push_back({role, "page", kSourceFolder, kDoujinTagConfidence, false});
    }
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
