#include "stats.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>

#include "quality.hpp"
#include "tagstore.hpp"

#include <cstdlib>
#include <ctime>
#include <iterator>

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
    if (newTagFormat(db)) {
        // 画像ごとに、最初の 1 行が見つかれば止める（タグの行を全部は読まない）
        Stmt st(db.handle(),
                "SELECT count(*) FROM images i WHERE EXISTS (SELECT 1 FROM tag_rows r WHERE r.image_id=i.id AND "
                "r.source_id NOT IN (SELECT id FROM tag_sources WHERE name='embedded') AND (r.manual=0 OR r.confidence>=0.5))");
        if (st.step()) s.tagged = st.i64(0);
    } else {
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
        Stmt st(db.handle(), "SELECT coalesce(sum(sharpness < ?),0), coalesce(sum(detail < ?),0) FROM quality");
        st.bind(1, kBlurryBelow).bind(2, kSoftBelow);
        if (st.step()) {
            s.blurry = st.i64(0);
            s.soft = st.i64(1);
        }
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

std::vector<std::vector<StatRow>> tagCountsMany(Db& db, const std::vector<std::vector<std::string>>& views, double minConfidence,
                                                size_t limit) {
    std::vector<std::vector<StatRow>> out(views.size());
    if (!newTagFormat(db)) {
        for (size_t v = 0; v < views.size(); ++v) out[v] = tagCounts(db, views[v], minConfidence, limit);
        return out;
    }
    auto lower = [](std::string t) {
        for (char& c : t)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return t;
    };
    std::unordered_map<std::string, std::vector<int>> viewOf;  // 種類 → 表（いくつもの表に入ることもある）
    for (size_t v = 0; v < views.size(); ++v)
        for (const auto& k : views[v]) viewOf[k].push_back(static_cast<int>(v));
    // 別名 → 元の名前（大文字小文字を区別しない）
    std::unordered_map<std::string, std::string> aliasOf;
    {
        Stmt st(db.handle(), "SELECT tag, alias_of FROM tag_dict WHERE alias_of IS NOT NULL AND alias_of<>''");
        while (st.step()) aliasOf.emplace(lower(st.text(0)), st.text(1));
    }
    // タグの番号 → 数える名前（表ごとに 1 つ）。名前は表ごとに、別名は元の名前にまとめ、大文字小文字を区別しない
    std::unordered_map<int64_t, std::vector<int>> keyOf;
    std::vector<std::string> names;
    std::vector<int> nameView;
    {
        std::unordered_map<std::string, int> nameIndex;
        Stmt st(db.handle(), "SELECT id, kind, tag FROM tag_keys");
        while (st.step()) {
            auto v = viewOf.find(st.text(1));
            if (v == viewOf.end()) continue;
            std::string name = st.text(2);
            if (auto a = aliasOf.find(lower(name)); a != aliasOf.end()) name = a->second;
            for (int view : v->second) {
                const auto [it, added] = nameIndex.emplace(std::to_string(view) + '\n' + lower(name), static_cast<int>(names.size()));
                if (added) {
                    names.push_back(name);
                    nameView.push_back(view);
                }
                keyOf[st.i64(0)].push_back(it->second);
            }
        }
    }
    aliasOf.clear();
    std::vector<int64_t> counts(names.size()), bytes(names.size());
    // タグの行を画像の順に 1 回読む（画像の大きさも画像の順に並べて読む）
    struct Agg {
        int name;
        bool manual;
        double manualConf, conf;
    };
    std::vector<Agg> aggs;  // 1 枚の画像の分
    Stmt sizes(db.handle(), "SELECT id, coalesce(size,0) FROM images ORDER BY id");
    bool sizeRow = sizes.step();
    auto sizeOf = [&](int64_t id) -> int64_t {
        while (sizeRow && sizes.i64(0) < id) sizeRow = sizes.step();
        return sizeRow && sizes.i64(0) == id ? sizes.i64(1) : 0;
    };
    auto flush = [&](int64_t image) {
        if (aggs.empty()) return;
        const int64_t size = sizeOf(image);
        for (const auto& a : aggs)
            if (a.manual ? a.manualConf >= 0.5 : a.conf >= minConfidence) {
                ++counts[static_cast<size_t>(a.name)];
                bytes[static_cast<size_t>(a.name)] += size;
            }
        aggs.clear();
    };
    Stmt rows(db.handle(), "SELECT image_id, tag_id, manual, confidence FROM tag_rows ORDER BY image_id");
    int64_t current = -1;
    while (rows.step()) {
        const int64_t image = rows.i64(0);
        if (image != current) {
            flush(current);
            current = image;
        }
        auto k = keyOf.find(rows.i64(1));
        if (k == keyOf.end()) continue;
        const bool manual = rows.i64(2) != 0;
        const double conf = rows.f64(3);
        for (int name : k->second) {
            Agg* a = nullptr;
            for (auto& x : aggs)
                if (x.name == name) a = &x;
            if (!a) {
                aggs.push_back({name, false, -1.0, -1.0});
                a = &aggs.back();
            }
            a->conf = std::max(a->conf, conf);
            if (manual) {
                a->manual = true;
                a->manualConf = std::max(a->manualConf, conf);
            }
        }
    }
    flush(current);
    // 表ごとに、多い順（同じ数なら名前の順）
    for (size_t i = 0; i < names.size(); ++i)
        if (counts[i]) out[static_cast<size_t>(nameView[i])].push_back({names[i], counts[i], bytes[i]});
    for (auto& v : out) {
        std::sort(v.begin(), v.end(), [](const StatRow& a, const StatRow& b) { return a.count != b.count ? a.count > b.count : a.key < b.key; });
        if (v.size() > limit) v.resize(limit);
    }
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

const std::vector<std::vector<std::string>>& statTagKinds() {
    static const std::vector<std::vector<std::string>> kinds = {{"artist"}, {"circle"}, {"account"}, {"character"},
                                                                {"copyright"}, {"event"}, {"title"}, {"action", "item"}};
    return kinds;
}

StatsSnapshot computeStats(Db& db, const std::function<void(int, int, const std::string&)>& progress) {
    const auto& kinds = statTagKinds();
    const int total = 4;
    auto step = [&](int i, const std::string& what) {
        if (progress) progress(i, total, what);
    };
    StatsSnapshot s;
    s.at = static_cast<int64_t>(std::time(nullptr));
    step(0, "全体の数");
    s.summary = statsSummary(db);
    step(1, "タグ（作者・キャラ・作品など）");
    s.views = tagCountsMany(db, kinds, 0.5, 500);
    step(2, "月ごとの増え方");
    s.views.push_back(monthlyCounts(db));
    step(3, "形式別");
    s.views.push_back(formatCounts(db));
    return s;
}

namespace {
std::string oneLine(std::string t) {
    for (char& c : t)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return t;
}
}  // namespace

void saveStatsSnapshot(Db& db, const StatsSnapshot& s) {
    std::string t = "v1\n";
    t += "at\t" + std::to_string(s.at) + "\n";
    const StatsSummary& m = s.summary;
    t += "summary";
    for (int64_t v : {m.images, m.bytes, m.exactGroups, m.exactExtra, m.exactExtraBytes, m.similarGroups, m.similarExtra,
                      m.similarExtraBytes, m.tagged, m.withSource, m.deadLinks, m.blurry, m.soft})
        t += "\t" + std::to_string(v);
    t += "\n";
    for (const auto& st : m.stages) t += "stage\t" + oneLine(st.name) + "\t" + std::to_string(st.done) + "\t" + std::to_string(st.failed) + "\n";
    for (size_t v = 0; v < s.views.size(); ++v) {
        t += "view\t" + std::to_string(v) + "\n";
        for (const auto& r : s.views[v]) t += "r\t" + std::to_string(r.count) + "\t" + std::to_string(r.bytes) + "\t" + oneLine(r.key) + "\n";
    }
    db.setSetting("stats:snapshot", t);
}

bool loadStatsSnapshot(Db& db, StatsSnapshot& out) {
    const std::string text = db.setting("stats:snapshot");
    if (text.compare(0, 3, "v1\n") != 0) return false;
    StatsSnapshot s;
    std::vector<StatRow>* view = nullptr;
    size_t a = 3;
    while (a < text.size()) {
        size_t e = text.find('\n', a);
        if (e == std::string::npos) e = text.size();
        std::vector<std::string> f;
        for (size_t p = a; p <= e;) {
            size_t q = text.find('\t', p);
            if (q == std::string::npos || q > e) q = e;
            f.push_back(text.substr(p, q - p));
            p = q + 1;
        }
        auto num = [&](size_t i) { return i < f.size() ? std::strtoll(f[i].c_str(), nullptr, 10) : 0LL; };
        if (f[0] == "at") {
            s.at = num(1);
        } else if (f[0] == "summary" && f.size() >= 14) {
            StatsSummary& m = s.summary;
            int64_t* fields[] = {&m.images, &m.bytes, &m.exactGroups, &m.exactExtra, &m.exactExtraBytes, &m.similarGroups, &m.similarExtra,
                                 &m.similarExtraBytes, &m.tagged, &m.withSource, &m.deadLinks, &m.blurry, &m.soft};
            for (size_t i = 0; i < std::size(fields); ++i) *fields[i] = num(i + 1);
        } else if (f[0] == "stage" && f.size() >= 4) {
            s.summary.stages.push_back({f[1], num(2), num(3)});
        } else if (f[0] == "view") {
            s.views.emplace_back();
            view = &s.views.back();
        } else if (f[0] == "r" && view && f.size() >= 4) {
            view->push_back({f[3], num(1), num(2)});
        }
        a = e + 1;
    }
    if (s.views.size() != statTagKinds().size() + 2) return false;  // 表の数が変わった版のもの
    out = std::move(s);
    return true;
}

}  // namespace imgdupe
