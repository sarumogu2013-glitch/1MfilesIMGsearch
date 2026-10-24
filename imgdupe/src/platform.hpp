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

// ハードリンク（フォルダへの展開）に使う小物
struct FileIdentity {
    uint64_t volume = 0, index = 0;  // 同じなら同じ中身（ハードリンクどうし）
    uint32_t links = 0;              // ハードリンクの数
    bool same(const FileIdentity& o) const { return volume == o.volume && index == o.index; }
};
bool fileIdentity(const std::string& path, FileIdentity& out);  // 無ければ false
bool pathExists(const std::string& path);
bool createHardLink(const std::string& existing, const std::string& link);  // link のフォルダは先に作っておく
bool removeFile(const std::string& path);
bool makeDirs(const std::string& dir);       // 途中のフォルダもまとめて作る（あれば true）
bool removeEmptyDir(const std::string& dir);  // 空の時だけ消える

}  // namespace imgdupe
