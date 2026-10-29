#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

#include "platform.hpp"

namespace imgdupe::logs {

namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::string g_dir, g_name, g_who;
uint64_t g_max = 0;
int g_keep = 0;
uint64_t g_size = 0;

std::string join(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    const char last = dir.back();
    if (last == '\\' || last == '/') return dir + file;
    // 区切りはフォルダの書き方にそろえる（Windows は \、テストの POSIX は /）
    return dir + (dir.find('\\') == std::string::npos && dir.find('/') != std::string::npos ? "/" : "\\") + file;
}

const char* levelText(Level l) {
    switch (l) {
        case Level::Warn: return "注意: ";
        case Level::Error: return "エラー: ";
        default: return "";
    }
}

int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// 大きくなった: name.log を name.1.log に（前の 1 は 2 に …、keep を超えた分は消す）
void rotateLocked() {
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
    removeFile(rotatedName(g_dir, g_name, g_keep));
    for (int i = g_keep - 1; i >= 0; --i) {
        const std::string from = rotatedName(g_dir, g_name, i);
        if (pathExists(from)) replaceFile(from, rotatedName(g_dir, g_name, i + 1));
    }
    g_file = openFile(rotatedName(g_dir, g_name, 0), "ab");
    g_size = 0;
}

}  // namespace

std::string rotatedName(const std::string& dir, const std::string& name, int index) {
    return join(dir, index == 0 ? name + ".log" : name + "." + std::to_string(index) + ".log");
}

std::string formatLine(int64_t unixMillis, const std::string& who, Level level, const std::string& message) {
    const std::time_t t = static_cast<std::time_t>(unixMillis / 1000);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char head[64];
    std::snprintf(head, sizeof head, "%04d-%02d-%02d %02d:%02d:%02d.%03d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour,
                  lt.tm_min, lt.tm_sec, static_cast<int>(unixMillis % 1000));
    std::string line = std::string(head) + " [" + who + "] " + levelText(level) + message;
    // 1 行に収める（改行は空白に）
    for (char& c : line)
        if (c == '\r' || c == '\n') c = ' ';
    return line + "\r\n";
}

void open(const std::string& dir, const std::string& name, const std::string& who, uint64_t maxBytes, int keep) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_dir = dir;
    g_name = name;
    g_who = who;
    g_max = maxBytes;
    g_keep = std::max(1, keep);
    makeDirs(dir);
    const std::string p = rotatedName(dir, name, 0);
    const int64_t size = fileSize(p);
    g_size = size > 0 ? static_cast<uint64_t>(size) : 0;
    g_file = openFile(p, "ab");
    if (g_file && g_size == 0) std::fwrite("\xEF\xBB\xBF", 1, 3, g_file);  // メモ帳でも UTF-8 と分かるように
}

bool isOpen() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_file != nullptr;
}

void write(Level level, const std::string& message) {
    const std::string line = formatLine(nowMillis(), g_who, level, message);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    if (g_max && g_size + line.size() > g_max) rotateLocked();
    if (!g_file) return;
    std::fwrite(line.data(), 1, line.size(), g_file);
    std::fflush(g_file);  // 落ちた時にも残るように
    g_size += line.size();
}

void close() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

std::string path() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return rotatedName(g_dir, g_name, 0);
}

}  // namespace imgdupe::logs
