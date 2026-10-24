#include "search.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <set>
#include <variant>

namespace imgdupe {

namespace {

using Param = std::variant<int64_t, double, std::string>;

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// そのタグと同じ意味のタグ（tag_dict の別名をたどる）
std::vector<std::string> aliasesOf(Db& db, const std::string& tag) {
    std::string canonical = tag;
    {
        Stmt st(db.handle(), "SELECT alias_of FROM tag_dict WHERE tag=? COLLATE NOCASE AND alias_of IS NOT NULL");
        st.bind(1, tag);
        if (st.step() && !st.text(0).empty()) canonical = st.text(0);
    }
    std::set<std::string> out = {tag, canonical};
    Stmt st(db.handle(), "SELECT tag FROM tag_dict WHERE alias_of=? COLLATE NOCASE");
    st.bind(1, canonical);
    while (st.step()) out.insert(st.text(0));
    return std::vector<std::string>(out.begin(), out.end());
}

std::string placeholders(size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) s += i ? ",?" : "?";
    return s;
}

}  // namespace

double combineConfidence(const std::vector<double>& ps) {
    double keep = 1;
    for (double p : ps) keep *= 1 - std::min(1.0, std::max(0.0, p));
    return 1 - keep;
}

void parseTagQuery(const std::string& text, std::vector<std::string>& tags, std::vector<std::string>& exclude) {
    tags.clear();
    exclude.clear();
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        if (cur[0] == '-' && cur.size() > 1) exclude.push_back(cur.substr(1));
        else if (cur != "-") tags.push_back(cur);
        cur.clear();
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        // 半角空白・タブ・全角空白（E3 80 80）で区切る
        if (c == ' ' || c == '\t' || c == ',') {
            flush();
        } else if (c == 0xE3 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                   static_cast<unsigned char>(text[i + 2]) == 0x80) {
            flush();
            i += 2;
        } else {
            cur += text[i];
        }
    }
    flush();
}

