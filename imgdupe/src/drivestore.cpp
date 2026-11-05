#include "drivestore.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <unordered_map>

#include "archive.hpp"
#include "log.hpp"
#include "platform.hpp"
#include "sqlite3.h"
#include "tagstore.hpp"
#include "vectors.hpp"

namespace imgdupe {

namespace {

// 正本の DB の形（ATTACH した名前 a の下に作る）
std::string masterSchema(const std::string& a) {
    return "CREATE TABLE IF NOT EXISTS " + a + ".meta(key TEXT PRIMARY KEY, value TEXT);"
           "CREATE TABLE IF NOT EXISTS " + a + ".images(rel TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, file_id INTEGER, sha BLOB, added INTEGER);"
           "CREATE INDEX IF NOT EXISTS " + a + ".images_size ON images(size);"
           "CREATE TABLE IF NOT EXISTS " + a + ".tags(rel TEXT NOT NULL, tag TEXT NOT NULL, kind TEXT, source TEXT, confidence REAL, manual INTEGER);"
           "CREATE INDEX IF NOT EXISTS " + a + ".tags_rel ON tags(rel);"
           "CREATE TABLE IF NOT EXISTS " + a + ".stages(rel TEXT NOT NULL, stage TEXT NOT NULL, version INTEGER, state INTEGER, updated INTEGER,"
           " PRIMARY KEY(rel, stage)) WITHOUT ROWID;"
           "CREATE TABLE IF NOT EXISTS " + a + ".vectors(rel TEXT PRIMARY KEY, model TEXT, dim INTEGER, data BLOB);";
}

std::string dirOf(const std::string& path) {
    const size_t e = path.find_last_of("\\/");
    return e == std::string::npos ? std::string() : path.substr(0, e);
}

// 正本を付ける（外す時に DETACH）。create: 無ければ作る
class AttachMaster {
public:
    AttachMaster(Db& db, const std::string& file, const std::string& alias, bool create) : db_(db), alias_(alias) {
        if (!create && fileSize(file) <= 0) return;
        if (create && !makeDirs(dirOf(file))) throw std::runtime_error("ドライブ記録のフォルダを作れません: " + dirOf(file));
        Stmt st(db.handle(), ("ATTACH DATABASE ? AS " + alias).c_str());
        st.bind(1, file);
        st.step();
        ok_ = true;
        db.exec(("PRAGMA " + alias + ".journal_mode=DELETE").c_str());  // ドライブに -wal を残さない（抜いても 1 つのファイル）
        if (!db.exec(masterSchema(alias).c_str())) {
            db.exec(("DETACH DATABASE " + alias).c_str());
            ok_ = false;
            throw std::runtime_error("ドライブ記録を開けません: " + file);
        }
    }
    ~AttachMaster() {
        if (ok_) db_.exec(("DETACH DATABASE " + alias_).c_str());
    }
    bool ok() const { return ok_; }

private:
    Db& db_;
    std::string alias_;
    bool ok_ = false;
};

std::string metaGet(Db& db, const std::string& a, const std::string& key) {
    Stmt st(db.handle(), ("SELECT value FROM " + a + ".meta WHERE key=?").c_str());
    st.bind(1, key);
    return st.step() ? st.text(0) : std::string();
}

void metaSet(Db& db, const std::string& a, const std::string& key, const std::string& value) {
    Stmt st(db.handle(), ("INSERT OR REPLACE INTO " + a + ".meta(key,value) VALUES(?,?)").c_str());
    st.bind(1, key).bind(2, value);
    st.step();
}

int64_t toInt(const std::string& s) { return s.empty() ? 0 : std::strtoll(s.c_str(), nullptr, 10); }

std::string newDriveId() {
    std::random_device rd;
    std::mt19937_64 g((static_cast<uint64_t>(rd()) << 32) ^ rd());
    char buf[40];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(g()), static_cast<unsigned long long>(g()));
    return buf;
}

// "E:\\" → "E:"（大文字）
std::string letterOf(const std::string& root) {
    if (root.size() < 2 || root[1] != ':') return "";
    return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(root[0])))) + ":";
}

// そのドライブの画像（今の DB）の条件。? にドライブ（"E:"）を入れる
const char* const kOnDrive = "upper(substr(path,1,2)) = ?";

int64_t rowOfDriveId(Db& db, const std::string& driveId) {
    Stmt st(db.handle(), "SELECT id FROM drives WHERE drive_id=?");
    st.bind(1, driveId);
    return st.step() ? st.i64(0) : 0;
}

