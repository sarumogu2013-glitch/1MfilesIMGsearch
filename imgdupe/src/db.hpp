// 画像の情報の置き場所（SQLite, WAL）。仕様「データ層（DB スキーマ）」のテーブルをすべて持つ。
// GUI と indexer が同時に読み書きする前提で、書き込みはまとめてトランザクションで行う。OS に依存しない。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace imgdupe {

// 今のスキーマの版。テーブルを変えたら上げ、Db::open の移行処理に足す
constexpr int kSchemaVersion = 10;

// 1 つの SQL 文（プリペアドステートメント）
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql);
    ~Stmt();
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int i, int64_t v);
    Stmt& bind(int i, double v);
    Stmt& bind(int i, const std::string& v);
    Stmt& bindBlob(int i, const void* data, size_t len);
    Stmt& bindNull(int i);
    bool step();  // 行があれば true（無ければ / 終わったら false）
    void reset();

    int64_t i64(int col) const;
    double f64(int col) const;
    std::string text(int col) const;
    std::vector<uint8_t> blob(int col) const;
    bool isNull(int col) const;

private:
    sqlite3* db_;
    sqlite3_stmt* st_ = nullptr;
};

class Db {
public:
    Db() = default;
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    // 開いて、無ければテーブルを作る（古い版なら移行する）。失敗したら false（error() に理由）
    bool open(const std::string& path);
    void close();
    bool isOpen() const { return db_ != nullptr; }
    const std::string& error() const { return error_; }
    sqlite3* handle() const { return db_; }

    bool exec(const char* sql);
    int schemaVersion();

    // 書き込みをまとめる（入れ子にしない）
    void begin();
    void commit();
    void rollback();
    int changes();  // 直前の INSERT / UPDATE / DELETE で変わった行の数

    std::string setting(const std::string& key, const std::string& def = "");
    void setSetting(const std::string& key, const std::string& value);

private:
    bool migrate();
    sqlite3* db_ = nullptr;
    std::string error_;
};

// 段階の状態（stages テーブル）
enum class StageState : int { Pending = 0, Done = 1, Failed = 2 };

// ---------------------------------------------------------------- 画像（images）

// images の行。特徴量（dHash など）も同じ行に持つ
struct ImageRow {
    int64_t id = 0;
    Entry e;
};

// パス → 画像。段階 "hash" が今の版で済んでいるものだけ（特徴量が使えるもの）を返す
std::unordered_map<std::string, ImageRow> loadHashedImages(Db& db, int hashVersion);

// 走査で見つけたファイルを登録する。大きさか更新日時が変わったものは特徴量と段階を消す。
// 戻り値: パス → 画像 ID
std::unordered_map<std::string, int64_t> upsertScanned(Db& db, const std::vector<ScannedFile>& files);

// 特徴量を 1 件書く（呼び出し側のトランザクションの中で）
void storeFeatures(Db& db, int64_t imageId, const Features& f);
// 特徴量を書き、段階 "hash" を済みにする（まとめて 1 つのトランザクションで）
void saveFeatures(Db& db, const std::vector<std::pair<int64_t, Features>>& rows, int hashVersion);
void saveSha(Db& db, const std::vector<std::pair<int64_t, Sha256>>& rows);

// roots の下で、alive に無い画像を消す（段階・タグなどもまとめて消える）。消した数を返す
size_t removeMissing(Db& db, const std::vector<std::string>& roots, const std::unordered_map<std::string, int64_t>& alive);

// ---------------------------------------------------------------- まとまり（groups）

// 重複のまとまりを置き換える（kind が完全一致・見た目一致のものを全部消して入れ直す）
void replaceDuplicateGroups(Db& db, const Result& r, const std::vector<int64_t>& imageIdOfFile);
const char* groupKindName(Kind k);  // "exact" / "similar" / "query" / "variant"
// DB の差分グループ（CLIP）のうち、r.files にある画像を 2 枚以上含むものを r.groups の後ろに足す（前に足した分は入れ替え）。
// 同じ重複のグループの中だけで閉じているものは足さない
void appendVariantGroups(Db& db, Result& r);

// ---------------------------------------------------------------- タグ（tags）

struct TagRow {
    std::string tag, kind, source;
    double confidence = 0;
    bool manual = false;
};
void replaceTags(Db& db, int64_t imageId, const std::string& source, const std::vector<TagRow>& tags);
std::vector<TagRow> tagsOf(Db& db, int64_t imageId);

// ---------------------------------------------------------------- 段階（stages）

void setStage(Db& db, int64_t imageId, const std::string& stage, StageState state, int version);
// その段階が version で済んでいない画像（新しく追加されたものから）。within があれば、その中から（その並びで）
std::vector<int64_t> pendingImages(Db& db, const std::string& stage, int version, size_t limit = 0,
                                   const std::vector<int64_t>* within = nullptr);

// ---------------------------------------------------------------- GUI と indexer のやりとり

// パスのドライブ（"D:"、"\\server\share"、"/"）
std::string driveOf(const std::string& path);

// 調べるフォルダ（settings の "roots"、改行区切り）
void setRoots(Db& db, const std::vector<std::string>& roots);
std::vector<std::string> getRoots(Db& db);

// GUI から indexer への指示（commands テーブル）。"quit" / "rescan" / "pause" / "resume"
void addCommand(Db& db, const std::string& command, const std::string& target = "");
// まだ済んでいない指示を古い順に取り出し、済みにする
std::vector<std::string> takeCommands(Db& db);

// スキャンタスク（scan_tasks）: GUI の「スキャンタスクに追加」1 回で 1 つ。paths はフォルダかファイル。
// indexer は並びの上から 1 つずつ、その中の画像だけ全部の段階を済ませ、済んだら消す
struct ScanTask {
    int64_t id = 0;
    std::string label;
    std::vector<std::string> paths;
    int64_t created = 0;
};
enum class TaskMove { Top, Up, Down, Bottom };
// top なら一番上、そうでなければ一番下に足す。ID を返す
int64_t addScanTask(Db& db, const std::string& label, const std::vector<std::string>& paths, bool top);
std::vector<ScanTask> scanTasks(Db& db);  // 処理する順
void moveScanTask(Db& db, int64_t id, TaskMove where);
void setScanTaskOrder(Db& db, const std::vector<int64_t>& ids);
void removeScanTask(Db& db, int64_t id);

// 古いキャッシュ（imgdupe_cache.bin）を読み込む。取り込んだ数を返す
size_t importCache(Db& db, const Cache& cache, int hashVersion);

int64_t nowUnix();

}  // namespace imgdupe
