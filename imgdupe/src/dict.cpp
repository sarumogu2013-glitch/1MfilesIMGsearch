#include "dict.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace imgdupe {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    auto space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (a < b && space(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && space(static_cast<unsigned char>(s[b - 1]))) --b;
    // 全角空白も
    while (b >= a + 3 && s.compare(b - 3, 3, "\xE3\x80\x80") == 0) b -= 3;
    while (a + 3 <= b && s.compare(a, 3, "\xE3\x80\x80") == 0) a += 3;
    return s.substr(a, b - a);
}

// "a", "b, c", d → [a, b, c, d]（"" で囲めば , も書ける）
std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, wasQuoted = false;
    auto flush = [&] {
        std::string t = wasQuoted ? cur : trim(cur);
        if (!t.empty()) out.push_back(t);
        cur.clear();
        wasQuoted = false;
    };
    for (char c : s) {
        if (c == '"') {
            quoted = !quoted;
            if (quoted) {
                cur.clear();
                wasQuoted = true;
            }
        } else if (c == ',' && !quoted) {
            flush();
        } else if (quoted || !wasQuoted) {
            cur += c;
        }
    }
    flush();
    return out;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

}  // namespace

bool parseDictionary(const std::string& text, std::vector<DictEntry>& out, std::string& error) {
    out.clear();
    error.clear();
    size_t a = 0;
    int lineNo = 0;
    while (a <= text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string line = trim(text.substr(a, b - a));
        a = b + 1;
        ++lineNo;
        if (lineNo == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line = trim(line.substr(3));  // BOM
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            const size_t close = line.rfind(']');
            const std::string name = close == std::string::npos ? "" : trim(line.substr(1, close - 1));
            if (name.empty()) {
                error = std::to_string(lineNo) + " 行目: [名前] の形で書いてください";
                return false;
            }
            out.push_back(DictEntry{});
            out.back().name = name;
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            error = std::to_string(lineNo) + " 行目: 「wd14:」「clip:」「種類:」のどれかで始めてください";
            return false;
        }
        if (out.empty()) {
            error = std::to_string(lineNo) + " 行目: 先に [名前] の行を書いてください";
            return false;
        }
        const std::string key = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        DictEntry& e = out.back();
        if (key == "wd14") {
            for (auto& t : splitList(value)) e.wd14.push_back(t);
        } else if (key == "clip") {
            for (auto& t : splitList(value)) e.clip.push_back(t);
        } else if (key == "kind" || key == "種類") {
            if (!value.empty()) e.kind = value;
        } else {
            error = std::to_string(lineNo) + " 行目: 「" + key + "」は使えません（wd14 / clip / 種類）";
            return false;
        }
    }
    // 中身の無い項目はあっても困らないので、そのまま
    return true;
}

std::string defaultDictionary() {
    return "# 行動・アイテム辞書\n"
           "# WD14 のタグと CLIP の文章を、自分の分類にまとめます。保存すると、すべての画像に当て直します。\n"
           "#\n"
           "# [分類の名前]\n"
           "# 種類: action          （省略すると action。item などにもできます）\n"
           "# wd14: タグ, タグ       （WD14 のタグのどれかが付いていれば。確度はその中で一番高いもの）\n"
           "# clip: \"文章\", \"文章\"  （画像の内容と文章の近さを確度にします。日本語でも英語でも）\n"
           "#\n"
           "# 例（先頭の # を消すと使えます）:\n"
           "# [テニスをしている]\n"
           "# wd14: tennis_racket, tennis_ball, tennis_uniform\n"
           "# clip: \"a person playing tennis\"\n"
           "#\n"
           "# [剣を振りかぶっている]\n"
           "# wd14: holding_sword, sword, fighting_stance\n"
           "# clip: \"a person raising a sword overhead\"\n";
}

std::string dictionaryHash(const std::string& text) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

void applyDictionaryTags(Db& db, const std::vector<DictEntry>& entries) {
    db.begin();
    try {
        db.exec("DELETE FROM tags WHERE source='dict' AND manual=0");
        for (const auto& e : entries) {
            if (e.wd14.empty()) continue;
            std::string in;
            for (size_t i = 0; i < e.wd14.size(); ++i) in += i ? ",?" : "?";
            const std::string sql =
                "INSERT OR REPLACE INTO tags(image_id,tag,kind,confidence,source,manual) "
                "SELECT image_id, ?, ?, max(confidence), 'dict', 0 FROM tags WHERE source='tagger' AND manual=0 AND tag IN (" +
                in + ") GROUP BY image_id";
            Stmt st(db.handle(), sql.c_str());
            st.bind(1, e.name).bind(2, e.kind);
            for (size_t i = 0; i < e.wd14.size(); ++i) st.bind(static_cast<int>(i + 3), e.wd14[i]);
            st.step();
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}

double clipProbability(double sim) { return 1.0 / (1.0 + std::exp(-(sim - 0.27) * 40.0)); }

std::vector<std::pair<int64_t, float>> clipMatches(const VectorIndex& index, const std::vector<std::vector<float>>& textVecs,
                                                   double minP, unsigned threads) {
    // 文章ごとに、確度 minP 以上の画像を全部（上位 k 件ではなく）。画像ごとに一番高い確度
    std::unordered_map<int64_t, float> best;
    for (const auto& tv : textVecs) {
        if (tv.size() != index.dim() || index.size() == 0) continue;
        for (const auto& [id, sim] : index.topK(tv.data(), index.size(), {}, threads)) {
            const double p = clipProbability(sim);
            if (p < minP) break;  // 内積の大きい順なので、ここから先は全部 minP 未満
            float& b = best[id];
            b = std::max(b, static_cast<float>(p));
        }
    }
    std::vector<std::pair<int64_t, float>> out(best.begin(), best.end());
    std::sort(out.begin(), out.end());
    return out;
}

void replaceClipDictTags(Db& db, const std::vector<DictEntry>& entries,
                         const std::vector<std::vector<std::pair<int64_t, float>>>& matches) {
    db.begin();
    try {
        db.exec("DELETE FROM tags WHERE source='clip' AND manual=0");
        Stmt ins(db.handle(), "INSERT OR REPLACE INTO tags(image_id,tag,kind,confidence,source,manual) VALUES(?,?,?,?,'clip',0)");
        for (size_t i = 0; i < entries.size() && i < matches.size(); ++i)
            for (const auto& [id, p] : matches[i]) {
                ins.bind(1, id).bind(2, entries[i].name).bind(3, entries[i].kind).bind(4, static_cast<double>(p));
                ins.step();
                ins.reset();
            }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}

}  // namespace imgdupe
