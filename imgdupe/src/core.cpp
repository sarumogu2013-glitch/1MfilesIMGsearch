#include "core.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "analyzer.hpp"
#include "db.hpp"
#include "platform.hpp"

namespace imgdupe {

namespace {

unsigned threadCount(unsigned requested) {
    if (requested) return requested;
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 4;
}

// n 個の仕事を threads 本のスレッドで分けて回す。fn(i) は複数のスレッドから呼ばれる
template <class Fn>
void parallelFor(size_t n, unsigned threads, const Callbacks* cb, const std::atomic<bool>& cancel, Fn fn) {
    std::atomic<size_t> next{0};
    auto worker = [&] {
        if (cb && cb->threadInit) cb->threadInit();
        for (;;) {
            if (cancel.load(std::memory_order_relaxed)) break;
            size_t i = next.fetch_add(1);
            if (i >= n) break;
            fn(i);
        }
        if (cb && cb->threadExit) cb->threadExit();
    };
    threads = static_cast<unsigned>(std::min<size_t>(threads, std::max<size_t>(n, 1)));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
}

// n×n の明るさを outW×outH に面積平均で縮める
std::vector<float> resampleArea(const std::vector<float>& src, int n, int outW, int outH) {
    // 1 次元の重み: 出力 o が入力 i をどれだけ含むか
    auto weights = [n](int out) {
        std::vector<float> w(static_cast<size_t>(out) * n, 0.0f);
        const double step = static_cast<double>(n) / out;
        for (int o = 0; o < out; ++o) {
            const double a = o * step, b = (o + 1) * step;
            for (int i = static_cast<int>(a); i < n && i < b; ++i) {
                const double lo = std::max<double>(a, i), hi = std::min<double>(b, i + 1);
                if (hi > lo) w[static_cast<size_t>(o) * n + i] = static_cast<float>((hi - lo) / step);
            }
        }
        return w;
    };
    const auto wx = weights(outW), wy = weights(outH);
    std::vector<float> tmp(static_cast<size_t>(n) * outW, 0.0f);  // n 行 × outW 列
    for (int y = 0; y < n; ++y)
        for (int o = 0; o < outW; ++o) {
            float s = 0;
            for (int x = 0; x < n; ++x) s += wx[static_cast<size_t>(o) * n + x] * src[static_cast<size_t>(y) * n + x];
            tmp[static_cast<size_t>(y) * outW + o] = s;
        }
    std::vector<float> out(static_cast<size_t>(outW) * outH, 0.0f);
    for (int o = 0; o < outH; ++o)
        for (int x = 0; x < outW; ++x) {
            float s = 0;
            for (int y = 0; y < n; ++y) s += wy[static_cast<size_t>(o) * n + y] * tmp[static_cast<size_t>(y) * outW + x];
            out[static_cast<size_t>(o) * outW + x] = s;
        }
    return out;
}

class UnionFind {
public:
    explicit UnionFind(size_t n) : p_(n) {
        for (size_t i = 0; i < n; ++i) p_[i] = static_cast<uint32_t>(i);
    }
    uint32_t find(uint32_t x) {
        while (p_[x] != x) {
            p_[x] = p_[p_[x]];
            x = p_[x];
        }
        return x;
    }
    void unite(uint32_t a, uint32_t b) {
        a = find(a);
        b = find(b);
        if (a != b) p_[std::max(a, b)] = std::min(a, b);
    }

private:
    std::vector<uint32_t> p_;
};

bool colorClose(const Features& a, const Features& b, int tol) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

Similar collect(UnionFind& uf, std::vector<uint8_t> nearest) {
    Similar s;
    std::unordered_map<uint32_t, size_t> slot;
    for (uint32_t i = 0; i < nearest.size(); ++i) {
        if (nearest[i] == 255) continue;
        uint32_t r = uf.find(i);
        auto it = slot.find(r);
        if (it == slot.end()) {
            it = slot.emplace(r, s.clusters.size()).first;
            s.clusters.emplace_back();
        }
        s.clusters[it->second].push_back(i);
    }
    s.nearest = std::move(nearest);
    return s;
}

}  // namespace

// ---------------------------------------------------------------- 特徴量

int popcount64(uint64_t x) { return __builtin_popcountll(x); }

void applyOrientation(std::vector<uint8_t>& rgb, int n, int orientation) {
    if (orientation < 2 || orientation > 8) return;
    std::vector<uint8_t> out(rgb.size());
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            int sx = x, sy = y;
            switch (orientation) {
                case 2: sx = n - 1 - x; break;
                case 3: sx = n - 1 - x; sy = n - 1 - y; break;
                case 4: sy = n - 1 - y; break;
                case 5: sx = y; sy = x; break;
                case 6: sx = y; sy = n - 1 - x; break;  // 時計回りに 90°
                case 7: sx = n - 1 - y; sy = n - 1 - x; break;
                case 8: sx = n - 1 - y; sy = x; break;  // 反時計回りに 90°
            }
            std::memcpy(&out[(static_cast<size_t>(y) * n + x) * 3], &rgb[(static_cast<size_t>(sy) * n + sx) * 3], 3);
        }
    rgb.swap(out);
}

