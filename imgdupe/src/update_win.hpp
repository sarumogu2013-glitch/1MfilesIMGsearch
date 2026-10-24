// アップデートの確認・ダウンロード・exe の差し替え（Windows 専用）。
// どの関数も UI を持たないので、別スレッドから呼んでよい。
#pragma once

#include <string>

#include "update_logic.hpp"

namespace upd {

struct CheckResult {
    std::wstring transportError;  // 接続できなかった理由。空なら HTTP の応答があった
    int status = 0;
    bool parsed = false;
    imgdupe::ReleaseInfo info;

    // 人向けのエラー文。確認できた時は空。
    std::wstring error() const;
};

// GET https://api.github.com/repos/<APP_REPOSITORY>/releases/latest
CheckResult checkLatest();

struct DownloadResult {
    bool ok = false;
    std::wstring error;
};

// imgdupe.exe をダウンロードして確かめ、実行中の exe を差し替える（次の起動から新しい版）。
DownloadResult downloadAndReplace(const imgdupe::ReleaseInfo& info);

// 前回の差し替えで残った <exe>.old を消す（起動時に呼ぶ）。
void cleanupOldExe();

std::wstring exePath();

}  // namespace upd
