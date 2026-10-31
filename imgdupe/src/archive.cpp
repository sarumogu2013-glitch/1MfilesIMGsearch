#include "archive.hpp"

#include <algorithm>
#include <ctime>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "log.hpp"
#include "sqlite3.h"
#include "platform.hpp"
#include "vectors.hpp"

namespace imgdupe {

namespace {

const char* const kArchiveSchema = R"SQL(
CREATE TABLE IF NOT EXISTS arc.archived(
    aid INTEGER PRIMARY KEY,
    orig_id INTEGER,
    path TEXT NOT NULL,
    drive TEXT,
    size INTEGER,
    mtime INTEGER,
    file_id INTEGER,
    sha BLOB,
    added INTEGER,
    archived INTEGER NOT NULL,
    reason TEXT
);
CREATE INDEX IF NOT EXISTS arc.archived_size ON archived(size);
CREATE INDEX IF NOT EXISTS arc.archived_time ON archived(archived);
CREATE TABLE IF NOT EXISTS arc.archived_tags(
    aid INTEGER NOT NULL,
    tag TEXT NOT NULL,
    kind TEXT,
    source TEXT,
    confidence REAL,
    manual INTEGER
);
CREATE INDEX IF NOT EXISTS arc.archived_tags_aid ON archived_tags(aid);
CREATE TABLE IF NOT EXISTS arc.archived_stages(
    aid INTEGER NOT NULL,
    stage TEXT NOT NULL,
    version INTEGER,
    state INTEGER,
    updated INTEGER
);
CREATE INDEX IF NOT EXISTS arc.archived_stages_aid ON archived_stages(aid);
CREATE TABLE IF NOT EXISTS arc.archived_vectors(
    aid INTEGER PRIMARY KEY,
    model TEXT,
    dim INTEGER,
    data BLOB
);
)SQL";

std::string dirOf(const std::string& path) {
    const size_t e = path.find_last_of("\\/");
    return e == std::string::npos ? std::string() : path.substr(0, e);
}

// 保管の DB を今の接続に arc として付ける（無ければ create の時だけ作る）。外す時に DETACH
class Attach {
public:
    Attach(Db& db, const std::string& file, bool create) : db_(db) {
        if (!create && fileSize(file) < 0) return;
        if (create) makeDirs(dirOf(file));
        Stmt st(db.handle(), "ATTACH DATABASE ? AS arc");
        st.bind(1, file);
        st.step();
        ok_ = true;
        if (!db.exec(kArchiveSchema)) {
            db.exec("DETACH DATABASE arc");
            ok_ = false;
            throw std::runtime_error("保管の DB を開けません: " + file);
        }
    }
    ~Attach() {
        if (ok_) db_.exec("DETACH DATABASE arc");
    }
    bool ok() const { return ok_; }

private:
    Db& db_;
    bool ok_ = false;
};

std::string placeholders(size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) s += i ? ",?" : "?";
    return s;
}

}  // namespace

std::string archiveFileOf(const std::string& dataDir) {
    if (dataDir.empty()) return "";
    const char sep = dataDir.find('\\') != std::string::npos ? '\\' : '/';
    std::string d = dataDir;
    if (d.back() != '\\' && d.back() != '/') d += sep;
    return d + "archive" + sep + "archive.db";
}

