#include "tagstore.hpp"

#include <algorithm>
#include <stdexcept>

#include "sqlite3.h"

namespace imgdupe {

namespace {

std::string placeholders(size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) s += i ? ",?" : "?";
    return s;
}

void run(Db& db, const char* sql) {
    if (!db.exec(sql)) throw std::runtime_error("SQL を実行できません: " + db.error());
}

int64_t scalar(Db& db, const char* sql) {
    Stmt st(db.handle(), sql);
    return st.step() ? st.i64(0) : 0;
}

constexpr size_t kIdCacheMax = 50000;  // 控えの数の上限（超えたら捨てる。メモリを使いすぎない）

// 新しい形のビューと、ビューに書いた時の置き換え（前と同じ形の INSERT・DELETE・UPDATE がそのまま使える）。
// 出どころごとの行の数は tag_rows の追加・削除で数える（tag_rows には INSERT OR REPLACE を使わない。消した分が数えられないので）
const char* const kNewFormatSql = R"SQL(
CREATE INDEX IF NOT EXISTS tag_rows_tag ON tag_rows(tag_id, confidence);
DROP TRIGGER IF EXISTS tags_conv_ins;
DROP TRIGGER IF EXISTS tags_conv_upd;
DROP TRIGGER IF EXISTS tags_conv_del;
DROP TABLE IF EXISTS tags_conv_dirty;
DROP TABLE IF EXISTS tags;
CREATE VIEW tags AS
    SELECT r.image_id AS image_id, k.tag AS tag, k.kind AS kind, r.confidence AS confidence, s.name AS source, r.manual AS manual
    FROM tag_rows r JOIN tag_keys k ON k.id = r.tag_id JOIN tag_sources s ON s.id = r.source_id;
CREATE TRIGGER tags_view_ins INSTEAD OF INSERT ON tags BEGIN
    INSERT INTO tag_keys(tag, kind) SELECT NEW.tag, NEW.kind
        WHERE NOT EXISTS (SELECT 1 FROM tag_keys WHERE tag = NEW.tag AND kind = NEW.kind);
    INSERT INTO tag_sources(name) SELECT NEW.source WHERE NOT EXISTS (SELECT 1 FROM tag_sources WHERE name = NEW.source);
    INSERT INTO tag_rows(image_id, tag_id, source_id, confidence, manual)
        VALUES(NEW.image_id, (SELECT id FROM tag_keys WHERE tag = NEW.tag AND kind = NEW.kind),
               (SELECT id FROM tag_sources WHERE name = NEW.source), NEW.confidence, coalesce(NEW.manual, 0))
        ON CONFLICT(image_id, tag_id, source_id) DO UPDATE SET confidence = excluded.confidence, manual = excluded.manual;
END;
CREATE TRIGGER tags_view_del INSTEAD OF DELETE ON tags BEGIN
    DELETE FROM tag_rows WHERE image_id = OLD.image_id
        AND tag_id = (SELECT id FROM tag_keys WHERE tag = OLD.tag AND kind = OLD.kind)
        AND source_id = (SELECT id FROM tag_sources WHERE name = OLD.source);
END;
CREATE TRIGGER tags_view_upd INSTEAD OF UPDATE ON tags BEGIN
    UPDATE tag_rows SET confidence = NEW.confidence, manual = NEW.manual WHERE image_id = OLD.image_id
        AND tag_id = (SELECT id FROM tag_keys WHERE tag = OLD.tag AND kind = OLD.kind)
        AND source_id = (SELECT id FROM tag_sources WHERE name = OLD.source);
END;
CREATE TRIGGER tag_rows_count_ins AFTER INSERT ON tag_rows BEGIN
    UPDATE tag_sources SET rows = rows + 1 WHERE id = NEW.source_id;
END;
CREATE TRIGGER tag_rows_count_del AFTER DELETE ON tag_rows BEGIN
    UPDATE tag_sources SET rows = rows - 1 WHERE id = OLD.source_id;
END;
)SQL";

const char* const kUptoKey = "tags:convertedUpTo";

