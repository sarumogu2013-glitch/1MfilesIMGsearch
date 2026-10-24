#include "links.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <set>
#include <unordered_set>

#include "platform.hpp"

namespace imgdupe {

namespace {

const std::string kRootKey = "vfolders:";

char sepOf(const std::string& path) {
    return (path.find('\\') != std::string::npos || (path.size() >= 2 && path[1] == ':')) ? '\\' : '/';
}

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char sep = sepOf(dir);
    return (dir.back() == '\\' || dir.back() == '/') ? dir + name : dir + sep + name;
}

size_t lastSep(const std::string& path) { return path.find_last_of("\\/"); }

std::string parentOf(const std::string& path) {
    const size_t i = lastSep(path);
    return i == std::string::npos ? std::string() : path.substr(0, i);
}

std::string baseName(const std::string& path) {
    const size_t i = lastSep(path);
    return i == std::string::npos ? path : path.substr(i + 1);
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool underDir(const std::string& path, const std::string& dir) {
    if (dir.empty()) return true;
    if (path.size() <= dir.size() + 1) return false;
    if (lowerAscii(path.substr(0, dir.size())) != lowerAscii(dir)) return false;
    const char c = path[dir.size()];
    return c == '\\' || c == '/';
}

std::string imagePath(Db& db, int64_t id) {
    Stmt st(db.handle(), "SELECT path FROM images WHERE id=?");
    st.bind(1, id);
    return st.step() ? st.text(0) : std::string();
}

struct LinkRow {
    std::string path;
    int64_t imageId = 0;
    bool temporary = false;
    int64_t created = 0;
};

std::vector<LinkRow> allLinks(Db& db) {
    std::vector<LinkRow> out;
    Stmt st(db.handle(), "SELECT path, image_id, temporary, created FROM links");
    while (st.step()) {
        LinkRow r;
        r.path = st.text(0);
        r.imageId = st.isNull(1) ? 0 : st.i64(1);
        r.temporary = st.i64(2) != 0;
        r.created = st.isNull(3) ? 0 : st.i64(3);
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace

std::string vfolderRoot(Db& db, const std::string& drive) {
    const std::string custom = db.setting(kRootKey + lowerAscii(drive));
    if (!custom.empty()) return custom;
    if (drive == "/") return std::string("/") + kVfolderDirName;
    return drive + "\\" + kVfolderDirName;
}

void setVfolderRoot(Db& db, const std::string& drive, const std::string& root) {
    std::string r = root;
    while (r.size() > 3 && (r.back() == '\\' || r.back() == '/')) r.pop_back();
    db.setSetting(kRootKey + lowerAscii(drive), r);
}

std::vector<std::string> customVfolderRoots(Db& db) {
    std::vector<std::string> out;
    Stmt st(db.handle(), "SELECT value FROM settings WHERE key LIKE 'vfolders:%' AND value<>''");
    while (st.step()) out.push_back(st.text(0));
    return out;
}

std::string safeFolderName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        out += (u < 0x20 || std::string("\\/:*?\"<>|").find(c) != std::string::npos) ? '_' : c;
    }
    // 先頭と末尾の空白、末尾の . は Windows で扱いにくい
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (out.empty() || out == "." || out == "..") out = "検索結果";
    return out;
}

ExpandResult expandLinks(Db& db, const std::vector<int64_t>& ids, const std::string& name, bool temporary,
                         const std::atomic<bool>* cancel) {
    ExpandResult r;
    const std::string folderName = safeFolderName(name);

    // 同じ名前で前に展開したものを消す（どのドライブの分も）
    {
        std::set<std::string> dirs;
        for (const auto& l : allLinks(db)) {
            const std::string dir = parentOf(l.path);
            if (lowerAscii(baseName(dir)) == lowerAscii(folderName)) dirs.insert(dir);
        }
        for (const auto& dir : dirs) r.removed += removeLinks(db, dir, false).removed;
    }

    std::map<std::string, std::string> folderOfDrive;  // ドライブ → 作るフォルダ
    std::map<std::string, std::unordered_set<std::string>> used;  // フォルダ → 使った名前（小文字）
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    Stmt ins(db.handle(), "INSERT OR REPLACE INTO links(path, image_id, temporary, created) VALUES(?,?,?,?)");
    db.begin();
    size_t sinceCommit = 0;
    for (int64_t id : ids) {
        if (cancel && *cancel) {
            r.cancelled = true;
            break;
        }
        const std::string src = imagePath(db, id);
        auto fail = [&] {
            ++r.failed;
            if (r.firstFailure.empty()) r.firstFailure = src;
        };
        if (src.empty()) {
            fail();
            continue;
        }
        const std::string drive = driveOf(src);
        auto it = folderOfDrive.find(drive);
        if (it == folderOfDrive.end()) {
            const std::string dir = join(vfolderRoot(db, drive), folderName);
            if (!makeDirs(dir)) {
                fail();
                continue;
            }
            it = folderOfDrive.emplace(drive, dir).first;
            r.folders.push_back(dir);
        }
        const std::string& dir = it->second;
        auto& names = used[dir];
        std::string file = baseName(src);
        if (names.count(lowerAscii(file)) || pathExists(join(dir, file))) file = std::to_string(id) + "_" + file;
        const std::string dst = join(dir, file);
        if (names.count(lowerAscii(file)) || pathExists(dst) || !createHardLink(src, dst)) {
            fail();
            continue;
        }
        names.insert(lowerAscii(file));
        ins.bind(1, dst).bind(2, id).bind(3, static_cast<int64_t>(temporary)).bind(4, now);
        ins.step();
        ins.reset();
        ++r.created;
        if (++sinceCommit >= 1000) {  // 途中で落ちても、作った分の記録は残す（次の起動で消せるように）
            db.commit();
            db.begin();
            sinceCommit = 0;
        }
    }
    db.commit();
    return r;
}

std::vector<LinkFolder> linkFolders(Db& db) {
    std::map<std::string, LinkFolder> m;
    for (const auto& l : allLinks(db)) {
        const std::string dir = parentOf(l.path);
        LinkFolder& f = m[lowerAscii(dir)];
        if (f.dir.empty()) f.dir = dir;
        ++f.count;
        if (l.temporary) ++f.temporary;
        f.created = std::max(f.created, l.created);
    }
    std::vector<LinkFolder> out;
    for (auto& [k, f] : m) out.push_back(std::move(f));
    std::sort(out.begin(), out.end(), [](const LinkFolder& a, const LinkFolder& b) { return a.created > b.created; });
    return out;
}

void setFolderTemporary(Db& db, const std::string& dir, bool temporary) {
    Stmt up(db.handle(), "UPDATE links SET temporary=? WHERE path=?");
    db.begin();
    for (const auto& l : allLinks(db)) {
        if (parentOf(l.path) != dir && lowerAscii(parentOf(l.path)) != lowerAscii(dir)) continue;
        up.bind(1, static_cast<int64_t>(temporary)).bind(2, l.path);
        up.step();
        up.reset();
    }
    db.commit();
}

RemoveResult removeLinks(Db& db, const std::string& dir, bool temporaryOnly) {
    RemoveResult r;
    std::set<std::string> dirs;
    Stmt del(db.handle(), "DELETE FROM links WHERE path=?");
    auto forget = [&](const std::string& path) {
        del.bind(1, path);
        del.step();
        del.reset();
    };
    db.begin();
    for (const auto& l : allLinks(db)) {
        if (temporaryOnly && !l.temporary) continue;
        if (!dir.empty() && !underDir(l.path, dir)) continue;
        FileIdentity link;
        if (!fileIdentity(l.path, link)) {
            if (pathExists(l.path)) {
                ++r.failed;  // 開けない（フォルダになっている、など）。記録は残す
            } else {
                ++r.gone;
                forget(l.path);
                dirs.insert(parentOf(l.path));
            }
            continue;
        }
        FileIdentity orig;
        const std::string src = l.imageId ? imagePath(db, l.imageId) : std::string();
        const bool haveOrig = !src.empty() && fileIdentity(src, orig);
        if (haveOrig && !link.same(orig)) {
            ++r.gone;  // 別のファイルに置き換わっている。触らずに記録だけ消す
            forget(l.path);
            continue;
        }
        if (!haveOrig && link.links <= 1) {
            ++r.kept;  // データがこのリンクにしか無い
            continue;
        }
        if (removeFile(l.path)) {
            ++r.removed;
            forget(l.path);
            dirs.insert(parentOf(l.path));
        } else {
            ++r.failed;
        }
    }
    db.commit();
    for (const auto& d : dirs) removeEmptyDir(d);
    return r;
}

std::vector<std::string> orphanLinks(Db& db) {
    std::vector<std::string> out;
    for (const auto& l : allLinks(db)) {
        FileIdentity link, orig;
        if (!fileIdentity(l.path, link) || link.links > 1) continue;
        const std::string src = l.imageId ? imagePath(db, l.imageId) : std::string();
        if (src.empty() || !fileIdentity(src, orig)) out.push_back(l.path);
    }
    return out;
}

}  // namespace imgdupe
