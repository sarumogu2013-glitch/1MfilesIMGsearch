// 保管（データ保護の実装仕様 1.1）: 見つからなくなった画像の「守る」もの（画像を見分ける情報・タガーのタグ・手動の確定・
// タガーと CLIP の段階の記録・CLIP のベクトル）を、別の DB（<データ>\archive\archive.db）へ移す。
// 保管の DB は普段は開かない（使う時だけ、今の DB の接続に ATTACH する）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core.hpp"
#include "db.hpp"

namespace imgdupe {

// 保管の DB のファイル（dataDir の下）
std::string archiveFileOf(const std::string& dataDir);

// ids の画像を保管へ移し、今の DB から消す。vectorFile（空なら使わない）から CLIP のベクトルも写す。移した数を返す
size_t archiveImages(Db& db, const std::string& archiveFile, const std::vector<int64_t>& ids, const std::string& vectorFile,
                     const std::string& reason);

struct ArchivedImage {
    int64_t aid = 0;
    std::string path;
    int64_t size = 0;
    int64_t archived = 0;  // 保管した時刻（Unix 秒）
    std::string reason;
    int tags = 0;          // 守ったタグの数
    bool vector = false;   // CLIP のベクトルがある
};

struct ArchiveSummary {
    int64_t images = 0;
    int64_t fileBytes = 0;  // 保管の DB のファイルの大きさ
    int64_t oldest = 0, newest = 0;
};

ArchiveSummary archiveSummary(Db& db, const std::string& archiveFile);
// 保管した時刻の古い順に limit 件（0 = 全部）
std::vector<ArchivedImage> listArchived(Db& db, const std::string& archiveFile, size_t limit);
// 選んだものを消す・before（Unix 秒）より前に保管したものを消す。消した数を返す
size_t deleteArchived(Db& db, const std::string& archiveFile, const std::vector<int64_t>& aids);
size_t deleteArchivedBefore(Db& db, const std::string& archiveFile, int64_t before);

// 動かした・名前を変えた画像を見分けて引き継ぐ（データ保護の実装仕様 2.1）。scanAndRegister が、登録する前に呼ぶ。
// files: 今回見つけたファイル、under: 見つからない画像を探すフォルダ（今つながっている調べるフォルダ）。
// 「見つからない画像」と同じもの（大きさ・更新日時が同じで、ファイル ID・SHA-256・ファイル名のどれかで確かめる）は、
// その画像のパスを書き換える（解析をやり直さない）。保管にあるものと同じなら、保管から戻す（タグ・段階・CLIP のベクトル）
struct MoveReport {
    size_t moved = 0, restored = 0;
};
MoveReport matchMovedImages(Db& db, const std::vector<ScannedFile>& files, const std::vector<std::string>& under,
                            const std::string& archiveFile, const std::string& vectorFile);

// 調べるフォルダの場所を付け替える（ドライブレターが変わった時など。仕様 2.2）: oldRoot の下の画像のパスを newRoot の下に書き換える。
// 書き換え先に別の画像の行が既にあれば（新しい場所を先に調べた）、手動の確定をこちらに移してから、その行を消す。書き換えた数を返す
size_t moveRootPaths(Db& db, const std::string& oldRoot, const std::string& newRoot);

}  // namespace imgdupe
