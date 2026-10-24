#include "vectors.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace imgdupe {

namespace {

constexpr char kMagic[4] = {'I', 'V', 'E', 'C'};

struct HalfTable {
    float v[65536];
    HalfTable() {
        for (uint32_t h = 0; h < 65536; ++h) {
            const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
            uint32_t bits;
            if (exp == 0) {
                if (man == 0) {
                    bits = sign;
                } else {  // 非正規化数
                    float f = std::ldexp(static_cast<float>(man), -24);
                    std::memcpy(&bits, &f, 4);
                    bits |= sign;
                }
            } else if (exp == 31) {
                bits = sign | 0x7f800000u | (man << 13);
            } else {
                bits = sign | ((exp + 112) << 23) | (man << 13);
            }
            std::memcpy(&v[h], &bits, 4);
        }
    }
};
const HalfTable& halfTable() {
    static const HalfTable t;
    return t;
}

uint32_t readU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

}  // namespace

uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((x >> 23) & 0xff) - 127 + 15;
    uint32_t man = x & 0x7fffffu;
    if (((x >> 23) & 0xff) == 0xff) return static_cast<uint16_t>(sign | 0x7c00u | (man ? 0x200u : 0));  // NaN / 無限大
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7c00u);                                    // 大きすぎ → 無限大
    if (exp <= 0) {                                                                                  // 非正規化数か 0
        if (exp < -10) return static_cast<uint16_t>(sign);
        man |= 0x800000u;
        const int shift = 14 - exp;
        uint32_t h = man >> shift;
        const uint32_t rem = man & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1))) ++h;
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = (static_cast<uint32_t>(exp) << 10) | (man >> 13);
    const uint32_t rem = man & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1))) ++h;  // 偶数への丸め（繰り上がりで指数も上がる）
    return static_cast<uint16_t>(sign | h);
}

float halfToFloat(uint16_t h) { return halfTable().v[h]; }

void normalizeVector(std::vector<float>& v) {
    double n = 0;
    for (float x : v) n += static_cast<double>(x) * x;
    if (n <= 0) return;
    const float inv = static_cast<float>(1.0 / std::sqrt(n));
    for (float& x : v) x *= inv;
}

// ---------------------------------------------------------------- 書く

bool VectorWriter::open(const std::string& path, uint32_t dim, const std::string& version, bool* created) {
    close();
    dim_ = dim;
    bool reuse = false;
    uint64_t keepBytes = 0;
    {
        MappedFile m;
        if (m.open(path) && m.size() >= 12 && std::memcmp(m.data(), kMagic, 4) == 0 && readU32(m.data() + 4) == dim) {
            const uint32_t len = readU32(m.data() + 8);
            reuse = m.size() >= 12ull + len && std::string(reinterpret_cast<const char*>(m.data() + 12), len) == version;
            if (reuse) {
                const uint64_t rec = 8 + 2ull * dim, body = m.size() - 12 - len;
                if (body % rec) keepBytes = 12 + len + body / rec * rec;  // 書きかけで終わったレコードは切り捨てる
            }
        }
    }
    // 切り詰められない時（GUI が開いているなど）は作り直さずに失敗にする（今あるベクトルを失わないため）
    if (keepBytes && !truncateFile(path, keepBytes)) return false;
    if (created) *created = !reuse;
    f_ = openFile(path, reuse ? "ab" : "wb");
    if (!f_) return false;
    if (!reuse) {
        const uint32_t len = static_cast<uint32_t>(version.size());
        std::fwrite(kMagic, 1, 4, f_);
        std::fwrite(&dim, 4, 1, f_);
        std::fwrite(&len, 4, 1, f_);
        std::fwrite(version.data(), 1, len, f_);
        std::fflush(f_);
    }
    buf_.resize(dim);
    return true;
}

bool VectorWriter::append(int64_t imageId, const float* v) {
    if (!f_) return false;
    for (uint32_t i = 0; i < dim_; ++i) buf_[i] = floatToHalf(v[i]);
    return std::fwrite(&imageId, 8, 1, f_) == 1 && std::fwrite(buf_.data(), 2, dim_, f_) == dim_;
}

void VectorWriter::flush() {
    if (f_) std::fflush(f_);
}

void VectorWriter::close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
}

// ---------------------------------------------------------------- 読む