// 前の表 tags の画像 ID の範囲 (lo, hi] を tag_rows に移す（名前・出どころの番号も作る）。移した行の数
int64_t copyRange(Db& db, int64_t lo, int64_t hi) {
    {
        Stmt st(db.handle(),
                "INSERT INTO tag_keys(tag, kind) SELECT DISTINCT t.tag, t.kind FROM tags t WHERE t.image_id > ? AND t.image_id <= ? "
                "AND NOT EXISTS (SELECT 1 FROM tag_keys k WHERE k.tag = t.tag AND k.kind = t.kind)");
        st.bind(1, lo).bind(2, hi);
        st.step();
    }
    {
        Stmt st(db.handle(),
                "INSERT INTO tag_sources(name) SELECT DISTINCT t.source FROM tags t WHERE t.image_id > ? AND t.image_id <= ? "
                "AND NOT EXISTS (SELECT 1 FROM tag_sources s WHERE s.name = t.source)");
        st.bind(1, lo).bind(2, hi);
        st.step();
    }
    Stmt st(db.handle(),
            "INSERT OR IGNORE INTO tag_rows(image_id, tag_id, source_id, confidence, manual) "
            "SELECT t.image_id, k.id, s.id, t.confidence, t.manual FROM tags t "
            "JOIN tag_keys k ON k.tag = t.tag AND k.kind = t.kind JOIN tag_sources s ON s.name = t.source "
            "WHERE t.image_id > ? AND t.image_id <= ?");
    st.bind(1, lo).bind(2, hi);
    st.step();
    return db.changes();
}

}  // namespace

bool newTagFormat(Db& db) {
    Stmt st(db.handle(), "SELECT type FROM sqlite_master WHERE name='tags'");
    return st.step() && st.text(0) == "view";
}

int64_t tagKeyId(Db& db, const std::string& tag, const std::string& kind) {
    auto& cache = db.tagIdCache();
    const std::string key = "k" + kind + '\x1f' + tag;
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    int64_t id = 0;
    {
        Stmt st(db.handle(), "SELECT id FROM tag_keys WHERE tag=? AND kind=?");
        st.bind(1, tag).bind(2, kind);
        if (st.step()) id = st.i64(0);
    }
    if (!id) {
        Stmt ins(db.handle(), "INSERT INTO tag_keys(tag, kind) VALUES(?, ?)");
        ins.bind(1, tag).bind(2, kind);
        ins.step();
        id = sqlite3_last_insert_rowid(db.handle());
    }
    if (cache.size() > kIdCacheMax) cache.clear();
    cache[key] = id;
    return id;
}

int64_t tagSourceId(Db& db, const std::string& source) {
    auto& cache = db.tagIdCache();
    const std::string key = "s" + source;
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    int64_t id = 0;
    {
        Stmt st(db.handle(), "SELECT id FROM tag_sources WHERE name=?");
        st.bind(1, source);
        if (st.step()) id = st.i64(0);
    }
    if (!id) {
        Stmt ins(db.handle(), "INSERT INTO tag_sources(name) VALUES(?)");
        ins.bind(1, source);
        ins.step();
        id = sqlite3_last_insert_rowid(db.handle());
    }
    if (cache.size() > kIdCacheMax) cache.clear();
    cache[key] = id;
    return id;
}

// ---------------------------------------------------------------- 書く

