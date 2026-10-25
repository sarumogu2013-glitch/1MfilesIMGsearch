#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX

#include "lookup_win.hpp"

#include <windows.h>

#include <algorithm>
#include <ctime>

#include "pack.hpp"
#include "update_win.hpp"
#include "win_util.hpp"

using namespace imgdupe;
using winutil::narrow;
using winutil::widen;

namespace lookup {

namespace {

std::wstring iniPath(const std::wstring& dataDir) { return dataDir + L"\\imgdupe.ini"; }

std::string envOr(const wchar_t* name, const char* def) {
    wchar_t buf[512] = L"";
    const DWORD n = GetEnvironmentVariableW(name, buf, 512);
    return n && n < 512 ? narrow(buf) : std::string(def);
}

HttpResult toResult(const upd::FetchResult& f) {
    HttpResult r;
    r.ok = f.ok;
    r.status = f.status;
    r.body = f.body;
    r.error = narrow(f.error);
    return r;
}

std::vector<std::wstring> wideHeaders(const std::vector<std::string>& h) {
    std::vector<std::wstring> out;
    for (const auto& x : h) out.push_back(widen(x));
    return out;
}

}  // namespace

Settings load(const std::wstring& dataDir) {
    const std::wstring ini = iniPath(dataDir);
    Settings s;
    s.idLookup = GetPrivateProfileIntW(L"imgdupe", L"lookupEnabled", 0, ini.c_str()) != 0;
    s.reverse = GetPrivateProfileIntW(L"imgdupe", L"sauceEnabled", 0, ini.c_str()) != 0;
    s.linkCheck = GetPrivateProfileIntW(L"imgdupe", L"linkCheckEnabled", 0, ini.c_str()) != 0;
    wchar_t key[256] = L"";
    GetPrivateProfileStringW(L"imgdupe", L"sauceKey", L"", key, 256, ini.c_str());
    s.sauceKey = key;
    s.dailyLimit = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"imgdupe", L"sauceDailyLimit", kSauceNaoDefaultDailyLimit, ini.c_str())),
                              0, 100000);
    return s;
}

void save(const std::wstring& dataDir, const Settings& s) {
    const std::wstring ini = iniPath(dataDir);
    WritePrivateProfileStringW(L"imgdupe", L"lookupEnabled", s.idLookup ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"imgdupe", L"sauceEnabled", s.reverse ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"imgdupe", L"linkCheckEnabled", s.linkCheck ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"imgdupe", L"sauceKey", s.sauceKey.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"imgdupe", L"sauceDailyLimit", std::to_wstring(s.dailyLimit).c_str(), ini.c_str());
}

LookupReport run(const std::wstring& dataDir, Db& db, const std::atomic<bool>& cancel, Progress* progress) {
    const Settings s = load(dataDir);
    LookupOptions opt;
    opt.idLookup = s.idLookup;
    opt.reverse = s.reverse;
    opt.linkCheck = s.linkCheck;
    opt.sauceKey = narrow(s.sauceKey);
    opt.sauceDailyLimit = s.dailyLimit;
    opt.danbooruUrl = envOr(L"IMGDUPE_DANBOORU_URL", "https://danbooru.donmai.us");
    opt.pixivUrl = envOr(L"IMGDUPE_PIXIV_URL", "https://www.pixiv.net");
    opt.sauceUrl = envOr(L"IMGDUPE_SAUCENAO_URL", "https://saucenao.com");
    opt.twitterUrl = envOr(L"IMGDUPE_TWITTER_URL", "https://publish.twitter.com");
    if (!opt.idLookup && !opt.linkCheck && !(opt.reverse && !opt.sauceKey.empty())) return {};

    ThumbPack pack(narrow(dataDir));
    LookupNet net;
    net.get = [](const std::string& url, const std::vector<std::string>& headers) {
        return toResult(upd::request(widen(url), wideHeaders(headers)));
    };
    net.post = [](const std::string& url, const std::vector<std::string>& headers, const std::string& body) {
        return toResult(upd::request(widen(url), wideHeaders(headers), &body));
    };
    net.thumbnail = [&](int64_t id, std::string& jpeg) {
        ThumbData t;
        if (!pack.get(db, id, t) || t.format != ThumbFormat::Jpeg) return false;
        jpeg.assign(t.bytes.begin(), t.bytes.end());
        return true;
    };
    net.sleep = [](int ms) { Sleep(static_cast<DWORD>(ms)); };
    net.now = [] { return static_cast<int64_t>(std::time(nullptr)); };
    return runLookups(db, opt, net, cancel, progress);
}

}  // namespace lookup
