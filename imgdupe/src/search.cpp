#include "search.hpp"
#include "jis_kanji.hpp"
#include "tagnames.hpp"
#include "tagstore.hpp"

#include "json.hpp"
#include "quality.hpp"

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
void dictAliases(Db& db, const std::string& tag, std::set<std::string>& out) {
    std::string canonical = tag;
    {
        Stmt st(db.handle(), "SELECT alias_of FROM tag_dict WHERE tag=? COLLATE NOCASE AND alias_of IS NOT NULL");
        st.bind(1, tag);
        if (st.step() && !st.text(0).empty()) canonical = st.text(0);
    }
    out.insert(tag);
    out.insert(canonical);
    Stmt st(db.handle(), "SELECT tag FROM tag_dict WHERE alias_of=? COLLATE NOCASE");
    st.bind(1, canonical);
    while (st.step()) out.insert(st.text(0));
}

// 別名に加えて、日本語の表示名がそのまま合うタグ（とその別名）も
std::vector<std::string> aliasesOf(Db& db, const std::string& tag) {
    std::set<std::string> out;
    dictAliases(db, tag, out);
    for (const auto& t : tagsNamed(db, tag)) dictAliases(db, t, out);
    return std::vector<std::string>(out.begin(), out.end());
}

// DB にあるタグの名前（同じ名前は 1 つ）
std::vector<std::string> allTagNames(Db& db) { return tagNames(db); }

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

void parseFreeText(const std::string& text, std::vector<std::string>& words, std::vector<std::string>& exactTags,
                   std::vector<std::string>& exclude) {
    words.clear();
    exactTags.clear();
    exclude.clear();
    std::string cur;
    bool quoted = false, minus = false, wasQuoted = false;
    auto flush = [&] {
        if (!cur.empty()) {
            if (minus) exclude.push_back(cur);
            else if (wasQuoted) exactTags.push_back(cur);
            else words.push_back(cur);
        }
        cur.clear();
        minus = wasQuoted = false;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        // " か全角の “ ” で囲む
        size_t qlen = c == '"' ? 1 : 0;
        if (!qlen && c == 0xE2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(text[i + 2]) == 0x9C || static_cast<unsigned char>(text[i + 2]) == 0x9D))
            qlen = 3;
        if (qlen) {
            if (quoted) {
                quoted = false;
                wasQuoted = true;
            } else {
                quoted = true;
            }
            i += qlen - 1;
            continue;
        }
        const bool fullSpace = c == 0xE3 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                               static_cast<unsigned char>(text[i + 2]) == 0x80;
        if (!quoted && (c == ' ' || c == '\t' || c == ',' || fullSpace)) {
            flush();
            if (fullSpace) i += 2;
            continue;
        }
        if (!quoted && c == '-' && cur.empty() && !minus) {
            minus = true;
            continue;
        }
        cur += text[i];
    }
    flush();
}

namespace {
// UTF-8 の 1 文字を読む（壊れていれば 1 バイトを 1 文字に）
uint32_t nextChar(const std::string& s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + n > s.size()) n = 1;
    uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += n;
    return cp;
}
bool inTable(const unsigned short* t, size_t n, uint32_t cp) {
    return cp <= 0xFFFF && std::binary_search(t, t + n, static_cast<unsigned short>(cp));
}
}  // namespace

double partialMatchWeight(const std::string& word) {
    double sum = 0;
    for (size_t i = 0; i < word.size();) {
        const uint32_t c = nextChar(word, i);
        double w = 1;
        const bool alnum = (c < 0x80 && std::isalnum(static_cast<int>(c))) || (c >= 0xFF10 && c <= 0xFF19) ||
                           (c >= 0xFF21 && c <= 0xFF3A) || (c >= 0xFF41 && c <= 0xFF5A);
        const bool kana = (c >= 0x3041 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0xFF66 && c <= 0xFF9F);
        const bool kanji = (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF) ||
                           (c >= 0x20000 && c <= 0x3FFFF);
        const bool symbol = (c < 0x80 && !alnum) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF01 && c <= 0xFF0F) ||
                            (c >= 0xFF1A && c <= 0xFF20) || (c >= 0x2000 && c <= 0x2BFF);
        if (alnum || kana) w = 1;
        else if (kanji) w = inTable(jis::kLevel1, std::size(jis::kLevel1), c) ? 2 : inTable(jis::kLevel2, std::size(jis::kLevel2), c) ? 3 : 4;
        else if (symbol) w = 0.5;
        sum += w;
    }
    if (sum < 2) return 0;
    if (sum < 3) return 0.4;
    if (sum < 4) return 0.55;
    if (sum < 5) return 0.7;
    if (sum < 6) return 0.8;
    return 0.9;
}

