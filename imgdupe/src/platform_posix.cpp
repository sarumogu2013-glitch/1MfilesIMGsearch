// platform.hpp の POSIX 版（テスト用）
#include <sys/stat.h>
#include <unistd.h>

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

bool fileIdentity(const std::string& path, FileIdentity& out) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    out.volume = static_cast<uint64_t>(st.st_dev);
    out.index = static_cast<uint64_t>(st.st_ino);
    out.links = static_cast<uint32_t>(st.st_nlink);
    return true;
}

bool pathExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool createHardLink(const std::string& existing, const std::string& link) { return ::link(existing.c_str(), link.c_str()) == 0; }

bool removeFile(const std::string& path) { return ::unlink(path.c_str()) == 0; }

bool makeDirs(const std::string& dir) {
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i < dir.size() && dir[i] != '/') continue;
        const std::string part = dir.substr(0, i);
        struct stat st;
        if (stat(part.c_str(), &st) == 0) {
            if (!S_ISDIR(st.st_mode)) return false;
        } else if (mkdir(part.c_str(), 0777) != 0) {
            return false;
        }
    }
    return true;
}

bool removeEmptyDir(const std::string& dir) { return ::rmdir(dir.c_str()) == 0; }

}  // namespace imgdupe