Features computeFeatures(const uint8_t* rgbIn, int orientation) {
    const int n = kThumb;
    std::vector<uint8_t> rgb(rgbIn, rgbIn + static_cast<size_t>(n) * n * 3);
    applyOrientation(rgb, n, orientation);

    Features f;
    f.ok = true;
    std::vector<float> gray(static_cast<size_t>(n) * n);
    double sr = 0, sg = 0, sb = 0, sum = 0, sum2 = 0;
    for (size_t i = 0; i < gray.size(); ++i) {
        const uint8_t r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        const float v = 0.299f * r + 0.587f * g + 0.114f * b;
        gray[i] = v;
        sr += r;
        sg += g;
        sb += b;
        sum += v;
        sum2 += static_cast<double>(v) * v;
    }
    const double cnt = static_cast<double>(gray.size());
    f.r = static_cast<uint8_t>(std::lround(sr / cnt));
    f.g = static_cast<uint8_t>(std::lround(sg / cnt));
    f.b = static_cast<uint8_t>(std::lround(sb / cnt));
    const double mean = sum / cnt;
    const double sd = std::sqrt(std::max(0.0, sum2 / cnt - mean * mean));
    f.contrast = static_cast<uint8_t>(std::min(127L, std::lround(sd)));

    const auto h = resampleArea(gray, n, 9, 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) f.dh = (f.dh << 1) | (h[y * 9 + x + 1] > h[y * 9 + x] ? 1u : 0u);
    const auto v = resampleArea(gray, n, 8, 9);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) f.dv = (f.dv << 1) | (v[(y + 1) * 8 + x] > v[y * 8 + x] ? 1u : 0u);
    return f;
}

int jpegExifOrientation(const uint8_t* d, size_t len) {
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return 0;
    size_t pos = 2;
    while (pos + 4 <= len) {
        if (d[pos] != 0xFF) return 0;
        const uint8_t marker = d[pos + 1];
        if (marker == 0xFF) {  // 詰め物
            ++pos;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) return 0;  // 画像データの始まり・終わり
        const size_t segLen = (static_cast<size_t>(d[pos + 2]) << 8) | d[pos + 3];
        if (segLen < 2) return 0;
        const size_t body = pos + 4, end = std::min(len, pos + 2 + segLen);
        if (marker == 0xE1 && end >= body + 14 && std::memcmp(d + body, "Exif\0\0", 6) == 0) {
            const uint8_t* t = d + body + 6;  // TIFF ヘッダー
            const size_t tlen = end - body - 6;
            const bool le = t[0] == 'I' && t[1] == 'I';
            if (!le && !(t[0] == 'M' && t[1] == 'M')) return 0;
            auto u16 = [&](size_t o) -> uint32_t {
                return o + 2 > tlen ? 0 : le ? (t[o] | (t[o + 1] << 8)) : ((t[o] << 8) | t[o + 1]);
            };
            auto u32 = [&](size_t o) -> uint32_t {
                return o + 4 > tlen ? 0 : le ? (u16(o) | (u16(o + 2) << 16)) : ((u16(o) << 16) | u16(o + 2));
            };
            const size_t ifd = u32(4);
            const uint32_t count = u16(ifd);
            for (uint32_t i = 0; i < count; ++i) {
                const size_t e = ifd + 2 + static_cast<size_t>(i) * 12;
                if (e + 12 > tlen) break;
                if (u16(e) == 0x0112) {
                    const uint32_t v = u16(e + 8);
                    return v >= 1 && v <= 8 ? static_cast<int>(v) : 0;
                }
            }
            return 0;
        }
        pos += 2 + segLen;
    }
    return 0;
}

