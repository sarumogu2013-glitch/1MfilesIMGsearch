#include "archive.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <ctime>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "log.hpp"
#include "sqlite3.h"
#include "platform.hpp"
#include "tagstore.hpp"
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

namespace {

std::string fileNameOf(const std::string& p) {
    const size_t e = p.find_last_of("\\/");
    return e == std::string::npos ? p : p.substr(e + 1);
}

bool sameNameNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// 見つからない画像・保管の画像の、見分ける情報
struct Candidate {
    int64_t id = 0;  // images.id か archived.aid
    std::string path;
    int64_t mtime = 0;
    uint64_t fileId = 0;
    std::vector<uint8_t> sha;
    bool used = false;
};

// 新しく見つけた newPath が、c と同じ画像か
bool sameImage(const std::string& newPath, const Candidate& c, std::optional<Sha256>& newSha) {
    // 1. 同じドライブで、ファイル ID が同じ（動かしても名前を変えても変わらない）
    if (c.fileId && driveOf(newPath) == driveOf(c.path)) {
        FileIdentity fi;
        if (fileIdentity(newPath, fi) && fi.index == c.fileId) return true;
    }
    // 2. 中身の SHA-256 が同じ
    if (c.sha.size() == 32) {
        if (!newSha) {
            Sha256 h{};
            if (!sha256File(newPath, h)) return false;
            newSha = h;
        }
        return std::equal(newSha->begin(), newSha->end(), c.sha.begin());
    }
    // 3. SHA が無い（同じ大きさの画像が無かった）: 大きさ・更新日時に加えて、ファイル名が同じ
    return sameNameNoCase(fileNameOf(newPath), fileNameOf(c.path));
}

}  // namespace

