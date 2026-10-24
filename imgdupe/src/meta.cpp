#include "meta.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <regex>

#include "db.hpp"

namespace imgdupe {

namespace {

std::string baseName(const std::string& path) {
    const size_t sep = path.find_last_of("/\\");
    std::string name = sep == std::string::npos ? path : path.substr(sep + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name;
}

std::string latin1ToUtf8(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

// 1970-01-01 からの日数（グレゴリオ暦）
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

struct MetaPayload {
    std::string path;
    FileMeta meta;
};

}  // namespace

std::vector<SourceId> parseFilenameIds(const std::string& fileName) {
    std::vector<SourceId> out;
    const std::string name = baseName(fileName);
    std::smatch m;
    // pixiv: 12345678_p0（ugoira などの後ろの文字は問わない）
    static const std::regex pixiv(R"((?:^|[^0-9])([0-9]{6,10})_p([0-9]{1,4})(?:[^0-9]|$))");
    if (std::regex_search(name, m, pixiv)) out.push_back({"pixiv", m[1].str(), std::stoi(m[2].str())});
    // X（Twitter）のポスト ID: 18〜20 桁の数字
    static const std::regex x(R"((?:^|[^0-9])([0-9]{18,20})(?:[^0-9]|$))");
    if (std::regex_search(name, m, x)) out.push_back({"x", m[1].str(), -1});
    // MD5（Danbooru 系）: 32 文字の 16 進
    static const std::regex md5(R"((?:^|[^0-9a-fA-F])([0-9a-fA-F]{32})(?:[^0-9a-fA-F]|$))");
    if (std::regex_search(name, m, md5)) {
        std::string h = m[1].str();
        for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out.push_back({"md5", h, -1});
    }
    return out;
}

bool parsePng(const uint8_t* d, size_t len, PngInfo& out) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (len < 8 || std::memcmp(d, sig, 8) != 0) return false;
    size_t pos = 8;
    while (pos + 12 <= len) {
        const uint32_t n = be32(d + pos);
        const std::string type(reinterpret_cast<const char*>(d + pos + 4), 4);
        if (n > len - pos - 12) break;  // 途中で切れている（先頭だけ読んだ時）
        const uint8_t* c = d + pos + 8;
        if (type == "acTL") {
            out.animated = true;
        } else if (type == "eXIf") {
            out.exif.assign(c, c + n);
        } else if (type == "tEXt") {
            const auto* z = static_cast<const uint8_t*>(std::memchr(c, 0, n));
            if (z) {
                const std::string key(reinterpret_cast<const char*>(c), static_cast<size_t>(z - c));
                const std::string text(reinterpret_cast<const char*>(z + 1), static_cast<size_t>(c + n - z - 1));
                out.texts.emplace_back(latin1ToUtf8(key), latin1ToUtf8(text));
            }
        } else if (type == "iTXt") {
            // キーワード \0 圧縮フラグ 圧縮方式 言語 \0 翻訳したキーワード \0 文字（UTF-8）
            const auto* z = static_cast<const uint8_t*>(std::memchr(c, 0, n));
            if (z && z + 3 <= c + n && z[1] == 0) {
                const uint8_t* lang = z + 3;
                const auto* z2 = static_cast<const uint8_t*>(std::memchr(lang, 0, static_cast<size_t>(c + n - lang)));
                const auto* z3 = z2 ? static_cast<const uint8_t*>(std::memchr(z2 + 1, 0, static_cast<size_t>(c + n - z2 - 1)))
                                    : nullptr;
                if (z3) {
                    const std::string key(reinterpret_cast<const char*>(c), static_cast<size_t>(z - c));
                    out.texts.emplace_back(latin1ToUtf8(key), std::string(reinterpret_cast<const char*>(z3 + 1),
                                                                           static_cast<size_t>(c + n - z3 - 1)));
                }
            }
        } else if (type == "IEND") {
            break;
        }
        pos += 12 + n;
    }
    return true;
}

namespace {

struct TiffReader {
    const uint8_t* t;
    size_t n;
    bool le;
    uint32_t u16(size_t o) const { return o + 2 > n ? 0 : le ? (t[o] | (t[o + 1] << 8)) : ((t[o] << 8) | t[o + 1]); }
    uint32_t u32(size_t o) const {
        return o + 4 > n ? 0 : le ? (u16(o) | (u16(o + 2) << 16)) : ((u16(o) << 16) | u16(o + 2));
    }
    static size_t typeSize(uint32_t type) {
        switch (type) {
            case 1: case 2: case 6: case 7: return 1;
            case 3: case 8: return 2;
            case 4: case 9: case 11: return 4;
            case 5: case 10: case 12: return 8;
            default: return 0;
        }
    }
    // 項目の値の場所（4 バイトに収まれば項目の中、そうでなければオフセットの先）。無効なら n
    size_t valueAt(size_t e, uint32_t type, uint32_t count) const {
        const size_t sz = typeSize(type) * count;
        if (!sz) return n;
        const size_t at = sz <= 4 ? e + 8 : u32(e + 8);
        return at + sz <= n ? at : n;
    }
};

std::string utf16leToUtf8(const uint8_t* p, size_t bytes) {
    std::string out;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        uint32_t c = p[i] | (p[i + 1] << 8);
        if (!c) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < bytes) {
            const uint32_t lo = p[i + 2] | (p[i + 3] << 8);
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::vector<std::string> splitKeywords(const std::string& s) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(';', a);
        if (b == std::string::npos) b = s.size();
        std::string t = s.substr(a, b - a);
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (!t.empty()) out.push_back(t);
        a = b + 1;
    }
    return out;
}

std::string xmlUnescape(std::string s) {
    static const std::pair<const char*, const char*> ents[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&#xA;", "\n"}, {"&#10;", "\n"}, {"&amp;", "&"}};
    for (const auto& [from, to] : ents) {
        size_t p = 0;
        while ((p = s.find(from, p)) != std::string::npos) {
            s.replace(p, std::strlen(from), to);
            p += std::strlen(to);
        }
    }
    return s;
}

// <tag ...> から </tag> までの中身（無ければ空）
std::string xmlElement(const std::string& xml, const std::string& tag) {
    const size_t a = xml.find("<" + tag);
    if (a == std::string::npos) return {};
    const size_t open = xml.find('>', a);
    if (open == std::string::npos || xml[open - 1] == '/') return {};
    const size_t b = xml.find("</" + tag + ">", open);
    return b == std::string::npos ? std::string() : xml.substr(open + 1, b - open - 1);
}

std::vector<std::string> rdfItems(const std::string& body) {
    std::vector<std::string> out;
    size_t p = 0;
    while ((p = body.find("<rdf:li", p)) != std::string::npos) {
        const size_t open = body.find('>', p);
        const size_t close = body.find("</rdf:li>", open);
        if (open == std::string::npos || close == std::string::npos) break;
        std::string v = xmlUnescape(body.substr(open + 1, close - open - 1));
        if (!v.empty()) out.push_back(v);
        p = close;
    }
    return out;
}

}  // namespace

void parseExif(const uint8_t* t, size_t n, FileMeta& m) {
    if (n < 8 || !((t[0] == 'I' && t[1] == 'I') || (t[0] == 'M' && t[1] == 'M'))) return;
    const TiffReader r{t, n, t[0] == 'I'};
    auto ascii = [&](size_t e, uint32_t type, uint32_t count) {
        const size_t at = r.valueAt(e, type, count);
        if (at >= n || (type != 2 && type != 7)) return std::string();
        std::string s(reinterpret_cast<const char*>(t + at), count);
        s.resize(std::strlen(s.c_str()));  // 終わりの \0
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    };
    auto number = [&](size_t e, uint32_t type, uint32_t count) -> double {
        const size_t at = r.valueAt(e, type, count);
        if (at >= n) return 0;
        switch (type) {
            case 3: return r.u16(at);
            case 4: return r.u32(at);
            case 5: {
                const uint32_t den = r.u32(at + 4);
                return den ? static_cast<double>(r.u32(at)) / den : 0;
            }
            default: return 0;
        }
    };
    uint32_t exifIfd = 0;
    std::string maker, model, dateTime, dateOriginal;
    auto walk = [&](uint32_t ifd, bool exif) {
        const uint32_t count = r.u16(ifd);
        for (uint32_t i = 0; i < count && i < 512; ++i) {
            const size_t e = ifd + 2 + static_cast<size_t>(i) * 12;
            if (e + 12 > n) break;
            const uint32_t tag = r.u16(e), type = r.u16(e + 2), cnt = r.u32(e + 4);
            if (!exif) {
                switch (tag) {
                    case 0x010F: maker = ascii(e, type, cnt); break;
                    case 0x0110: model = ascii(e, type, cnt); break;
                    case 0x0131: m.software = ascii(e, type, cnt); break;
                    case 0x0132: dateTime = ascii(e, type, cnt); break;
                    case 0x8769: exifIfd = r.u32(e + 8); break;
                    case 0x8825: m.hasGps = true; break;
                    case 0x4746: m.rating = std::min(5, static_cast<int>(number(e, type, cnt))); break;
                    case 0x9C9B: case 0x9C9C: case 0x9C9D: case 0x9C9E: case 0x9C9F: {  // XPTitle など（UTF-16LE）
                        const size_t at = r.valueAt(e, type, cnt);
                        if (at >= n) break;
                        const std::string v = utf16leToUtf8(t + at, cnt);
                        if (tag == 0x9C9B) m.title = v;
                        else if (tag == 0x9C9C) m.comment = v;
                        else if (tag == 0x9C9D) m.author = v;
                        else if (tag == 0x9C9E) m.keywords = splitKeywords(v);
                        else m.subject = v;
                        break;
                    }
                }
            } else {
                switch (tag) {
                    case 0x9003: dateOriginal = ascii(e, type, cnt); break;
                    case 0x8827: m.iso = static_cast<int>(number(e, type, cnt)); break;
                    case 0x920A: m.focal = number(e, type, cnt); break;
                    case 0xA434: m.lens = ascii(e, type, cnt); break;
                }
            }
        }
    };
    walk(r.u32(4), false);
    if (exifIfd && exifIfd < n) walk(exifIfd, true);
    m.camera = maker.empty() || model.compare(0, maker.size(), maker) == 0 ? model : maker + " " + model;
    m.taken = parseExifDate(dateOriginal.empty() ? dateTime : dateOriginal);
}

void parseXmp(const std::string& xml, FileMeta& m) {
    auto subject = rdfItems(xmlElement(xml, "dc:subject"));
    if (!subject.empty()) m.keywords = subject;
    auto title = rdfItems(xmlElement(xml, "dc:title"));
    if (!title.empty()) m.title = title[0];
    auto desc = rdfItems(xmlElement(xml, "dc:description"));
    if (!desc.empty()) m.comment = desc[0];
    auto creator = rdfItems(xmlElement(xml, "dc:creator"));
    if (!creator.empty()) {
        m.author.clear();
        for (const auto& c : creator) m.author += (m.author.empty() ? "" : "; ") + c;
    }
    // 星: xmp:Rating="4" か <xmp:Rating>4</xmp:Rating>
    std::string rating = xmlElement(xml, "xmp:Rating");
    const size_t at = xml.find("xmp:Rating=\"");
    if (rating.empty() && at != std::string::npos) rating = xml.substr(at + 12, xml.find('"', at + 12) - at - 12);
    if (!rating.empty()) {
        const int v = std::atoi(rating.c_str());
        m.rating = v < 0 ? 0 : std::min(v, 5);  // -1（不採用）は 0
    }
}

bool parseJpegMeta(const uint8_t* d, size_t len, FileMeta& m) {
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    std::string xmp;
    size_t pos = 2;
    while (pos + 4 <= len) {
        if (d[pos] != 0xFF) break;
        const uint8_t marker = d[pos + 1];
        if (marker == 0xFF) {
            ++pos;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) break;
        const size_t segLen = (static_cast<size_t>(d[pos + 2]) << 8) | d[pos + 3];
        if (segLen < 2) break;
        const size_t body = pos + 4, end = std::min(len, pos + 2 + segLen);
        if (marker == 0xE1 && end > body + 6 && std::memcmp(d + body, "Exif\0\0", 6) == 0) {
            parseExif(d + body + 6, end - body - 6, m);
        } else if (marker == 0xE1) {
            static const char ns[] = "http://ns.adobe.com/xap/1.0/";
            if (end > body + sizeof(ns) && std::memcmp(d + body, ns, sizeof(ns)) == 0)
                xmp.assign(reinterpret_cast<const char*>(d + body + sizeof(ns)), end - body - sizeof(ns));
        }
        pos += 2 + segLen;
    }
    if (!xmp.empty()) parseXmp(xmp, m);  // XMP を優先
    m.format = "JPEG";
    return true;
}

void applyPng(const PngInfo& pi, FileMeta& m) {
    m.format = "PNG";
    m.animated = m.animated || pi.animated;
    if (!pi.exif.empty()) parseExif(pi.exif.data(), pi.exif.size(), m);
    for (const auto& [key, text] : pi.texts) {
        if (key == "XML:com.adobe.xmp") continue;
        m.pngText += key + ": " + text + "\n";
    }
    for (const auto& [key, text] : pi.texts)
        if (key == "XML:com.adobe.xmp") parseXmp(text, m);  // XMP を優先
}

void fillMissing(FileMeta& a, const FileMeta& b) {
    auto str = [](std::string& x, const std::string& y) {
        if (x.empty()) x = y;
    };
    str(a.format, b.format);
    if (!a.bitDepth) a.bitDepth = b.bitDepth;
    a.animated = a.animated || b.animated;
    if (!a.taken) a.taken = b.taken;
    str(a.camera, b.camera);
    str(a.lens, b.lens);
    str(a.software, b.software);
    if (a.focal <= 0) a.focal = b.focal;
    if (!a.iso) a.iso = b.iso;
    a.hasGps = a.hasGps || b.hasGps;
    if (a.keywords.empty()) a.keywords = b.keywords;
    if (a.rating < 0) a.rating = b.rating;
    str(a.title, b.title);
    str(a.subject, b.subject);
    str(a.comment, b.comment);
    str(a.author, b.author);
    str(a.pngText, b.pngText);
}

void parseZoneIdentifier(const std::string& text, std::string& hostUrl, std::string& referrerUrl) {
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string line = text.substr(a, b - a);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.compare(0, 8, "HostUrl=") == 0) hostUrl = line.substr(8);
        else if (line.compare(0, 12, "ReferrerUrl=") == 0) referrerUrl = line.substr(12);
        a = b + 1;
    }
}

