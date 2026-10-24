// OS に依存しない部分のテスト（./build.sh test）
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>

#include "core.hpp"

using namespace imgdupe;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

using Pixels = std::vector<uint8_t>;  // kThumb×kThumb×3

// 滑らかな模様（seed ごとに違う）
static Pixels pattern(unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    double fx[3], fy[3], ph[3];
    for (int c = 0; c < 3; ++c) {
        fx[c] = 0.5 + u(rng) * 3;
        fy[c] = 0.5 + u(rng) * 3;
        ph[c] = u(rng) * 6.28;
    }
    Pixels p(kThumb * kThumb * 3);
    for (int y = 0; y < kThumb; ++y)
        for (int x = 0; x < kThumb; ++x)
            for (int c = 0; c < 3; ++c) {
                double v = 128 + 100 * std::sin(fx[c] * x / kThumb * 6.28 + ph[c]) * std::cos(fy[c] * y / kThumb * 6.28);
                p[(y * kThumb + x) * 3 + c] = static_cast<uint8_t>(std::clamp(v, 0.0, 255.0));
            }
    return p;
}

static Pixels noisy(Pixels p, unsigned seed, int amount) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> d(-amount, amount);
    for (auto& v : p) v = static_cast<uint8_t>(std::clamp(v + d(rng), 0, 255));
    return p;
}