TagWriter::TagWriter(Db& db, TagConflict how) : db_(db), newFormat_(newTagFormat(db)) {
    std::string sql;
    if (newFormat_) {
        sql = "INSERT INTO tag_rows(image_id, tag_id, source_id, confidence, manual) VALUES(?,?,?,?,?) ON CONFLICT(image_id, tag_id, source_id) ";
        switch (how) {
            case TagConflict::Replace: sql += "DO UPDATE SET confidence=excluded.confidence, manual=excluded.manual"; break;
            case TagConflict::Ignore: sql += "DO NOTHING"; break;
            case TagConflict::KeepHigher: sql += "DO UPDATE SET confidence=max(confidence, excluded.confidence) WHERE manual=0"; break;
            case TagConflict::KeepManual: sql += "DO UPDATE SET confidence=excluded.confidence WHERE manual=0"; break;
        }
    } else {
        switch (how) {
            case TagConflict::Replace: sql = "INSERT OR REPLACE INTO tags(image_id,tag,kind,confidence,source,manual) VALUES(?,?,?,?,?,?)"; break;
            case TagConflict::Ignore: sql = "INSERT OR IGNORE INTO tags(image_id,tag,kind,confidence,source,manual) VALUES(?,?,?,?,?,?)"; break;
            case TagConflict::KeepHigher:
                sql = "INSERT INTO tags(image_id,tag,kind,confidence,source,manual) VALUES(?,?,?,?,?,?) "
                      "ON CONFLICT(image_id,kind,tag,source) DO UPDATE SET confidence=max(confidence, excluded.confidence) WHERE manual=0";
                break;
            case TagConflict::KeepManual:
                sql = "INSERT INTO tags(image_id,tag,kind,confidence,source,manual) VALUES(?,?,?,?,?,?) "
                      "ON CONFLICT(image_id,kind,tag,source) DO UPDATE SET confidence=excluded.confidence WHERE tags.manual=0";
                break;
        }
    }
    st_ = new Stmt(db.handle(), sql.c_str());
}

TagWriter::~TagWriter() { delete st_; }

void TagWriter::put(int64_t imageId, const std::string& tag, const std::string& kind, double confidence, const std::string& source,
                    bool manual) {
    if (newFormat_) {
        st_->bind(1, imageId).bind(2, tagKeyId(db_, tag, kind)).bind(3, tagSourceId(db_, source)).bind(4, confidence);
        st_->bind(5, static_cast<int64_t>(manual ? 1 : 0));
    } else {
        st_->bind(1, imageId).bind(2, tag).bind(3, kind).bind(4, confidence).bind(5, source).bind(6, static_cast<int64_t>(manual ? 1 : 0));
    }
    st_->step();
    st_->reset();
}

void replaceTags(Db& db, int64_t imageId, const std::string& source, const std::vector<TagRow>& tags) {
    deleteImageTags(db, imageId, {source});
    if (tags.empty()) return;
    TagWriter w(db, TagConflict::KeepManual);
    for (const auto& t : tags) w.put(imageId, t.tag, t.kind, t.confidence, source, t.manual);
}

void deleteSourceTags(Db& db, const std::string& source) {
    if (newTagFormat(db)) {
        Stmt st(db.handle(), "DELETE FROM tag_rows WHERE source_id=? AND manual=0");
        st.bind(1, tagSourceId(db, source));
        st.step();
    } else {
        Stmt st(db.handle(), "DELETE FROM tags WHERE source=? AND manual=0");
        st.bind(1, source);
        st.step();
    }
}

void deleteImageTags(Db& db, int64_t imageId, const std::vector<std::string>& sources) {
    if (sources.empty()) return;
    const bool nf = newTagFormat(db);
    const std::string sql = nf ? "DELETE FROM tag_rows WHERE image_id=? AND manual=0 AND source_id IN (" + placeholders(sources.size()) + ")"
                               : "DELETE FROM tags WHERE image_id=? AND manual=0 AND source IN (" + placeholders(sources.size()) + ")";
    Stmt st(db.handle(), sql.c_str());
    st.bind(1, imageId);
    for (size_t i = 0; i < sources.size(); ++i) {
        if (nf) st.bind(static_cast<int>(i + 2), tagSourceId(db, sources[i]));
        else st.bind(static_cast<int>(i + 2), sources[i]);
    }
    st.step();
}

void deleteManualTags(Db& db, int64_t imageId, const std::vector<std::string>& names) {
    if (names.empty()) return;
    const std::string sql =
        newTagFormat(db)
            ? "DELETE FROM tag_rows WHERE image_id=? AND manual=1 AND tag_id IN (SELECT id FROM tag_keys WHERE tag COLLATE NOCASE IN (" +
                  placeholders(names.size()) + "))"
            : "DELETE FROM tags WHERE image_id=? AND manual=1 AND tag COLLATE NOCASE IN (" + placeholders(names.size()) + ")";
    Stmt st(db.handle(), sql.c_str());
    st.bind(1, imageId);
    for (size_t i = 0; i < names.size(); ++i) st.bind(static_cast<int>(i + 2), names[i]);
    st.step();
}