std::vector<Shrinker::Span> Shrinker::spans(uint32_t size, int n) {
    std::vector<Span> out(size);
    for (uint32_t i = 0; i < size; ++i) {
        if (size >= static_cast<uint32_t>(n)) {
            const auto b = static_cast<uint16_t>(static_cast<uint64_t>(i) * n / size);
            out[i] = {b, static_cast<uint16_t>(b + 1)};
        } else {
            // 入力の方が小さい: 出力の画素の中心がこの画素に入るものすべて
            uint16_t b = 0, e = 0;
            bool found = false;
            for (int o = 0; o < n; ++o) {
                const auto src = static_cast<uint32_t>((o + 0.5) * size / n);
                if (src == i) {
                    if (!found) b = static_cast<uint16_t>(o);
                    e = static_cast<uint16_t>(o + 1);
                    found = true;
                }
            }
            out[i] = {b, e};
        }
    }
    return out;
}

Shrinker::Shrinker(uint32_t width, uint32_t height, int n)
    : n_(n), xs_(spans(width, n)), ys_(spans(height, n)), sum_(static_cast<size_t>(n) * n * 3, 0),
      count_(static_cast<size_t>(n) * n, 0) {}

void Shrinker::addRows(const uint8_t* px, size_t stride, uint32_t y, uint32_t count, bool bgr) {
    const int r = bgr ? 2 : 0, b = bgr ? 0 : 2;
    for (uint32_t k = 0; k < count && y + k < ys_.size(); ++k) {
        const uint8_t* row = px + stride * k;
        const Span sy = ys_[y + k];
        for (uint16_t oy = sy.begin; oy < sy.end; ++oy) {
            uint64_t* s = &sum_[static_cast<size_t>(oy) * n_ * 3];
            uint32_t* c = &count_[static_cast<size_t>(oy) * n_];
            for (size_t x = 0; x < xs_.size(); ++x) {
                const uint8_t* p = row + x * 3;
                const Span sx = xs_[x];
                for (uint16_t ox = sx.begin; ox < sx.end; ++ox) {
                    s[ox * 3] += p[r];
                    s[ox * 3 + 1] += p[1];
                    s[ox * 3 + 2] += p[b];
                    ++c[ox];
                }
            }
        }
    }
}

std::vector<uint8_t> Shrinker::finish() const {
    std::vector<uint8_t> out(sum_.size(), 0);
    for (size_t i = 0; i < count_.size(); ++i)
        if (count_[i])
            for (int c = 0; c < 3; ++c)
                out[i * 3 + c] = static_cast<uint8_t>((sum_[i * 3 + c] + count_[i] / 2) / count_[i]);
    return out;
}

// ---------------------------------------------------------------- SHA-256

namespace {

struct Sha256Ctx {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t bufLen = 0;
    uint64_t total = 0;

    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const uint8_t* p, size_t n) {
        total += n;
        if (bufLen) {
            size_t take = std::min(n, 64 - bufLen);
            std::memcpy(buf + bufLen, p, take);
            bufLen += take;
            p += take;
            n -= take;
            if (bufLen == 64) {
                block(buf);
                bufLen = 0;
            }
        }
        for (; n >= 64; p += 64, n -= 64) block(p);
        std::memcpy(buf, p, n);
        bufLen += n;
    }

    Sha256 finish() {
        const uint64_t bits = total * 8;
        uint8_t pad[72] = {0x80};
        size_t padLen = (bufLen < 56 ? 56 : 120) - bufLen;
        for (int i = 0; i < 8; ++i) pad[padLen + i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        update(pad, padLen + 8);
        Sha256 out;
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
        return out;
    }
};

}  // namespace

Sha256 sha256(const void* data, size_t len) {
    Sha256Ctx c;
    c.update(static_cast<const uint8_t*>(data), len);
    return c.finish();
}

bool sha256File(const std::string& path, Sha256& out) {
    FILE* f = openFile(path, "rb");
    if (!f) return false;
    Sha256Ctx c;
    std::vector<uint8_t> buf(1 << 20);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) c.update(buf.data(), n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    if (ok) out = c.finish();
    return ok;
}

