// ファイルを開く・置き換えるなど、OS で書き方が違う小物（UTF-8 のパスを受け取る）。
#pragma once

#include <cstdio>
#include <string>

namespace imgdupe {

FILE* openFile(const std::string& path, const char* mode);
// tmp を dst に置き換える（dst があれば上書き）
bool replaceFile(const std::string& tmp, const std::string& dst);

}  // namespace imgdupe
