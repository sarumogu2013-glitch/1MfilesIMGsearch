// 動作履歴（ログ）を Windows で始める・終える小物: 起動の様子、前回きちんと終わったか、落ちた時の例外を書く。
#pragma once

#include <windows.h>

#include <cstdio>
#include <exception>
#include <string>
#include <thread>

#include "log.hpp"
#include "version.hpp"
#include "win_util.hpp"

namespace logwin {

inline std::wstring& markerPath() {
    static std::wstring p;
    return p;
}

// 落ちた時: 例外の種類と場所（モジュールと、その中の位置）を書く
inline LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    char t[512];
    const auto* r = ep ? ep->ExceptionRecord : nullptr;
    const void* addr = r ? r->ExceptionAddress : nullptr;
    char module[MAX_PATH] = "?";
    uintptr_t offset = 0;
    HMODULE mod = nullptr;
    if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCSTR>(addr), &mod)) {
        GetModuleFileNameA(mod, module, MAX_PATH);
        offset = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
    }
    std::snprintf(t, sizeof t, "落ちました: 例外 0x%08lX、場所 %s+0x%llX（スレッド %lu）", r ? r->ExceptionCode : 0UL, module,
                  static_cast<unsigned long long>(offset), GetCurrentThreadId());
    imgdupe::logs::error(t);
    return EXCEPTION_CONTINUE_SEARCH;
}

inline std::string windowsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof(v)};
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto f = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")))) f(&v);
    char t[64];
    std::snprintf(t, sizeof t, "%lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return t;
}

// <dataDir>\logs\<name>.log に書き始める。前回きちんと終わらなかったら（<name>.running が残っていれば）そう書く
inline void start(const std::wstring& dataDir, const char* name, const char* who) {
    const std::wstring dir = dataDir + L"\\logs";
    imgdupe::logs::open(winutil::narrow(dir), name, who);
    markerPath() = dir + L"\\" + winutil::widen(name) + L".running";
    if (GetFileAttributesW(markerPath().c_str()) != INVALID_FILE_ATTRIBUTES)
        imgdupe::logs::warn("前回はきちんと終わりませんでした（落ちた・強制終了・電源が切れた など）");
    if (HANDLE h = CreateFileW(markerPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    wchar_t exe[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    char t[256];
    std::snprintf(t, sizeof t, "起動: imgdupe %s（Windows %s、CPU %u コア、メモリ %.1f GB、プロセス %lu）", IMGDUPE_VERSION,
                  windowsVersion().c_str(), std::thread::hardware_concurrency(), double(ms.ullTotalPhys) / (1024.0 * 1024 * 1024),
                  GetCurrentProcessId());
    imgdupe::logs::info(t);
    imgdupe::logs::info("exe: " + winutil::narrow(exe) + "、データ: " + winutil::narrow(dataDir));
    SetUnhandledExceptionFilter(crashFilter);
    std::set_terminate([] {
        imgdupe::logs::error("落ちました: 処理しきれない C++ の例外（std::terminate）");
        std::abort();
    });
}

// きちんと終わった
inline void stop() {
    imgdupe::logs::info("終了");
    if (!markerPath().empty()) DeleteFileW(markerPath().c_str());
    imgdupe::logs::close();
}

}  // namespace logwin
