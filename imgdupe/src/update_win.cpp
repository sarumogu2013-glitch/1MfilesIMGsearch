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

#include <functional>
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

// 受け取ったデータを渡す先（false を返したら中止）。無ければ body にためる
using Sink = std::function<bool(const char*, size_t)>;

// リダイレクトを追わずに 1 回だけ送る（body が無ければ GET、あれば POST）。
Response requestOnce(const std::wstring& url, const std::vector<std::wstring>& headers, size_t maxBytes,
                     const Sink* sink = nullptr, const std::string* body = nullptr) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return fail(GetLastError());
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    // テスト用に手元のサーバーだけ http を許す
    const bool local = host == L"127.0.0.1" || host == L"localhost";
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && !(local && uc.nScheme == INTERNET_SCHEME_HTTP))
        return failText(L"HTTPS 以外の URL は使えません");
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
    Handle request(WinHttpOpenRequest(connect.h, body ? L"POST" : L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request.h) return fail(GetLastError());
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    std::wstring headerText;
    for (const auto& h : headers) headerText += h + L"\r\n";
    const BOOL sent = WinHttpSendRequest(request.h, headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
                                         static_cast<DWORD>(-1L),
                                         body ? const_cast<char*>(body->data()) : WINHTTP_NO_REQUEST_DATA,
                                         body ? static_cast<DWORD>(body->size()) : 0, body ? static_cast<DWORD>(body->size()) : 0, 0);
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
        if (sink && res.status == 200) {
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(request.h, buf.data(), avail, &read)) return fail(GetLastError());
            if (!(*sink)(buf.data(), read)) return failText(L"中止しました");
            continue;
        }
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
Response get(std::wstring url, const std::vector<std::wstring>& headers, size_t maxBytes, const Sink* sink = nullptr) {
    for (int i = 0; i <= kMaxRedirects; ++i) {
        Response res = requestOnce(url, headers, maxBytes, sink);
        if (res.err || res.status < 300 || res.status >= 400 || res.location.empty()) return res;
        url = widen(imgdupe::resolveRedirect(narrow(url), narrow(res.location)));  // Hugging Face は相対の Location を返す
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

// 少しずつ SHA-256 を計算する
class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) alg_ = nullptr;
        if (alg_ && BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) != 0) hash_ = nullptr;
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    void add(const char* p, size_t n) {
        if (hash_) BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(p)), static_cast<ULONG>(n), 0);
    }
    std::string hex() {
        unsigned char digest[32];
        if (!hash_ || BCryptFinishHash(hash_, digest, sizeof(digest), 0) != 0) return {};
        static const char* kHex = "0123456789abcdef";
        std::string out;
        for (unsigned char b : digest) {
            out += kHex[b >> 4];
            out += kHex[b & 15];
        }
        return out;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

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
    // 前の更新の .old をまだ使っているもの（古い版のバックグラウンドの解析など）があると消せないので、その時は別の名前にする
    std::wstring oldPath = exe + L".old";
    for (int i = 1; i <= 50 && !DeleteFileW(oldPath.c_str()) && GetFileAttributesW(oldPath.c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
        oldPath = exe + L".old" + std::to_wstring(i);

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

FetchResult fetchText(const std::wstring& url, size_t maxBytes) {
    FetchResult r;
    Response res = get(url, {}, maxBytes);
    if (res.err) {
        r.error = L"接続できませんでした: " + res.errText;
        return r;
    }
    r.status = res.status;
    if (res.status != 200) {
        r.error = widen(imgdupe::httpErrorMessage(res.status));
        return r;
    }
    r.ok = true;
    r.body = std::move(res.body);
    return r;
}

FetchResult request(const std::wstring& url, const std::vector<std::wstring>& headers, const std::string* body, size_t maxBytes) {
    FetchResult r;
    // POST はリダイレクトを追わない（送り直すことになるので）
    Response res = body ? requestOnce(url, headers, maxBytes, nullptr, body) : get(url, headers, maxBytes);
    if (res.err) {
        r.error = L"接続できませんでした: " + res.errText;
        return r;
    }
    r.ok = true;
    r.status = res.status;
    r.body = std::move(res.body);
    return r;
}

std::string fileSha256(const std::wstring& path, const std::function<bool(uint64_t)>& progress) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    Sha256 h;
    std::vector<char> buf(1 << 20);
    uint64_t done = 0;
    DWORD n = 0;
    bool ok = true;
    while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0) {
        h.add(buf.data(), n);
        done += n;
        if (progress && !progress(done)) {
            ok = false;
            break;
        }
    }
    CloseHandle(f);
    return ok ? h.hex() : std::string();
}

DownloadResult downloadFile(const std::wstring& url, const std::wstring& path, const std::string& sha256, uint64_t size,
                            const std::function<bool(uint64_t)>& progress) {
    DownloadResult result;
    const std::wstring part = path + L".part";
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        result.error = L"保存できませんでした: " + path;
        return result;
    }
    Sha256 hash;
    uint64_t received = 0;
    bool writeFailed = false;
    const Sink sink = [&](const char* p, size_t n) {
        DWORD written = 0;
        if (!WriteFile(f, p, static_cast<DWORD>(n), &written, nullptr) || written != n) {
            writeFailed = true;
            return false;
        }
        hash.add(p, n);
        received += n;
        if (size && received > size) return false;  // 一覧と大きさが違う
        return !progress || progress(received);
    };
    Response res = get(url, {L"Accept: application/octet-stream"}, kMaxApiBytes, &sink);
    CloseHandle(f);
    auto failWith = [&](const std::wstring& msg) {
        DeleteFileW(part.c_str());
        result.error = msg;
        return result;
    };
    if (writeFailed) return failWith(L"保存できませんでした（ディスクの空きを確認してください）: " + path);
    if (res.err) return failWith(res.errText == L"中止しました" ? res.errText : L"ダウンロードできませんでした: " + res.errText);
    if (res.status != 200) return failWith(L"ダウンロードできませんでした (HTTP " + std::to_wstring(res.status) + L"): " + url);
    if (size && received != size) return failWith(L"ダウンロードしたファイルの大きさが一覧と違います: " + url);
    if (hash.hex() != sha256) return failWith(L"ダウンロードしたファイルの SHA-256 が一致しません: " + url);
    if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return failWith(L"置き換えられませんでした（使用中かもしれません）: " + path);
    result.ok = true;
    return result;
}

// 前の更新で残した古い exe（imgdupe.exe.old、imgdupe.exe.old1 など）を消す（まだ使われていれば次の機会に）
void cleanupOldExe() {
    const std::wstring exe = exePath();
    const size_t sep = exe.find_last_of(L"\\/");
    const std::wstring dir = sep == std::wstring::npos ? L"" : exe.substr(0, sep + 1);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((exe + L".old*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        DeleteFileW((dir + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

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
