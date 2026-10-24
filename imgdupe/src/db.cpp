#include "db.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "sqlite3.h"

namespace imgdupe {

namespace {

// 版 1: 仕様「データ層」のテーブル。子のテーブルは画像を消すとまとめて消える（ON DELETE CASCADE）
const char* const kSchemaV1 = R"SQL(
CREATE TABLE IF NOT EXISTS settings(
    key TEXT PRIMARY KEY,
    value TEXT
) WITHOUT ROWID;

-- 画像の基本情報。見た目の特徴量（横と縦の dHash・平均の色・明るさのばらつき）もここに持つ
CREATE TABLE IF NOT EXISTS images(
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL UNIQUE,
    drive TEXT,
    size INTEGER,
    mtime INTEGER,
    file_id INTEGER,
    dh INTEGER,
    dv INTEGER,
    color INTEGER,
    contrast INTEGER,
    width INTEGER,
    height INTEGER,
    readable INTEGER,
    sha BLOB,
    added INTEGER
);
CREATE INDEX IF NOT EXISTS images_size ON images(size);

-- 段階ごとの処理状態（深化の管理）。version は処理した解析器の版
CREATE TABLE IF NOT EXISTS stages(
    image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
    stage TEXT NOT NULL,
    state INTEGER NOT NULL,
    version INTEGER NOT NULL,
    updated INTEGER,
    PRIMARY KEY(image_id, stage)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS stages_stage ON stages(stage, version, state);

-- 確度付きタグ。kind: artist / character / copyright / general / action / item / rating など
CREATE TABLE IF NOT EXISTS tags(
    image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
    tag TEXT NOT NULL,
    kind TEXT NOT NULL,
    confidence REAL NOT NULL,
    source TEXT NOT NULL,
    manual INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(image_id, kind, tag, source)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS tags_search ON tags(kind, tag, confidence);
CREATE INDEX IF NOT EXISTS tags_image ON tags(image_id, kind, confidence);

-- 別名と親子関係
CREATE TABLE IF NOT EXISTS tag_dict(
    tag TEXT PRIMARY KEY,
    alias_of TEXT,
    parent TEXT,
    kind TEXT
) WITHOUT ROWID;

-- 類似検索用ベクトル（CLIP など大きいものは別のファイルに置き、ここには小さいものだけ）
CREATE TABLE IF NOT EXISTS embeddings(
    image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
    kind TEXT NOT NULL,
    vector BLOB,
    model_version TEXT,
    PRIMARY KEY(image_id, kind)
) WITHOUT ROWID;

-- 画像のまとまり。kind: exact（完全一致）/ similar（見た目一致）/ variant（差分）/ series（シリーズ）
CREATE TABLE IF NOT EXISTS groups(
    id INTEGER PRIMARY KEY,
    kind TEXT NOT NULL,
    created INTEGER
);
CREATE TABLE IF NOT EXISTS group_members(
    group_id INTEGER NOT NULL REFERENCES groups(id) ON DELETE CASCADE,
    image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
    representative INTEGER NOT NULL DEFAULT 0,
    distance INTEGER,
    PRIMARY KEY(group_id, image_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS group_members_image ON group_members(image_id);

-- 出典
CREATE TABLE IF NOT EXISTS sources(
    image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
    site TEXT NOT NULL,
    post_id TEXT,
    author_id TEXT,
    url TEXT,
    checked INTEGER,
    PRIMARY KEY(image_id, site, post_id)
) WITHOUT ROWID;

-- ハードリンクの管理（temporary: 1 = 一時、0 = 固定）
CREATE TABLE IF NOT EXISTS links(
    path TEXT PRIMARY KEY,
    image_id INTEGER REFERENCES images(id) ON DELETE SET NULL,
    temporary INTEGER NOT NULL,
    created INTEGER
);

-- 取り消し用の履歴
CREATE TABLE IF NOT EXISTS history(
    id INTEGER PRIMARY KEY,
    op TEXT NOT NULL,
    target TEXT,
    before TEXT,
    after TEXT,
    at INTEGER
);

-- GUI から indexer への指示
CREATE TABLE IF NOT EXISTS commands(
    id INTEGER PRIMARY KEY,
    command TEXT NOT NULL,
    target TEXT,
    state TEXT NOT NULL DEFAULT 'pending',
    created INTEGER
);

-- ファイルのメタデータ（検索とソートの基準）
CREATE TABLE IF NOT EXISTS file_meta(
    image_id INTEGER PRIMARY KEY REFERENCES images(id) ON DELETE CASCADE,
    format TEXT,
    bit_depth INTEGER,
    animated INTEGER,
    taken INTEGER,
    camera TEXT,
    lens TEXT,
    focal REAL,
    iso INTEGER,
    has_gps INTEGER,
    software TEXT,
    keywords TEXT,
    rating INTEGER,
    title TEXT,
    subject TEXT,
    comment TEXT,
    author TEXT,
    png_text TEXT,
    created INTEGER,
    accessed INTEGER,
    attributes INTEGER,
    hardlinks INTEGER,
    zone_url TEXT,
    zone_referrer TEXT
);
CREATE INDEX IF NOT EXISTS file_meta_taken ON file_meta(taken);
CREATE INDEX IF NOT EXISTS file_meta_rating ON file_meta(rating);
)SQL";

// 版 2: サムネイルのパックファイルの索引
const char* const kSchemaV2 = R"SQL(
CREATE TABLE IF NOT EXISTS thumbs(
    image_id INTEGER PRIMARY KEY REFERENCES images(id) ON DELETE CASCADE,
    pack INTEGER NOT NULL,
    offset INTEGER NOT NULL,
    length INTEGER NOT NULL,
    format INTEGER NOT NULL,
    width INTEGER,
    height INTEGER
);
)SQL";

// 版 3: タイトル・件名・コメント・キーワード・PNG のテキストの全文検索（trigram なので日本語の部分一致もできる）
const char* const kSchemaV3 = R"SQL(
CREATE VIRTUAL TABLE IF NOT EXISTS meta_fts USING fts5(title, subject, comment, keywords, png_text, tokenize='trigram');
CREATE TRIGGER IF NOT EXISTS images_delete_fts AFTER DELETE ON images BEGIN
    DELETE FROM meta_fts WHERE rowid=old.id;
END;
)SQL";

// 版 4: 検索と並べ替えのための索引（タグは大文字小文字を区別せずに引く）
const char* const kSchemaV4 = R"SQL(
CREATE INDEX IF NOT EXISTS tags_tag ON tags(tag COLLATE NOCASE, image_id);
CREATE INDEX IF NOT EXISTS images_mtime ON images(mtime);
CREATE INDEX IF NOT EXISTS file_meta_created ON file_meta(created);
CREATE INDEX IF NOT EXISTS tag_dict_alias ON tag_dict(alias_of COLLATE NOCASE);
)SQL";

// 版 5: スマートフォルダ（検索条件の保存。query は SearchQuery の JSON）
const char* const kSchemaV5 = R"SQL(
CREATE TABLE IF NOT EXISTS smart_folders(
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE COLLATE NOCASE,
    query TEXT NOT NULL,
    created INTEGER,
    updated INTEGER
);
)SQL";


bool underRoot(const std::string& path, const std::string& root) {
    if (path.size() <= root.size() || path.compare(0, root.size(), root) != 0) return false;
    const char last = root.empty() ? '/' : root.back();
    return last == '/' || last == '\\' || path[root.size()] == '/' || path[root.size()] == '\\';
}

int64_t packColor(const Features& f) { return (int64_t(f.r) << 16) | (int64_t(f.g) << 8) | f.b; }

}  // namespace

