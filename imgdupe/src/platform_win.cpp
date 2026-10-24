// platform.hpp の Windows 版
#include <windows.h>

#include <algorithm>
#include <cstdio>

#include "platform.hpp"
#include "win_util.hpp"

namespace imgdupe {

FILE* openFile(const std::string& path, const char* mode) {
    return _wfopen(winutil::widen(path).c_str(), winutil::widen(mode).c_str());
}

bool replaceFile(const std::string& tmp, const std::string& dst) {
    return MoveFileExW(winutil::widen(tmp).c_str(), winutil::widen(dst).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool seekFile(FILE* f, uint64_t pos) { return _fseeki64(f, static_cast<__int64>(pos), SEEK_SET) == 0; }

int64_t tellFile(FILE* f) { return _ftelli64(f); }

int64_t fileSize(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(winutil::widen(path).c_str(), GetFileExInfoStandard, &d)) return -1;
    return (static_cast<int64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

bool fileIdentity(const std::string& path, FileIdentity& out) {
    HANDLE h = CreateFileW(winutil::widen(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION fi;
    const bool ok = GetFileInformationByHandle(h, &fi) && !(fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    CloseHandle(h);
    if (!ok) return false;
    out.volume = fi.dwVolumeSerialNumber;
    out.index = (static_cast<uint64_t>(fi.nFileIndexHigh) << 32) | fi.nFileIndexLow;
    out.links = fi.nNumberOfLinks;
    return true;
}

bool pathExists(const std::string& path) { return GetFileAttributesW(winutil::widen(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

bool createHardLink(const std::string& existing, const std::string& link) {
    return CreateHardLinkW(winutil::widen(link).c_str(), winutil::widen(existing).c_str(), nullptr) != 0;
}

bool removeFile(const std::string& path) {
    const std::wstring w = winutil::widen(path);
    if (DeleteFileW(w.c_str())) return true;
    // 読み取り専用なら外してもう一度（属性はリンクどうしで共通なので、消せなければ戻す）
    const DWORD attr = GetFileAttributesW(w.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_READONLY)) return false;
    SetFileAttributesW(w.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
    if (DeleteFileW(w.c_str())) return true;
    SetFileAttributesW(w.c_str(), attr);
    return false;
}

bool makeDirs(const std::string& dir) {
    const std::wstring w = winutil::widen(dir);
    for (size_t i = 1; i <= w.size(); ++i) {
        if (i < w.size() && w[i] != L'\\' && w[i] != L'/') continue;
        const std::wstring part = w.substr(0, i);
        if (part.size() <= 3 && part.size() >= 2 && part[1] == L':') continue;  // ドライブ（C:、C:\）
        if (part.size() >= 2 && part[0] == L'\\' && part[1] == L'\\' && std::count(part.begin(), part.end(), L'\\') < 4)
            continue;  // \\server\share
        const DWORD attr = GetFileAttributesW(part.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) {
            if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
        } else if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return true;
}

bool removeEmptyDir(const std::string& dir) { return RemoveDirectoryW(winutil::widen(dir).c_str()) != 0; }

bool truncateFile(const std::string& path, uint64_t size) {
    HANDLE f = CreateFileW(winutil::widen(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(size);
    const bool ok = SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) && SetEndOfFile(f);
    CloseHandle(f);
    return ok;
}

bool MappedFile::open(const std::string& path) {
    close();
    // 書いている途中のファイルも読めるように、共有の指定は広く
    HANDLE f = CreateFileW(winutil::widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0) {
        CloseHandle(f);
        return false;
    }
    HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(f);
    if (!map) return false;
    const void* p = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    if (!p) {
        CloseHandle(map);
        return false;
    }
    data_ = static_cast<const uint8_t*>(p);
    size_ = static_cast<uint64_t>(sz.QuadPart);
    handle_ = map;
    return true;
}

void MappedFile::close() {
    if (data_) UnmapViewOfFile(data_);
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
    data_ = nullptr;
    size_ = 0;
    handle_ = nullptr;
}

}  // namespace imgdupe