void copyManualTags(Db& db, int64_t from, int64_t to) {
    Stmt st(db.handle(),
            newTagFormat(db) ? "INSERT OR IGNORE INTO tag_rows(image_id, tag_id, source_id, confidence, manual) "
                               "SELECT ?, tag_id, source_id, confidence, manual FROM tag_rows WHERE image_id=? AND manual=1"
                             : "INSERT OR IGNORE INTO tags(image_id,tag,kind,confidence,source,manual) "
                               "SELECT ?,tag,kind,confidence,source,manual FROM tags WHERE image_id=? AND manual=1");
    st.bind(1, to).bind(2, from);
    st.step();
}

// ---------------------------------------------------------------- 数える

int64_t tagRowCount(Db& db, const std::string& source) {
    Stmt st(db.handle(), newTagFormat(db) ? "SELECT coalesce(sum(rows),0) FROM tag_sources WHERE name=?" : "SELECT count(*) FROM tags WHERE source=?");
    st.bind(1, source);
    return st.step() ? st.i64(0) : 0;
}

int64_t tagRowCountExcept(Db& db, const std::string& source) {
    Stmt st(db.handle(),
            newTagFormat(db) ? "SELECT coalesce(sum(rows),0) FROM tag_sources WHERE name<>?" : "SELECT count(*) FROM tags WHERE source<>?");
    st.bind(1, source);
    return st.step() ? st.i64(0) : 0;
}

std::vector<std::pair<std::string, int64_t>> tagSourceCounts(Db& db) {
    std::vector<std::pair<std::string, int64_t>> out;
    Stmt st(db.handle(), newTagFormat(db) ? "SELECT name, rows FROM tag_sources WHERE rows > 0 ORDER BY 2 DESC"
                                          : "SELECT source, COUNT(*) FROM tags GROUP BY source ORDER BY 2 DESC");
    while (st.step()) out.emplace_back(st.text(0), st.i64(1));
    return out;
}

std::vector<std::string> tagNames(Db& db) {
    std::vector<std::string> out;
    if (newTagFormat(db)) {
        // 名前の表は小さい。行が無くなった名前は除く（索引で 1 つずつ確かめる）
        Stmt st(db.handle(),
                "SELECT min(k.tag) FROM tag_keys k WHERE EXISTS (SELECT 1 FROM tag_rows r WHERE r.tag_id = k.id) "
                "GROUP BY k.tag COLLATE NOCASE ORDER BY k.tag COLLATE NOCASE");
        while (st.step()) out.push_back(st.text(0));
        return out;
    }
    // 前の形: 索引を飛び飛びにたどる（行が多くても速い）
    Stmt st(db.handle(),
            "WITH RECURSIVE d(t) AS (SELECT min(tag COLLATE NOCASE) FROM tags UNION ALL "
            "SELECT (SELECT min(tag COLLATE NOCASE) FROM tags WHERE tag > d.t COLLATE NOCASE) FROM d WHERE d.t IS NOT NULL) "
            "SELECT t FROM d WHERE t IS NOT NULL");
    while (st.step()) out.push_back(st.text(0));
    return out;
}

// ---------------------------------------------------------------- 前の版の DB の変換

TagConversionState tagConversionState(Db& db) {
    TagConversionState s;
    if (newTagFormat(db)) return s;
    s.pending = true;
    s.done = std::atoll(db.setting(kUptoKey, "0").c_str());
    s.total = std::max(scalar(db, "SELECT coalesce(max(image_id),0) FROM tags"), s.done);
    return s;
}

bool finishNewTagFormat(Db& db) {
    // 出どころごとの行の数（tag_rows を 1 回読む）
    {
        Stmt st(db.handle(), "SELECT source_id, count(*) FROM tag_rows GROUP BY source_id");
        Stmt up(db.handle(), "UPDATE tag_sources SET rows=? WHERE id=?");
        run(db, "UPDATE tag_sources SET rows=0");
        while (st.step()) {
            up.bind(1, st.i64(1)).bind(2, st.i64(0));
            up.step();
            up.reset();
        }
    }
    return db.exec(kNewFormatSql);
}

