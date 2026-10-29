// 動作履歴（ログ）。変な動きをした時に後から読めるように、起動・段階・中断・エラーなどを日時つきでファイルに残す。
// <dataDir>\logs\<名前>.log。大きくなったら <名前>.1.log … に回して、古いものから消す。OS に依存しない（UTF-8 のパス）。
#pragma once

#include <cstdint>
#include <string>

namespace imgdupe::logs {

enum class Level { Info, Warn, Error };

// 書き始める（dir は無ければ作る）。who は行の頭に付ける名前（"画面" / "解析" など）。
// maxBytes を超えたら回し、keep 個まで残す
void open(const std::string& dir, const std::string& name, const std::string& who, uint64_t maxBytes = 4u << 20, int keep = 4);
bool isOpen();
void write(Level level, const std::string& message);
inline void info(const std::string& m) { write(Level::Info, m); }
inline void warn(const std::string& m) { write(Level::Warn, m); }
inline void error(const std::string& m) { write(Level::Error, m); }
void close();
std::string path();  // 今書いているファイル

// 行の形（テスト用）: "2026-10-29 12:34:56.789 [解析] 注意: メッセージ"
std::string formatLine(int64_t unixMillis, const std::string& who, Level level, const std::string& message);
// 回す時の名前: name.log → name.1.log → name.2.log …
std::string rotatedName(const std::string& dir, const std::string& name, int index);

}  // namespace imgdupe::logs
