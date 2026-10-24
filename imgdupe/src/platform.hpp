// ファイルを開く・置き換えるなど、OS で書き方が違う小物（UTF-8 のパスを受け取る）。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace imgdupe {

FILE* openFile(const std::string& path, const char* mode);
// tmp を dst に置き換える（dst があれば上書き）
bool replaceFile(const std::string& tmp, const std::string& dst);
// 2 GB を超える位置へも動ける fseek / ftell
bool seekFile(FILE* f, uint64_t pos);
int64_t tellFile(FILE* f);
// ファイルの大きさ。無ければ -1
int64_t fileSize(const std::string& path);

}  // namespace imgdupe
