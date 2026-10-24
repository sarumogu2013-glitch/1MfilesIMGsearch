#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX

#include "update_win.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <vector>

#include "version.hpp"
#include "win_util.hpp"

namespace upd {

using winutil::narrow;
using winutil::widen;

namespace {

constexpr size_t kMaxApiBytes = 4 * 1024 * 1024;
constexpr size_t kMaxExeBytes = 64 * 1024 * 1024;
constexpr size_t kMinExeBytes = 100 * 1024;
constexpr int kMaxRedirects = 5;

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET x) : h(x) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct Response {
    DWORD err = 0;  // WinHTTP / Win32 のエラー。0 なら HTTP の応答あり
    std::wstring errText;
    int status = 0;
    std::string body;
    std::wstring location;
};

std::wstring winhttpMessage(DWORD err) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE |
                                 FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             GetModuleHandleW(L"winhttp.dll"), err, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring msg = n ? std::wstring(buf, n) : L"";
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ')) msg.pop_back();
    return msg.empty() ? L"エラー " + std::to_wstring(err) : msg + L" (" + std::to_wstring(err) + L")";
}

Response fail(DWORD err) {
    Response r;
    r.err = err ? err : ERROR_WINHTTP_INTERNAL_ERROR;
    r.errText = winhttpMessage(r.err);
    return r;
}

Response failText(const std::wstring& text) {
    Response r;
    r.err = ERROR_WINHTTP_INTERNAL_ERROR;
    r.errText = text;
    return r;
}

std::wstring userAgent() { return L"imgdupe/" + widen(IMGDUPE_VERSION); }

// リダイレクトを追わずに 1 回だけ GET する。
Response requestOnce(const std::wstring& url, const std::vector<std::wstring>& headers, size_t maxBytes) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return fail(GetLastError());
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) return failText(L"HTTPS 以外の URL は使えません");
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    Handle session(WinHttpOpen(userAgent().c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        session.h = WinHttpOpen(userAgent().c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.h) return fail(GetLastError());
    WinHttpSetTimeouts(session.h, 10000, 10000, 10000, 30000);

    Handle connect(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
    if (!connect.h) return fail(GetLastError());
    Handle request(WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.h) return fail(GetLastError());
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    std::wstring headerText;
    for (const auto& h : headers) headerText += h + L"\r\n";
    const BOOL sent = WinHttpSendRequest(request.h, headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
                                         static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(request.h, nullptr)) return fail(GetLastError());

    Response res;
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    res.status = static_cast<int>(status);

    if (res.status >= 300 && res.status < 400) {
        DWORD len = 0;
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &len,
                            WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0) {
            std::wstring loc(len / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc.data(), &len,
                                    WINHTTP_NO_HEADER_INDEX)) {
                loc.resize(len / sizeof(wchar_t));
                res.location = loc;
            }
        }
        return res;
    }

    while (true) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) return fail(GetLastError());
        if (avail == 0) break;
        if (res.body.size() + avail > maxBytes) return failText(L"受信するデータが大きすぎます");
        const size_t old = res.body.size();
        res.body.resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, res.body.data() + old, avail, &read)) return fail(GetLastError());
        res.body.resize(old + read);
    }
    return res;
}

// リダイレクトを自分で追う GET（GitHub の添付ファイルは別のホストへ転送される）。
Response get(std::wstring url, const std::vector<std::wstring>& headers, size_t maxBytes) {
    for (int i = 0; i <= kMaxRedirects; ++i) {
        Response res = requestOnce(url, headers, maxBytes);
        if (res.err || res.status < 300 || res.status >= 400 || res.location.empty()) return res;
        url = res.location;
    }
    return failText(L"リダイレクトが多すぎます");
}

std::string sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32];
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                               static_cast<ULONG>(data.size()), 0) == 0 &&
                BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                static const char* kHex = "0123456789abcdef";
                for (unsigned char b : digest) {
                    hex += kHex[b >> 4];
                    hex += kHex[b & 15];
                }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return hex;
}

std::vector<std::wstring> apiHeaders() {
    return {L"Accept: application/vnd.github+json", L"X-GitHub-Api-Version: 2022-11-28"};
}

}  // namespace

std::wstring CheckResult::error() const {
    if (!transportError.empty()) return L"接続できませんでした: " + transportError;
    if (status != 200) return widen(imgdupe::httpErrorMessage(status));
    if (!parsed) return L"GitHub の応答を解析できません";
    if (!info.hasAsset) return L"最新のリリース " + widen(info.tag) + L" に imgdupe.exe がありません";
    return {};
}

CheckResult checkLatest() {
    CheckResult result;
    const std::wstring url = L"https://api.github.com/repos/" APP_REPOSITORY L"/releases/latest";
    Response res = get(url, apiHeaders(), kMaxApiBytes);
    if (res.err) {
        result.transportError = res.errText;
        return result;
    }
    result.status = res.status;
    if (res.status == 200) result.parsed = imgdupe::parseLatestRelease(res.body, "imgdupe.exe", result.info);
    return result;
}

DownloadResult downloadAndReplace(const imgdupe::ReleaseInfo& info) {
    DownloadResult result;
    Response res = get(widen(info.assetUrl), {L"Accept: application/octet-stream"}, kMaxExeBytes);
    if (res.err) {
        result.error = L"ダウンロードできませんでした: " + res.errText;
        return result;
    }
    if (res.status != 200) {
        result.error = L"ダウンロードできませんでした (HTTP " + std::to_wstring(res.status) + L")";
        return result;
    }
    if (res.body.size() < kMinExeBytes || res.body.compare(0, 2, "MZ") != 0) {
        result.error = L"ダウンロードしたファイルが実行ファイルではありません";
        return result;
    }
    if (!info.sha256.empty() && sha256Hex(res.body) != info.sha256) {
        result.error = L"ダウンロードしたファイルの SHA-256 が一致しません";
        return result;
    }

    const std::wstring exe = exePath();
    const std::wstring newPath = exe + L".new";
    const std::wstring oldPath = exe + L".old";

    HANDLE f = CreateFileW(newPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        result.error = L"新しい版を保存できませんでした。フォルダに書き込み権限があるか確認してください";
        return result;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(f, res.body.data(), static_cast<DWORD>(res.body.size()), &written, nullptr) &&
                    written == res.body.size();
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(newPath.c_str());
        result.error = L"新しい版を保存できませんでした。フォルダに書き込み権限があるか確認してください";
        return result;
    }

    DeleteFileW(oldPath.c_str());
    if (!MoveFileExW(exe.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(newPath.c_str());
        result.error = L"今の exe を改名できませんでした";
        return result;
    }
    if (!MoveFileExW(newPath.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(oldPath.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(newPath.c_str());
        result.error = L"新しい版に差し替えられませんでした";
        return result;
    }
    result.ok = true;
    return result;
}

void cleanupOldExe() { DeleteFileW((exePath() + L".old").c_str()); }

std::wstring exePath() {
    std::wstring buf(MAX_PATH, L'\0');
    while (true) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

}  // namespace upd