SearchIn effectiveSearchIn(const SearchQuery& q) {
    SearchIn in = q.in;
    switch (q.freeScope) {
        case FreeScope::All: break;
        case FreeScope::Tags: in = {true, true, false, false, false, false}; break;
        case FreeScope::Title: in = {false, false, true, false, false, false}; break;
        case FreeScope::Name: in = {false, false, false, false, true, true}; break;
    }
    return in;
}

SearchResult search(Db& db, const SearchQuery& q) {
    std::vector<Param> params, withParams;  // WITH の中の値は、ほかの値より先に結び付ける
    std::string with, joins, where = " WHERE 1";
    const SearchIn in = effectiveSearchIn(q);
    // フリーワード: 語・" で囲んだ語（タグの名前どおり。タグの条件と同じに扱う）・除く語
    std::vector<std::string> words, exactTags, excludeWords;
    parseFreeText(q.freeText, words, exactTags, excludeWords);
    std::vector<std::string> allTags = q.tags;
    if (in.tags) allTags.insert(allTags.end(), exactTags.begin(), exactTags.end());
    else if (!exactTags.empty()) where += " AND 0";  // タグを探さないのに、タグの名前どおりの語がある

    // タグごとに「確度を合成して、しきい値以上の画像」を作る。手動で確定したものは、その値をそのまま使う
    auto tagCte = [&](const std::string& name, const std::string& tag) {
        const auto names = aliasesOf(db, tag);
        std::string sql = name +
                          " AS (SELECT image_id, CASE WHEN max(manual)=1 THEN max(CASE WHEN manual=1 THEN confidence END) "
                          "ELSE 1 - exp(sum(ln(1 - min(confidence, 0.999999)))) END AS conf FROM tags WHERE tag COLLATE NOCASE IN (" +
                          placeholders(names.size()) + ")";
        for (const auto& n : names) withParams.emplace_back(n);
        if (!q.sources.empty()) {
            sql += " AND (manual=1 OR source IN (" + placeholders(q.sources.size()) + "))";
            for (const auto& s : q.sources) withParams.emplace_back(s);
        }
        if (!q.excludeSources.empty()) {
            sql += " AND (manual=1 OR source NOT IN (" + placeholders(q.excludeSources.size()) + "))";
            for (const auto& s : q.excludeSources) withParams.emplace_back(s);
        }
        sql += " GROUP BY image_id HAVING conf >= ?)";
        withParams.emplace_back(q.minConfidence);
        with += (with.empty() ? "WITH " : ", ") + sql;
    };
    std::string tagRelevance;  // " で囲んだ語の確度の和（並びの関連度に足す）
    for (size_t i = 0; i < allTags.size(); ++i) {
        const std::string n = "t" + std::to_string(i);
        tagCte(n, allTags[i]);
        joins += " JOIN " + n + " ON " + n + ".image_id=i.id";
        if (i >= q.tags.size()) tagRelevance += (tagRelevance.empty() ? "" : " + ") + n + ".conf";
    }
    if (q.review && !allTags.empty()) {
        const auto names = aliasesOf(db, allTags[0]);
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

    // フリーワード
    std::string relevance = tagRelevance;
    {
        auto ftsPhrase = [](const std::string& w) {
            std::string p = "\"";
            for (char c : w) p += c == '"' ? std::string("\"\"") : std::string(1, c);
            return p + "\"";
        };
        auto charCount = [](const std::string& w) {
            size_t n = 0;
            for (unsigned char c : w)
                if ((c & 0xC0) != 0x80) ++n;
            return n;
        };
        // 語を含むタグの名前（そのまま合うもの・別名は除く。多すぎる時は 300 個まで）
        std::vector<std::string> tagNames;
        if (!words.empty() || !excludeWords.empty())
            if (in.tags && in.partialTags) tagNames = allTagNames(db);
        auto partialTags = [&](const std::string& w) {
            std::vector<std::string> out;
            const auto exact = aliasesOf(db, w);
            const std::string lw = lowerAscii(w);
            auto add = [&](const std::string& t) {
                const std::string lt = lowerAscii(t);
                for (const auto& e : exact)
                    if (lowerAscii(e) == lt) return;
                for (const auto& o : out)
                    if (lowerAscii(o) == lt) return;
                out.push_back(t);
            };
            for (const auto& t : tagNames) {
                if (out.size() >= 300) break;
                if (lowerAscii(t).find(lw) != std::string::npos) add(t);
            }
            // 日本語の表示名に語を含むタグ（髪 → long_hair など。英数字だけの語は英語のタグ名で足りる）
            bool ascii = true;
            for (unsigned char c : w) ascii = ascii && c < 0x80;
            if (!ascii)
                for (const auto& t : tagsWithNameContaining(db, w, 300))
                    if (out.size() < 300) add(t);
            return out;
        };
        // 語 w に合う画像と点数の CTE を name で作る
        auto wordCte = [&](const std::string& name, const std::string& w) {
            std::vector<std::string> parts;
            if (in.tags) {
                tagCte(name + "e", w);  // タグがそのまま合う（別名・確度の合成・手動の確定）。点数は確度
                parts.push_back("SELECT image_id, conf AS s FROM " + name + "e");
                // 語を含むタグ（miku → hatsune_miku）。点数は語の確からしさ w（確度は足切りだけ）。手動で「違う」と確定したものは除く
                const double weight = in.partialTags ? partialMatchWeight(w) : 0.0;
                const auto partial = weight > 0 ? partialTags(w) : std::vector<std::string>{};
                if (!partial.empty()) {
                    char ws[32];
                    std::snprintf(ws, sizeof ws, "%.4f", weight);
                    std::string sql = std::string("SELECT image_id, ") + ws + " AS s FROM tags WHERE tag COLLATE NOCASE IN (" +
                                      placeholders(partial.size()) + ") AND manual=0 AND confidence >= ? AND NOT EXISTS (SELECT 1 FROM tags m "
                                      "WHERE m.image_id=tags.image_id AND m.tag=tags.tag COLLATE NOCASE AND m.manual=1 AND m.confidence < 0.5)";
                    for (const auto& t : partial) withParams.emplace_back(t);
                    withParams.emplace_back(q.minConfidence);
                    if (!q.sources.empty()) {
                        sql += " AND source IN (" + placeholders(q.sources.size()) + ")";
                        for (const auto& s : q.sources) withParams.emplace_back(s);
                    }
                    if (!q.excludeSources.empty()) {
                        sql += " AND source NOT IN (" + placeholders(q.excludeSources.size()) + ")";
                        for (const auto& s : q.excludeSources) withParams.emplace_back(s);
                    }
                    parts.push_back(sql + " GROUP BY image_id");
                }
            }
            if (charCount(w) >= 3) {
                if (in.title) {
                    parts.push_back("SELECT rowid AS image_id, 1.0 AS s FROM meta_fts WHERE meta_fts MATCH ?");
                    withParams.emplace_back("title : " + ftsPhrase(w));
                }
                if (in.text) {
                    parts.push_back("SELECT rowid AS image_id, 1.0 AS s FROM meta_fts WHERE meta_fts MATCH ?");
                    withParams.emplace_back("{subject comment keywords png_text} : " + ftsPhrase(w));
                }
            } else {
                // 2 文字以下は索引を使えないので、そのまま探す
                if (in.title) {
                    parts.push_back("SELECT image_id, 1.0 AS s FROM file_meta WHERE instr(title, ?)");
                    withParams.emplace_back(w);
                }
                if (in.text) {
                    parts.push_back("SELECT image_id, 1.0 AS s FROM file_meta WHERE instr(subject,?) OR instr(comment,?) OR "
                                    "instr(keywords,?) OR instr(png_text,?)");
                    for (int k = 0; k < 4; ++k) withParams.emplace_back(w);
                }
            }
            if (in.fileName) {
                // ファイル名（フォルダの名前は含めない）
                parts.push_back("SELECT id AS image_id, 1.0 AS s FROM images WHERE instr(lower(substr(path, length(rtrim(path, "
                                "replace(replace(path, '\\', ''), '/', ''))) + 1)), ?)");
                withParams.emplace_back(lowerAscii(w));
            }
            if (parts.empty()) parts.push_back("SELECT NULL AS image_id, 0 AS s WHERE 0");  // 探す所が 1 つも無い
            std::string u;
            for (const auto& p : parts) u += (u.empty() ? "" : " UNION ALL ") + p;
            with += (with.empty() ? "WITH " : ", ") + name + " AS (SELECT image_id, max(s) AS s FROM (" + u + ") GROUP BY image_id)";
        };
        for (size_t i = 0; i < words.size(); ++i) {
            const std::string n = "w" + std::to_string(i);
            wordCte(n, words[i]);
            joins += " JOIN " + n + " ON " + n + ".image_id=i.id";
            relevance += (relevance.empty() ? "" : " + ") + n + ".s";
        }
        for (size_t i = 0; i < excludeWords.size(); ++i) {
            const std::string n = "v" + std::to_string(i);
            wordCte(n, excludeWords[i]);
            where += " AND i.id NOT IN (SELECT image_id FROM " + n + ")";
        }
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
    {
        // 更新日時は images.mtime（FILETIME）、ほかは file_meta の Unix 秒
        const char* col = q.dateField == DateField::Modified ? "i.mtime" : q.dateField == DateField::Created ? "f.created" : "f.taken";
        auto bound = [&](int64_t t) { return q.dateField == DateField::Modified ? (t + 11644473600LL) * 10000000LL : t; };
        if (q.takenFrom) {
            where += std::string(" AND ") + col + " >= ?";
            params.emplace_back(bound(q.takenFrom));
        }
        if (q.takenTo) {
            where += std::string(" AND ") + col + " <= ?";
            params.emplace_back(q.dateField == DateField::Modified ? bound(q.takenTo + 1) - 1 : q.takenTo);
        }
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
    if (q.unclassified) {
        // 出どころの絞り込みは確度の合成と同じ
        std::string cond;
        if (!q.sources.empty()) {
            cond += " AND (u.manual=1 OR u.source IN (" + placeholders(q.sources.size()) + "))";
            for (const auto& x : q.sources) params.emplace_back(x);
        }
        if (!q.excludeSources.empty()) {
            cond += " AND (u.manual=1 OR u.source NOT IN (" + placeholders(q.excludeSources.size()) + "))";
            for (const auto& x : q.excludeSources) params.emplace_back(x);
        }
        where += " AND NOT EXISTS (SELECT 1 FROM tags u WHERE u.image_id=i.id" + cond +
                 " GROUP BY lower(u.tag) HAVING CASE WHEN max(u.manual)=1 THEN max(CASE WHEN u.manual=1 THEN u.confidence END) "
                 "ELSE 1 - exp(sum(ln(1 - min(u.confidence, 0.999999)))) END >= ?)";
        params.emplace_back(q.minConfidence);
    }
    if (!q.ids.empty()) {
        // 数が多くても 1 つの値で渡せるよう、JSON の配列にする
        std::string arr = "[";
        for (size_t k = 0; k < q.ids.size(); ++k) arr += (k ? "," : "") + std::to_string(q.ids[k]);
        where += " AND i.id IN (SELECT value FROM json_each(?))";
        params.emplace_back(arr + "]");
    }
    if (!q.underFolder.empty()) {
        // このフォルダとその下（区切りは \ と / のどちらでも）
        std::string a = q.underFolder, b = q.underFolder;
        while (!a.empty() && (a.back() == '\\' || a.back() == '/')) a.pop_back();
        b = a;
        where += " AND ((i.path >= ? AND i.path < ?) OR (i.path >= ? AND i.path < ?))";
        params.emplace_back(a + "\\");
        params.emplace_back(a + "\\\xff");
        params.emplace_back(b + "/");
        params.emplace_back(b + "/\xff");
    }
    if (!q.pathContains.empty()) {
        where += " AND instr(lower(i.path), ?) > 0";
        params.emplace_back(lowerAscii(q.pathContains));
    }
    if (!q.downloadContains.empty()) {
        where += " AND instr(lower(f.zone_url), ?) > 0";
        params.emplace_back(lowerAscii(q.downloadContains));
    }
    switch (q.quality) {
        case QualityFilter::Any: break;
        case QualityFilter::Good:
            where += " AND qa.sharpness >= " + std::to_string(kBlurryBelow) + " AND qa.detail >= " + std::to_string(kSoftBelow);
            break;
        case QualityFilter::Blurry: where += " AND qa.sharpness < " + std::to_string(kBlurryBelow); break;
        case QualityFilter::Soft: where += " AND qa.detail < " + std::to_string(kSoftBelow); break;
        case QualityFilter::LowRes: where += " AND qa.effective > 0 AND qa.effective < " + std::to_string(kLowResBelow); break;
    }
    if (q.deadLinks) where += " AND EXISTS (SELECT 1 FROM sources s WHERE s.image_id=i.id AND s.link_status=0)";

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
        case SortKey::Confidence: key = allTags.empty() ? "i.path" : "t0.conf"; break;
        case SortKey::Aesthetic: key = "qa.aesthetic"; break;
        case SortKey::Effective: key = "qa.effective"; break;
        case SortKey::FileName:
            key = "lower(substr(i.path, length(rtrim(i.path, replace(replace(i.path, '\\', ''), '/', ''))) + 1))";
            break;
        case SortKey::Relevance: key = relevance.empty() ? "i.path" : "(" + relevance + ")"; break;
    }
    // 関連度は高い順が「昇順」（降順にすると低い順）
    const bool desc = q.sort == SortKey::Relevance && !relevance.empty() ? !q.descending : q.descending;
    const std::string dir = desc ? " DESC" : "";
    std::string order = " ORDER BY (" + key + ") IS NULL, " + key + dir;
    if (key != "i.path") order += ", i.path";
    if (q.review && !allTags.empty()) order = " ORDER BY abs(t0.conf - 0.5), i.path";

    const std::string from =
        " FROM images i LEFT JOIN file_meta f ON f.image_id=i.id LEFT JOIN quality qa ON qa.image_id=i.id" + joins + where;
    params.insert(params.begin(), withParams.begin(), withParams.end());
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
            "SELECT i.path,i.size,i.mtime,f.taken,i.width,i.height,f.rating,f.title,f.format,"
            "qa.sharpness,qa.detail,qa.effective,qa.aesthetic FROM images i "
            "LEFT JOIN file_meta f ON f.image_id=i.id LEFT JOIN quality qa ON qa.image_id=i.id WHERE i.id=?");
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
    out.sharpness = st.isNull(9) ? -1 : st.f64(9);
    out.detail = st.isNull(10) ? -1 : st.f64(10);
    out.effective = st.isNull(11) ? 0 : static_cast<int>(st.i64(11));
    out.aesthetic = st.isNull(12) ? -1 : st.f64(12);
    return true;
}

std::string sourcePageUrl(const std::string& site, const std::string& postId, const std::string& url) {
    if (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0) return url;
    if (postId.empty() || postId.find_first_not_of("0123456789") != std::string::npos) return {};
    if (site == "pixiv") return "https://www.pixiv.net/artworks/" + postId;
    if (site == "danbooru") return "https://danbooru.donmai.us/posts/" + postId;
    if (site == "x" || site == "twitter") return "https://x.com/i/status/" + postId;
    return {};
}

std::vector<SourceLink> sourceLinks(Db& db, int64_t imageId) {
    std::vector<SourceLink> out;
    Stmt st(db.handle(), "SELECT site, post_id, url, link_status FROM sources WHERE image_id=? ORDER BY site, post_id");
    st.bind(1, imageId);
    while (st.step()) {
        SourceLink l;
        l.site = st.text(0);
        l.postId = st.text(1);
        l.url = sourcePageUrl(l.site, l.postId, st.text(2));
        l.status = st.isNull(3) ? -1 : static_cast<int>(st.i64(3));
        if (l.url.empty()) continue;
        bool dup = false;
        for (const auto& o : out) dup = dup || o.url == l.url;
        if (!dup) out.push_back(std::move(l));
    }
    return out;
}

void setManualTag(Db& db, int64_t imageId, const std::string& tag, bool correct) {
    clearManualTag(db, imageId, tag);
    std::string kind = "general";
    {
        Stmt st(db.handle(), "SELECT kind FROM tags WHERE image_id=? AND tag=? COLLATE NOCASE ORDER BY confidence DESC LIMIT 1");
        st.bind(1, imageId).bind(2, tag);
        if (st.step()) kind = st.text(0);
    }
    TagWriter(db, TagConflict::Replace).put(imageId, tag, kind, correct ? 1.0 : 0.0, "manual", true);
}

void clearManualTag(Db& db, int64_t imageId, const std::string& tag) {
    deleteManualTags(db, imageId, aliasesOf(db, tag));
}

void TagHistory::touch(int64_t imageId, const std::string& tag) {
    if (!seen_.insert({imageId, lowerAscii(tag)}).second) return;
    const auto names = aliasesOf(db_, tag);
    const std::string sql = "SELECT tag, kind, confidence FROM tags WHERE image_id=? AND manual=1 AND tag COLLATE NOCASE IN (" +
                            placeholders(names.size()) + ")";
    Stmt st(db_.handle(), sql.c_str());
    st.bind(1, imageId);
    for (size_t i = 0; i < names.size(); ++i) st.bind(static_cast<int>(i + 2), names[i]);
    std::string item = "{\"image\":" + std::to_string(imageId) + ",\"tag\":";
    jsonString(item, tag);
    item += ",\"rows\":[";
    bool first = true;
    while (st.step()) {
        item += first ? "[" : ",[";
        first = false;
        jsonString(item, st.text(0));
        item += ",";
        jsonString(item, st.text(1));
        char conf[32];
        snprintf(conf, sizeof conf, ",%.17g]", st.f64(2));
        item += conf;
    }
    item += "]}";
    before_ += (before_.empty() ? "" : ",") + item;
}

int64_t TagHistory::save(const std::string& description) {
    if (before_.empty()) return 0;
    Stmt ins(db_.handle(), "INSERT INTO history(op, target, before, at) VALUES('tags', ?, ?, ?)");
    ins.bind(1, description).bind(2, "[" + before_ + "]").bind(3, static_cast<int64_t>(std::time(nullptr)));
    ins.step();
    before_.clear();
    seen_.clear();
    Stmt id(db_.handle(), "SELECT last_insert_rowid()");
    return id.step() ? id.i64(0) : 0;
}

bool undoLastTagEdit(Db& db, std::string& description) {
    int64_t id = 0;
    std::string before;
    {
        Stmt st(db.handle(), "SELECT id, target, before FROM history WHERE op='tags' ORDER BY id DESC LIMIT 1");
        if (!st.step()) return false;
        id = st.i64(0);
        description = st.text(1);
        before = st.text(2);
    }
    Json j;
    if (parseJson(before, j) && j.type == Json::Array) {
        TagWriter ins(db, TagConflict::Replace);
        for (const Json& e : j.arr) {
            const Json* image = e.get("image");
            if (!image || image->type != Json::Number) continue;
            const int64_t imageId = std::strtoll(image->str.c_str(), nullptr, 10);
            clearManualTag(db, imageId, e.getString("tag"));
            if (const Json* rows = e.get("rows"); rows && rows->type == Json::Array) {
                for (const Json& r : rows->arr) {
                    if (r.type != Json::Array || r.arr.size() != 3) continue;
                    ins.put(imageId, r.arr[0].str, r.arr[1].str, std::strtod(r.arr[2].str.c_str(), nullptr), "manual", true);
                }
            }
        }
    }
    Stmt del(db.handle(), "DELETE FROM history WHERE id=?");
    del.bind(1, id);
    del.step();
    return true;
}

std::vector<std::string> indexedFolders(Db& db) {
    std::set<std::string> dirs;
    Stmt st(db.handle(), "SELECT path FROM images");
    std::string last;
    while (st.step()) {
        const std::string p = st.text(0);
        const size_t e = p.find_last_of("\\/");
        if (e == std::string::npos) continue;
        std::string d = p.substr(0, e);
        if (d == last) continue;  // 同じフォルダの画像が続く
        last = d;
        // 上のフォルダもたどる（もうあれば、その上もある）
        while (!d.empty() && !(d.size() == 2 && d[1] == ':') && dirs.insert(d).second) {
            const size_t k = d.find_last_of("\\/");
            if (k == std::string::npos || k == 0) break;
            d = d.substr(0, k);
            if (d.size() == 1 && (d[0] == '\\' || d[0] == '/')) break;
        }
    }
    return {dirs.begin(), dirs.end()};
}

std::vector<std::string> matchFolders(const std::vector<std::string>& folders, const std::string& freeText, size_t limit) {
    std::vector<std::string> words, exclude;
    parseTagQuery(freeText, words, exclude);
    for (auto& w : words) w = lowerAscii(w);
    for (auto& w : exclude) w = lowerAscii(w);
    std::vector<std::string> out;
    if (words.empty()) return out;
    for (const auto& f : folders) {
        const size_t k = f.find_last_of("\\/");
        const std::string name = lowerAscii(k == std::string::npos ? f : f.substr(k + 1));
        bool ok = true;
        for (const auto& w : words) ok = ok && name.find(w) != std::string::npos;
        for (const auto& w : exclude) ok = ok && name.find(w) == std::string::npos;
        if (ok) out.push_back(f);
    }
    // 名前が語に近い（短い）ものを先に
    std::stable_sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const size_t ka = a.find_last_of("\\/"), kb = b.find_last_of("\\/");
        return a.size() - (ka == std::string::npos ? 0 : ka) < b.size() - (kb == std::string::npos ? 0 : kb);
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

bool hasConditions(const SearchQuery& q) {
    return !q.freeText.empty() || !q.tags.empty() || !q.excludeTags.empty() || !q.sources.empty() || !q.excludeSources.empty() || !q.text.empty() ||
           q.minRating >= 0 || q.noRating || q.takenFrom || q.takenTo || q.noTaken || q.minWidth > 0 || q.minHeight > 0 ||
           q.minSize > 0 || q.maxSize > 0 || !q.format.empty() || q.animatedOnly || q.unclassified || !q.pathContains.empty() ||
           !q.downloadContains.empty() || q.deadLinks || q.quality != QualityFilter::Any || !q.ids.empty();
}

namespace {

const char* const kSortNames[] = {"path", "created", "modified", "taken", "resolution", "size", "rating", "title", "confidence",
                                  "aesthetic", "effective", "filename", "relevance"};
const char* const kScopeNames[] = {"all", "tags", "title", "name"};
const char* const kDateNames[] = {"taken", "modified", "created"};
const char* const kQualityNames[] = {"", "good", "blurry", "soft", "lowres"};

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
    putText(out, "freeText", q.freeText);
    if (q.freeScope != FreeScope::All) putText(out, "freeScope", kScopeNames[static_cast<int>(q.freeScope)]);
    if (!(q.in == SearchIn{})) {
        // 探す所（既定と違う時だけ）
        out += ",\"in\":{";
        out += std::string("\"tags\":") + (q.in.tags ? "true" : "false") + ",\"partialTags\":" + (q.in.partialTags ? "true" : "false") +
               ",\"title\":" + (q.in.title ? "true" : "false") + ",\"text\":" + (q.in.text ? "true" : "false") +
               ",\"fileName\":" + (q.in.fileName ? "true" : "false") + ",\"folderName\":" + (q.in.folderName ? "true" : "false") + "}";
    }
    putText(out, "underFolder", q.underFolder);
    if (q.dateField != DateField::Taken) putText(out, "dateField", kDateNames[static_cast<int>(q.dateField)]);
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
    putBool(out, "unclassified", q.unclassified);
    putText(out, "path", q.pathContains);
    putText(out, "download", q.downloadContains);
    putBool(out, "deadLinks", q.deadLinks);
    if (q.quality != QualityFilter::Any) putText(out, "quality", kQualityNames[static_cast<int>(q.quality)]);
    putBool(out, "review", q.review);
    putText(out, "clipText", q.clipText);
    putInt(out, "similarTo", q.similarTo);
    if (!q.ids.empty()) {
        out += ",\"ids\":[";
        for (size_t i = 0; i < q.ids.size(); ++i) out += (i ? "," : "") + std::to_string(q.ids[i]);
        out += "]";
    }
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
    q.freeText = j.getString("freeText");
    for (int i = 0; i < 4; ++i)
        if (j.getString("freeScope") == kScopeNames[i]) q.freeScope = static_cast<FreeScope>(i);
    if (const Json* in = j.get("in"); in && in->type == Json::Object) {
        auto b = [&](const char* key, bool& v) {
            if (const Json* x = in->get(key); x && x->type == Json::Bool) v = x->str == "true";
        };
        b("tags", q.in.tags);
        b("partialTags", q.in.partialTags);
        b("title", q.in.title);
        b("text", q.in.text);
        b("fileName", q.in.fileName);
        b("folderName", q.in.folderName);
    }
    q.underFolder = j.getString("underFolder");
    for (int i = 0; i < 3; ++i)
        if (j.getString("dateField") == kDateNames[i]) q.dateField = static_cast<DateField>(i);
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
    flag("deadLinks", q.deadLinks);
    {
        const std::string qn = j.getString("quality");
        for (int i = 1; i < 5; ++i)
            if (qn == kQualityNames[i]) q.quality = static_cast<QualityFilter>(i);
    }
    flag("unclassified", q.unclassified);
    q.pathContains = j.getString("path");
    q.downloadContains = j.getString("download");
    const std::string sort = j.getString("sort");
    for (size_t i = 0; i < std::size(kSortNames); ++i)
        if (sort == kSortNames[i]) q.sort = static_cast<SortKey>(i);
    flag("descending", q.descending);
    flag("review", q.review);
    q.clipText = j.getString("clipText");
    if (const Json* n = j.get("similarTo"); n && n->type == Json::Number) q.similarTo = std::strtoll(n->str.c_str(), nullptr, 10);
    if (const Json* a = j.get("ids"); a && a->type == Json::Array)
        for (const Json& e : a->arr)
            if (e.type == Json::Number) q.ids.push_back(std::strtoll(e.str.c_str(), nullptr, 10));
    out = std::move(q);
    return true;
}

std::vector<SmartFolder> smartFolders(Db& db) {
    std::vector<SmartFolder> out;
    Stmt st(db.handle(), "SELECT id, name, query FROM smart_folders ORDER BY sort IS NULL, sort, name COLLATE NOCASE");
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

void setSmartFolderOrder(Db& db, const std::vector<int64_t>& ids) {
    db.begin();
    try {
        Stmt st(db.handle(), "UPDATE smart_folders SET sort=? WHERE id=?");
        for (size_t i = 0; i < ids.size(); ++i) {
            st.bind(1, static_cast<int64_t>(i)).bind(2, ids[i]);
            st.step();
            st.reset();
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}

std::string exportSmartFolders(const std::vector<SmartFolder>& folders) {
    std::string out = "{\n  \"imgdupe\": \"saved-queries\",\n  \"version\": 1,\n  \"queries\": [";
    for (size_t i = 0; i < folders.size(); ++i) {
        out += i ? ",\n    {\"name\": " : "\n    {\"name\": ";
        jsonString(out, folders[i].name);
        out += ", \"query\": " + queryToJson(folders[i].query) + "}";
    }
    out += folders.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

bool importSmartFolders(const std::string& text, std::vector<SmartFolder>& out) {
    Json root;
    if (!parseJson(text, root) || root.type != Json::Object) return false;
    const Json* list = root.get("queries");
    if (!list || list->type != Json::Array) return false;
    for (const Json& item : list->arr) {
        const Json* q = item.get("query");
        SmartFolder f;
        f.name = item.getString("name");
        if (f.name.empty() || !q || q->type != Json::Object) continue;
        if (!queryFromJson(jsonText(*q), f.query)) continue;
        out.push_back(std::move(f));
    }
    return true;
}

std::vector<std::pair<std::string, int64_t>> tagSources(Db& db) {
    return tagSourceCounts(db);
}

}  // namespace imgdupe