// 正本の画像を今の DB へ読み戻す（今の DB に同じパスがあれば、そちらを残す）。CLIP のベクトルは、モデルと次元が今のファイルと同じ時だけ
size_t importFromMaster(Db& db, const std::string& a, const std::string& root, const std::string& vectorFile) {
    std::string curModel;
    uint32_t curDim = 0;
    std::unordered_map<int64_t, std::vector<uint16_t>> none;
    const bool haveVectors = !vectorFile.empty() && readVectorsOf(vectorFile, {}, none, curModel, curDim);
    VectorWriter vw;
    bool vwOpen = false;
    size_t n = 0;
    std::vector<int64_t> imported;
    db.begin();
    try {
        Stmt list(db.handle(), ("SELECT rel, size, mtime, file_id, sha, added FROM " + a + ".images").c_str());
        Stmt exists(db.handle(), "SELECT 1 FROM images WHERE path=?");
        Stmt ins(db.handle(), "INSERT INTO images(path,drive,size,mtime,file_id,sha,added) VALUES(?,?,?,?,?,?,?)");
        Stmt tags(db.handle(), ("SELECT tag, kind, source, confidence, manual FROM " + a + ".tags WHERE rel=?").c_str());
        Stmt stages(db.handle(), ("SELECT stage, version, state, updated FROM " + a + ".stages WHERE rel=?").c_str());
        Stmt insStage(db.handle(), "INSERT OR REPLACE INTO stages(image_id,stage,state,version,updated) VALUES(?,?,?,?,?)");
        Stmt vec(db.handle(), ("SELECT model, dim, data FROM " + a + ".vectors WHERE rel=?").c_str());
        TagWriter tw(db, TagConflict::KeepManual);
        while (list.step()) {
            const std::string rel = list.text(0);
            const std::string path = pathOnDrive(root, rel);
            exists.bind(1, path);
            const bool have = exists.step();
            exists.reset();
            if (have) continue;
            ins.bind(1, path).bind(2, letterOf(root)).bind(3, list.i64(1)).bind(4, list.i64(2));
            if (list.isNull(3)) ins.bindNull(5);
            else ins.bind(5, list.i64(3));
            const auto sha = list.blob(4);
            if (sha.empty()) ins.bindNull(6);
            else ins.bindBlob(6, sha.data(), sha.size());
            ins.bind(7, list.i64(5));
            ins.step();
            ins.reset();
            const int64_t id = sqlite3_last_insert_rowid(db.handle());
            tags.bind(1, rel);
            while (tags.step()) tw.put(id, tags.text(0), tags.text(1), tags.f64(3), tags.text(2), tags.i64(4) != 0);
            tags.reset();
            bool vectorBack = false;
            vec.bind(1, rel);
            if (vec.step() && haveVectors && vec.text(0) == curModel && static_cast<uint32_t>(vec.i64(1)) == curDim) {
                const auto blob = vec.blob(2);
                if (blob.size() == 2ull * curDim) {
                    if (!vwOpen) vwOpen = vw.open(vectorFile, curDim, curModel);
                    if (vwOpen) {
                        std::vector<float> v(curDim);
                        for (uint32_t k = 0; k < curDim; ++k) v[k] = halfToFloat(static_cast<uint16_t>(blob[2 * k] | (blob[2 * k + 1] << 8)));
                        vectorBack = vw.append(id, v.data());
                    }
                }
            }
            vec.reset();
            stages.bind(1, rel);
            while (stages.step()) {
                if (stages.text(0) == "clip" && !vectorBack) continue;  // ベクトルが無ければ CLIP は作り直す
                insStage.bind(1, id).bind(2, stages.text(0)).bind(3, stages.i64(2)).bind(4, stages.i64(1)).bind(5, stages.i64(3));
                insStage.step();
                insStage.reset();
            }
            stages.reset();
            imported.push_back(id);
            ++n;
        }
        // 読み戻したものは正本と同じなので、書き戻さない
        Stmt clean(db.handle(), "DELETE FROM dirty_images WHERE image_id=?");
        for (int64_t id : imported) {
            clean.bind(1, id);
            clean.step();
            clean.reset();
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    if (vwOpen) {
        vw.close();
        // 足したベクトルは、正本にもうあるので流し読みでは飛ばす
        const int64_t size = fileSize(vectorFile);
        if (size > 0 && toInt(db.setting("vec_offset")) > 0) db.setSetting("vec_offset", std::to_string(size));
    }
    return n;
}

// 今の DB のそのドライブの画像の ID
std::vector<int64_t> imagesOnDrive(Db& db, const std::string& letter) {
    std::vector<int64_t> ids;
    Stmt st(db.handle(), (std::string("SELECT id FROM images WHERE ") + kOnDrive).c_str());
    st.bind(1, letter);
    while (st.step()) ids.push_back(st.i64(0));
    return ids;
}

}  // namespace

std::string masterFileOf(const std::string& driveRoot) {
    std::string r = driveRoot;
    if (!r.empty() && r.back() != '\\' && r.back() != '/') r += '\\';
    return r + kMasterDirName + "\\drive.db";
}

std::string driveRootOf(const std::string& path) {
    const std::string l = letterOf(path);
    return l.empty() ? "" : l + "\\";
}

std::string relativeOnDrive(const std::string& path) { return letterOf(path).empty() ? path : path.substr(2); }

std::string pathOnDrive(const std::string& driveRoot, const std::string& rel) { return letterOf(driveRoot) + rel; }

DriveSyncResult syncDriveOnConnect(Db& db, const DriveRef& d, const std::string& vectorFile, int64_t now) {
    DriveSyncResult r;
    const std::string file = masterFileOf(d.root);
    const bool existed = fileSize(file) > 0;
    AttachMaster m(db, file, "m", true);
    std::string driveId = metaGet(db, "m", "drive_id");
    const int64_t seq = toInt(metaGet(db, "m", "seq"));
    if (!existed || driveId.empty()) {
        // 正本を作る（今の DB のこのドライブの分を、次の書き戻しで全部書く）
        driveId = newDriveId();
        metaSet(db, "m", "drive_id", driveId);
        metaSet(db, "m", "seq", "0");
        metaSet(db, "m", "updated", std::to_string(now));
        Stmt ins(db.handle(), "INSERT INTO drives(drive_id,serial,root,label,last_seen,master_seq) VALUES(?,?,?,?,?,0)");
        ins.bind(1, driveId).bind(2, static_cast<int64_t>(d.serial)).bind(3, d.root).bind(4, d.label).bind(5, now);
        ins.step();
        r.driveRow = sqlite3_last_insert_rowid(db.handle());
        Stmt dirty(db.handle(), (std::string("INSERT OR IGNORE INTO dirty_images(image_id) SELECT id FROM images WHERE ") + kOnDrive).c_str());
        dirty.bind(1, letterOf(d.root));
        dirty.step();
        r.kind = DriveSync::Created;
        logs::info("ドライブ記録を作った: " + file);
        return r;
    }
    r.driveRow = rowOfDriveId(db, driveId);
    if (!r.driveRow) {
        // この PC では初めての正本（別の PC で作った・今の DB を作り直した）
        const bool hasClone = !imagesOnDrive(db, letterOf(d.root)).empty();
        Stmt ins(db.handle(), "INSERT INTO drives(drive_id,serial,root,label,last_seen,master_seq,conflict) VALUES(?,?,?,?,?,?,?)");
        ins.bind(1, driveId).bind(2, static_cast<int64_t>(d.serial)).bind(3, d.root).bind(4, d.label).bind(5, now).bind(6, seq);
        ins.bind(7, static_cast<int64_t>(hasClone ? 1 : 0));
        ins.step();
        r.driveRow = sqlite3_last_insert_rowid(db.handle());
        if (hasClone) {
            r.kind = DriveSync::Conflict;
            return r;
        }
        r.imported = importFromMaster(db, "m", d.root, vectorFile);
        r.kind = DriveSync::Imported;
        return r;
    }
    {
        Stmt up(db.handle(), "UPDATE drives SET serial=?, root=?, label=?, last_seen=?, gone_since=0 WHERE id=?");
        up.bind(1, static_cast<int64_t>(d.serial)).bind(2, d.root).bind(3, d.label).bind(4, now).bind(5, r.driveRow);
        up.step();
    }
    Stmt st(db.handle(), "SELECT clone_deleted, conflict, master_seq FROM drives WHERE id=?");
    st.bind(1, r.driveRow);
    st.step();
    const bool deleted = st.i64(0) != 0, conflict = st.i64(1) != 0;
    const int64_t mySeq = st.i64(2);
    if (deleted) {
        r.imported = importFromMaster(db, "m", d.root, vectorFile);
        Stmt up(db.handle(), "UPDATE drives SET clone_deleted=0, master_seq=? WHERE id=?");
        up.bind(1, seq).bind(2, r.driveRow);
        up.step();
        r.kind = DriveSync::Imported;
        logs::info("ドライブ記録から取り込んだ: " + file + "（画像 " + std::to_string(r.imported) + " 枚）");
        return r;
    }
    if (conflict || seq != mySeq) {
        Stmt up(db.handle(), "UPDATE drives SET conflict=1 WHERE id=?");
        up.bind(1, r.driveRow);
        up.step();
        r.kind = DriveSync::Conflict;
        return r;
    }
    r.kind = DriveSync::Same;
    return r;
}

void markDrivesSeen(Db& db, const std::vector<int64_t>& seenRows, int64_t now) {
    std::vector<int64_t> all;
    {
        Stmt st(db.handle(), "SELECT id FROM drives");
        while (st.step()) all.push_back(st.i64(0));
    }
    Stmt gone(db.handle(), "UPDATE drives SET gone_since=? WHERE id=? AND gone_since=0");
    Stmt seen(db.handle(), "UPDATE drives SET gone_since=0, last_seen=? WHERE id=?");
    for (int64_t id : all) {
        if (std::find(seenRows.begin(), seenRows.end(), id) != seenRows.end()) {
            seen.bind(1, now).bind(2, id);
            seen.step();
            seen.reset();
        } else {
            gone.bind(1, now).bind(2, id);
            gone.step();
            gone.reset();
        }
    }
}

std::vector<WritebackResult> writebackDrives(Db& db, const std::vector<DriveRef>& drives, const std::string& vectorFile, int64_t now,
                                             const std::atomic<bool>* cancel) {
    std::vector<WritebackResult> out(drives.size());
    struct Live {
        size_t index;
        std::string alias, letter;
        int64_t row, seq;
        std::unique_ptr<AttachMaster> attach;
    };
    std::vector<Live> live;
    std::map<std::string, size_t> byLetter;  // ドライブ → live の番号
    for (size_t i = 0; i < drives.size(); ++i) {
        out[i].root = drives[i].root;
        try {
            const std::string file = masterFileOf(drives[i].root);
            if (fileSize(file) <= 0) throw std::runtime_error("ドライブ記録がありません: " + file);
            Live l{i, "w" + std::to_string(i), letterOf(drives[i].root), 0, 0, nullptr};
            l.attach = std::make_unique<AttachMaster>(db, file, l.alias, false);
            const std::string driveId = metaGet(db, l.alias, "drive_id");
            l.seq = toInt(metaGet(db, l.alias, "seq"));
            Stmt st(db.handle(), "SELECT id, master_seq, conflict FROM drives WHERE drive_id=?");
            st.bind(1, driveId);
            if (!st.step() || st.i64(2) != 0 || st.i64(1) != l.seq) {
                out[i].conflict = true;  // 別の所で書き換わった（つないだ時の確認の画面で選んでもらう）
                continue;
            }
            l.row = st.i64(0);
            byLetter[l.letter] = live.size();
            live.push_back(std::move(l));
        } catch (const std::exception& e) {
            out[i].failed = true;
            out[i].error = e.what();
        }
    }
    if (live.empty()) return out;

    // 1. 画像の行・守るタグ・段階（まとめて少しずつ）
    for (Live& l : live) {
        WritebackResult& res = out[l.index];
        const std::string a = l.alias;
        try {
            for (;;) {
                if (cancel && cancel->load()) break;
                std::vector<std::pair<int64_t, std::string>> chunk;
                {
                    Stmt st(db.handle(), "SELECT d.image_id, i.path FROM dirty_images d JOIN images i ON i.id=d.image_id "
                                         "WHERE upper(substr(i.path,1,2)) = ? LIMIT 5000");
                    st.bind(1, l.letter);
                    while (st.step()) chunk.emplace_back(st.i64(0), st.text(1));
                }
                if (chunk.empty()) break;
                db.begin();
                try {
                    Stmt img(db.handle(), ("INSERT OR REPLACE INTO " + a + ".images(rel,size,mtime,file_id,sha,added) "
                                           "SELECT ?,size,mtime,file_id,sha,added FROM images WHERE id=?").c_str());
                    Stmt delTags(db.handle(), ("DELETE FROM " + a + ".tags WHERE rel=?").c_str());
                    Stmt tags(db.handle(), ("INSERT INTO " + a + ".tags(rel,tag,kind,source,confidence,manual) "
                                            "SELECT ?,tag,kind,source,confidence,manual FROM tags WHERE image_id=? AND (manual=1 OR source='tagger')").c_str());
                    Stmt delStages(db.handle(), ("DELETE FROM " + a + ".stages WHERE rel=?").c_str());
                    Stmt stages(db.handle(), ("INSERT INTO " + a + ".stages(rel,stage,version,state,updated) "
                                              "SELECT ?,stage,version,state,updated FROM stages WHERE image_id=? AND stage IN ('tagger','clip')").c_str());
                    // CLIP が済んでいるのに正本にベクトルが無い（初めて・動かした）: ベクトルのファイルから拾う
                    Stmt needVec(db.handle(), ("INSERT OR IGNORE INTO dirty_vectors(image_id) SELECT ? WHERE EXISTS (SELECT 1 FROM stages "
                                               "WHERE image_id=? AND stage='clip' AND state=1) AND NOT EXISTS (SELECT 1 FROM " + a + ".vectors WHERE rel=?)").c_str());
                    Stmt done(db.handle(), "DELETE FROM dirty_images WHERE image_id=?");
                    for (const auto& [id, path] : chunk) {
                        const std::string rel = relativeOnDrive(path);
                        img.bind(1, rel).bind(2, id);
                        img.step();
                        img.reset();
                        delTags.bind(1, rel);
                        delTags.step();
                        delTags.reset();
                        tags.bind(1, rel).bind(2, id);
                        tags.step();
                        tags.reset();
                        delStages.bind(1, rel);
                        delStages.step();
                        delStages.reset();
                        stages.bind(1, rel).bind(2, id);
                        stages.step();
                        stages.reset();
                        needVec.bind(1, id).bind(2, id).bind(3, rel);
                        needVec.step();
                        needVec.reset();
                        done.bind(1, id);
                        done.step();
                        done.reset();
                    }
                    db.commit();
                } catch (...) {
                    db.rollback();
                    throw;
                }
                res.images += chunk.size();
            }
            // 消えた・動かしたパス（正本からも消す。保管は今の DB の側の保管にある）
            std::vector<std::string> removed;
            {
                Stmt st(db.handle(), "SELECT path FROM dirty_removed WHERE upper(substr(path,1,2)) = ?");
                st.bind(1, l.letter);
                while (st.step()) removed.push_back(st.text(0));
            }
            if (!removed.empty()) {
                db.begin();
                try {
                    Stmt live2(db.handle(), "SELECT 1 FROM images WHERE path=?");
                    Stmt done(db.handle(), "DELETE FROM dirty_removed WHERE path=?");
                    for (const auto& p : removed) {
                        live2.bind(1, p);
                        const bool still = live2.step();  // 消した後に同じパスでまた入った
                        live2.reset();
                        if (!still) {
                            const std::string rel = relativeOnDrive(p);
                            for (const char* t : {"images", "tags", "stages", "vectors"}) {
                                Stmt del(db.handle(), ("DELETE FROM " + a + "." + t + " WHERE rel=?").c_str());
                                del.bind(1, rel);
                                del.step();
                            }
                            ++res.removed;
                        }
                        done.bind(1, p);
                        done.step();
                        done.reset();
                    }
                    db.commit();
                } catch (...) {
                    db.rollback();
                    throw;
                }
            }
        } catch (const std::exception& e) {
            res.failed = true;
            res.error = e.what();
        }
    }

    // 2. CLIP のベクトル: ファイルの後ろに足された分と、拾い直す分（dirty_vectors）を 1 回で流し読む
    if (!vectorFile.empty() && !(cancel && cancel->load())) {
        try {
            MappedFile mf;
            if (mf.open(vectorFile) && mf.size() >= 12 && std::memcmp(mf.data(), "IVEC", 4) == 0) {
                const uint8_t* p = mf.data();
                uint32_t dim, len;
                std::memcpy(&dim, p + 4, 4);
                std::memcpy(&len, p + 8, 4);
                const std::string model(reinterpret_cast<const char*>(p + 12), len);
                const uint64_t header = 12ull + len, rec = 8 + 2ull * dim;
                // ファイルを作り直した（モデルが変わった）・小さくなった（詰めた）なら、頭から
                uint64_t from = static_cast<uint64_t>(toInt(db.setting("vec_offset")));
                if (db.setting("vec_model") != model || from > mf.size() || from < header) from = header;
                bool anyDirty = false;
                {
                    Stmt st(db.handle(), "SELECT 1 FROM dirty_vectors LIMIT 1");
                    anyDirty = st.step();
                }
                const uint64_t start = anyDirty ? header : from;
                const uint64_t end = header + (mf.size() - header) / rec * rec;
                db.begin();
                try {
                    Stmt path(db.handle(), "SELECT path FROM images WHERE id=?");
                    Stmt dirty(db.handle(), "SELECT 1 FROM dirty_vectors WHERE image_id=?");
                    Stmt undirty(db.handle(), "DELETE FROM dirty_vectors WHERE image_id=?");
                    Stmt later(db.handle(), "INSERT OR IGNORE INTO dirty_vectors(image_id) VALUES(?)");
                    std::map<size_t, std::unique_ptr<Stmt>> put;
                    size_t inTxn = 0;
                    for (uint64_t off = start; off + rec <= end; off += rec) {
                        if (cancel && cancel->load()) break;
                        int64_t id;
                        std::memcpy(&id, p + off, 8);
                        const bool fresh = off >= from;
                        if (!fresh) {
                            dirty.bind(1, id);
                            const bool want = dirty.step();
                            dirty.reset();
                            if (!want) continue;
                        }
                        path.bind(1, id);
                        std::string ip;
                        if (path.step()) ip = path.text(0);
                        path.reset();
                        if (ip.empty()) continue;
                        auto it = byLetter.find(letterOf(ip));
                        if (it == byLetter.end()) {
                            // つながっていない・食い違っているドライブ: そのドライブを書き戻す時に拾い直す（正本の無いドライブは要らない）
                            Stmt known(db.handle(), "SELECT 1 FROM drives WHERE upper(substr(root,1,2)) = ?");
                            known.bind(1, letterOf(ip));
                            if (known.step()) {
                                later.bind(1, id);
                                later.step();
                                later.reset();
                            }
                            continue;
                        }
                        Live& l = live[it->second];
                        auto& s = put[it->second];
                        if (!s) s = std::make_unique<Stmt>(db.handle(), ("INSERT OR REPLACE INTO " + l.alias + ".vectors(rel,model,dim,data) VALUES(?,?,?,?)").c_str());
                        s->bind(1, relativeOnDrive(ip)).bind(2, model).bind(3, static_cast<int64_t>(dim)).bindBlob(4, p + off + 8, 2ull * dim);
                        s->step();
                        s->reset();
                        undirty.bind(1, id);
                        undirty.step();
                        undirty.reset();
                        ++out[l.index].vectors;
                        if (++inTxn >= 20000) {  // 大きなトランザクションにしない
                            put.clear();
                            db.commit();
                            db.begin();
                            inTxn = 0;
                        }
                    }
                    put.clear();
                    db.commit();
                } catch (...) {
                    db.rollback();
                    throw;
                }
                if (!(cancel && cancel->load())) {
                    db.setSetting("vec_offset", std::to_string(end));
                    db.setSetting("vec_model", model);
                }
            }
        } catch (const std::exception& e) {
            for (Live& l : live) {
                out[l.index].failed = true;
                out[l.index].error = std::string("CLIP のベクトルを書けなかった: ") + e.what();
            }
        }
    }

    // 3. 書いた正本の番号を進める（別の所で書き換わったかを見分ける）
    for (Live& l : live) {
        WritebackResult& res = out[l.index];
        if (res.failed) {
            Stmt st(db.handle(), "UPDATE drives SET fail_since=? WHERE id=? AND fail_since=0");
            st.bind(1, now).bind(2, l.row);
            st.step();
            continue;
        }
        try {
            db.begin();
            const int64_t seq = l.seq + ((res.images || res.removed || res.vectors) ? 1 : 0);
            metaSet(db, l.alias, "seq", std::to_string(seq));
            metaSet(db, l.alias, "updated", std::to_string(now));
            Stmt st(db.handle(), "UPDATE drives SET master_seq=?, last_writeback=?, fail_since=0 WHERE id=?");
            st.bind(1, seq).bind(2, now).bind(3, l.row);
            st.step();
            db.commit();
        } catch (const std::exception& e) {
            db.rollback();
            res.failed = true;
            res.error = e.what();
        }
    }
    return out;
}

void markWritebackFailed(Db& db, const std::string& driveRoot, int64_t now) {
    Stmt st(db.handle(), "UPDATE drives SET fail_since=? WHERE upper(substr(root,1,2)) = ? AND gone_since=0 AND fail_since=0");
    st.bind(1, now).bind(2, letterOf(driveRoot));
    st.step();
}

namespace {
// 守る段階（タガー・CLIP）の済み具合から、作成の進み具合（処理の時間の重み）と、作り終えた枚数
constexpr double kTaggerWeight = 0.159, kClipWeight = 0.0067;
void fillSide(Stmt& st, DriveSide& s) {
    // 列: images, tagger の済み, clip の済み, 両方済み, タガーの版（一番多いもの）
    if (!st.step()) return;
    s.images = st.i64(0);
    const double t = static_cast<double>(st.i64(1)), c = static_cast<double>(st.i64(2));
    s.complete = st.i64(3);
    s.taggerVersion = static_cast<int>(st.i64(4));
    s.progress = s.images ? (kTaggerWeight * t + kClipWeight * c) / ((kTaggerWeight + kClipWeight) * static_cast<double>(s.images)) : 0;
}
}  // namespace

DriveSide masterSide(Db& db, const DriveRef& d) {
    DriveSide s;
    const std::string file = masterFileOf(d.root);
    if (fileSize(file) <= 0) return s;
    AttachMaster m(db, file, "c", false);
    s.exists = true;
    Stmt st(db.handle(),
            "SELECT (SELECT count(*) FROM c.images),"
            " (SELECT count(*) FROM c.stages WHERE stage='tagger' AND state<>0),"
            " (SELECT count(*) FROM c.stages WHERE stage='clip' AND state<>0),"
            " (SELECT count(*) FROM c.stages t JOIN c.stages k ON k.rel=t.rel AND k.stage='clip' AND k.state<>0 WHERE t.stage='tagger' AND t.state<>0),"
            " coalesce((SELECT version FROM c.stages WHERE stage='tagger' GROUP BY version ORDER BY count(*) DESC LIMIT 1), 0)");
    fillSide(st, s);
    s.updated = toInt(metaGet(db, "c", "updated"));
    return s;
}

DriveSide cloneSide(Db& db, const std::string& driveRoot) {
    DriveSide s;
    const std::string l = letterOf(driveRoot);
    Stmt st(db.handle(),
            "WITH d AS (SELECT id FROM images WHERE upper(substr(path,1,2)) = ?1) "
            "SELECT (SELECT count(*) FROM d),"
            " (SELECT count(*) FROM stages s JOIN d ON d.id=s.image_id WHERE s.stage='tagger' AND s.state<>0),"
            " (SELECT count(*) FROM stages s JOIN d ON d.id=s.image_id WHERE s.stage='clip' AND s.state<>0),"
            " (SELECT count(*) FROM stages t JOIN d ON d.id=t.image_id JOIN stages k ON k.image_id=t.image_id AND k.stage='clip' AND k.state<>0"
            "  WHERE t.stage='tagger' AND t.state<>0),"
            " coalesce((SELECT s.version FROM stages s JOIN d ON d.id=s.image_id WHERE s.stage='tagger' GROUP BY s.version ORDER BY count(*) DESC LIMIT 1), 0)");
    st.bind(1, l);
    fillSide(st, s);
    s.exists = s.images > 0;
    Stmt u(db.handle(), "SELECT coalesce(max(s.updated),0) FROM stages s JOIN images i ON i.id=s.image_id WHERE upper(substr(i.path,1,2)) = ?");
    u.bind(1, l);
    if (u.step()) s.updated = u.i64(0);
    return s;
}

void resolveConflict(Db& db, const DriveRef& d, bool useMaster, const std::string& archiveFile, const std::string& vectorFile,
                     int64_t now) {
    const std::string file = masterFileOf(d.root);
    const std::string letter = letterOf(d.root);
    if (useMaster) {
        // クローンを保管へ（選ばなかった方は消さない）→ 正本から読み戻す
        const std::vector<int64_t> ids = imagesOnDrive(db, letter);
        archiveImages(db, archiveFile, ids, vectorFile, "食い違いで選ばなかった（この PC の側）");
        {
            Stmt st(db.handle(), "DELETE FROM dirty_removed WHERE upper(substr(path,1,2)) = ?");  // 正本からは消さない
            st.bind(1, letter);
            st.step();
        }
        AttachMaster m(db, file, "m", false);
        if (!m.ok()) throw std::runtime_error("ドライブ記録がありません: " + file);
        importFromMaster(db, "m", d.root, vectorFile);
        const std::string driveId = metaGet(db, "m", "drive_id");
        Stmt st(db.handle(), "UPDATE drives SET master_seq=?, conflict=0, clone_deleted=0, last_seen=?, gone_since=0 WHERE drive_id=?");
        st.bind(1, toInt(metaGet(db, "m", "seq"))).bind(2, now).bind(3, driveId);
        st.step();
        logs::info("食い違いを解いた（ドライブ記録を使う）: " + file);
        return;
    }
    // 正本を保管のフォルダに写して残し、クローンで作り直す
    {
        const std::string adir = dirOf(archiveFile);
        const char sep = adir.find('\\') != std::string::npos ? '\\' : '/';
        const std::string dst = adir + sep + "drive-" + letter.substr(0, 1) + "-" + std::to_string(now) + ".db";
        makeDirs(dirOf(archiveFile));
        FILE* in = openFile(file, "rb");
        FILE* outF = openFile(dst, "wb");
        bool ok = in && outF;
        char buf[1 << 16];
        while (ok) {
            const size_t n = std::fread(buf, 1, sizeof buf, in);
            if (!n) break;
            ok = std::fwrite(buf, 1, n, outF) == n;
        }
        if (in) std::fclose(in);
        if (outF) std::fclose(outF);
        if (!ok) throw std::runtime_error("ドライブ記録を退避のフォルダに写せません: " + dst);
        logs::info("食い違いで選ばなかったドライブ記録を写して残した: " + dst);
    }
    AttachMaster m(db, file, "m", false);
    if (!m.ok()) throw std::runtime_error("ドライブ記録がありません: " + file);
    db.begin();
    try {
        for (const char* t : {"images", "tags", "stages", "vectors"}) db.exec(("DELETE FROM m." + std::string(t)).c_str());
        const int64_t seq = toInt(metaGet(db, "m", "seq")) + 1;
        metaSet(db, "m", "seq", std::to_string(seq));
        metaSet(db, "m", "updated", std::to_string(now));
        Stmt dirty(db.handle(), (std::string("INSERT OR IGNORE INTO dirty_images(image_id) SELECT id FROM images WHERE ") + kOnDrive).c_str());
        dirty.bind(1, letter);
        dirty.step();
        const std::string driveId = metaGet(db, "m", "drive_id");
        Stmt st(db.handle(), "UPDATE drives SET master_seq=?, conflict=0, clone_deleted=0, last_seen=?, gone_since=0 WHERE drive_id=?");
        st.bind(1, seq).bind(2, now).bind(3, driveId);
        st.step();
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    db.setSetting("vec_offset", "0");  // ベクトルも全部書き直す
    logs::info("食い違いを解いた（この PC の側を使う）: " + file);
}

std::vector<DriveRow> driveRows(Db& db) {
    std::vector<DriveRow> out;
    Stmt st(db.handle(),
            "SELECT id, drive_id, root, label, serial, last_seen, gone_since, last_writeback, fail_since, snooze_until, clone_deleted, conflict "
            "FROM drives ORDER BY root");
    while (st.step()) {
        DriveRow r;
        r.id = st.i64(0);
        r.driveId = st.text(1);
        r.root = st.text(2);
        r.label = st.text(3);
        r.serial = static_cast<uint32_t>(st.i64(4));
        r.lastSeen = st.i64(5);
        r.goneSince = st.i64(6);
        r.lastWriteback = st.i64(7);
        r.failSince = st.i64(8);
        r.snoozeUntil = st.i64(9);
        r.cloneDeleted = st.i64(10) != 0;
        r.conflict = st.i64(11) != 0;
        out.push_back(std::move(r));
    }
    for (DriveRow& r : out) {
        const std::string l = letterOf(r.root);
        Stmt c(db.handle(), (std::string("SELECT count(*) FROM images WHERE ") + kOnDrive).c_str());
        c.bind(1, l);
        if (c.step()) r.cloneImages = c.i64(0);
        Stmt u(db.handle(), "SELECT count(*) FROM dirty_images d JOIN images i ON i.id=d.image_id WHERE upper(substr(i.path,1,2)) = ?");
        u.bind(1, l);
        if (u.step()) r.unwritten = u.i64(0);
    }
    return out;
}

size_t deleteClone(Db& db, int64_t driveRow, const std::string& archiveFile, const std::string& vectorFile) {
    std::string root;
    {
        Stmt st(db.handle(), "SELECT root FROM drives WHERE id=?");
        st.bind(1, driveRow);
        if (!st.step()) return 0;
        root = st.text(0);
    }
    const std::string letter = letterOf(root);
    // 書き戻していない分は保管へ（書き戻し失敗分）
    std::vector<int64_t> unwritten;
    {
        Stmt st(db.handle(), "SELECT d.image_id FROM dirty_images d JOIN images i ON i.id=d.image_id WHERE upper(substr(i.path,1,2)) = ?");
        st.bind(1, letter);
        while (st.step()) unwritten.push_back(st.i64(0));
    }
    const size_t archived = archiveImages(db, archiveFile, unwritten, vectorFile, "ドライブへ記録できなかった分");
    if (archived) logs::info("PC 記録を消す前に、ドライブへ記録していない画像 " + std::to_string(archived) + " 枚を退避した");
    size_t n = 0;
    db.begin();
    try {
        Stmt del(db.handle(), (std::string("DELETE FROM images WHERE ") + kOnDrive).c_str());
        del.bind(1, letter);
        del.step();
        n = static_cast<size_t>(db.changes());
        // 正本からは消さない（クローンを消しただけ）
        Stmt clean(db.handle(), "DELETE FROM dirty_removed WHERE upper(substr(path,1,2)) = ?");
        clean.bind(1, letter);
        clean.step();
        Stmt up(db.handle(), "UPDATE drives SET clone_deleted=1 WHERE id=?");
        up.bind(1, driveRow);
        up.step();
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    logs::info("ドライブ " + root + " の PC 記録を消した（画像 " + std::to_string(n) + " 枚。ドライブ記録はドライブにある）");
    return n + archived;
}

std::string driveArchiveFileOf(const std::string& driveRoot) {
    std::string r = driveRoot;
    if (!r.empty() && r.back() != '\\' && r.back() != '/') r += '\\';
    return r + kMasterDirName + "\\archive.db";
}

std::string archiveFileForPath(Db& db, const std::string& path, const std::string& fallback) {
    const std::string l = letterOf(path);
    if (l.empty()) return fallback;
    Stmt st(db.handle(), "SELECT root FROM drives WHERE upper(substr(root,1,2)) = ? AND gone_since=0 AND clone_deleted=0 AND conflict=0");
    st.bind(1, l);
    if (!st.step()) return fallback;
    const std::string root = st.text(0);
    return fileSize(masterFileOf(root)) > 0 ? driveArchiveFileOf(root) : fallback;
}

std::vector<std::string> archiveFilesInUse(Db& db, const std::string& fallback) {
    std::vector<std::string> out;
    if (fileSize(fallback) > 0) out.push_back(fallback);
    Stmt st(db.handle(), "SELECT root FROM drives WHERE gone_since=0 ORDER BY root");
    while (st.step()) {
        const std::string f = driveArchiveFileOf(st.text(0));
        if (fileSize(f) > 0 && std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
    }
    return out;
}

void snoozeDrive(Db& db, int64_t driveRow, int64_t until) {
    Stmt st(db.handle(), "UPDATE drives SET snooze_until=? WHERE id=?");
    st.bind(1, until).bind(2, driveRow);
    st.step();
}

}  // namespace imgdupe