static void testSha256() {
    CHECK(toHex(sha256("abc", 3)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(toHex(sha256("", 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::string m(1000, 'a');
    CHECK(toHex(sha256(m.data(), m.size())) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

static void testFeatures() {
    const Pixels a = pattern(1);
    const Features fa = computeFeatures(a.data(), 1);
    CHECK(fa.ok);
    CHECK(fa.contrast > 20);
    CHECK(distance(fa, computeFeatures(noisy(a, 7, 6).data(), 1)) <= 6);
    CHECK(distance(fa, computeFeatures(pattern(2).data(), 1)) > 20);

    // 単色
    Pixels flat(kThumb * kThumb * 3, 200);
    CHECK(computeFeatures(flat.data(), 1).contrast == 0);
    CHECK(computeFeatures(flat.data(), 1).r == 200);

    // 向き: 反時計回りに回した画像に「時計回りに 90°」(6) を当てると元に戻る
    for (int o : {2, 3, 4, 5, 6, 7, 8}) {
        Pixels rotated = a;
        // o を当てると元に戻るような画像を作る（逆変換）
        static const int inverse[9] = {1, 1, 2, 3, 4, 5, 8, 7, 6};
        applyOrientation(rotated, kThumb, inverse[o]);
        if (o != 1) CHECK(rotated != a);
        const Features fr = computeFeatures(rotated.data(), o);
        CHECK(fr.dh == fa.dh && fr.dv == fa.dv);
    }
}

static void testShrinker() {
    // 64×48 の画像（左半分が赤、右半分が青）を 32×32 に
    const uint32_t w = 64, h = 48;
    std::vector<uint8_t> img(w * h * 3);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &img[(y * w + x) * 3];
            p[0] = x < w / 2 ? 200 : 10;  // B,G,R の順で渡す
            p[1] = static_cast<uint8_t>(y * 5);
            p[2] = x < w / 2 ? 10 : 200;
        }
    Shrinker s(w, h);
    for (uint32_t y = 0; y < h; y += 7) s.addRows(&img[y * w * 3], w * 3, y, std::min<uint32_t>(7, h - y), true);
    const auto out = s.finish();
    CHECK(out.size() == kThumb * kThumb * 3);
    CHECK(out[0] == 10 && out[2] == 200);                             // 左上は赤
    CHECK(out[(kThumb - 1) * 3] == 200 && out[(kThumb - 1) * 3 + 2] == 10);  // 右上は青
    CHECK(out[((kThumb - 1) * kThumb) * 3 + 1] > out[1]);              // 下ほど緑が多い

    // 入力が小さい時も全部の画素が埋まる
    std::vector<uint8_t> tiny(5 * 3 * 3, 77);
    Shrinker t(5, 3);
    t.addRows(tiny.data(), 15, 0, 3, false);
    for (uint8_t v : t.finish()) CHECK(v == 77);
}

static void testSimilarMatchesBruteForce() {
    std::mt19937_64 rng(42);
    std::vector<Features> items;
    // 似たもの同士の塊 + ばらばら
    for (int c = 0; c < 300; ++c) {
        Features base;
        base.ok = true;
        base.dh = rng();
        base.dv = rng();
        base.r = base.g = base.b = static_cast<uint8_t>(rng() % 256);
        const int copies = 1 + static_cast<int>(rng() % 4);
        for (int k = 0; k < copies; ++k) {
            Features f = base;
            const int flips = static_cast<int>(rng() % 9);
            for (int b = 0; b < flips; ++b) (b % 2 ? f.dh : f.dv) ^= 1ull << (rng() % 64);
            if (rng() % 10 == 0) f.r = static_cast<uint8_t>(f.r + 100);  // 色だけ違う
            items.push_back(f);
        }
    }
    for (int threshold : {0, 1, 4, 5, 10, 16}) {
        SimilarOptions o;
        o.threshold = threshold;
        o.threads = 3;
        auto canon = [](Similar s) {
            for (auto& c : s.clusters) std::sort(c.begin(), c.end());
            std::sort(s.clusters.begin(), s.clusters.end());
            return s;
        };
        const Similar a = canon(findSimilar(items, o));
        const Similar b = canon(findSimilarBruteForce(items, o));
        CHECK(a.clusters == b.clusters);
        CHECK(a.nearest == b.nearest);
        if (threshold == 10) CHECK(!a.clusters.empty());
    }
}

static void writeRaw(const fs::path& p, const Pixels& px) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(px.data()), static_cast<std::streamsize>(px.size()));
}

static Callbacks fakeCallbacks(int& decodes) {
    Callbacks cb;
    cb.scan = [](const std::string& root, std::vector<ScannedFile>& out) {
        for (const auto& e : fs::recursive_directory_iterator(root)) {
            if (!e.is_regular_file() || !isImageExt(e.path().string())) continue;
            out.push_back({e.path().string(), static_cast<uint64_t>(e.file_size()),
                           static_cast<int64_t>(e.last_write_time().time_since_epoch().count())});
        }
    };
    // テスト用の「画像」は 32×32×3 の生データ。それ以外の大きさは読めない画像
    cb.decode = [&decodes](const std::string& path) {
        static std::mutex m;
        {
            std::lock_guard<std::mutex> lock(m);
            ++decodes;
        }
        std::ifstream in(path, std::ios::binary);
        Pixels px(kThumb * kThumb * 3);
        in.read(reinterpret_cast<char*>(px.data()), static_cast<std::streamsize>(px.size()));
        if (in.gcount() != static_cast<std::streamsize>(px.size()) || in.peek() != EOF) return Features{};
        Features f = computeFeatures(px.data(), 1);
        f.width = f.height = kThumb;
        return f;
    };
    return cb;
}

static void testRun() {
    const fs::path root = fs::temp_directory_path() / "imgdupe-test";
    fs::remove_all(root);
    const fs::path pics = root / "pics";
    writeRaw(pics / "a.jpg", pattern(1));
    writeRaw(pics / "sub/a_copy.jpg", pattern(1));        // 完全一致
    writeRaw(pics / "a_noisy.png", noisy(pattern(1), 3, 5));  // 見た目一致
    writeRaw(pics / "b.jpg", pattern(2));
    writeRaw(pics / "b2.jpg", noisy(pattern(2), 4, 3));     // 見た目一致
    writeRaw(pics / "c.jpg", pattern(3));                   // 1 枚だけ
    writeRaw(pics / "white1.png", Pixels(kThumb * kThumb * 3, 250));
    writeRaw(pics / "white2.png", Pixels(kThumb * kThumb * 3, 245));
    writeRaw(pics / "d.jpg", pattern(4));
    writeRaw(pics / "d_copy.jpg", pattern(4));  // 完全一致だけ（見た目一致では出さない）
    std::ofstream(pics / "broken.jpg") << "not an image";
    std::ofstream(pics / "memo.txt") << "x";

    RunOptions opt;
    opt.roots = {pics.string()};
    opt.cacheFile = (root / "cache.bin").string();
    opt.threads = 2;
    int decodes = 0;
    const Callbacks cb = fakeCallbacks(decodes);
    Progress prog;
    std::atomic<bool> cancel{false};

    Result r = run(opt, cb, prog, cancel);
    CHECK(!r.cancelled);
    CHECK(r.files.size() == 11);
    CHECK(r.unreadable == 1);
    CHECK(r.flatSkipped == 2);
    CHECK(decodes == 11);
    int exact = 0, similar = 0;
    for (const auto& g : r.groups) (g.kind == Kind::Exact ? exact : similar)++;
    CHECK(exact == 2);
    CHECK(similar == 2);  // a 系（a, a_copy, a_noisy）と b 系
    for (const auto& g : r.groups) {
        if (g.kind != Kind::Similar) continue;
        CHECK(g.members.size() == 2 || g.members.size() == 3);
        CHECK(g.members[0].dist <= g.members.back().dist);
    }
    CHECK(prog.stage == static_cast<int>(Stage::Done));

    // 2 回目はキャッシュを使う。消したファイルはキャッシュからも消える
    fs::remove(pics / "c.jpg");
    writeRaw(pics / "e.jpg", pattern(5));
    decodes = 0;
    r = run(opt, cb, prog, cancel);
    CHECK(decodes == 1);
    CHECK(r.hashed == 1);
    CHECK(r.files.size() == 11);
    Cache cache;
    CHECK(cache.load(opt.cacheFile));
    CHECK(cache.entries.size() == 11);
    CHECK(!cache.entries.count((pics / "c.jpg").string()));
    CHECK(cache.entries.at((pics / "a.jpg").string()).hasSha);

    // 単色に近い画像も比べる
    opt.includeFlat = true;
    r = run(opt, cb, prog, cancel);
    CHECK(r.flatSkipped == 0);
    CHECK(r.groups.size() == 5);  // 白 2 枚がまとまる

    // 完全一致だけ
    opt.exactOnly = true;
    r = run(opt, cb, prog, cancel);
    CHECK(r.groups.size() == 2);

    // 1 枚で検索
    const Features q = computeFeatures(noisy(pattern(2), 11, 4).data(), 1);
    const Group g = query(r, q, 10, 40);
    CHECK(g.members.size() == 2);

    // 出力
    CHECK(writeCsv(r, (root / "out.csv").string()));
    CHECK(writeHtml(r, (root / "out.html").string()));
    std::ifstream csv(root / "out.csv");
    std::string line;
    int lines = 0;
    while (std::getline(csv, line)) ++lines;
    CHECK(lines == 5);

    // 中止
    cancel = true;
    r = run(opt, cb, prog, cancel);
    CHECK(r.cancelled);
    fs::remove_all(root);
}

static void testExifOrientation() {
    // FFD8, APP0(JFIF), APP1(Exif: IFD0 に向き 1 項目), SOS
    auto jpeg = [](bool le, uint16_t orientation) {
        std::vector<uint8_t> tiff = le ? std::vector<uint8_t>{'I', 'I', 42, 0, 8, 0, 0, 0}
                                       : std::vector<uint8_t>{'M', 'M', 0, 42, 0, 0, 0, 8};
        auto p16 = [&](uint16_t v) {
            if (le) tiff.insert(tiff.end(), {uint8_t(v), uint8_t(v >> 8)});
            else tiff.insert(tiff.end(), {uint8_t(v >> 8), uint8_t(v)});
        };
        auto p32 = [&](uint32_t v) {
            if (le) { p16(uint16_t(v)); p16(uint16_t(v >> 16)); }
            else { p16(uint16_t(v >> 16)); p16(uint16_t(v)); }
        };
        p16(2);
        p16(0x010F); p16(2); p32(4); p32(0);  // Make（関係ない項目）
        p16(0x0112); p16(3); p32(1); p16(orientation); p16(0);
        p32(0);
        std::vector<uint8_t> d = {0xFF, 0xD8, 0xFF, 0xE0, 0, 6, 'J', 'F', 'I', 'F'};
        std::vector<uint8_t> app1 = {'E', 'x', 'i', 'f', 0, 0};
        app1.insert(app1.end(), tiff.begin(), tiff.end());
        const size_t len = app1.size() + 2;
        d.insert(d.end(), {0xFF, 0xE1, uint8_t(len >> 8), uint8_t(len)});
        d.insert(d.end(), app1.begin(), app1.end());
        d.insert(d.end(), {0xFF, 0xDA, 0, 2});
        return d;
    };
    for (bool le : {true, false})
        for (uint16_t o : {1, 3, 6, 8}) {
            const auto d = jpeg(le, o);
            CHECK(jpegExifOrientation(d.data(), d.size()) == o);
            CHECK(jpegExifOrientation(d.data(), d.size() / 2) == 0);  // 途中で切れていても落ちない
        }
    const auto bad = jpeg(true, 9);
    CHECK(jpegExifOrientation(bad.data(), bad.size()) == 0);
    const uint8_t png[] = {0x89, 'P', 'N', 'G'};
    CHECK(jpegExifOrientation(png, sizeof(png)) == 0);
}

static void testMisc() {
    CHECK(isImageExt("C:\\a\\B.JPG"));
    CHECK(isImageExt("x.heic"));
    CHECK(!isImageExt("x.txt"));
    CHECK(!isImageExt("dir.jpg/file"));
    CHECK(fileUri("C:\\写真\\a b.jpg") == "file:///C:/%E5%86%99%E7%9C%9F/a%20b.jpg");
    CHECK(fileUri("\\\\nas\\share\\x.jpg") == "file://nas/share/x.jpg");
    CHECK(fileUri("/tmp/x#1.png") == "file:///tmp/x%231.png");
    CHECK(formatSize(512) == "512 B");
    CHECK(formatSize(1536) == "1.5 KB");
    CHECK(formatSize(5ull * 1024 * 1024) == "5.0 MB");

    // 壊れたキャッシュは空として扱う
    const fs::path bad = fs::temp_directory_path() / "imgdupe-bad-cache.bin";
    std::ofstream(bad) << "IMGDUPE1garbage";
    Cache c;
    CHECK(!c.load(bad.string()));
    CHECK(c.entries.empty());
    fs::remove(bad);
}

int main() {
    testSha256();
    testFeatures();
    testShrinker();
    testSimilarMatchesBruteForce();
    testRun();
    testExifOrientation();
    testMisc();
    if (failures) {
        std::fprintf(stderr, "%d 件失敗\n", failures);
        return 1;
    }
    std::puts("すべて成功");
    return 0;
}
