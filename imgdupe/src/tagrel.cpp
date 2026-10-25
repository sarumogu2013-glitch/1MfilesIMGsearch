#include "tagrel.hpp"

#include "json.hpp"

namespace imgdupe {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

}  // namespace

bool parseTagRelations(const std::string& text, std::vector<TagRelation>& out, std::string& error) {
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
        if (lineNo == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line = trim(line.substr(3));
        if (line.empty() || line[0] == '#') continue;
        size_t op = line.find('=');
        std::string kind = "alias";
        if (op == std::string::npos) {
            op = line.find('>');
            kind = "parent";
        }
        const std::string left = op == std::string::npos ? "" : trim(line.substr(0, op));
        const std::string right = op == std::string::npos ? "" : trim(line.substr(op + 1));
        if (left.empty() || right.empty() || right.find_first_of("=>") != std::string::npos) {
            error = std::to_string(lineNo) + " 行目: 「別名 = 元のタグ」か「子のタグ > 親のタグ」の形で書いてください";
            return false;
        }
        out.push_back({left, right, kind});
    }
    return true;
}

std::string formatTagRelations(const std::vector<TagRelation>& rels) {
    std::string out =
        "# タグ辞書（自分で書く分）\n"
        "# 別名:   初音ミク = hatsune_miku     （左は右の別名。検索ではどちらでも同じタグとして探します）\n"
        "# 親子:   hatsune_miku > vocaloid     （左が付いた画像には右も付けます。確度は左と同じ）\n"
        "# 行の先頭の # はコメントです。\n";
    for (const auto& r : rels) out += r.tag + (r.kind == "alias" ? " = " : " > ") + r.other + "\n";
    return out;
}

std::vector<TagRelation> loadRelations(Db& db, const std::string& source) {
    std::vector<TagRelation> out;
    Stmt st(db.handle(), "SELECT tag, other, kind FROM tag_relations WHERE source=? ORDER BY kind, tag, other");
    st.bind(1, source);
    while (st.step()) out.push_back({st.text(0), st.text(1), st.text(2)});
    return out;
}

void replaceRelations(Db& db, const std::string& source, const std::vector<TagRelation>& rels) {
    db.begin();
    try {
        {
            Stmt del(db.handle(), "DELETE FROM tag_relations WHERE source=?");
            del.bind(1, source);
            del.step();
        }
        Stmt ins(db.handle(), "INSERT OR IGNORE INTO tag_relations(tag, other, kind, source) VALUES(?,?,?,?)");
        for (const auto& r : rels) {
            if (r.tag.empty() || r.other.empty() || r.tag == r.other) continue;
            ins.bind(1, r.tag).bind(2, r.other).bind(3, r.kind).bind(4, source);
            ins.step();
            ins.reset();
        }
        // 検索が使う別名（tag_dict）を作り直す。自分で書いたものを優先する
        db.exec("DELETE FROM tag_dict");
        db.exec("INSERT OR IGNORE INTO tag_dict(tag, alias_of) SELECT tag, other FROM tag_relations WHERE kind='alias' AND source='user'");
        db.exec("INSERT OR IGNORE INTO tag_dict(tag, alias_of) SELECT tag, other FROM tag_relations WHERE kind='alias' AND source<>'user'");
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}

bool parseDanbooruPairs(const std::string& json, std::vector<std::pair<std::string, std::string>>& out) {
    Json j;
    if (!parseJson(json, j) || j.type != Json::Array) return false;
    for (const Json& e : j.arr) {
        if (e.type != Json::Object) continue;
        const std::string a = e.getString("antecedent_name"), c = e.getString("consequent_name");
        const std::string status = e.getString("status");
        if (!a.empty() && !c.empty() && (status.empty() || status == "active")) out.emplace_back(a, c);
    }
    return true;
}

size_t applyParents(Db& db, int depth) {
    db.begin();
    try {
        db.exec("DELETE FROM tags WHERE source='parent' AND manual=0");
        // 子のタグ（別名ならその元の名前で）に親子関係があれば、親のタグを付ける。手動で「違う」と確定したものは子にしない
        const char* sql =
            "INSERT OR REPLACE INTO tags(image_id, tag, kind, confidence, source, manual) "
            "SELECT t.image_id, r.other, 'general', max(t.confidence), 'parent', 0 "
            "FROM tags t LEFT JOIN tag_dict d ON d.tag = t.tag COLLATE NOCASE "
            "JOIN tag_relations r ON r.kind='parent' AND r.tag = COALESCE(d.alias_of, t.tag) COLLATE NOCASE "
            "WHERE t.confidence >= 0.2 AND NOT (t.manual=1 AND t.confidence < 0.5) AND (t.source<>'parent' OR ?) "
            "GROUP BY t.image_id, r.other";
        size_t total = 0;
        for (int pass = 0; pass < depth; ++pass) {
            Stmt st(db.handle(), sql);
            st.bind(1, static_cast<int64_t>(pass > 0));  // 2 回目からは、付けた親のさらに親
            st.step();
            const size_t n = static_cast<size_t>(db.changes());
            if (pass == 0) total = n;
            if (n == 0) break;
        }
        Stmt count(db.handle(), "SELECT COUNT(*) FROM tags WHERE source='parent'");
        if (count.step()) total = static_cast<size_t>(count.i64(0));
        db.commit();
        return total;
    } catch (...) {
        db.rollback();
        throw;
    }
}

RelationCounts relationCounts(Db& db) {
    RelationCounts c;
    Stmt st(db.handle(), "SELECT source, kind, COUNT(*) FROM tag_relations GROUP BY source, kind");
    while (st.step()) {
        const bool user = st.text(0) == "user", alias = st.text(1) == "alias";
        const size_t n = static_cast<size_t>(st.i64(2));
        (user ? (alias ? c.userAlias : c.userParent) : (alias ? c.danbooruAlias : c.danbooruParent)) += n;
    }
    return c;
}

}  // namespace imgdupe