bool VectorIndex::load(const std::string& path) {
    if (path == path_ && map_.data()) {
        const int64_t now = fileSize(path);
        if (now >= 0 && static_cast<uint64_t>(now) == mappedSize_) return true;  // 変わっていない
    }
    close();
    if (!map_.open(path)) return false;
    const uint8_t* d = map_.data();
    const uint64_t n = map_.size();
    if (n < 12 || std::memcmp(d, kMagic, 4) != 0) {
        close();
        return false;
    }
    dim_ = readU32(d + 4);
    const uint32_t len = readU32(d + 8);
    if (dim_ == 0 || n < 12ull + len) {
        close();
        return false;
    }
    version_.assign(reinterpret_cast<const char*>(d + 12), len);
    header_ = 12 + len;
    const uint64_t rec = 8 + 2ull * dim_;
    records_ = static_cast<size_t>((n - header_) / rec);  // 書きかけの最後のレコードは数えない
    for (size_t i = 0; i < records_; ++i) {
        const uint64_t off = header_ + i * rec;
        int64_t id;
        std::memcpy(&id, d + off, 8);
        auto [it, inserted] = pos_.emplace(id, ids_.size());
        if (inserted) {
            ids_.push_back(id);
            offsets_.push_back(off);
        } else {
            offsets_[it->second] = off;  // 後のものを使う
        }
    }
    path_ = path;
    mappedSize_ = n;
    return true;
}

void VectorIndex::close() {
    map_.close();
    path_.clear();
    mappedSize_ = 0;
    dim_ = 0;
    version_.clear();
    header_ = records_ = 0;
    ids_.clear();
    offsets_.clear();
    pos_.clear();
}

const uint16_t* VectorIndex::vec(size_t i) const { return reinterpret_cast<const uint16_t*>(map_.data() + offsets_[i] + 8); }

bool VectorIndex::get(int64_t id, std::vector<float>& out) const {
    auto it = pos_.find(id);
    if (it == pos_.end()) return false;
    out.resize(dim_);
    const uint16_t* v = vec(it->second);
    for (uint32_t i = 0; i < dim_; ++i) {
        uint16_t h;
        std::memcpy(&h, v + i, 2);
        out[i] = halfToFloat(h);
    }
    return true;
}

std::vector<std::pair<int64_t, float>> VectorIndex::topK(const float* q, size_t k, const std::function<bool(int64_t)>& accept,
                                                         unsigned threads) const {
    using Hit = std::pair<int64_t, float>;
    if (!k || ids_.empty()) return {};
    if (!threads) threads = std::max(1u, std::thread::hardware_concurrency());
    threads = static_cast<unsigned>(std::min<size_t>(threads, (ids_.size() + 4095) / 4096));
    threads = std::max(1u, threads);
    const float* table = halfTable().v;
    auto better = [](const Hit& a, const Hit& b) { return a.second > b.second || (a.second == b.second && a.first < b.first); };
    std::vector<std::vector<Hit>> parts(threads);
    auto work = [&](unsigned t) {
        std::vector<Hit>& heap = parts[t];  // 小さい方が先頭に来るヒープ（上位 k 個を持つ）
        const size_t begin = ids_.size() * t / threads, end = ids_.size() * (t + 1) / threads;
        for (size_t i = begin; i < end; ++i) {
            if (accept && !accept(ids_[i])) continue;
            const uint16_t* v = vec(i);
            float s = 0;
            for (uint32_t j = 0; j < dim_; ++j) {
                uint16_t h;
                std::memcpy(&h, v + j, 2);
                s += table[h] * q[j];
            }
            const Hit hit{ids_[i], s};
            if (heap.size() < k) {
                heap.push_back(hit);
                std::push_heap(heap.begin(), heap.end(), better);
            } else if (better(hit, heap.front())) {
                std::pop_heap(heap.begin(), heap.end(), better);
                heap.back() = hit;
                std::push_heap(heap.begin(), heap.end(), better);
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work, t);
    work(0);
    for (auto& th : pool) th.join();
    std::vector<Hit> all;
    for (auto& p : parts) all.insert(all.end(), p.begin(), p.end());
    std::sort(all.begin(), all.end(), better);
    if (all.size() > k) all.resize(k);
    return all;
}

bool compactVectors(const std::string& path, const std::function<bool(int64_t)>& keep) {
    VectorIndex idx;
    if (!idx.load(path)) return false;
    const std::string tmp = path + ".tmp";
    removeFile(tmp);
    {
        VectorWriter w;
        if (!w.open(tmp, idx.dim(), idx.version())) return false;
        std::vector<float> v;
        for (int64_t id : idx.ids()) {
            if (keep && !keep(id)) continue;
            if (idx.get(id, v) && !w.append(id, v.data())) return false;
        }
        w.close();
    }
    idx.close();
    return replaceFile(tmp, path);
}

}  // namespace imgdupe
