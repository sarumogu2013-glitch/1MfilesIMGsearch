#include "search.hpp"

#include <algorithm>
#include <cctype>
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
        sql += " GROUP BY image_id HAVING conf >= ?)";
        params.emplace_back(q.minConfidence);
        with += (with.empty() ? "WITH " : ", ") + sql;
    };
    for (size_t i = 0; i < q.tags.size(); ++i) {
        const std::string n = "t" + std::to_string(i);
        tagCte(n, q.tags[i]);
        joins += " JOIN " + n + " ON " + n + ".image_id=i.id";
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

    const std::string sql = with + " SELECT i.id FROM images i LEFT JOIN file_meta f ON f.image_id=i.id" + joins + where + order;
    Stmt st(db.handle(), sql.c_str());
    for (size_t i = 0; i < params.size(); ++i) {
        const int idx = static_cast<int>(i + 1);
        if (const auto* v = std::get_if<int64_t>(&params[i])) st.bind(idx, *v);
        else if (const auto* d = std::get_if<double>(&params[i])) st.bind(idx, *d);
        else st.bind(idx, std::get<std::string>(params[i]));
    }
    SearchResult r;
    while (st.step()) {
        ++r.total;
        if (!q.limit || r.ids.size() < q.limit) r.ids.push_back(st.i64(0));
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

std::vector<std::pair<std::string, int64_t>> tagSources(Db& db) {
    std::vector<std::pair<std::string, int64_t>> out;
    Stmt st(db.handle(), "SELECT source, COUNT(*) FROM tags GROUP BY source ORDER BY 2 DESC");
    while (st.step()) out.emplace_back(st.text(0), st.i64(1));
    return out;
}

}  // namespace imgdupe
