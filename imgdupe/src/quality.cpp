#include "quality.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

#include "aesthetic_weights.hpp"
#include "vectors.hpp"

namespace imgdupe {

namespace {

// 平均（面積）で 1/k に縮める（端の余りは捨てる）
std::vector<float> boxDown(const std::vector<float>& g, int w, int h, int k, int& dw, int& dh) {
    dw = w / k;
    dh = h / k;
    std::vector<float> out(static_cast<size_t>(dw) * dh);
    const float inv = 1.0f / static_cast<float>(k * k);
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            float s = 0;
            for (int yy = 0; yy < k; ++yy) {
                const float* row = &g[static_cast<size_t>(y * k + yy) * w + static_cast<size_t>(x) * k];
                for (int xx = 0; xx < k; ++xx) s += row[xx];
            }
            out[static_cast<size_t>(y) * dw + x] = s * inv;
        }
    return out;
}

// 平均の勾配（横と縦の差の絶対値の平均）
double meanGradient(const std::vector<float>& g, int w, int h) {
    double s = 0;
    size_t n = 0;
    for (int y = 0; y + 1 < h; ++y)
        for (int x = 0; x + 1 < w; ++x) {
            const float p = g[static_cast<size_t>(y) * w + x];
            s += std::fabs(g[static_cast<size_t>(y) * w + x + 1] - p) + std::fabs(g[static_cast<size_t>(y + 1) * w + x] - p);
            n += 2;
        }
    return n ? s / static_cast<double>(n) : 0;
}

constexpr double kPi = 3.14159265358979323846;

// その場で FFT（n は 2 のべき）。tw: exp(-2πi k / n)（k < n/2）
void fft(std::vector<std::complex<double>>& a, const std::vector<std::complex<double>>& tw) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t step = n / len;
        for (size_t i = 0; i < n; i += len)
            for (size_t j = 0; j < len / 2; ++j) {
                const std::complex<double> u = a[i + j], v = a[i + j + len / 2] * tw[j * step];
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
            }
    }
}

// 細かさ（ヘッダーの説明を参照）
double measureDetail(const std::vector<float>& g, int w, int h) {
    int n = 512;
    while (n > 64 && (w < n || h < n)) n /= 2;
    if (w < n || h < n) return -1;
    std::vector<double> win(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) win[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2 * kPi * i / (n - 1));
    // 真ん中と、大きければ左上・右下の 4 分の 1 の真ん中
    std::vector<std::pair<int, int>> crops = {{(h - n) / 2, (w - n) / 2}};
    if (h >= 2 * n && w >= 2 * n) crops.insert(crops.end(), {{h / 4 - n / 2, w / 4 - n / 2}, {3 * h / 4 - n / 2, 3 * w / 4 - n / 2}});
    const size_t nn = static_cast<size_t>(n);
    std::vector<std::complex<double>> tw(nn / 2);
    for (size_t i = 0; i < nn / 2; ++i) tw[i] = std::polar(1.0, -2 * kPi * static_cast<double>(i) / n);
    std::vector<double> power(nn * nn, 0.0);
    std::vector<std::complex<double>> a(nn * nn), col(nn), row(nn);
    double winSum2 = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) winSum2 += std::pow(win[static_cast<size_t>(y)] * win[static_cast<size_t>(x)], 2);
    for (const auto& [y0, x0] : crops) {
        double mean = 0;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) mean += g[static_cast<size_t>(y0 + y) * w + x0 + x];
        mean /= static_cast<double>(nn * nn);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x)
                row[static_cast<size_t>(x)] = (g[static_cast<size_t>(y0 + y) * w + x0 + x] - mean) * win[static_cast<size_t>(y)] *
                                              win[static_cast<size_t>(x)];
            fft(row, tw);
            std::copy(row.begin(), row.end(), a.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * nn));
        }
        for (size_t x = 0; x < nn; ++x) {
            for (size_t y = 0; y < nn; ++y) col[y] = a[y * nn + x];
            fft(col, tw);
            for (size_t y = 0; y < nn; ++y) power[y * nn + x] += std::norm(col[y]);
        }
    }
    for (double& p : power) p /= static_cast<double>(crops.size());
    // 周波数の帯（0〜0.5 を 32 に分ける）ごとの中央値。縦・横の軸の上（JPEG のブロックなど）は除く
    constexpr int kBins = 32;
    std::vector<std::vector<double>> bins(kBins);
    for (size_t y = 0; y < nn; ++y)
        for (size_t x = 0; x < nn; ++x) {
            const double fy = (y < nn / 2 ? static_cast<double>(y) : static_cast<double>(y) - n) / n;
            const double fx = (x < nn / 2 ? static_cast<double>(x) : static_cast<double>(x) - n) / n;
            if (std::fabs(fx) <= 1.5 / n || std::fabs(fy) <= 1.5 / n) continue;
            const double r = std::sqrt(fx * fx + fy * fy);
            const int b = static_cast<int>(r / 0.5 * kBins);
            if (b < kBins) bins[static_cast<size_t>(b)].push_back(power[y * nn + x]);
        }
    std::vector<double> prof(kBins, 0.0), f(kBins);
    for (int i = 0; i < kBins; ++i) {
        f[static_cast<size_t>(i)] = (i + 0.5) * 0.5 / kBins;
        auto& v = bins[static_cast<size_t>(i)];
        if (v.empty()) continue;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        prof[static_cast<size_t>(i)] = v[v.size() / 2];
    }
    // 低い周波数で log(強さ) = b + k log(f) を合わせる
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int m = 0;
    for (int i = 0; i < kBins; ++i) {
        const double fi = f[static_cast<size_t>(i)];
        if (fi <= 0.03 || fi >= 0.12 || prof[static_cast<size_t>(i)] <= 0) continue;
        const double lx = std::log(fi), ly = std::log(prof[static_cast<size_t>(i)]);
        sx += lx;
        sy += ly;
        sxx += lx * lx;
        sxy += lx * ly;
        ++m;
    }
    if (m < 3) return -1;
    double k = (m * sxy - sx * sy) / (m * sxx - sx * sx);
    const double b = (sy - k * sx) / m;
    k = std::max(k, -3.0);  // 急すぎる（もうぼやけ始めている）傾きは使わない
    const double floor = winSum2 / 12.0;  // 8 ビットに丸めた誤差の強さ
    for (int i = 0; i < kBins; ++i) {
        const double fi = f[static_cast<size_t>(i)];
        if (fi <= 0.06) continue;
        const double signal = std::max(prof[static_cast<size_t>(i)] - 2 * floor, 0.0);
        if (signal / std::exp(b + k * std::log(fi)) < 0.1) return fi * 2;
    }
    return 1.0;
}

}  // namespace

