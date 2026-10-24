#include "json.hpp"

#include <cstdint>

namespace imgdupe {

namespace {

// GitHub API の応答を読むための最小限の JSON パーサ。

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    bool parse(Json& out) {
        if (!value(out, 0)) return false;
        ws();
        return i_ == s_.size();
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool lit(const char* word) {
        const size_t n = std::char_traits<char>::length(word);
        if (s_.compare(i_, n, word) != 0) return false;
        i_ += n;
        return true;
    }
    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(uint32_t& v) {
        if (i_ + 4 > s_.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && s_.compare(i_, 2, "\\u") == 0) {
                        i_ += 2;
                        uint32_t lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(Json& out, int depth) {
        if (depth > 64) return false;
        ws();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{') {
            ++i_;
            out.type = Json::Object;
            ws();
            if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
            while (true) {
                ws();
                std::string key;
                if (!string(key)) return false;
                ws();
                if (i_ >= s_.size() || s_[i_++] != ':') return false;
                Json v;
                if (!value(v, depth + 1)) return false;
                out.obj[key] = std::move(v);
                ws();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                if (s_[i_] == '}') return ++i_, true;
                return false;
            }
        }
        if (c == '[') {
            ++i_;
            out.type = Json::Array;
            ws();
            if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
            while (true) {
                Json v;
                if (!value(v, depth + 1)) return false;
                out.arr.push_back(std::move(v));
                ws();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                if (s_[i_] == ']') return ++i_, true;
                return false;
            }
        }
        if (c == '"') {
            out.type = Json::String;
            return string(out.str);
        }
        if (lit("true")) return out.type = Json::Bool, out.str = "true", true;
        if (lit("false")) return out.type = Json::Bool, out.str = "false", true;
        if (lit("null")) return out.type = Json::Null, true;
        // 数値（値そのものは使わないので範囲だけ読む）
        const size_t start = i_;
        while (i_ < s_.size() && std::string("+-0123456789.eE").find(s_[i_]) != std::string::npos) ++i_;
        if (i_ == start) return false;
        out.type = Json::Number;
        out.str = s_.substr(start, i_ - start);
        return true;
    }
};


}  // namespace

bool parseJson(const std::string& text, Json& out) { return Parser(text).parse(out); }

void jsonString(std::string& out, const std::string& s) {
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[c >> 4];
                    out += kHex[c & 15];
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
}

}  // namespace imgdupe
