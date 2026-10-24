// platform.hpp の POSIX 版（テスト用）
#include <sys/stat.h>

#include <cstdio>

#include "platform.hpp"

namespace imgdupe {

FILE* openFile(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }

bool replaceFile(const std::string& tmp, const std::string& dst) { return std::rename(tmp.c_str(), dst.c_str()) == 0; }

bool seekFile(FILE* f, uint64_t pos) { return fseeko(f, static_cast<off_t>(pos), SEEK_SET) == 0; }

int64_t tellFile(FILE* f) { return static_cast<int64_t>(ftello(f)); }

int64_t fileSize(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_size) : -1;
}

}  // namespace imgdupe