SearchResult search(Db& db, const SearchQuery& q) {
    std::vector<Param> params;
    std::string with, joins, where = " WHERE 1";

    // タグごとに「確度を合成して、しきい値以上の画像」を作る。手動で確定したものは、その値をそのまま使う
    auto tagCte = [&](const std::string& name, const std::string& tag) {
        const auto names = aliasesOf(db, tag);
        std::string sql = name +
                          " AS (SELECT image_id, CASE WHEN max(manual)=1 THEN max(CASE WHEN manual=1 THEN confidence END) "
                          "ELSE 1 - exp(sum(ln(1 - min(confidence, 0.999999)))) END AS conf FROM tags WHERE tag COLLATE NOCASE IN (" +
                          placeholders(names.size()) + ")";
        for (const auto& n : names) params.emplace_back(n);
        if (!q.sources.empty()) {
            sql += " AND (manual=1 OR source IN (" + placeholders(q.sources.size()) + "))";
            for (const auto& s : q.sources) params.emplace_back(s);
        }
        if (!q.excludeSources.empty()) {
            sql += " AND (manual=1 OR source NOT IN (" + placeholders(q.excludeSources.size()) + "))";
            for (const auto& s : q.excludeSources) params.emplace_back(s);
        }
        sql += " GROUP BY image_id HAVING conf >= ?)";
        params.emplace_back(q.minConfidence);
        with += (with.empty() ? "WITH " : ", ") + sql;
    };
    for (size_t i = 0; i < q.tags.size(); ++i) {
        const std::string n = "t" + std::to_string(i);
        tagCte(n, q.tags[i]);
        joins += " JOIN " + n + " ON " + n + ".image_id=i.id";
    }
    if (q.review && !q.tags.empty()) {
        const auto names = aliasesOf(db, q.tags[0]);
        where += " AND t0.conf < ? AND NOT EXISTS (SELECT 1 FROM tags m WHERE m.image_id=i.id AND m.manual=1 AND m.tag COLLATE NOCASE IN (" +
                 placeholders(names.size()) + "))";
        params.emplace_back(kReviewMax);
        for (const auto& n : names) params.emplace_back(n);
    }
    for (size_t i = 0; i < q.excludeTags.size(); ++i) {
        const std::string n = "x" + std::to_string(i);
        tagCte(n, q.excludeTags[i]);
        where += " AND i.id NOT IN (SELECT image_id FROM " + n + ")";
    }

    // 文字（全文検索）
    if (!q.text.empty()) {
        size_t chars = 0;
        for (unsigned char c : q.text)
            if ((c & 0xC0) != 0x80) ++chars;
        if (chars >= 3) {
            std::string phrase = "\"";
            for (char c : q.text) phrase += c == '"' ? std::string("\"\"") : std::string(1, c);
            phrase += "\"";
            where += " AND i.id IN (SELECT rowid FROM meta_fts WHERE meta_fts MATCH ?)";
            params.emplace_back(phrase);
        } else {
            // 2 文字以下は索引を使えないので、そのまま探す
            where += " AND (instr(f.title,?) OR instr(f.subject,?) OR instr(f.comment,?) OR instr(f.keywords,?) OR "
                     "instr(f.png_text,?))";
            for (int k = 0; k < 5; ++k) params.emplace_back(q.text);
        }
    }

    // メタデータ
    if (q.minRating >= 0) {
        where += " AND f.rating >= ?";
        params.emplace_back(static_cast<int64_t>(q.minRating));
    }
    if (q.noRating) where += " AND f.rating IS NULL";
    if (q.takenFrom) {
        where += " AND f.taken >= ?";
        params.emplace_back(q.takenFrom);
    }
    if (q.takenTo) {
        where += " AND f.taken <= ?";
        params.emplace_back(q.takenTo);
    }
    if (q.noTaken) where += " AND f.taken IS NULL";
    if (q.minWidth > 0) {
        where += " AND i.width >= ?";
        params.emplace_back(static_cast<int64_t>(q.minWidth));
    }
    if (q.minHeight > 0) {
        where += " AND i.height >= ?";
        params.emplace_back(static_cast<int64_t>(q.minHeight));
    }
    if (q.minSize > 0) {
        where += " AND i.size >= ?";
        params.emplace_back(q.minSize);
    }
    if (q.maxSize > 0) {
        where += " AND i.size <= ?";
        params.emplace_back(q.maxSize);
    }
    if (!q.format.empty()) {
        where += " AND f.format = ?";
        params.emplace_back(q.format);
    }
    if (q.animatedOnly) where += " AND f.animated = 1";
    if (!q.pathContains.empty()) {
        where += " AND instr(lower(i.path), ?) > 0";
        params.emplace_back(lowerAscii(q.pathContains));
    }
    if (!q.downloadContains.empty()) {
        where += " AND instr(lower(f.zone_url), ?) > 0";
        params.emplace_back(lowerAscii(q.downloadContains));
    }

    // 並べ替え（空欄は最後。同じなら パス順）
    std::string key;
    switch (q.sort) {
        case SortKey::Path: key = "i.path"; break;
        case SortKey::Created: key = "f.created"; break;
        case SortKey::Modified: key = "i.mtime"; break;
        case SortKey::Taken: key = "f.taken"; break;
        case SortKey::Resolution: key = "i.width * i.height"; break;
        case SortKey::Size: key = "i.size"; break;
        case SortKey::Rating: key = "f.rating"; break;
        case SortKey::Title: key = "NULLIF(f.title, '')"; break;
        case SortKey::Confidence: key = q.tags.empty() ? "i.path" : "t0.conf"; break;
    }
    const std::string dir = q.descending ? " DESC" : "";
    std::string order = " ORDER BY (" + key + ") IS NULL, " + key + dir;
    if (key != "i.path") order += ", i.path";
    if (q.review && !q.tags.empty()) order = " ORDER BY abs(t0.conf - 0.5), i.path";

    const std::string from = " FROM images i LEFT JOIN file_meta f ON f.image_id=i.id" + joins + where;
    auto bindAll = [&](Stmt& st) {
        for (size_t i = 0; i < params.size(); ++i) {
            const int idx = static_cast<int>(i + 1);
            if (const auto* v = std::get_if<int64_t>(&params[i])) st.bind(idx, *v);
            else if (const auto* d = std::get_if<double>(&params[i])) st.bind(idx, *d);
            else st.bind(idx, std::get<std::string>(params[i]));
        }
    };
    SearchResult r;
    // limit があれば、DB に上位だけを並べさせ（全部を並べ替えるより速い）、数は別に数える
    const std::string sql = with + " SELECT i.id" + from + order + (q.limit ? " LIMIT " + std::to_string(q.limit) : "");
    Stmt st(db.handle(), sql.c_str());
    bindAll(st);
    while (st.step()) r.ids.push_back(st.i64(0));
    if (q.limit && r.ids.size() >= q.limit) {
        Stmt count(db.handle(), (with + " SELECT COUNT(*)" + from).c_str());
        bindAll(count);
        r.total = count.step() ? static_cast<size_t>(count.i64(0)) : r.ids.size();
    } else {
        r.total = r.ids.size();
    }
    return r;
}