std::string toHex(const Sha256& h) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : h) {
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

// ---------------------------------------------------------------- キャッシュ

namespace {

// 特徴量の作り方やファイルの形を変えたら最後の数字を上げる（古いキャッシュは読まずに作り直す）
const char kMagic[8] = {'I', 'M', 'G', 'D', 'U', 'P', 'E', '1'};

template <class T>
bool readPod(FILE* f, T& v) {
    return std::fread(&v, sizeof(T), 1, f) == 1;
}
template <class T>
void writePod(FILE* f, const T& v) {
    std::fwrite(&v, sizeof(T), 1, f);
}

}  // namespace

bool Cache::load(const std::string& file) {
    entries.clear();
    FILE* f = openFile(file, "rb");
    if (!f) return false;
    std::vector<char> io(1 << 20);
    std::setvbuf(f, io.data(), _IOFBF, io.size());
    char magic[8];
    uint64_t count = 0;
    bool ok = std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, kMagic, 8) == 0 && readPod(f, count);
    if (ok) entries.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; ok && i < count; ++i) {
        Entry e;
        uint32_t len = 0;
        uint8_t flags = 0;
        ok = readPod(f, len) && len < (1u << 16);
        if (!ok) break;
        e.path.resize(len);
        ok = std::fread(&e.path[0], 1, len, f) == len && readPod(f, e.size) && readPod(f, e.mtime) && readPod(f, flags) &&
             readPod(f, e.f.dh) && readPod(f, e.f.dv) && readPod(f, e.f.r) && readPod(f, e.f.g) && readPod(f, e.f.b) &&
             readPod(f, e.f.contrast) && readPod(f, e.f.width) && readPod(f, e.f.height);
        e.f.ok = flags & 1;
        e.hasSha = flags & 2;
        if (ok && e.hasSha) ok = std::fread(e.sha.data(), 1, 32, f) == 32;
        if (ok) entries.emplace(e.path, std::move(e));
    }
    std::fclose(f);
    if (!ok) entries.clear();
    return ok;
}

bool Cache::save(const std::string& file) const {
    const std::string tmp = file + ".tmp";
    FILE* f = openFile(tmp, "wb");
    if (!f) return false;
    std::vector<char> io(1 << 20);
    std::setvbuf(f, io.data(), _IOFBF, io.size());
    std::fwrite(kMagic, 1, 8, f);
    writePod(f, static_cast<uint64_t>(entries.size()));
    for (const auto& [path, e] : entries) {
        writePod(f, static_cast<uint32_t>(path.size()));
        std::fwrite(path.data(), 1, path.size(), f);
        writePod(f, e.size);
        writePod(f, e.mtime);
        writePod(f, static_cast<uint8_t>((e.f.ok ? 1 : 0) | (e.hasSha ? 2 : 0)));
        writePod(f, e.f.dh);
        writePod(f, e.f.dv);
        writePod(f, e.f.r);
        writePod(f, e.f.g);
        writePod(f, e.f.b);
        writePod(f, e.f.contrast);
        writePod(f, e.f.width);
        writePod(f, e.f.height);
        if (e.hasSha) std::fwrite(e.sha.data(), 1, 32, f);
    }
    const bool ok = !std::ferror(f);
    if (std::fclose(f) != 0 || !ok) {
        std::remove(tmp.c_str());
        return false;
    }
    return replaceFile(tmp, file);
}

// ---------------------------------------------------------------- 比較

Similar findSimilarBruteForce(const std::vector<Features>& items, const SimilarOptions& opt) {
    const size_t n = items.size();
    UnionFind uf(n);
    std::vector<uint8_t> nearest(n, 255);
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = i + 1; j < n; ++j) {
            const int d = distance(items[i], items[j]);
            if (d <= opt.threshold && colorClose(items[i], items[j], opt.colorTolerance)) {
                uf.unite(i, j);
                nearest[i] = static_cast<uint8_t>(std::min<int>(nearest[i], d));
                nearest[j] = static_cast<uint8_t>(std::min<int>(nearest[j], d));
            }
        }
    return collect(uf, std::move(nearest));
}