size_t archiveImages(Db& db, const std::string& archiveFile, const std::vector<int64_t>& ids, const std::string& vectorFile,
                     const std::string& reason) {
    if (ids.empty()) return 0;
    // CLIP のベクトルは先に読んでおく（索引は作らない。メモリは保管する分だけ）
    std::unordered_map<int64_t, std::vector<uint16_t>> vectors;
    std::string model;
    uint32_t dim = 0;
    if (!vectorFile.empty() && fileSize(vectorFile) > 0) {
        const std::unordered_set<int64_t> want(ids.begin(), ids.end());
        if (!readVectorsOf(vectorFile, want, vectors, model, dim)) vectors.clear();
    }
    Attach a(db, archiveFile, true);
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    size_t moved = 0;
    db.begin();
    try {
        Stmt img(db.handle(),
                 "INSERT INTO arc.archived(orig_id,path,drive,size,mtime,file_id,sha,added,archived,reason) "
                 "SELECT id,path,drive,size,mtime,file_id,sha,added,?,? FROM images WHERE id=?");
        // 守るタグ: タガーのタグと手動の確定（ほかは作り直せる）
        Stmt tags(db.handle(),
                  "INSERT INTO arc.archived_tags(aid,tag,kind,source,confidence,manual) "
                  "SELECT ?,tag,kind,source,confidence,manual FROM tags WHERE image_id=? AND (manual=1 OR source='tagger')");
        Stmt stages(db.handle(),
                    "INSERT INTO arc.archived_stages(aid,stage,version,state,updated) "
                    "SELECT ?,stage,version,state,updated FROM stages WHERE image_id=? AND stage IN ('tagger','clip')");
        Stmt vec(db.handle(), "INSERT INTO arc.archived_vectors(aid,model,dim,data) VALUES(?,?,?,?)");
        Stmt del(db.handle(), "DELETE FROM images WHERE id=?");
        for (int64_t id : ids) {
            img.bind(1, now).bind(2, reason).bind(3, id);
            img.step();
            img.reset();
            if (sqlite3_changes(db.handle()) == 0) continue;  // もう無い
            const int64_t aid = sqlite3_last_insert_rowid(db.handle());
            tags.bind(1, aid).bind(2, id);
            tags.step();
            tags.reset();
            stages.bind(1, aid).bind(2, id);
            stages.step();
            stages.reset();
            auto v = vectors.find(id);
            if (v != vectors.end()) {
                vec.bind(1, aid).bind(2, model).bind(3, static_cast<int64_t>(dim)).bindBlob(4, v->second.data(), v->second.size() * 2);
                vec.step();
                vec.reset();
            }
            del.bind(1, id);
            del.step();
            del.reset();
            ++moved;
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    return moved;
}

ArchiveSummary archiveSummary(Db& db, const std::string& archiveFile) {
    ArchiveSummary s;
    s.fileBytes = std::max<int64_t>(0, fileSize(archiveFile));
    Attach a(db, archiveFile, false);
    if (!a.ok()) return s;
    Stmt st(db.handle(), "SELECT count(*), coalesce(min(archived),0), coalesce(max(archived),0) FROM arc.archived");
    if (st.step()) {
        s.images = st.i64(0);
        s.oldest = st.i64(1);
        s.newest = st.i64(2);
    }
    return s;
}

std::vector<ArchivedImage> listArchived(Db& db, const std::string& archiveFile, size_t limit) {
    std::vector<ArchivedImage> out;
    Attach a(db, archiveFile, false);
    if (!a.ok()) return out;
    std::string sql =
        "SELECT a.aid, a.path, coalesce(a.size,0), a.archived, coalesce(a.reason,''), "
        "(SELECT count(*) FROM arc.archived_tags t WHERE t.aid=a.aid), "
        "EXISTS (SELECT 1 FROM arc.archived_vectors v WHERE v.aid=a.aid) "
        "FROM arc.archived a ORDER BY a.archived, a.aid";
    if (limit) sql += " LIMIT " + std::to_string(limit);
    Stmt st(db.handle(), sql.c_str());
    while (st.step()) {
        ArchivedImage r;
        r.aid = st.i64(0);
        r.path = st.text(1);
        r.size = st.i64(2);
        r.archived = st.i64(3);
        r.reason = st.text(4);
        r.tags = static_cast<int>(st.i64(5));
        r.vector = st.i64(6) != 0;
        out.push_back(std::move(r));
    }
    return out;
}

namespace {
size_t deleteWhere(Db& db, const std::string& archiveFile, const std::string& cond, const std::vector<int64_t>& params, bool vacuum) {
    Attach a(db, archiveFile, false);
    if (!a.ok()) return 0;
    size_t n = 0;
    db.begin();
    try {
        for (const char* t : {"archived_tags", "archived_stages", "archived_vectors"}) {
            const std::string sql = std::string("DELETE FROM arc.") + t + " WHERE aid IN (SELECT aid FROM arc.archived WHERE " + cond + ")";
            Stmt st(db.handle(), sql.c_str());
            for (size_t i = 0; i < params.size(); ++i) st.bind(static_cast<int>(i) + 1, params[i]);
            st.step();
        }
        const std::string sql = "DELETE FROM arc.archived WHERE " + cond;
        Stmt st(db.handle(), sql.c_str());
        for (size_t i = 0; i < params.size(); ++i) st.bind(static_cast<int>(i) + 1, params[i]);
        st.step();
        n = static_cast<size_t>(sqlite3_changes(db.handle()));
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    if (n && vacuum) db.exec("VACUUM arc");  // ファイルも小さくする
    return n;
}
}  // namespace

size_t deleteArchived(Db& db, const std::string& archiveFile, const std::vector<int64_t>& aids) {
    size_t n = 0;
    for (size_t i = 0; i < aids.size(); i += 500) {
        const std::vector<int64_t> part(aids.begin() + static_cast<std::ptrdiff_t>(i),
                                        aids.begin() + static_cast<std::ptrdiff_t>(std::min(aids.size(), i + 500)));
        n += deleteWhere(db, archiveFile, "aid IN (" + placeholders(part.size()) + ")", part, i + 500 >= aids.size());
    }
    return n;
}

size_t deleteArchivedBefore(Db& db, const std::string& archiveFile, int64_t before) {
    return deleteWhere(db, archiveFile, "archived < ?", {before}, true);
}

}  // namespace imgdupe