bool convertTagsStep(Db& db, int64_t maxImages, std::string* step) {
    if (newTagFormat(db)) return true;
    maxImages = std::max<int64_t>(1, maxImages);
    // 1. 画像の ID の順に、少しずつ移す（1 回ずつトランザクション。ほかの書き込みを長く待たせない）
    {
        db.begin();
        try {
            if (newTagFormat(db)) {  // ほかの窓（GUI か解析）が先に済ませた
                db.commit();
                return true;
            }
            const int64_t upto = std::atoll(db.setting(kUptoKey, "0").c_str());
            const int64_t maxId = scalar(db, "SELECT coalesce(max(image_id),0) FROM tags");
            if (upto < maxId) {
                const int64_t hi = std::min(maxId, upto + maxImages);
                if (step) *step = "移しています";
                copyRange(db, upto, hi);
                // 移した範囲の書き換えの印は要らない（今の中身を移した）
                Stmt clear(db.handle(), "DELETE FROM tags_conv_dirty WHERE image_id > ? AND image_id <= ?");
                clear.bind(1, upto).bind(2, hi);
                clear.step();
                db.setSetting(kUptoKey, std::to_string(hi));
                db.commit();
                return false;
            }
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }
    }
    // 2. 検索用の索引を作る（並べ替えはメモリではなく一時ファイルで）
    if (step) *step = "索引を作っています";
    run(db, "PRAGMA temp_store=FILE");
    try {
        db.begin();
        try {
            run(db, "CREATE INDEX IF NOT EXISTS tag_rows_tag ON tag_rows(tag_id, confidence)");
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }
        // 3. 入れ替える: 移した後に書き換わった画像を移し直し、前の表を消してビューにする
        if (step) *step = "入れ替えています";
        db.begin();
        try {
            if (!newTagFormat(db)) {
                const int64_t upto = std::atoll(db.setting(kUptoKey, "0").c_str());
                const int64_t maxId = scalar(db, "SELECT coalesce(max(image_id),0) FROM tags");
                if (maxId > upto) copyRange(db, upto, maxId);  // 索引を作っている間に足された画像
                std::vector<int64_t> dirty;
                {
                    Stmt st(db.handle(), "SELECT image_id FROM tags_conv_dirty WHERE image_id <= ?");
                    st.bind(1, upto);
                    while (st.step()) dirty.push_back(st.i64(0));
                }
                Stmt del(db.handle(), "DELETE FROM tag_rows WHERE image_id=?");
                for (int64_t id : dirty) {
                    del.bind(1, id);
                    del.step();
                    del.reset();
                    copyRange(db, id - 1, id);
                }
                if (!finishNewTagFormat(db)) throw std::runtime_error("タグの表を入れ替えられません: " + db.error());
                Stmt clearKey(db.handle(), "DELETE FROM settings WHERE key=?");
                clearKey.bind(1, std::string(kUptoKey));
                clearKey.step();
                // 前の表の分が空き領域として残る（ファイルは小さくならない）ので、画面で「詰める」をすすめる
                db.setSetting("maintain:suggestCompact", "1");
            }
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }
    } catch (...) {
        db.exec("PRAGMA temp_store=MEMORY");
        throw;
    }
    db.exec("PRAGMA temp_store=MEMORY");
    // 検索の読み方を選ぶための数（少しだけ読んで見積もる）
    if (step) *step = "仕上げています";
    db.exec("PRAGMA analysis_limit=1000; ANALYZE;");
    return true;
}

DbSpace dbSpace(Db& db) {
    DbSpace s;
    const int64_t pageSize = scalar(db, "PRAGMA page_size");
    s.fileBytes = scalar(db, "PRAGMA page_count") * pageSize;
    s.freeBytes = scalar(db, "PRAGMA freelist_count") * pageSize;
    return s;
}

bool compactDb(Db& db, std::string& error) {
    db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    db.exec("PRAGMA temp_store=FILE");
    const bool ok = db.exec("VACUUM");
    if (!ok) error = db.error();
    db.exec("PRAGMA temp_store=MEMORY");
    db.exec("PRAGMA wal_checkpoint(TRUNCATE)");  // 詰めた分を書き込み記録（-wal）から本体へ移して、記録を空にする
    return ok;
}

}  // namespace imgdupe
