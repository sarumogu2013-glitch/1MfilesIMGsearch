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

}  // namespace imgdupe
