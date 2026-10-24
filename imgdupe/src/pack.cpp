#include "pack.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "db.hpp"
#include "platform.hpp"

namespace imgdupe {

namespace {

// 1 件の並び: "TPK1" | 画像 ID (8) | 形式 (1) | 幅 (2) | 高さ (2) | 長さ (4) | データ
constexpr char kMagic[4] = {'T', 'P', 'K', '1'};
constexpr size_t kHeader = 4 + 8 + 1 + 2 + 2 + 4;

template <class T>
void putLe(std::vector<uint8_t>& b, T v) {
    for (size_t i = 0; i < sizeof(T); ++i) b.push_back(static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i)));
}

template <class T>
T getLe(const uint8_t* p) {
    uint64_t v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return static_cast<T>(v);
}

}  // namespace

ThumbPack::ThumbPack(std::string dir, uint64_t maxPackBytes) : dir_(std::move(dir)), max_(maxPackBytes) {
    while (!dir_.empty() && (dir_.back() == '/' || dir_.back() == '\\')) dir_.pop_back();
}

std::string ThumbPack::packPath(int index) const {
    char name[32];
    std::snprintf(name, sizeof(name), "thumbs_%03d.pack", index);
#ifdef _WIN32
    return dir_ + "\\" + name;
#else
    return dir_ + "/" + name;
#endif
}

bool ThumbPack::put(Db& db, int64_t imageId, const ThumbData& t) {
    if (t.bytes.empty() || t.bytes.size() > 0x7FFFFFFF) return false;
    std::vector<uint8_t> rec;
    rec.reserve(kHeader + t.bytes.size());
    rec.insert(rec.end(), kMagic, kMagic + 4);
    putLe<int64_t>(rec, imageId);
    putLe<uint8_t>(rec, static_cast<uint8_t>(t.format));
    putLe<uint16_t>(rec, t.width);
    putLe<uint16_t>(rec, t.height);
    putLe<uint32_t>(rec, static_cast<uint32_t>(t.bytes.size()));
    rec.insert(rec.end(), t.bytes.begin(), t.bytes.end());

    // 今のパックがいっぱいなら次へ
    int index = std::atoi(db.setting("thumbPack", "0").c_str());
    int64_t size = fileSize(packPath(index));
    if (size > 0 && static_cast<uint64_t>(size) + rec.size() > max_) {
        ++index;
        db.setSetting("thumbPack", std::to_string(index));
    }
    FILE* f = openFile(packPath(index), "ab");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const int64_t offset = tellFile(f);
    const bool ok = offset >= 0 && std::fwrite(rec.data(), 1, rec.size(), f) == rec.size();
    if (std::fclose(f) != 0 || !ok) return false;

    Stmt st(db.handle(),
            "INSERT INTO thumbs(image_id,pack,offset,length,format,width,height) VALUES(?,?,?,?,?,?,?) "
            "ON CONFLICT(image_id) DO UPDATE SET pack=excluded.pack,offset=excluded.offset,length=excluded.length,"
            "format=excluded.format,width=excluded.width,height=excluded.height");
    st.bind(1, imageId).bind(2, static_cast<int64_t>(index)).bind(3, offset);
    st.bind(4, static_cast<int64_t>(rec.size())).bind(5, static_cast<int64_t>(t.format));
    st.bind(6, static_cast<int64_t>(t.width)).bind(7, static_cast<int64_t>(t.height));
    st.step();
    return true;
}

bool ThumbPack::get(Db& db, int64_t imageId, ThumbData& out) const {
    int64_t pack = 0, offset = 0, length = 0;
    {
        Stmt st(db.handle(), "SELECT pack,offset,length FROM thumbs WHERE image_id=?");
        st.bind(1, imageId);
        if (!st.step()) return false;
        pack = st.i64(0);
        offset = st.i64(1);
        length = st.i64(2);
    }
    if (length < static_cast<int64_t>(kHeader)) return false;
    FILE* f = openFile(packPath(static_cast<int>(pack)), "rb");
    if (!f) return false;
    std::vector<uint8_t> rec(static_cast<size_t>(length));
    const bool read = seekFile(f, static_cast<uint64_t>(offset)) && std::fread(rec.data(), 1, rec.size(), f) == rec.size();
    std::fclose(f);
    if (!read || std::memcmp(rec.data(), kMagic, 4) != 0 || getLe<int64_t>(&rec[4]) != imageId) return false;
    const uint32_t len = getLe<uint32_t>(&rec[17]);
    if (kHeader + len != rec.size()) return false;
    out.format = static_cast<ThumbFormat>(rec[12]);
    out.width = getLe<uint16_t>(&rec[13]);
    out.height = getLe<uint16_t>(&rec[15]);
    out.bytes.assign(rec.begin() + kHeader, rec.end());
    return true;
}

uint64_t ThumbPack::garbageBytes(Db& db) const {
    uint64_t total = 0;
    const int last = std::atoi(db.setting("thumbPack", "0").c_str());
    for (int i = 0; i <= last; ++i) {
        const int64_t s = fileSize(packPath(i));
        if (s > 0) total += static_cast<uint64_t>(s);
    }
    Stmt st(db.handle(), "SELECT COALESCE(SUM(length),0) FROM thumbs");
    const uint64_t used = st.step() ? static_cast<uint64_t>(st.i64(0)) : 0;
    return total > used ? total - used : 0;
}

}  // namespace imgdupe