Similar findSimilar(const std::vector<Features>& items, const SimilarOptions& opt) {
    const size_t n = items.size();
    const int t = std::max(0, opt.threshold);
    const int parts = t / 2 + 1;  // 1 つのハッシュをいくつの塊に分けるか
    if (n < 2) {
        UnionFind none(n);
        return collect(none, std::vector<uint8_t>(n, 255));
    }
    if (parts > 12) return findSimilarBruteForce(items, opt);  // 塊が小さすぎると絞り込めない

    // 鍵 = (横 or 縦, 塊の番号)。鍵ごとに同じ値の画像をまとめる
    struct Key {
        int which;  // 0: dh, 1: dv
        int shift;
        uint64_t mask;
    };
    std::vector<Key> keys;
    for (int which = 0; which < 2; ++which)
        for (int c = 0; c < parts; ++c) {
            const int lo = c * 64 / parts, hi = (c + 1) * 64 / parts;
            const int bits = hi - lo;
            keys.push_back({which, lo, bits >= 64 ? ~0ull : ((1ull << bits) - 1)});
        }
    auto keyValue = [&](const Key& k, uint32_t i) {
        return ((k.which ? items[i].dv : items[i].dh) >> k.shift) & k.mask;
    };

    // 2 枚以上ある値だけを残す（buckets: 鍵の番号・members の範囲）
    struct Bucket {
        uint32_t key;
        uint32_t begin, end;
    };
    std::vector<uint32_t> members;
    std::vector<Bucket> buckets;
    {
        std::vector<std::pair<uint64_t, uint32_t>> sorted(n);
        for (uint32_t k = 0; k < keys.size(); ++k) {
            for (uint32_t i = 0; i < n; ++i) sorted[i] = {keyValue(keys[k], i), i};
            std::sort(sorted.begin(), sorted.end());
            for (size_t a = 0; a < n;) {
                size_t b = a + 1;
                while (b < n && sorted[b].first == sorted[a].first) ++b;
                if (b - a >= 2) {
                    Bucket bk{k, static_cast<uint32_t>(members.size()), 0};
                    for (size_t x = a; x < b; ++x) members.push_back(sorted[x].second);
                    bk.end = static_cast<uint32_t>(members.size());
                    buckets.push_back(bk);
                }
                a = b;
            }
        }
    }
    // 大きなバケツから先に配る（終わりの待ちを減らす）
    std::sort(buckets.begin(), buckets.end(),
              [](const Bucket& a, const Bucket& b) { return a.end - a.begin > b.end - b.begin; });
    double totalWork = 0;
    for (const auto& b : buckets) totalWork += 0.5 * (b.end - b.begin) * (b.end - b.begin - 1.0);

    const unsigned threads = threadCount(opt.threads);
    std::vector<std::unique_ptr<UnionFind>> ufs;
    std::vector<std::vector<uint8_t>> nears;
    for (unsigned i = 0; i < threads; ++i) {
        ufs.push_back(std::make_unique<UnionFind>(n));
        nears.emplace_back(n, 255);
    }
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> doneWork{0};
    std::mutex progressMutex;
    const std::atomic<bool> noCancel{false};
    const std::atomic<bool>& cancel = opt.cancel ? *opt.cancel : noCancel;

    auto worker = [&](unsigned tid) {
        UnionFind& uf = *ufs[tid];
        std::vector<uint8_t>& near = nears[tid];
        for (;;) {
            if (cancel.load(std::memory_order_relaxed)) return;
            const size_t bi = next.fetch_add(1);
            if (bi >= buckets.size()) return;
            const Bucket& bk = buckets[bi];
            for (uint32_t x = bk.begin; x < bk.end; ++x) {
                const uint32_t i = members[x];
                const Features& fi = items[i];
                for (uint32_t y = x + 1; y < bk.end; ++y) {
                    const uint32_t j = members[y];
                    const Features& fj = items[j];
                    const int dh = popcount64(fi.dh ^ fj.dh);
                    if (dh > t) continue;
                    const int d = dh + popcount64(fi.dv ^ fj.dv);
                    if (d > t || !colorClose(fi, fj, opt.colorTolerance)) continue;
                    // 前の鍵でも同じ値なら、その鍵のバケツで比べ済み
                    bool seen = false;
                    for (uint32_t k = 0; k < bk.key && !seen; ++k) seen = keyValue(keys[k], i) == keyValue(keys[k], j);
                    if (seen) continue;
                    uf.unite(i, j);
                    if (d < near[i]) near[i] = static_cast<uint8_t>(d);
                    if (d < near[j]) near[j] = static_cast<uint8_t>(d);
                }
                if ((x & 255) == 255 && cancel.load(std::memory_order_relaxed)) return;
            }
            const uint64_t sz = bk.end - bk.begin;
            const uint64_t w = doneWork.fetch_add(sz * (sz - 1) / 2) + sz * (sz - 1) / 2;
            if (opt.progress && totalWork > 0) {
                std::lock_guard<std::mutex> lock(progressMutex);
                opt.progress(std::min(1.0, w / totalWork));
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < threads; ++i) pool.emplace_back(worker, i);
    worker(0);
    for (auto& th : pool) th.join();

    UnionFind uf(n);
    std::vector<uint8_t> nearest(n, 255);
    for (unsigned tid = 0; tid < threads; ++tid)
        for (uint32_t i = 0; i < n; ++i) {
            if (nears[tid][i] == 255) continue;
            uf.unite(i, ufs[tid]->find(i));
            nearest[i] = std::min(nearest[i], nears[tid][i]);
        }
    return collect(uf, std::move(nearest));
}

// ---------------------------------------------------------------- 全体の流れ

bool isImageExt(const std::string& path) {
    static const std::unordered_set<std::string> exts = {".jpg", ".jpeg", ".jpe", ".jfif", ".png",  ".bmp",
                                                         ".dib", ".gif",  ".tif", ".tiff", ".webp", ".heic",
                                                         ".heif", ".avif", ".jxr", ".wdp"};
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    const size_t sep = path.find_last_of("/\\");
    if (sep != std::string::npos && sep > dot) return false;
    std::string ext = path.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return exts.count(ext) > 0;
}

void finishGroup(Group& g, const std::vector<Entry>& files) {
    std::sort(g.members.begin(), g.members.end(), [&](const Member& a, const Member& b) {
        if (a.dist != b.dist) return a.dist < b.dist;
        if (files[a.index].size != files[b.index].size) return files[a.index].size > files[b.index].size;
        return files[a.index].path < files[b.index].path;
    });
    g.totalSize = 0;
    uint64_t biggest = 0;
    for (const auto& m : g.members) {
        g.totalSize += files[m.index].size;
        biggest = std::max(biggest, files[m.index].size);
    }
    g.reclaim = g.totalSize - biggest;
}

namespace {

double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

Result run(const RunOptions& opt, const Callbacks& cb, Progress& prog, const std::atomic<bool>& cancel) {
    Result res;
    const unsigned threads = threadCount(opt.threads);

    // 走査
    prog.stage = static_cast<int>(Stage::Scan);
    prog.done = 0;
    prog.total = 0;
    std::vector<ScannedFile> scanned;
    for (const auto& root : opt.roots) {
        if (cancel) break;
        cb.scan(root, scanned);
    }
    std::sort(scanned.begin(), scanned.end(), [](const ScannedFile& a, const ScannedFile& b) { return a.path < b.path; });
    scanned.erase(std::unique(scanned.begin(), scanned.end(),
                              [](const ScannedFile& a, const ScannedFile& b) { return a.path == b.path; }),
                  scanned.end());
    if (cancel) {
        res.cancelled = true;
        return res;
    }

    // DB に登録（新しいファイル・変わったファイルは特徴量と段階を消す）
    Db db;
    if (!db.open(opt.dbFile.empty() ? ":memory:" : opt.dbFile))
        throw std::runtime_error("画像の情報（DB）を開けません: " + db.error());
    if (!opt.legacyCacheFile.empty() && db.setting("legacyCacheImported").empty()) {
        Cache old;
        if (old.load(opt.legacyCacheFile)) importCache(db, old, kHashVersion);
        db.setSetting("legacyCacheImported", "1");
    }
    std::unordered_map<std::string, int64_t> ids = upsertScanned(db, scanned);
    std::unordered_map<std::string, ImageRow> known = loadHashedImages(db, kHashVersion);

    // ハッシュ化（段階 "hash" が今の版で済んでいないものだけ）
    std::vector<uint32_t> todo;
    for (uint32_t i = 0; i < scanned.size(); ++i)
        if (!known.count(scanned[i].path)) todo.push_back(i);
    prog.stage = static_cast<int>(Stage::Hash);
    prog.done = 0;
    prog.total = todo.size();
    auto t0 = std::chrono::steady_clock::now();
    std::vector<Features> feats(todo.size());
    std::vector<uint8_t> finished(todo.size(), 0);
    parallelFor(todo.size(), threads, &cb, cancel, [&](size_t k) {
        feats[k] = cb.decode(scanned[todo[k]].path);
        finished[k] = 1;
        prog.done.fetch_add(1, std::memory_order_relaxed);
    });
    res.secHash = secondsSince(t0);
    {
        std::vector<std::pair<int64_t, Features>> rows;
        for (size_t k = 0; k < todo.size(); ++k) {
            if (!finished[k]) continue;
            const auto& s = scanned[todo[k]];
            ImageRow r;
            r.id = ids.at(s.path);
            r.e.path = s.path;
            r.e.size = s.size;
            r.e.mtime = s.mtime;
            r.e.f = feats[k];
            rows.emplace_back(r.id, r.e.f);
            known[s.path] = std::move(r);
            ++res.hashed;
        }
        saveFeatures(db, rows, kHashVersion);  // 中止しても、読み込んだ分は残す
    }
    // 消えたファイルを DB から除く（今回のフォルダの下だけ）
    removeMissing(db, opt.roots, ids);
    if (cancel) {
        res.cancelled = true;
        return res;
    }

    res.files.reserve(scanned.size());
    res.ids.reserve(scanned.size());
    for (const auto& s : scanned) {
        auto it = known.find(s.path);
        if (it == known.end()) continue;
        res.files.push_back(it->second.e);
        res.ids.push_back(it->second.id);
    }
    // ここから先は走査結果を使わないので、メモリを返す（100 万枚だと数百 MB になる）
    known = {};
    ids = {};
    scanned = {};
    for (const auto& e : res.files)
        if (!e.f.ok) ++res.unreadable;
    if (opt.featuresOnly) {
        prog.stage = static_cast<int>(Stage::Done);
        return res;
    }

    // 完全一致: 大きさが同じものだけ SHA-256 を比べる
    prog.stage = static_cast<int>(Stage::Exact);
    std::unordered_map<uint64_t, std::vector<uint32_t>> bySize;
    for (uint32_t i = 0; i < res.files.size(); ++i)
        if (res.files[i].size > 0) bySize[res.files[i].size].push_back(i);
    std::vector<uint32_t> need;
    std::vector<uint32_t> candidates;
    for (const auto& [size, v] : bySize)
        if (v.size() > 1)
            for (uint32_t i : v) {
                candidates.push_back(i);
                if (!res.files[i].hasSha) need.push_back(i);
            }
    prog.done = 0;
    prog.total = need.size();
    std::vector<uint8_t> shaOk(need.size(), 0);
    parallelFor(need.size(), threads, nullptr, cancel, [&](size_t k) {
        Entry& e = res.files[need[k]];
        shaOk[k] = sha256File(e.path, e.sha);
        prog.done.fetch_add(1, std::memory_order_relaxed);
    });
    {
        std::vector<std::pair<int64_t, Sha256>> rows;
        for (size_t k = 0; k < need.size(); ++k) {
            if (!shaOk[k]) continue;
            Entry& e = res.files[need[k]];
            e.hasSha = true;
            rows.emplace_back(res.ids[need[k]], e.sha);
        }
        saveSha(db, rows);
    }
    if (cancel) {
        res.cancelled = true;
        return res;
    }
    std::vector<int> exactOf(res.files.size(), -1);
    {
        std::map<std::pair<uint64_t, Sha256>, std::vector<uint32_t>> bySha;
        for (uint32_t i : candidates)
            if (res.files[i].hasSha) bySha[{res.files[i].size, res.files[i].sha}].push_back(i);
        for (auto& [key, v] : bySha) {
            if (v.size() < 2) continue;
            Group g;
            g.kind = Kind::Exact;
            for (uint32_t i : v) {
                g.members.push_back({i, 0});
                exactOf[i] = static_cast<int>(res.groups.size());
            }
            res.groups.push_back(std::move(g));
        }
    }

    // 見た目一致
    if (!opt.exactOnly) {
        prog.stage = static_cast<int>(Stage::Similar);
        prog.done = 0;
        prog.total = 1000;
        std::vector<Features> items;
        std::vector<uint32_t> back;
        for (uint32_t i = 0; i < res.files.size(); ++i) {
            const auto& f = res.files[i].f;
            if (!f.ok) continue;
            if (!opt.includeFlat && f.contrast < opt.flatContrast) {
                ++res.flatSkipped;
                continue;
            }
            items.push_back(f);
            back.push_back(i);
        }
        SimilarOptions so;
        so.threshold = opt.threshold;
        so.colorTolerance = opt.colorTolerance;
        so.threads = threads;
        so.cancel = &cancel;
        so.progress = [&](double p) { prog.done = static_cast<uint64_t>(p * 1000); };
        t0 = std::chrono::steady_clock::now();
        Similar sim = findSimilar(items, so);
        res.secCompare = secondsSince(t0);
        if (cancel) {
            res.cancelled = true;
            return res;
        }
        const size_t exactCount = res.groups.size();
        for (const auto& cl : sim.clusters) {
            // 1 つの完全一致グループとまったく同じなら出さない
            const int ex = exactOf[back[cl[0]]];
            bool same = ex >= 0 && res.groups[ex].members.size() == cl.size();
            for (uint32_t k : cl) same = same && exactOf[back[k]] == ex;
            if (same) continue;
            Group g;
            g.kind = Kind::Similar;
            for (uint32_t k : cl) g.members.push_back({back[k], sim.nearest[k]});
            res.groups.push_back(std::move(g));
        }
        for (auto& g : res.groups) finishGroup(g, res.files);
        auto byReclaim = [](const Group& a, const Group& b) {
            if (a.reclaim != b.reclaim) return a.reclaim > b.reclaim;
            return a.members.size() > b.members.size();
        };
        std::stable_sort(res.groups.begin(), res.groups.begin() + exactCount, byReclaim);
        std::stable_sort(res.groups.begin() + exactCount, res.groups.end(), byReclaim);
    } else {
        for (auto& g : res.groups) finishGroup(g, res.files);
        std::stable_sort(res.groups.begin(), res.groups.end(),
                         [](const Group& a, const Group& b) { return a.reclaim > b.reclaim; });
    }
    replaceDuplicateGroups(db, res, res.ids);
    prog.stage = static_cast<int>(Stage::Done);
    return res;
}

Group query(const Result& result, const Features& q, int threshold, int colorTolerance) {
    Group g;
    g.kind = Kind::Query;
    for (uint32_t i = 0; i < result.files.size(); ++i) {
        const auto& f = result.files[i].f;
        if (!f.ok) continue;
        const int d = distance(f, q);
        if (d <= threshold && colorClose(f, q, colorTolerance)) g.members.push_back({i, static_cast<uint8_t>(d)});
    }
    finishGroup(g, result.files);
    return g;
}

// ---------------------------------------------------------------- 出力

std::string fileUri(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    std::string prefix;
    if (p.size() >= 2 && p[0] == '/' && p[1] == '/') {
        prefix = "file:";  // UNC: //server/share
    } else if (p.size() >= 2 && p[1] == ':') {
        prefix = "file:///";
    } else {
        prefix = "file://";
    }
    static const char* hex = "0123456789ABCDEF";
    std::string out = prefix;
    for (unsigned char c : p) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string kindName(Kind k) {
    switch (k) {
        case Kind::Exact: return "完全一致";
        case Kind::Similar: return "見た目一致";
        case Kind::Query: return "検索結果";
    }
    return "";
}

std::string formatSize(uint64_t bytes) {
    char buf[32];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ull * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024ull * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }
    return buf;
}

namespace {

std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

std::string htmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

bool writeText(const std::string& file, const std::string& text) {
    FILE* f = openFile(file, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace

bool writeCsv(const Result& r, const std::string& file) {
    std::string s = "\xEF\xBB\xBF" "group,種類,距離,サイズ(KB),幅,高さ,パス\r\n";
    char buf[128];
    for (size_t gi = 0; gi < r.groups.size(); ++gi)
        for (const auto& m : r.groups[gi].members) {
            const Entry& e = r.files[m.index];
            std::snprintf(buf, sizeof(buf), "%zu,%s,%d,%.1f,%u,%u,", gi + 1, kindName(r.groups[gi].kind).c_str(), m.dist,
                          e.size / 1024.0, e.f.width, e.f.height);
            s += buf;
            s += csvField(e.path);
            s += "\r\n";
        }
    return writeText(file, s);
}

bool writeHtml(const Result& r, const std::string& file) {
    std::string s =
        "<!doctype html><meta charset=utf-8><title>重複画像</title><style>"
        "body{font-family:sans-serif;margin:16px}.g{border-bottom:1px solid #ccc;padding:8px 0}"
        ".i{display:inline-block;width:180px;margin:4px;vertical-align:top;font-size:11px;word-break:break-all}"
        "img{max-width:180px;max-height:180px;display:block}</style>";
    s += "<h2>重複・類似グループ " + std::to_string(r.groups.size()) + " 件</h2>";
    for (size_t gi = 0; gi < r.groups.size(); ++gi) {
        const Group& g = r.groups[gi];
        s += "<div class=g><b>#" + std::to_string(gi + 1) + " " + kindName(g.kind) + "（" +
             std::to_string(g.members.size()) + "枚・消せば " + formatSize(g.reclaim) + "）</b><br>";
        for (const auto& m : g.members) {
            const Entry& e = r.files[m.index];
            const std::string uri = htmlEscape(fileUri(e.path));
            s += "<div class=i><a href='" + uri + "'><img loading=lazy src='" + uri + "'></a>距離" +
                 std::to_string(m.dist) + " / " + formatSize(e.size) + " / " + std::to_string(e.f.width) + "×" +
                 std::to_string(e.f.height) + "<br>" + htmlEscape(e.path) + "</div>";
        }
        s += "</div>";
    }
    return writeText(file, s);
}

}  // namespace imgdupe