int64_t fileTimeToUnix(uint64_t ft) {
    constexpr uint64_t kEpoch = 116444736000000000ull;  // 1601 年から 1970 年までの 100ns
    return ft < kEpoch ? 0 : static_cast<int64_t>((ft - kEpoch) / 10000000ull);
}

int64_t civilToUnix(int y, int mo, int d, int h, int mi, int s) {
    return daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + h * 3600 + mi * 60 + s;
}

void unixToCivil(int64_t t, int& y, int& mo, int& d, int& h, int& mi, int& s) {
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    h = static_cast<int>(rem / 3600);
    mi = static_cast<int>(rem % 3600 / 60);
    s = static_cast<int>(rem % 60);
    // days_from_civil の逆
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    mo = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(static_cast<int64_t>(yoe) + era * 400 + (mo <= 2));
}

int64_t parseExifDate(const std::string& s) {
    int y, mo, d, h = 0, mi = 0, se = 0;
    if (std::sscanf(s.c_str(), "%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3 &&
        std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3)
        return 0;
    if (y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    return civilToUnix(y, mo, d, h, mi, se);
}

void storeMeta(Db& db, int64_t imageId, const std::string& path, const FileMeta& m) {
    std::string keywords;
    for (const auto& k : m.keywords) keywords += (keywords.empty() ? "" : "; ") + k;
    {
        Stmt st(db.handle(),
                "INSERT OR REPLACE INTO file_meta(image_id,format,bit_depth,animated,taken,camera,lens,focal,iso,has_gps,"
                "software,keywords,rating,title,subject,comment,author,png_text,created,accessed,attributes,hardlinks,"
                "zone_url,zone_referrer) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        st.bind(1, imageId).bind(2, m.format).bind(3, static_cast<int64_t>(m.bitDepth));
        st.bind(4, static_cast<int64_t>(m.animated));
        if (m.taken) st.bind(5, m.taken);
        else st.bindNull(5);
        st.bind(6, m.camera).bind(7, m.lens).bind(8, m.focal).bind(9, static_cast<int64_t>(m.iso));
        st.bind(10, static_cast<int64_t>(m.hasGps)).bind(11, m.software).bind(12, keywords);
        if (m.rating >= 0) st.bind(13, static_cast<int64_t>(m.rating));
        else st.bindNull(13);
        st.bind(14, m.title).bind(15, m.subject).bind(16, m.comment).bind(17, m.author).bind(18, m.pngText);
        st.bind(19, m.created).bind(20, m.accessed).bind(21, static_cast<int64_t>(m.attributes));
        st.bind(22, static_cast<int64_t>(m.hardlinks)).bind(23, m.zoneUrl).bind(24, m.zoneReferrer);
        st.step();
    }
    if (m.fileId) {
        Stmt st(db.handle(), "UPDATE images SET file_id=? WHERE id=?");
        st.bind(1, static_cast<int64_t>(m.fileId)).bind(2, imageId);
        st.step();
    }
    // 埋め込みのタグ（出どころ「ファイル埋め込み」）
    std::vector<TagRow> tags;
    for (const auto& k : m.keywords)
        if (!k.empty()) tags.push_back({k, "general", kSourceEmbedded, kEmbeddedTagConfidence, false});
    replaceTags(db, imageId, kSourceEmbedded, tags);
    // ファイル名とダウンロード元からの出典
    {
        Stmt del(db.handle(), "DELETE FROM sources WHERE image_id=? AND site IN ('pixiv','x','md5','download')");
        del.bind(1, imageId);
        del.step();
        Stmt ins(db.handle(), "INSERT OR IGNORE INTO sources(image_id,site,post_id,url) VALUES(?,?,?,?)");
        for (const auto& s : parseFilenameIds(path)) {
            std::string url;
            if (s.site == "pixiv") url = "https://www.pixiv.net/artworks/" + s.postId;
            ins.bind(1, imageId).bind(2, s.site).bind(3, s.postId).bind(4, url);
            ins.step();
            ins.reset();
        }
        if (!m.zoneUrl.empty()) {
            ins.bind(1, imageId).bind(2, std::string("download")).bind(3, m.zoneUrl).bind(4, m.zoneReferrer);
            ins.step();
        }
    }
    // 全文検索の索引
    {
        Stmt del(db.handle(), "DELETE FROM meta_fts WHERE rowid=?");
        del.bind(1, imageId);
        del.step();
        if (!(m.title + m.subject + m.comment + keywords + m.pngText).empty()) {
            Stmt ins(db.handle(), "INSERT INTO meta_fts(rowid,title,subject,comment,keywords,png_text) VALUES(?,?,?,?,?,?)");
            ins.bind(1, imageId).bind(2, m.title).bind(3, m.subject).bind(4, m.comment).bind(5, keywords).bind(6, m.pngText);
            ins.step();
        }
    }
}

std::vector<int64_t> searchMetaText(Db& db, const std::string& query, size_t limit) {
    std::vector<int64_t> out;
    if (query.empty()) return out;
    size_t chars = 0;  // UTF-8 の文字数
    for (unsigned char c : query)
        if ((c & 0xC0) != 0x80) ++chars;
    const std::string lim = " LIMIT " + std::to_string(limit);
    if (chars >= 3) {
        // trigram の索引で部分一致（" は二重にして 1 つの語句として渡す）
        std::string phrase = "\"";
        for (char c : query) phrase += c == '"' ? std::string("\"\"") : std::string(1, c);
        phrase += "\"";
        Stmt st(db.handle(), ("SELECT rowid FROM meta_fts WHERE meta_fts MATCH ? ORDER BY rank" + lim).c_str());
        st.bind(1, phrase);
        while (st.step()) out.push_back(st.i64(0));
    } else {
        // 2 文字以下は索引を使えないので、そのまま探す
        Stmt st(db.handle(), ("SELECT image_id FROM file_meta WHERE instr(title,?1) OR instr(subject,?1) OR "
                              "instr(comment,?1) OR instr(keywords,?1) OR instr(png_text,?1)" + lim).c_str());
        st.bind(1, query);
        while (st.step()) out.push_back(st.i64(0));
    }
    return out;
}

AnalyzerOutput MetaAnalyzer::analyze(const AnalyzerInput& in) {
    AnalyzerOutput out;
    auto p = std::make_shared<MetaPayload>();
    p->path = in.path;
    out.ok = reader_(in.path, p->meta);
    out.payload = p;
    return out;
}

bool MetaAnalyzer::store(Db& db, int64_t imageId, AnalyzerOutput& out) {
    auto p = std::static_pointer_cast<MetaPayload>(out.payload);
    if (!p) return false;
    storeMeta(db, imageId, p->path, p->meta);
    return true;
}

}  // namespace imgdupe
