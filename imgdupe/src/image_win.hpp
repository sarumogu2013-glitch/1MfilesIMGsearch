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

// root 以下の画像を out に追加する（リパースポイントのフォルダはたどらない）
void scanFolder(const std::wstring& root, std::vector<imgdupe::ScannedFile>& out, std::atomic<uint64_t>& found,
                const std::atomic<bool>& cancel);

}  // namespace imgwin