GrayImage grayFromBgra(const std::vector<uint8_t>& bgra, int w, int h) {
    GrayImage g;
    g.width = w;
    g.height = h;
    g.px.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < g.px.size(); ++i) {
        const uint8_t* p = &bgra[i * 4];
        // 乗算済みのアルファ: 透明な部分は白
        const int y = (p[0] * 29 + p[1] * 150 + p[2] * 77) >> 8;
        g.px[i] = static_cast<uint8_t>(std::min(255, y + (255 - p[3])));
    }
    return g;
}

QualityResult measureQuality(const GrayImage& img, double scale) {
    QualityResult r;
    const int w = img.width, h = img.height;
    if (w < 16 || h < 16 || img.px.size() != static_cast<size_t>(w) * h) return r;
    std::vector<float> g(img.px.begin(), img.px.end());
    const double grad = meanGradient(g, w, h);
    const int longSide = std::max(w, h);
    r.effectiveSide = static_cast<int>(std::lround(longSide / std::max(scale, 1e-6)));
    if (grad < 0.5) return r;  // 平らな画像

    // 細かさ
    r.detail = measureDetail(g, w, h);
    if (r.detail > 0) r.effectiveSide = static_cast<int>(std::lround(longSide / std::max(scale, 1e-6) * r.detail));

    // くっきり度: 見る大きさ（長い辺 1024 以下）に縮めた画像での細かさ
    int k = 1;
    while (longSide / k > 1024) ++k;
    if (k == 1) {
        r.sharpness = r.detail;
    } else {
        int sw = 0, sh = 0;
        const std::vector<float> s = boxDown(g, w, h, k, sw, sh);
        r.sharpness = measureDetail(s, sw, sh);
    }
    return r;
}

double aestheticScore(const float* v) {
    double s = kAestheticBias;
    for (int i = 0; i < 512; ++i) s += static_cast<double>(kAestheticWeights[i]) * v[i];
    return s;
}

AnalyzerOutput QualityAnalyzer::analyze(const AnalyzerInput& in) {
    AnalyzerOutput out;
    std::vector<uint8_t> bgra;
    int w = 0, h = 0, ow = 0, oh = 0;
    if (!decode_ || !decode_(in.path, kQualityDecodeSide, bgra, w, h, ow, oh) || w <= 0 || h <= 0) {
        out.ok = false;
        return out;
    }
    const double scale = std::max(ow, oh) > 0 ? static_cast<double>(std::max(w, h)) / std::max(ow, oh) : 1.0;
    auto r = std::make_shared<QualityResult>(measureQuality(grayFromBgra(bgra, w, h), scale));
    if (r->effectiveSide <= 0) r->effectiveSide = std::max(ow, oh);
    out.payload = r;
    return out;
}

bool QualityAnalyzer::store(Db& db, int64_t imageId, AnalyzerOutput& out) {
    auto r = std::static_pointer_cast<QualityResult>(out.payload);
    if (!r) return false;
    Stmt st(db.handle(),
            "INSERT INTO quality(image_id, sharpness, detail, effective) VALUES(?,?,?,?) "
            "ON CONFLICT(image_id) DO UPDATE SET sharpness=excluded.sharpness, detail=excluded.detail, effective=excluded.effective");
    st.bind(1, imageId);
    if (r->sharpness >= 0) st.bind(2, r->sharpness);
    if (r->detail >= 0) st.bind(3, r->detail);
    st.bind(4, static_cast<int64_t>(r->effectiveSide));
    st.step();
    return true;
}

size_t applyAesthetic(Db& db, const VectorIndex& vecs) {
    if (vecs.dim() != 512 || vecs.size() == 0) return 0;
    // まだ点数の無い画像（CLIP のベクトルが変わったら呼ぶ側が消す）
    std::vector<int64_t> todo;
    {
        Stmt st(db.handle(), "SELECT i.id FROM images i LEFT JOIN quality q ON q.image_id=i.id WHERE q.aesthetic IS NULL");
        while (st.step())
            if (vecs.has(st.i64(0))) todo.push_back(st.i64(0));
    }
    if (todo.empty()) return 0;
    db.begin();
    try {
        Stmt st(db.handle(),
                "INSERT INTO quality(image_id, aesthetic) VALUES(?,?) ON CONFLICT(image_id) DO UPDATE SET aesthetic=excluded.aesthetic");
        std::vector<float> v;
        for (int64_t id : todo) {
            if (!vecs.get(id, v)) continue;
            st.bind(1, id).bind(2, aestheticScore(v.data()));
            st.step();
            st.reset();
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    return todo.size();
}

}  // namespace imgdupe