MoveReport matchMovedImages(Db& db, const std::vector<ScannedFile>& files, const std::vector<std::string>& under,
                            const std::string& archiveFile, const std::string& vectorFile) {
    MoveReport rep;
    if (files.empty()) return rep;
    // DB の画像: 今回見つかったか（見つからないものは、under の下なら引き継ぎの候補）
    std::unordered_set<std::string> found;
    found.reserve(files.size());
    for (const auto& f : files) found.insert(f.path);
    std::unordered_set<std::string> known;
    std::unordered_map<int64_t, std::vector<Candidate>> gone;  // 大きさ → 見つからない画像
    {
        Stmt st(db.handle(), "SELECT id, path, size, mtime, file_id, sha FROM images");
        while (st.step()) {
            std::string p = st.text(1);
            if (found.count(p)) {
                known.insert(std::move(p));
                continue;
            }
            bool inside = false;
            for (const auto& r : under) inside = inside || underRoot(p, r);
            if (!inside) continue;
            Candidate c;
            c.id = st.i64(0);
            c.path = std::move(p);
            c.mtime = st.i64(3);
            c.fileId = static_cast<uint64_t>(st.isNull(4) ? 0 : st.i64(4));
            c.sha = st.blob(5);
            gone[st.i64(2)].push_back(std::move(c));
        }
    }
    std::vector<const ScannedFile*> fresh;  // DB に無い（新しく見つけた）ファイル
    for (const auto& f : files)
        if (!known.count(f.path)) fresh.push_back(&f);
    if (fresh.empty()) return rep;

    // 1. 見つからない画像と同じなら、パスを書き換える
    std::vector<const ScannedFile*> rest;
    if (!gone.empty()) {
        Stmt upd(db.handle(), "UPDATE images SET path=?, drive=? WHERE id=?");
        db.begin();
        try {
            for (const ScannedFile* f : fresh) {
                bool moved = false;
                auto it = gone.find(static_cast<int64_t>(f->size));
                if (it != gone.end()) {
                    std::optional<Sha256> sha;
                    for (Candidate& c : it->second) {
                        if (c.used || c.mtime != f->mtime || !sameImage(f->path, c, sha)) continue;
                        upd.bind(1, f->path).bind(2, driveOf(f->path)).bind(3, c.id);
                        upd.step();
                        upd.reset();
                        c.used = true;
                        moved = true;
                        ++rep.moved;
                        break;
                    }
                }
                if (!moved) rest.push_back(f);
            }
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }
    } else {
        rest = fresh;
    }

    // 2. 保管にあるものと同じなら、保管から戻す
    if (rest.empty() || archiveFile.empty() || fileSize(archiveFile) < 0) return rep;
    Attach a(db, archiveFile, false);
    if (!a.ok()) return rep;
    // ベクトルは、今のファイルと同じモデル・次元の時だけ戻す（違えば CLIP は作り直し）
    std::string curModel;
    uint32_t curDim = 0;
    std::unordered_map<int64_t, std::vector<uint16_t>> none;
    const bool haveVectors = !vectorFile.empty() && readVectorsOf(vectorFile, {}, none, curModel, curDim);
    VectorWriter vw;
    bool vwOpen = false;
    Stmt find(db.handle(), "SELECT aid, path, mtime, file_id, sha FROM arc.archived WHERE size=? AND mtime=?");
    db.begin();
    try {
        Stmt ins(db.handle(), "INSERT INTO images(path,drive,size,mtime,file_id,sha,added) "
                              "SELECT ?,?,size,mtime,file_id,sha,added FROM arc.archived WHERE aid=?");
        Stmt tags(db.handle(), "SELECT tag, kind, source, confidence, manual FROM arc.archived_tags WHERE aid=?");
        Stmt stages(db.handle(), "SELECT stage, version, state, updated FROM arc.archived_stages WHERE aid=?");
        Stmt insStage(db.handle(), "INSERT OR REPLACE INTO stages(image_id,stage,state,version,updated) VALUES(?,?,?,?,?)");
        Stmt vec(db.handle(), "SELECT model, dim, data FROM arc.archived_vectors WHERE aid=?");
        std::vector<int64_t> restoredAids;
        TagWriter tw(db, TagConflict::KeepManual);
        for (const ScannedFile* f : rest) {
            find.bind(1, static_cast<int64_t>(f->size)).bind(2, f->mtime);
            std::vector<Candidate> cands;
            while (find.step()) {
                Candidate c;
                c.id = find.i64(0);
                c.path = find.text(1);
                c.mtime = find.i64(2);
                c.fileId = static_cast<uint64_t>(find.isNull(3) ? 0 : find.i64(3));
                c.sha = find.blob(4);
                cands.push_back(std::move(c));
            }
            find.reset();
            std::optional<Sha256> sha;
            for (const Candidate& c : cands) {
                if (std::find(restoredAids.begin(), restoredAids.end(), c.id) != restoredAids.end() || !sameImage(f->path, c, sha))
                    continue;
                ins.bind(1, f->path).bind(2, driveOf(f->path)).bind(3, c.id);
                ins.step();
                ins.reset();
                const int64_t id = sqlite3_last_insert_rowid(db.handle());
                tags.bind(1, c.id);
                while (tags.step()) tw.put(id, tags.text(0), tags.text(1), tags.f64(3), tags.text(2), tags.i64(4) != 0);
                tags.reset();
                // CLIP のベクトルを戻せた時だけ、CLIP の段階も済みにする
                bool vectorBack = false;
                vec.bind(1, c.id);
                if (vec.step() && haveVectors && vec.text(0) == curModel && static_cast<uint32_t>(vec.i64(1)) == curDim) {
                    const auto blob = vec.blob(2);
                    if (blob.size() == 2ull * curDim) {
                        if (!vwOpen) vwOpen = vw.open(vectorFile, curDim, curModel);
                        if (vwOpen) {
                            std::vector<float> v(curDim);
                            for (uint32_t k = 0; k < curDim; ++k)
                                v[k] = halfToFloat(static_cast<uint16_t>(blob[2 * k] | (blob[2 * k + 1] << 8)));
                            vectorBack = vw.append(id, v.data());
                        }
                    }
                }
                vec.reset();
                stages.bind(1, c.id);
                while (stages.step()) {
                    if (stages.text(0) == "clip" && !vectorBack) continue;
                    insStage.bind(1, id).bind(2, stages.text(0)).bind(3, stages.i64(2)).bind(4, stages.i64(1)).bind(5, stages.i64(3));
                    insStage.step();
                    insStage.reset();
                }
                stages.reset();
                restoredAids.push_back(c.id);
                ++rep.restored;
                break;
            }
        }
        // 戻したものは保管から消す
        for (int64_t aid : restoredAids)
            for (const char* t : {"archived_tags", "archived_stages", "archived_vectors", "archived"}) {
                Stmt del(db.handle(), (std::string("DELETE FROM arc.") + t + " WHERE aid=?").c_str());
                del.bind(1, aid);
                del.step();
            }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    if (vwOpen) vw.close();
    return rep;
}

size_t moveRootPaths(Db& db, const std::string& oldRoot, const std::string& newRoot) {
    auto trim = [](std::string s) {
        while (s.size() > 1 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
        return s;
    };
    const std::string from = trim(oldRoot), to = trim(newRoot);
    if (from.empty() || to.empty() || from == to) return 0;
    struct Row {
        int64_t id;
        std::string path;
    };
    std::vector<Row> rows;
    {
        Stmt st(db.handle(), "SELECT id, path FROM images");
        while (st.step()) {
            std::string p = st.text(1);
            if (underRoot(p, from)) rows.push_back({st.i64(0), std::move(p)});
        }
    }
    if (rows.empty()) return 0;
    size_t n = 0;
    db.begin();
    try {
        Stmt find(db.handle(), "SELECT id FROM images WHERE path=?");
        Stmt del(db.handle(), "DELETE FROM images WHERE id=?");
        Stmt upd(db.handle(), "UPDATE images SET path=?, drive=? WHERE id=?");
        for (const Row& r : rows) {
            const std::string np = to + r.path.substr(from.size());
            find.bind(1, np);
            if (find.step()) {
                // 新しい場所を先に調べて、同じ画像が新しい行で入っていた: 手動の確定をこちらへ移して、その行を消す
                const int64_t other = find.i64(0);
                find.reset();
                copyManualTags(db, other, r.id);
                del.bind(1, other);
                del.step();
                del.reset();
            } else {
                find.reset();
            }
            upd.bind(1, np).bind(2, driveOf(np)).bind(3, r.id);
            upd.step();
            upd.reset();
            ++n;
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    return n;
}

}  // namespace imgdupe