bool imageInfo(Db& db, int64_t id, ImageInfo& out) {
    Stmt st(db.handle(),
            "SELECT i.path,i.size,i.mtime,f.taken,i.width,i.height,f.rating,f.title,f.format FROM images i "
            "LEFT JOIN file_meta f ON f.image_id=i.id WHERE i.id=?");
    st.bind(1, id);
    if (!st.step()) return false;
    out.id = id;
    out.path = st.text(0);
    out.size = st.i64(1);
    out.mtime = st.i64(2);
    out.taken = st.isNull(3) ? 0 : st.i64(3);
    out.width = static_cast<int>(st.i64(4));
    out.height = static_cast<int>(st.i64(5));
    out.rating = st.isNull(6) ? -1 : static_cast<int>(st.i64(6));
    out.title = st.text(7);
    out.format = st.text(8);
    return true;
}

void setManualTag(Db& db, int64_t imageId, const std::string& tag, bool correct) {
    clearManualTag(db, imageId, tag);
    std::string kind = "general";
    {
        Stmt st(db.handle(), "SELECT kind FROM tags WHERE image_id=? AND tag=? COLLATE NOCASE ORDER BY confidence DESC LIMIT 1");
        st.bind(1, imageId).bind(2, tag);
        if (st.step()) kind = st.text(0);
    }
    Stmt ins(db.handle(),
             "INSERT OR REPLACE INTO tags(image_id,tag,kind,confidence,source,manual) VALUES(?,?,?,?,'manual',1)");
    ins.bind(1, imageId).bind(2, tag).bind(3, kind).bind(4, correct ? 1.0 : 0.0);
    ins.step();
}

void clearManualTag(Db& db, int64_t imageId, const std::string& tag) {
    const auto names = aliasesOf(db, tag);
    const std::string sql =
        "DELETE FROM tags WHERE image_id=? AND manual=1 AND tag COLLATE NOCASE IN (" + placeholders(names.size()) + ")";
    Stmt del(db.handle(), sql.c_str());
    del.bind(1, imageId);
    for (size_t i = 0; i < names.size(); ++i) del.bind(static_cast<int>(i + 2), names[i]);
    del.step();
}

namespace {

const char* const kSortNames[] = {"path", "created", "modified", "taken", "resolution", "size", "rating", "title", "confidence"};

void putList(std::string& out, const char* key, const std::vector<std::string>& v) {
    if (v.empty()) return;
    out += ",\"";
    out += key;
    out += "\":[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += ",";
        jsonString(out, v[i]);
    }
    out += "]";
}
void putText(std::string& out, const char* key, const std::string& v) {
    if (v.empty()) return;
    out += ",\"";
    out += key;
    out += "\":";
    jsonString(out, v);
}
void putInt(std::string& out, const char* key, int64_t v, int64_t unset = 0) {
    if (v != unset) out += ",\"" + std::string(key) + "\":" + std::to_string(v);
}
void putBool(std::string& out, const char* key, bool v) {
    if (v) out += ",\"" + std::string(key) + "\":true";
}

}  // namespace

std::string queryToJson(const SearchQuery& q) {
    std::string out = "{\"version\":1";
    putList(out, "tags", q.tags);
    putList(out, "excludeTags", q.excludeTags);
    char conf[32];
    snprintf(conf, sizeof conf, "%.4g", q.minConfidence);
    out += ",\"minConfidence\":" + std::string(conf);
    putList(out, "sources", q.sources);
    putList(out, "excludeSources", q.excludeSources);
    putText(out, "text", q.text);
    putInt(out, "minRating", q.minRating, -1);
    putBool(out, "noRating", q.noRating);
    putInt(out, "takenFrom", q.takenFrom);
    putInt(out, "takenTo", q.takenTo);
    putBool(out, "noTaken", q.noTaken);
    putInt(out, "minWidth", q.minWidth);
    putInt(out, "minHeight", q.minHeight);
    putInt(out, "minSize", q.minSize);
    putInt(out, "maxSize", q.maxSize);
    putText(out, "format", q.format);
    putBool(out, "animatedOnly", q.animatedOnly);
    putText(out, "path", q.pathContains);
    putText(out, "download", q.downloadContains);
    putBool(out, "review", q.review);
    out += ",\"sort\":\"" + std::string(kSortNames[static_cast<int>(q.sort)]) + "\"";
    putBool(out, "descending", q.descending);
    out += "}";
    return out;
}