std::string driveOf(const std::string& path) {
    if (path.size() >= 2 && path[1] == ':') return path.substr(0, 2);
    if (path.size() >= 2 && (path[0] == '\\' || path[0] == '/') && path[0] == path[1]) {
        // \\server\share
        size_t a = path.find_first_of("\\/", 2);
        if (a == std::string::npos) return path;
        size_t b = path.find_first_of("\\/", a + 1);
        return path.substr(0, b == std::string::npos ? path.size() : b);
    }
    return "/";
}

int64_t nowUnix() { return static_cast<int64_t>(std::time(nullptr)); }

// ---------------------------------------------------------------- Stmt

Stmt::Stmt(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("SQL の準備に失敗: ") + sqlite3_errmsg(db) + " : " + sql);
}

Stmt::~Stmt() { sqlite3_finalize(st_); }

Stmt& Stmt::bind(int i, int64_t v) {
    sqlite3_bind_int64(st_, i, v);
    return *this;
}
Stmt& Stmt::bind(int i, double v) {
    sqlite3_bind_double(st_, i, v);
    return *this;
}
Stmt& Stmt::bind(int i, const std::string& v) {
    sqlite3_bind_text(st_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    return *this;
}
Stmt& Stmt::bindBlob(int i, const void* data, size_t len) {
    sqlite3_bind_blob(st_, i, data, static_cast<int>(len), SQLITE_TRANSIENT);
    return *this;
}
Stmt& Stmt::bindNull(int i) {
    sqlite3_bind_null(st_, i);
    return *this;
}

bool Stmt::step() {
    const int rc = sqlite3_step(st_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw std::runtime_error(std::string("SQL の実行に失敗: ") + sqlite3_errmsg(db_));
}

void Stmt::reset() {
    sqlite3_reset(st_);
    sqlite3_clear_bindings(st_);
}

int64_t Stmt::i64(int col) const { return sqlite3_column_int64(st_, col); }
double Stmt::f64(int col) const { return sqlite3_column_double(st_, col); }
std::string Stmt::text(int col) const {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st_, col));
    return p ? std::string(p, static_cast<size_t>(sqlite3_column_bytes(st_, col))) : std::string();
}
std::vector<uint8_t> Stmt::blob(int col) const {
    const auto* p = static_cast<const uint8_t*>(sqlite3_column_blob(st_, col));
    return p ? std::vector<uint8_t>(p, p + sqlite3_column_bytes(st_, col)) : std::vector<uint8_t>();
}
bool Stmt::isNull(int col) const { return sqlite3_column_type(st_, col) == SQLITE_NULL; }

