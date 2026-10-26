#include "tagnames.hpp"

#include <cstdint>
#include <cstdio>

namespace imgdupe {

namespace {

constexpr const char* kInstalledKey = "tag_names:builtin";  // 入れた同梱の訳の内容（FNV-1a）

std::string trimCr(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

}  // namespace

std::string tagNameKey(const std::string& tag) {
    std::string s = tag;
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::vector<std::pair<std::string, std::string>> parseTagNames(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string line = trimCr(text.substr(a, b - a));
        a = b + 1;
        if (out.empty() && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line = line.substr(3);
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) continue;
        out.emplace_back(line.substr(0, tab), line.substr(tab + 1));
    }
    return out;
}

bool installTagNames(Db& db, const std::string& text) {
    const std::string hash = fnv1a(text);
    if (db.setting(kInstalledKey) == hash) return false;
    const auto names = parseTagNames(text);
    db.begin();
    try {
        {
            Stmt del(db.handle(), "DELETE FROM tag_names WHERE source=?");
            del.bind(1, std::string(kTagNameBuiltin));
            del.step();
        }
        Stmt ins(db.handle(), "INSERT OR REPLACE INTO tag_names(tag, source, name) VALUES(?,?,?)");
        for (const auto& [tag, name] : names) {
            ins.bind(1, tag).bind(2, std::string(kTagNameBuiltin)).bind(3, name);
            ins.step();
            ins.reset();
        }
        db.setSetting(kInstalledKey, hash);
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    return true;
}

std::unordered_map<std::string, std::string> loadTagNames(Db& db) {
    std::unordered_map<std::string, std::string> out;
    // user が後に来るように並べて、上書きさせる
    Stmt st(db.handle(), "SELECT tag, name FROM tag_names ORDER BY source=?");
    st.bind(1, std::string(kTagNameUser));
    while (st.step()) out[tagNameKey(st.text(0))] = st.text(1);
    return out;
}

void setUserTagName(Db& db, const std::string& tag, const std::string& name) {
    if (name.empty()) {
        Stmt st(db.handle(), "DELETE FROM tag_names WHERE tag=? AND source=?");
        st.bind(1, tag).bind(2, std::string(kTagNameUser));
        st.step();
        return;
    }
    Stmt st(db.handle(), "INSERT OR REPLACE INTO tag_names(tag, source, name) VALUES(?,?,?)");
    st.bind(1, tag).bind(2, std::string(kTagNameUser)).bind(3, name);
    st.step();
}

std::vector<std::string> tagsNamed(Db& db, const std::string& name) {
    std::vector<std::string> out;
    // 自分で付けた名前があるタグは、同梱の訳では合わせない
    Stmt st(db.handle(),
            "SELECT tag FROM tag_names n WHERE name=? AND (source=? OR NOT EXISTS (SELECT 1 FROM tag_names u "
            "WHERE u.tag=n.tag AND u.source=?))");
    st.bind(1, name).bind(2, std::string(kTagNameUser)).bind(3, std::string(kTagNameUser));
    while (st.step()) out.push_back(st.text(0));
    return out;
}

std::vector<std::string> tagsWithNameContaining(Db& db, const std::string& word, size_t limit) {
    std::vector<std::string> out;
    Stmt st(db.handle(),
            "SELECT tag FROM tag_names n WHERE instr(name, ?) AND (source=? OR NOT EXISTS (SELECT 1 FROM tag_names u "
            "WHERE u.tag=n.tag AND u.source=?)) LIMIT ?");
    st.bind(1, word).bind(2, std::string(kTagNameUser)).bind(3, std::string(kTagNameUser)).bind(4, static_cast<int64_t>(limit));
    while (st.step()) out.push_back(st.text(0));
    return out;
}

}  // namespace imgdupe
