// ハードリンク展開（仕様「ハードリンク展開」）。検索結果などを、ハードリンクでフォルダに並べる。
// 容量は増えず、エクスプローラーや他のソフトからは普通のフォルダに見える。作ったリンクは links テーブルに記録する。
// OS に依存しない（ファイルの操作は platform.hpp）。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "db.hpp"

namespace imgdupe {

constexpr const char* kVfolderDirName = "_vfolders";

// 生成先（ハードリンクは同じドライブにしか作れないので、ドライブごと）。
// settings の "vfolders:<ドライブ>"。無ければ <ドライブ>\_vfolders
std::string vfolderRoot(Db& db, const std::string& drive);
void setVfolderRoot(Db& db, const std::string& drive, const std::string& root);  // 空 = 既定に戻す
// 設定で変えた生成先（走査から外すため。既定の _vfolders は名前で外す）
std::vector<std::string> customVfolderRoots(Db& db);

// フォルダ名に使えない文字を _ にする（空なら "検索結果"）
std::string safeFolderName(const std::string& name);

struct ExpandResult {
    size_t created = 0;       // 作ったリンク
    size_t failed = 0;        // 作れなかった（元画像が無い、別のドライブ、FAT32 など）
    size_t removed = 0;       // 同じ名前で前に展開した分から消したリンク
    std::vector<std::string> folders;  // 作ったフォルダ（ドライブごと）
    std::string firstFailure;          // 最初に作れなかった画像のパス
    bool cancelled = false;
};
// ids の画像を、各ドライブの生成先の name フォルダにハードリンクで並べる。
// 同じ名前で前に展開したリンクは消してから作り直す（中身を今の結果にする）。ファイル名がぶつかったら "<画像 ID>_" を頭に付ける
ExpandResult expandLinks(Db& db, const std::vector<int64_t>& ids, const std::string& name, bool temporary,
                         const std::atomic<bool>* cancel = nullptr);

// 展開したフォルダの一覧
struct LinkFolder {
    std::string dir;
    size_t count = 0;
    size_t temporary = 0;  // そのうち一時リンクの数
    int64_t created = 0;
};
std::vector<LinkFolder> linkFolders(Db& db);
void setFolderTemporary(Db& db, const std::string& dir, bool temporary);  // 「一時を解除」= false

struct RemoveResult {
    size_t removed = 0;  // 消した（記録も消した）
    size_t gone = 0;     // もう無かった・別のファイルに置き換わっていた（記録だけ消した）
    size_t failed = 0;   // 開いていて消せなかった（記録を残し、次に起動した時にまた試す）
    size_t kept = 0;     // 元画像が消えて、データがこのリンクにしか無い（消さずに残す）
};
// リンクを消す。dir が空ならすべて、temporaryOnly なら一時リンクだけ。
// 消すのは「記録したリンクで、今も元画像と同じ中身のもの」だけ（別のファイルに置き換わっていたら触らない）。
// 消した後、空になったフォルダも消す（生成先そのものは残す）
RemoveResult removeLinks(Db& db, const std::string& dir, bool temporaryOnly);

// 元画像が消えて、リンクだけがデータを抱えているもの（元画像を消しても容量が空かない）
std::vector<std::string> orphanLinks(Db& db);

}  // namespace imgdupe
