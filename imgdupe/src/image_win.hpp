// WIC での画像の読み込み（特徴量・サムネイル）とフォルダの走査（Windows 専用）。
#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

#include "core.hpp"

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

// root 以下の画像を out に追加する（リパースポイントのフォルダはたどらない）
void scanFolder(const std::wstring& root, std::vector<imgdupe::ScannedFile>& out, std::atomic<uint64_t>& found,
                const std::atomic<bool>& cancel);

}  // namespace imgwin
