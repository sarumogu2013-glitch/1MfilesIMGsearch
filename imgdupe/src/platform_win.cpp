// platform.hpp の Windows 版
#include <windows.h>

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

}  // namespace imgdupe
