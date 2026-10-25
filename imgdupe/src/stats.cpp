#include "stats.hpp"

#include <algorithm>
#include <map>

namespace imgdupe {

namespace {

std::string upperAscii(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

std::string extensionOf(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const size_t sep = path.find_last_of("\\/");
    if (dot == std::string::npos || (sep != std::string::npos && dot < sep) || dot + 1 == path.size()) return "（不明）";
    return upperAscii(path.substr(dot + 1));
}

}  // namespace

StatsSummary statsSummary(Db& db) {
    StatsSummary s;
    {
        Stmt st(db.handle(), "SELECT count(*), coalesce(sum(size),0) FROM images");
        if (st.step()) {
            s.images = st.i64(0);
            s.bytes = st.i64(1);
        }
    }
    {
        Stmt st(db.handle(),
                "SELECT g.kind, count(DISTINCT g.id), sum(m.representative=0), coalesce(sum(CASE WHEN m.representative=0 THEN i.size END),0) "
                "FROM groups g JOIN group_members m ON m.group_id=g.id JOIN images i ON i.id=m.image_id "
                "WHERE g.kind IN ('exact','similar') GROUP BY g.kind");
        while (st.step()) {
            const bool exact = st.text(0) == "exact";
            (exact ? s.exactGroups : s.similarGroups) = st.i64(1);
            (exact ? s.exactExtra : s.similarExtra) = st.i64(2);
            (exact ? s.exactExtraBytes : s.similarExtraBytes) = st.i64(3);
        }
    }
    {
        Stmt st(db.handle(), "SELECT count(DISTINCT image_id) FROM tags WHERE source<>'embedded' AND (manual=0 OR confidence>=0.5)");
        if (st.step()) s.tagged = st.i64(0);
    }
    {
        Stmt st(db.handle(), "SELECT count(DISTINCT image_id) FROM sources WHERE url IS NOT NULL AND url<>''");
        if (st.step()) s.withSource = st.i64(0);
    }
    {
        Stmt st(db.handle(), "SELECT count(DISTINCT image_id) FROM sources WHERE link_status=0");
        if (st.step()) s.deadLinks = st.i64(0);
    }
    {
        Stmt st(db.handle(), "SELECT stage, sum(state=1), sum(state=2) FROM stages GROUP BY stage");
        while (st.step()) s.stages.push_back({st.text(0), st.i64(1), st.i64(2)});
    }
    return s;
}

std::vector<StatRow> tagCounts(Db& db, const std::vector<std::string>& kinds, double minConfidence, size_t limit) {
    std::vector<StatRow> out;
    if (kinds.empty()) return out;
    std::string in;
    for (size_t i = 0; i < kinds.size(); ++i) in += i ? ",?" : "?";
    // 画像とタグ（別名は元の名前）ごとに、手動の確定があればそれ、無ければ一番高い確度
    const std::string sql =
            "WITH t AS (SELECT image_id, coalesce((SELECT d.alias_of FROM tag_dict d WHERE d.tag=tags.tag COLLATE NOCASE "
            "AND d.alias_of IS NOT NULL AND d.alias_of<>''), tag) AS tag, manual, confidence FROM tags WHERE kind IN (" + in + ")), "
            "x AS (SELECT image_id, tag, max(manual) AS man, max(CASE WHEN manual=1 THEN confidence END) AS mc, max(confidence) AS c "
            "FROM t GROUP BY image_id, tag COLLATE NOCASE) "
            "SELECT x.tag, count(*), coalesce(sum(i.size),0) FROM x JOIN images i ON i.id=x.image_id "
            "WHERE (x.man=1 AND x.mc>=0.5) OR (x.man=0 AND x.c>=?) "
            "GROUP BY x.tag COLLATE NOCASE ORDER BY 2 DESC, 1 LIMIT ?";
    Stmt st(db.handle(), sql.c_str());
    int n = 1;
    for (const auto& k : kinds) st.bind(n++, k);
    st.bind(n, minConfidence);
    st.bind(n + 1, static_cast<int64_t>(limit));
    while (st.step()) out.push_back({st.text(0), st.i64(1), st.i64(2)});
    return out;
}

std::vector<StatRow> monthlyCounts(Db& db, bool localTime) {
    std::vector<StatRow> out;
    // images.mtime は FILETIME（1601 年からの 100ns）、file_meta.created は UNIX 時刻
    const std::string sql = std::string(
                                "SELECT strftime('%Y-%m', t, 'unixepoch'") +
                            (localTime ? ", 'localtime'" : "") +
                            ") AS m, count(*), coalesce(sum(size),0) FROM ("
                            "SELECT CASE WHEN f.created>0 AND f.created < i.mtime/10000000-11644473600 THEN f.created "
                            "ELSE i.mtime/10000000-11644473600 END AS t, i.size AS size "
                            "FROM images i LEFT JOIN file_meta f ON f.image_id=i.id WHERE i.mtime>0) "
                            "GROUP BY m ORDER BY m";
    Stmt st(db.handle(), sql.c_str());
    while (st.step())
        if (!st.text(0).empty()) out.push_back({st.text(0), st.i64(1), st.i64(2)});
    return out;
}

std::vector<StatRow> formatCounts(Db& db) {
    std::map<std::string, StatRow> by;
    auto add = [&](const std::string& key, int64_t n, int64_t bytes) {
        StatRow& r = by[key];
        r.key = key;
        r.count += n;
        r.bytes += bytes;
    };
    {
        Stmt st(db.handle(),
                "SELECT upper(f.format), count(*), coalesce(sum(i.size),0) FROM images i JOIN file_meta f ON f.image_id=i.id "
                "WHERE f.format IS NOT NULL AND f.format<>'' GROUP BY upper(f.format)");
        while (st.step()) add(st.text(0), st.i64(1), st.i64(2));
    }
    {
        Stmt st(db.handle(),
                "SELECT i.path, i.size FROM images i LEFT JOIN file_meta f ON f.image_id=i.id WHERE f.format IS NULL OR f.format=''");
        while (st.step()) add(extensionOf(st.text(0)), 1, st.i64(1));
    }
    std::vector<StatRow> out;
    for (auto& [k, r] : by) out.push_back(r);
    std::sort(out.begin(), out.end(), [](const StatRow& a, const StatRow& b) { return a.count != b.count ? a.count > b.count : a.key < b.key; });
    return out;
}

}  // namespace imgdupe
