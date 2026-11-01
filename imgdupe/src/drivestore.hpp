// ドライブの正本（データ保護の実装仕様 3）: ドライブの隠しフォルダ <ドライブ>:\.imgdupe\drive.db に、そのドライブの画像の「守るもの」
// （画像を見分ける情報・タガーのタグ・手動の確定・タガーと CLIP の段階・CLIP のベクトル）を、ドライブの中の相対パスで持つ。
// 今の DB（imgdupe.db）の中の、そのドライブの分を「クローン」として読み書きし、10 分ごとに変わった分だけ正本へ書き戻す。
// OS に依存しない（書けるドライブか・隠し属性などは呼ぶ側）。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "db.hpp"

namespace imgdupe {

constexpr const char* kMasterDirName = ".imgdupe";  // スキャンしない

// 正本のあるドライブ（呼ぶ側が、つながっていて書けるドライブについて作る）
struct DriveRef {
    std::string root;   // "E:\\"（ドライブの直下）
    uint32_t serial = 0;
    std::string label;
};

std::string masterFileOf(const std::string& driveRoot);  // <root>.imgdupe\drive.db
// 今の DB のパス ⇔ ドライブの中の相対パス（"E:\\a\\b.jpg" ⇔ "\\a\\b.jpg"）
std::string relativeOnDrive(const std::string& path);
std::string pathOnDrive(const std::string& driveRoot, const std::string& rel);
// パスのドライブ（"E:\\a.jpg" → "E:\\"。ドライブレターで始まらなければ空）
std::string driveRootOf(const std::string& path);

// 正本とクローンの見比べ（食い違った時の確認の画面）
struct DriveSide {
    bool exists = false;
    int taggerVersion = 0;   // タガーの段階の版（一番多いもの。無ければ 0）
    int64_t images = 0;      // 対象の枚数
    double progress = 0;     // 作成の進み具合（0〜1。処理の時間の重み）
    int64_t complete = 0;    // 作り終えた（全部の段階が済んだ）枚数
    int64_t updated = 0;     // 最後に書いた日時（Unix 秒）
};

// つないだ時の合わせ方
enum class DriveSync {
    Same,       // 正本とクローンが合っている（書き戻しを続ける）
    Created,    // 正本が無かったので作った（今の DB の分を書き戻す）
    Imported,   // クローンが無かったので正本から読み戻した
    Conflict,   // 正本とクローンが食い違う（ユーザーに選んでもらう。drives.conflict = 1）
};
struct DriveSyncResult {
    DriveSync kind = DriveSync::Same;
    int64_t driveRow = 0;  // drives.id
    size_t imported = 0;
};

// ドライブをつないだ（見えた）時に呼ぶ。正本が無ければ作り、クローンが無ければ正本から読み戻し、食い違えば Conflict
DriveSyncResult syncDriveOnConnect(Db& db, const DriveRef& d, const std::string& vectorFile, int64_t now);
// seenRows（今つながっている、正本のあるドライブの drives.id）以外に gone_since を付ける（もう付いていればそのまま）
void markDrivesSeen(Db& db, const std::vector<int64_t>& seenRows, int64_t now);

// 書き戻し: つながっているドライブ（drives）のクローンの変わった分（dirty_*）を、それぞれの正本へ。
// CLIP のベクトルは、ファイルの後ろに足された分（settings の vec_offset から）を 1 回だけ流し読んで、それぞれの正本へ振り分ける。
// 食い違っている（正本が別の所で書き換わった）ドライブには書かない（conflict）。返すのは drives と同じ並び
struct WritebackResult {
    std::string root;
    size_t images = 0, removed = 0, vectors = 0;
    bool conflict = false, failed = false;
    std::string error;
};
std::vector<WritebackResult> writebackDrives(Db& db, const std::vector<DriveRef>& drives, const std::string& vectorFile, int64_t now,
                                             const std::atomic<bool>* cancel = nullptr);
// 書き戻しに失敗した（正本に書けない）: fail_since を付ける（成功したら writebackDrive が消す）
void markWritebackFailed(Db& db, const std::string& driveRoot, int64_t now);

// 見比べる値
DriveSide masterSide(Db& db, const DriveRef& d);
DriveSide cloneSide(Db& db, const std::string& driveRoot);

// 食い違いの解き方
// useMaster: クローン（今の DB のそのドライブの分）を保管へ移し、正本から読み戻す
// !useMaster: 正本を保管のフォルダに写して残し、クローンで正本を作り直す
void resolveConflict(Db& db, const DriveRef& d, bool useMaster, const std::string& archiveFile, const std::string& vectorFile,
                     int64_t now);

// ドライブごとの様子（片付けの確認の画面）
struct DriveRow {
    int64_t id = 0;
    std::string driveId, root, label;
    uint32_t serial = 0;
    int64_t lastSeen = 0, goneSince = 0, lastWriteback = 0, failSince = 0, snoozeUntil = 0;
    bool cloneDeleted = false, conflict = false;
    int64_t cloneImages = 0;  // 今の DB のそのドライブの画像の数
    int64_t unwritten = 0;    // まだ書き戻していない画像の数
};
std::vector<DriveRow> driveRows(Db& db);
// クローンを消す: 書き戻していない分は保管へ（"書き戻し失敗分"）、ほかは今の DB から消す（正本にあるので）。消した数を返す
size_t deleteClone(Db& db, int64_t driveRow, const std::string& archiveFile, const std::string& vectorFile);
void snoozeDrive(Db& db, int64_t driveRow, int64_t until);

}  // namespace imgdupe
