// 最小限の JSON パーサ（GitHub API の応答を読むため）。
#pragma once

#include <map>
#include <string>
#include <vector>

namespace imgdupe {

// GitHub API の応答を読むための最小限の JSON の値。
struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    const Json* get(const std::string& key) const {
        auto it = obj.find(key);
        return (type == Object && it != obj.end()) ? &it->second : nullptr;
    }
    std::string getString(const std::string& key) const {
        const Json* v = get(key);
        return (v && v->type == String) ? v->str : std::string();
    }
};

// JSON として読めなければ false。
bool parseJson(const std::string& text, Json& out);

// JSON の文字列として書き出す（"..." とエスケープ込み）。
void jsonString(std::string& out, const std::string& s);

}  // namespace imgdupe