bool queryFromJson(const std::string& json, SearchQuery& out) {
    Json j;
    if (!parseJson(json, j) || j.type != Json::Object) return false;
    SearchQuery q;
    auto list = [&](const char* key, std::vector<std::string>& v) {
        if (const Json* a = j.get(key); a && a->type == Json::Array)
            for (const Json& e : a->arr)
                if (e.type == Json::String) v.push_back(e.str);
    };
    auto num = [&](const char* key, auto& v) {
        if (const Json* n = j.get(key); n && n->type == Json::Number)
            v = static_cast<std::remove_reference_t<decltype(v)>>(std::strtod(n->str.c_str(), nullptr));
    };
    auto flag = [&](const char* key, bool& v) {
        if (const Json* b = j.get(key); b && b->type == Json::Bool) v = b->str == "true";
    };
    list("tags", q.tags);
    list("excludeTags", q.excludeTags);
    num("minConfidence", q.minConfidence);
    list("sources", q.sources);
    list("excludeSources", q.excludeSources);
    q.text = j.getString("text");
    num("minRating", q.minRating);
    flag("noRating", q.noRating);
    num("takenFrom", q.takenFrom);
    num("takenTo", q.takenTo);
    flag("noTaken", q.noTaken);
    num("minWidth", q.minWidth);
    num("minHeight", q.minHeight);
    num("minSize", q.minSize);
    num("maxSize", q.maxSize);
    q.format = j.getString("format");
    flag("animatedOnly", q.animatedOnly);
    q.pathContains = j.getString("path");
    q.downloadContains = j.getString("download");
    const std::string sort = j.getString("sort");
    for (size_t i = 0; i < std::size(kSortNames); ++i)
        if (sort == kSortNames[i]) q.sort = static_cast<SortKey>(i);
    flag("descending", q.descending);
    flag("review", q.review);
    out = std::move(q);
    return true;
}

std::vector<SmartFolder> smartFolders(Db& db) {
    std::vector<SmartFolder> out;
    Stmt st(db.handle(), "SELECT id, name, query FROM smart_folders ORDER BY name COLLATE NOCASE");
    while (st.step()) {
        SmartFolder f;
        f.id = st.i64(0);
        f.name = st.text(1);
        queryFromJson(st.text(2), f.query);
        out.push_back(std::move(f));
    }
    return out;
}

int64_t saveSmartFolder(Db& db, const std::string& name, const SearchQuery& q) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    Stmt st(db.handle(),
            "INSERT INTO smart_folders(name, query, created, updated) VALUES(?,?,?,?) "
            "ON CONFLICT(name) DO UPDATE SET query=excluded.query, updated=excluded.updated");
    st.bind(1, name).bind(2, queryToJson(q)).bind(3, now).bind(4, now);
    st.step();
    Stmt id(db.handle(), "SELECT id FROM smart_folders WHERE name=?");
    id.bind(1, name);
    return id.step() ? id.i64(0) : 0;
}

bool renameSmartFolder(Db& db, int64_t id, const std::string& name) {
    {
        Stmt st(db.handle(), "SELECT 1 FROM smart_folders WHERE name=? AND id<>?");
        st.bind(1, name).bind(2, id);
        if (st.step()) return false;
    }
    Stmt st(db.handle(), "UPDATE smart_folders SET name=?, updated=? WHERE id=?");
    st.bind(1, name).bind(2, static_cast<int64_t>(std::time(nullptr))).bind(3, id);
    st.step();
    return true;
}

void deleteSmartFolder(Db& db, int64_t id) {
    Stmt st(db.handle(), "DELETE FROM smart_folders WHERE id=?");
    st.bind(1, id);
    st.step();
}

std::vector<std::pair<std::string, int64_t>> tagSources(Db& db) {
    std::vector<std::pair<std::string, int64_t>> out;
    Stmt st(db.handle(), "SELECT source, COUNT(*) FROM tags GROUP BY source ORDER BY 2 DESC");
    while (st.step()) out.emplace_back(st.text(0), st.i64(1));
    return out;
}

}  // namespace imgdupe
