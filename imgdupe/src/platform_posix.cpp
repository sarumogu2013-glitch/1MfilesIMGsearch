// platform.hpp の POSIX 版（テスト用）
#include <cstdio>

#include "platform.hpp"

namespace imgdupe {

FILE* openFile(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }

bool replaceFile(const std::string& tmp, const std::string& dst) { return std::rename(tmp.c_str(), dst.c_str()) == 0; }

}  // namespace imgdupe
