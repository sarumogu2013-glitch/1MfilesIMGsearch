// アップデートの確認・ダウンロード・exe の差し替え（Windows 専用）。
// どの関数も UI を持たないので、別スレッドから呼んでよい。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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

// URL の中身を取る（models.json など。最大 maxBytes）
struct FetchResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::wstring error;
};
FetchResult fetchText(const std::wstring& url, size_t maxBytes = 4 * 1024 * 1024);

// GET（body が nullptr）か POST。fetchText と違い、HTTP の応答があれば 200 以外でも ok = true（status を見る）
FetchResult request(const std::wstring& url, const std::vector<std::wstring>& headers, const std::string* body = nullptr,
                    size_t maxBytes = 4 * 1024 * 1024);

// 大きなファイルを path にダウンロードする（path.part に書いて、大きさと SHA-256 を確かめてから置き換える）。
// progress(受け取ったバイト数) が false を返したら中止する
DownloadResult downloadFile(const std::wstring& url, const std::wstring& path, const std::string& sha256, uint64_t size,
                            const std::function<bool(uint64_t)>& progress);

// ファイルの SHA-256（16 進の小文字。読めなければ空）。progress(読んだバイト数) が false を返したら中止して空
std::string fileSha256(const std::wstring& path, const std::function<bool(uint64_t)>& progress = {});

// 前回の差し替えで残った <exe>.old を消す（起動時に呼ぶ）。
void cleanupOldExe();

std::wstring exePath();

}  // namespace upd
