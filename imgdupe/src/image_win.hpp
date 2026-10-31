// WIC での画像の読み込み（特徴量・サムネイル）とフォルダの走査（Windows 専用）。
#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

#include "core.hpp"
#include "meta.hpp"

namespace imgwin {

// 画像を読むスレッドの始めと終わりに呼ぶ（COM と WIC の用意・後片付け）
void threadBegin();
void threadEnd();

imgdupe::Features decodeFeatures(const std::wstring& path);

// box×box に収まるよう縮めて中央に置いた 32bpp の DIB（背景は bg）。読めなければ nullptr
HBITMAP loadThumbnail(const std::wstring& path, int box, COLORREF bg);

// パックに入れたサムネイル（JPEG など）から、box×box の DIB を作る
HBITMAP thumbFromBytes(const std::vector<uint8_t>& bytes, int box, COLORREF bg);

// パックに入れるサムネイル: 向きを直し、一辺 maxSide 以内に縮めた JPEG
bool encodeThumbnail(const std::wstring& path, int maxSide, std::vector<uint8_t>& jpeg, int& width, int& height);

// 向きを直し、box×box に収まるよう縮めた 32bpp の PBGRA の画素（AI の解析用）。読めなければ false。
// origW / origH があれば元の画像の大きさ
bool decodeBgra(const std::wstring& path, int box, std::vector<uint8_t>& px, int& w, int& h, int* origW = nullptr,
                int* origH = nullptr);

// メタデータ（形式・EXIF・XMP/IPTC のタグや星・PNG のテキスト・NTFS の情報・ダウンロード元）を読む。
// ファイルが無ければ false
bool readFileMeta(const std::wstring& path, imgdupe::FileMeta& out);

// 走査から外すフォルダ（ハードリンクの生成先を設定で変えたもの）。名前が _vfolders のフォルダはいつも外す
void setScanExcludes(const std::vector<std::string>& dirs);

// root 以下の画像を out に追加する（リパースポイントのフォルダ、_vfolders、外すフォルダはたどらない）
// ゴミ箱（$Recycle.Bin）・システムの復元（System Volume Information）の名前か / パスがその中か（スキャンしない）
bool isSystemDirName(const std::wstring& name);
bool inSystemDir(const std::wstring& path);
void scanFolder(const std::wstring& root, std::vector<imgdupe::ScannedFile>& out, std::atomic<uint64_t>& found,
                const std::atomic<bool>& cancel);

}  // namespace imgwin