// ---------------------------------------------------------------- Db

Db::~Db() { close(); }

bool Db::open(const std::string& path) {
    close();
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) !=
        SQLITE_OK) {
        error_ = db_ ? sqlite3_errmsg(db_) : "開けません";
        close();
        return false;
    }
    sqlite3_busy_timeout(db_, 10000);  // indexer と同時に書く時は少し待つ
    // WAL: GUI が読んでいる間も indexer が書ける。NORMAL は WAL なら電源断でも壊れない
    if (!exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON; PRAGMA temp_store=MEMORY;") ||
        !migrate()) {
        close();
        return false;
    }
    return true;
}

void Db::close() {
    if (db_) sqlite3_close_v2(db_);
    db_ = nullptr;
}

bool Db::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        error_ = err ? err : "SQL の実行に失敗";
        sqlite3_free(err);
        return false;
    }
    return true;
}

int Db::schemaVersion() {
    Stmt st(db_, "PRAGMA user_version");
    return st.step() ? static_cast<int>(st.i64(0)) : 0;
}

bool Db::migrate() {
    const int v = schemaVersion();
    if (v > kSchemaVersion) {
        error_ = "この DB は新しい版の imgdupe で作られています（スキーマ " + std::to_string(v) + "）";
        return false;
    }
    if (v == kSchemaVersion) return true;
    // 版ごとの変更を順に当てる（版を足す時は、ここに 1 行足す）
    const char* const steps[] = {kSchemaV1, kSchemaV2, kSchemaV3, kSchemaV4, kSchemaV5};
    if (!exec("BEGIN IMMEDIATE")) return false;
    for (int i = v; i < kSchemaVersion; ++i)
        if (!exec(steps[i])) {
            exec("ROLLBACK");
            return false;
        }
    if (!exec(("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str()) || !exec("COMMIT")) {
        exec("ROLLBACK");
        return false;
    }
    return true;
}

void Db::begin() {
    if (!exec("BEGIN IMMEDIATE")) throw std::runtime_error("トランザクションを始められません: " + error_);
}
void Db::commit() {
    if (!exec("COMMIT")) throw std::runtime_error("書き込めません: " + error_);
}
void Db::rollback() { exec("ROLLBACK"); }

int Db::changes() { return sqlite3_changes(db_); }

std::string Db::setting(const std::string& key, const std::string& def) {
    Stmt st(db_, "SELECT value FROM settings WHERE key=?");
    st.bind(1, key);
    return st.step() ? st.text(0) : def;
}

void Db::setSetting(const std::string& key, const std::string& value) {
    Stmt st(db_, "INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    st.bind(1, key).bind(2, value);
    st.step();
}

// ---------------------------------------------------------------- 画像

std::unordered_map<std::string, ImageRow> loadHashedImages(Db& db, int hashVersion) {
    std::unordered_map<std::string, ImageRow> out;
    Stmt st(db.handle(),
            "SELECT i.id,i.path,i.size,i.mtime,i.dh,i.dv,i.color,i.contrast,i.width,i.height,i.readable,i.sha "
            "FROM images i JOIN stages s ON s.image_id=i.id AND s.stage='hash' "
            "WHERE s.version=? AND s.state=1");
    st.bind(1, static_cast<int64_t>(hashVersion));
    while (st.step()) {
        ImageRow r;
        r.id = st.i64(0);
        Entry& e = r.e;
        e.path = st.text(1);
        e.size = static_cast<uint64_t>(st.i64(2));
        e.mtime = st.i64(3);
        e.f.dh = static_cast<uint64_t>(st.i64(4));
        e.f.dv = static_cast<uint64_t>(st.i64(5));
        const int64_t c = st.i64(6);
        e.f.r = static_cast<uint8_t>(c >> 16);
        e.f.g = static_cast<uint8_t>(c >> 8);
        e.f.b = static_cast<uint8_t>(c);
        e.f.contrast = static_cast<uint8_t>(st.i64(7));
        e.f.width = static_cast<uint32_t>(st.i64(8));
        e.f.height = static_cast<uint32_t>(st.i64(9));
        e.f.ok = st.i64(10) != 0;
        const auto sha = st.blob(11);
        if (sha.size() == 32) {
            e.hasSha = true;
            std::memcpy(e.sha.data(), sha.data(), 32);
        }
        out.emplace(e.path, std::move(r));
    }
    return out;
}

std::unordered_map<std::string, int64_t> upsertScanned(Db& db, const std::vector<ScannedFile>& files) {
    struct Known {
        int64_t id;
        int64_t size, mtime;
    };
    std::unordered_map<std::string, Known> known;
    {
        Stmt st(db.handle(), "SELECT id,path,size,mtime FROM images");
        while (st.step()) known.emplace(st.text(1), Known{st.i64(0), st.i64(2), st.i64(3)});
    }
    std::unordered_map<std::string, int64_t> ids;
    ids.reserve(files.size());
    db.begin();
    try {
        Stmt ins(db.handle(), "INSERT INTO images(path,drive,size,mtime,added) VALUES(?,?,?,?,?)");
        Stmt upd(db.handle(),
                 "UPDATE images SET size=?,mtime=?,dh=NULL,dv=NULL,color=NULL,contrast=NULL,width=NULL,height=NULL,"
                 "readable=NULL,sha=NULL WHERE id=?");
        Stmt clearStages(db.handle(), "DELETE FROM stages WHERE image_id=?");
        const int64_t now = nowUnix();
        for (const auto& f : files) {
            auto it = known.find(f.path);
            if (it == known.end()) {
                ins.bind(1, f.path).bind(2, driveOf(f.path)).bind(3, static_cast<int64_t>(f.size)).bind(4, f.mtime).bind(5, now);
                ins.step();
                ins.reset();
                ids.emplace(f.path, sqlite3_last_insert_rowid(db.handle()));
                continue;
            }
            ids.emplace(f.path, it->second.id);
            if (it->second.size == static_cast<int64_t>(f.size) && it->second.mtime == f.mtime) continue;
            // 中身が変わった: 特徴量と、すべての段階をやり直す
            upd.bind(1, static_cast<int64_t>(f.size)).bind(2, f.mtime).bind(3, it->second.id);
            upd.step();
            upd.reset();
            clearStages.bind(1, it->second.id);
            clearStages.step();
            clearStages.reset();
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    return ids;
}

namespace {
const char* const kUpdateFeatures = "UPDATE images SET dh=?,dv=?,color=?,contrast=?,width=?,height=?,readable=? WHERE id=?";
void bindFeatures(Stmt& st, int64_t id, const Features& f) {
    st.bind(1, static_cast<int64_t>(f.dh)).bind(2, static_cast<int64_t>(f.dv)).bind(3, packColor(f));
    st.bind(4, static_cast<int64_t>(f.contrast)).bind(5, static_cast<int64_t>(f.width));
    st.bind(6, static_cast<int64_t>(f.height)).bind(7, static_cast<int64_t>(f.ok ? 1 : 0)).bind(8, id);
}
}  // namespace

void storeFeatures(Db& db, int64_t imageId, const Features& f) {
    Stmt st(db.handle(), kUpdateFeatures);
    bindFeatures(st, imageId, f);
    st.step();
}

void saveFeatures(Db& db, const std::vector<std::pair<int64_t, Features>>& rows, int hashVersion) {
    if (rows.empty()) return;
    db.begin();
    try {
        Stmt st(db.handle(), kUpdateFeatures);
        const int64_t now = nowUnix();
        for (const auto& [id, f] : rows) {
            bindFeatures(st, id, f);
            st.step();
            st.reset();
        }
        Stmt stage(db.handle(),
                   "INSERT INTO stages(image_id,stage,state,version,updated) VALUES(?,'hash',1,?,?) "
                   "ON CONFLICT(image_id,stage) DO UPDATE SET state=1,version=excluded.version,updated=excluded.updated");
        for (const auto& row : rows) {
            stage.bind(1, row.first).bind(2, static_cast<int64_t>(hashVersion)).bind(3, now);
            stage.step();
            stage.reset();
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}

void saveSha(Db& db, const std::vector<std::pair<int64_t, Sha256>>& rows) {
    if (rows.empty()) return;
    db.begin();
    try {
        Stmt st(db.handle(), "UPDATE images SET sha=? WHERE id=?");
        for (const auto& [id, sha] : rows) {
            st.bindBlob(1, sha.data(), sha.size()).bind(2, id);
            st.step();
            st.reset();
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}

size_t removeMissing(Db& db, const std::vector<std::string>& roots,
                     const std::unordered_map<std::string, int64_t>& alive) {
    std::vector<int64_t> gone;
    {
        Stmt st(db.handle(), "SELECT id,path FROM images");
        while (st.step()) {
            const std::string p = st.text(1);
            if (alive.count(p)) continue;
            for (const auto& r : roots)
                if (underRoot(p, r)) {
                    gone.push_back(st.i64(0));
                    break;
                }
        }
    }
    if (gone.empty()) return 0;
    db.begin();
    try {
        Stmt del(db.handle(), "DELETE FROM images WHERE id=?");
        for (int64_t id : gone) {
            del.bind(1, id);
            del.step();
            del.reset();
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    return gone.size();
}

// ---------------------------------------------------------------- まとまり

const char* groupKindName(Kind k) {
    switch (k) {
        case Kind::Exact: return "exact";
        case Kind::Similar: return "similar";
        case Kind::Query: return "query";
        case Kind::Variant: return "variant";
    }
    return "";
}

void appendVariantGroups(Db& db, Result& r) {
    r.groups.erase(std::remove_if(r.groups.begin(), r.groups.end(), [](const Group& g) { return g.kind == Kind::Variant; }),
                   r.groups.end());
    std::unordered_map<int64_t, uint32_t> fileOf;  // 画像 ID → files の添字
    for (size_t i = 0; i < r.ids.size(); ++i)
        if (r.ids[i]) fileOf[r.ids[i]] = static_cast<uint32_t>(i);
    std::vector<int> dupOf(r.files.size(), -1);  // 重複のグループ（同じ重複のグループの中だけの差分は出さない）
    for (size_t g = 0; g < r.groups.size(); ++g)
        for (const auto& m : r.groups[g].members) dupOf[m.index] = static_cast<int>(g);
    Stmt st(db.handle(), "SELECT m.group_id, m.image_id FROM group_members m JOIN groups g ON g.id=m.group_id "
                         "WHERE g.kind='variant' ORDER BY m.group_id");
    int64_t current = -1;
    Group grp;
    auto flush = [&] {
        if (grp.members.size() >= 2) {
            const int first = dupOf[grp.members[0].index];
            bool sameDup = first >= 0;
            for (const auto& m : grp.members) sameDup &= dupOf[m.index] == first;
            if (!sameDup) {
                for (const auto& m : grp.members) grp.totalSize += r.files[m.index].size;
                r.groups.push_back(grp);
            }
        }
        grp = Group{};
        grp.kind = Kind::Variant;
    };
    grp.kind = Kind::Variant;
    while (st.step()) {
        const int64_t gid = st.i64(0);
        if (gid != current) {
            flush();
            current = gid;
        }
        auto it = fileOf.find(st.i64(1));
        if (it != fileOf.end()) grp.members.push_back({it->second, 0});  // 今回のフォルダにある画像だけ
    }
    flush();
}

void replaceDuplicateGroups(Db& db, const Result& r, const std::vector<int64_t>& imageIdOfFile) {
    db.begin();
    try {
        db.exec("DELETE FROM groups WHERE kind IN ('exact','similar')");
        Stmt g(db.handle(), "INSERT INTO groups(kind,created) VALUES(?,?)");
        Stmt m(db.handle(), "INSERT INTO group_members(group_id,image_id,representative,distance) VALUES(?,?,?,?)");
        const int64_t now = nowUnix();
        for (const auto& grp : r.groups) {
            if (grp.kind == Kind::Query || grp.kind == Kind::Variant) continue;
            g.bind(1, std::string(groupKindName(grp.kind))).bind(2, now);
            g.step();
            g.reset();
            const int64_t gid = sqlite3_last_insert_rowid(db.handle());
            // 代表（残す 1 枚の候補）: 解像度が高く、同じなら大きいもの
            size_t best = 0;
            for (size_t i = 1; i < grp.members.size(); ++i) {
                const Entry& a = r.files[grp.members[i].index];
                const Entry& b = r.files[grp.members[best].index];
                const uint64_t pa = uint64_t(a.f.width) * a.f.height, pb = uint64_t(b.f.width) * b.f.height;
                if (pa > pb || (pa == pb && a.size > b.size)) best = i;
            }
            for (size_t i = 0; i < grp.members.size(); ++i) {
                m.bind(1, gid).bind(2, imageIdOfFile[grp.members[i].index]).bind(3, static_cast<int64_t>(i == best));
                m.bind(4, static_cast<int64_t>(grp.members[i].dist));
                m.step();
                m.reset();
            }
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}

// ---------------------------------------------------------------- タグ

void replaceTags(Db& db, int64_t imageId, const std::string& source, const std::vector<TagRow>& tags) {
    Stmt del(db.handle(), "DELETE FROM tags WHERE image_id=? AND source=? AND manual=0");
    del.bind(1, imageId).bind(2, source);
    del.step();
    Stmt ins(db.handle(),
             "INSERT INTO tags(image_id,tag,kind,confidence,source,manual) VALUES(?,?,?,?,?,?) "
             "ON CONFLICT(image_id,kind,tag,source) DO UPDATE SET confidence=excluded.confidence "
             "WHERE tags.manual=0");
    for (const auto& t : tags) {
        ins.bind(1, imageId).bind(2, t.tag).bind(3, t.kind).bind(4, t.confidence).bind(5, source);
        ins.bind(6, static_cast<int64_t>(t.manual ? 1 : 0));
        ins.step();
        ins.reset();
    }
}

std::vector<TagRow> tagsOf(Db& db, int64_t imageId) {
    std::vector<TagRow> out;
    Stmt st(db.handle(),
            "SELECT tag,kind,source,confidence,manual FROM tags WHERE image_id=? ORDER BY kind,confidence DESC,tag");
    st.bind(1, imageId);
    while (st.step()) out.push_back({st.text(0), st.text(1), st.text(2), st.f64(3), st.i64(4) != 0});
    return out;
}

// ---------------------------------------------------------------- 段階

void setStage(Db& db, int64_t imageId, const std::string& stage, StageState state, int version) {
    Stmt st(db.handle(),
            "INSERT INTO stages(image_id,stage,state,version,updated) VALUES(?,?,?,?,?) "
            "ON CONFLICT(image_id,stage) DO UPDATE SET state=excluded.state,version=excluded.version,"
            "updated=excluded.updated");
    st.bind(1, imageId).bind(2, stage).bind(3, static_cast<int64_t>(state)).bind(4, static_cast<int64_t>(version));
    st.bind(5, nowUnix());
    st.step();
}

std::vector<int64_t> pendingImages(Db& db, const std::string& stage, int version, size_t limit) {
    std::string sql =
        "SELECT i.id FROM images i WHERE NOT EXISTS (SELECT 1 FROM stages s WHERE s.image_id=i.id AND s.stage=?1 "
        "AND s.version=?2 AND s.state<>0) ORDER BY i.added DESC, i.id DESC";
    if (limit) sql += " LIMIT " + std::to_string(limit);
    Stmt st(db.handle(), sql.c_str());
    st.bind(1, stage).bind(2, static_cast<int64_t>(version));
    std::vector<int64_t> out;
    while (st.step()) out.push_back(st.i64(0));
    return out;
}

// ---------------------------------------------------------------- GUI と indexer のやりとり

void setRoots(Db& db, const std::vector<std::string>& roots) {
    std::string joined;
    for (const auto& r : roots) joined += r + "\n";
    db.setSetting("roots", joined);
}

std::vector<std::string> getRoots(Db& db) {
    std::vector<std::string> out;
    const std::string s = db.setting("roots");
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find('\n', a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

void addCommand(Db& db, const std::string& command, const std::string& target) {
    Stmt st(db.handle(), "INSERT INTO commands(command,target,state,created) VALUES(?,?,'pending',?)");
    st.bind(1, command).bind(2, target).bind(3, nowUnix());
    st.step();
}

std::vector<std::string> takeCommands(Db& db) {
    std::vector<std::string> out;
    db.begin();
    try {
        {
            Stmt st(db.handle(), "SELECT command FROM commands WHERE state='pending' ORDER BY id");
            while (st.step()) out.push_back(st.text(0));
        }
        db.exec("UPDATE commands SET state='done' WHERE state='pending'");
        db.exec("DELETE FROM commands WHERE state='done' AND created < strftime('%s','now') - 86400");
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    return out;
}

// ---------------------------------------------------------------- 古いキャッシュ

size_t importCache(Db& db, const Cache& cache, int hashVersion) {
    if (cache.entries.empty()) return 0;
    size_t n = 0;
    db.begin();
    try {
        Stmt ins(db.handle(),
                 "INSERT OR IGNORE INTO images(path,drive,size,mtime,dh,dv,color,contrast,width,height,readable,sha,added) "
                 "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
        Stmt stage(db.handle(),
                   "INSERT OR IGNORE INTO stages(image_id,stage,state,version,updated) VALUES(?,'hash',1,?,?)");
        const int64_t now = nowUnix();
        for (const auto& [path, e] : cache.entries) {
            ins.bind(1, path).bind(2, driveOf(path)).bind(3, static_cast<int64_t>(e.size)).bind(4, e.mtime);
            ins.bind(5, static_cast<int64_t>(e.f.dh)).bind(6, static_cast<int64_t>(e.f.dv)).bind(7, packColor(e.f));
            ins.bind(8, static_cast<int64_t>(e.f.contrast)).bind(9, static_cast<int64_t>(e.f.width));
            ins.bind(10, static_cast<int64_t>(e.f.height)).bind(11, static_cast<int64_t>(e.f.ok ? 1 : 0));
            if (e.hasSha) ins.bindBlob(12, e.sha.data(), e.sha.size());
            else ins.bindNull(12);
            ins.bind(13, now);
            ins.step();
            ins.reset();
            if (sqlite3_changes(db.handle()) == 0) continue;  // もう DB にある
            stage.bind(1, static_cast<int64_t>(sqlite3_last_insert_rowid(db.handle())));
            stage.bind(2, static_cast<int64_t>(hashVersion)).bind(3, now);
            stage.step();
            stage.reset();
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
