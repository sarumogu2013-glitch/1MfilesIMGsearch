// OS に依存しない部分のテスト（./build.sh test）
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <random>
#include <set>

#include "analyzer.hpp"
#include "commands.hpp"
#include "db.hpp"
#include "indexer.hpp"
#include "meta.hpp"
#include "search.hpp"
#include "links.hpp"
#include "names.hpp"
#include "pathinfer.hpp"
#include "log.hpp"
#include "models.hpp"
#include "tagger.hpp"
#include "vectors.hpp"
#include "clip.hpp"
#include "ann.hpp"
#include "dict.hpp"
#include "refs.hpp"
#include "tagnames.hpp"
#include "tagrel.hpp"
#include "lookup.hpp"
#include "stats.hpp"
#include "quality.hpp"
#include "sameimg.hpp"
#include "aesthetic_weights.hpp"
#include "platform.hpp"
#include "pack.hpp"
#include "core.hpp"
#include "update_logic.hpp"

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

static void testOrientPixels() {
    // 3×2 の画像（1 画素 1 バイト）: 1 2 3 / 4 5 6
    const std::vector<uint8_t> src = {1, 2, 3, 4, 5, 6};
    auto oriented = [&](int o, uint32_t& w, uint32_t& h) {
        std::vector<uint8_t> px = src;
        w = 3;
        h = 2;
        orientPixels(px, w, h, 1, o);
        return px;
    };
    uint32_t w, h;
    CHECK(oriented(1, w, h) == src && w == 3);
    CHECK((oriented(6, w, h) == std::vector<uint8_t>{4, 1, 5, 2, 6, 3}) && w == 2 && h == 3);  // 時計回り
    CHECK((oriented(8, w, h) == std::vector<uint8_t>{3, 6, 2, 5, 1, 4}) && w == 2 && h == 3);  // 反時計回り
    CHECK((oriented(3, w, h) == std::vector<uint8_t>{6, 5, 4, 3, 2, 1}) && w == 3 && h == 2);
    CHECK((oriented(2, w, h) == std::vector<uint8_t>{3, 2, 1, 6, 5, 4}));
    CHECK((oriented(5, w, h) == std::vector<uint8_t>{1, 4, 2, 5, 3, 6}) && w == 2);  // 転置
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
    opt.dbFile = (root / "imgdupe.db").string();
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

    // 重複のまとまりは DB の groups にも残る
    {
        Db db;
        CHECK(db.open(opt.dbFile));
        Stmt g(db.handle(), "SELECT COUNT(*) FROM groups");
        CHECK(g.step() && g.i64(0) == 4);
        Stmt m(db.handle(), "SELECT COUNT(*), SUM(representative) FROM group_members");
        CHECK(m.step() && m.i64(0) == 9 && m.i64(1) == 4);  // グループごとに代表が 1 枚
    }

    // 2 回目は DB の結果を使う。消したファイルは DB からも消える
    fs::remove(pics / "c.jpg");
    writeRaw(pics / "e.jpg", pattern(5));
    decodes = 0;
    r = run(opt, cb, prog, cancel);
    CHECK(decodes == 1);
    CHECK(r.hashed == 1);
    CHECK(r.files.size() == 11);
    CHECK(r.ids.size() == r.files.size());
    {
        Db db;
        CHECK(db.open(opt.dbFile));
        const auto known = loadHashedImages(db, kHashVersion);
        CHECK(known.size() == 11);
        CHECK(!known.count((pics / "c.jpg").string()));
        CHECK(known.at((pics / "a.jpg").string()).e.hasSha);
    }

    // 中身を書き換えたファイル（更新日時が変わる）は読み直す
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writeRaw(pics / "e.jpg", pattern(6));
    fs::last_write_time(pics / "e.jpg", fs::last_write_time(pics / "e.jpg") + std::chrono::seconds(5));
    decodes = 0;
    r = run(opt, cb, prog, cancel);
    CHECK(decodes == 1);

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

static void testUpdateLogic() {
    CHECK(parseVersion("v1.2.30").patch == 30);
    CHECK(parseVersion("dev").major == 0);
    CHECK(compareVersion("v1.0.3", "v1.0.3") == VersionCompare::Latest);
    CHECK(compareVersion("dev", "v1.0.0") == VersionCompare::RemoteNewer);
    CHECK(compareVersion("v1.0.9", "v1.0.10") == VersionCompare::RemoteNewer);
    CHECK(compareVersion("v1.1.0", "v1.0.15") == VersionCompare::LocalNewer);
    const std::string json =
        R"({"tag_name":"v1.0.4","assets":[{"name":"imgdupe.zip","url":"https://api.github.com/a/1"},)"
        R"({"name":"imgdupe.exe","url":"https://api.github.com/a/2","digest":"sha256:ABCdef01"}]})";
    ReleaseInfo info;
    CHECK(parseLatestRelease(json, "imgdupe.exe", info));
    CHECK(info.tag == "v1.0.4" && info.hasAsset && info.assetUrl == "https://api.github.com/a/2");
    CHECK(info.sha256 == "abcdef01");
    CHECK(!parseLatestRelease("{broken", "imgdupe.exe", info));
    CHECK(httpErrorMessage(200).empty());
    CHECK(!httpErrorMessage(404).empty());
}

static void testKeys() {
    KeyStroke k;
    CHECK(parseKey("Ctrl+Shift+o", k) && k.mods == (KMOD_CTRL | KMOD_SHIFT) && k.code == 'O');
    CHECK(keyName(k) == "Ctrl+Shift+O");
    for (const char* t : {"F5", "Ctrl+Up", "Alt+Enter", "Num5", "Num+", "PageDown", "Shift+F12", "VK1F"}) {
        KeyStroke a;
        CHECK(parseKey(t, a));
        CHECK(keyName(a) == t);
    }
    CHECK(!parseKey("Ctrl+", k));
    CHECK(!parseKey("F99", k));
    CHECK(!parseKey("Nothing", k));

    const KeyMap def = defaultKeys();
    KeyStroke f5;
    parseKey("F5", f5);
    CHECK(def.at(f5) == CMD_SEARCH);
    CHECK(keyLabel(def, CMD_SEARCH) == "F5");
    CHECK(keyLabel(def, CMD_ABOUT).empty());

    // 保存した行から読み直すと同じ。知らない機能・キーは飛ばす
    auto lines = keyLines(def);
    CHECK(!lines.empty());
    CHECK(keysFromLines(lines) == def);
    lines.push_back("NoSuchCommand = F9");
    lines.push_back("Help = F1, Bogus, Ctrl+H");
    const KeyMap m = keysFromLines(lines);
    KeyStroke ch;
    parseKey("Ctrl+H", ch);
    CHECK(m.at(ch) == CMD_HELP);
    CHECK(m.size() == def.size() + 1);
    for (const auto& c : allCommands()) CHECK(findCommand(c.name) == &c && findCommand(c.id) == &c);
}

// 画像のパスに "fail" があれば失敗、それ以外はパスの文字数をタグにする解析器
class FakeAnalyzer : public Analyzer {
public:
    explicit FakeAnalyzer(int v) : v_(v) {}
    std::string stageName() const override { return stage::kTagger; }
    int version() const override { return v_; }
    AnalyzerOutput analyze(const AnalyzerInput& in) override {
        calls.fetch_add(1);
        AnalyzerOutput out;
        out.ok = in.path.find("fail") == std::string::npos;
        if (out.ok) out.tags.push_back({"len" + std::to_string(in.path.size()), "general", "", 0.5, false});
        return out;
    }
    std::atomic<int> calls{0};

private:
    int v_;
};

static void testDb() {
    const fs::path dir = fs::temp_directory_path() / "imgdupe-db-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "t.db").string();
    Db db;
    CHECK(db.open(path));
    CHECK(db.schemaVersion() == kSchemaVersion);
    for (const char* t : {"images", "stages", "tags", "tag_dict", "embeddings", "groups", "group_members", "sources",
                          "links", "history", "commands", "settings", "file_meta", "thumbs"}) {
        Stmt st(db.handle(), "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?");
        st.bind(1, std::string(t));
        CHECK(st.step() && st.i64(0) == 1);
    }
    {
        Stmt st(db.handle(), "PRAGMA journal_mode");
        CHECK(st.step() && st.text(0) == "wal");
    }
    db.setSetting("a", "1");
    db.setSetting("a", "2");
    CHECK(db.setting("a") == "2");
    CHECK(db.setting("none", "def") == "def");

    // 登録と、変わったファイルの段階のやり直し
    std::vector<ScannedFile> files = {{"/p/a.jpg", 10, 100}, {"/p/b.jpg", 20, 200}, {"/p/fail.jpg", 30, 300}};
    auto ids = upsertScanned(db, files);
    CHECK(ids.size() == 3);
    CHECK(pendingImages(db, stage::kHash, kHashVersion).size() == 3);
    Features f;
    f.ok = true;
    f.dh = 0xFFFFFFFFFFFFFFFFull;  // 符号付きで保存しても戻る
    f.r = 1, f.g = 2, f.b = 3;
    saveFeatures(db, {{ids["/p/a.jpg"], f}}, kHashVersion);
    auto known = loadHashedImages(db, kHashVersion);
    CHECK(known.size() == 1);
    CHECK(known.at("/p/a.jpg").e.f.dh == 0xFFFFFFFFFFFFFFFFull && known.at("/p/a.jpg").e.f.b == 3);
    CHECK(loadHashedImages(db, kHashVersion + 1).empty());  // 版が変われば使わない
    files[0].mtime = 101;
    CHECK(upsertScanned(db, files)["/p/a.jpg"] == ids["/p/a.jpg"]);  // 同じ ID のまま
    CHECK(loadHashedImages(db, kHashVersion).empty());

    // 解析器: 済んだ画像は飛ばし、版が上がったらやり直す。失敗は同じ版では再挑戦しない
    std::atomic<bool> cancel{false};
    FakeAnalyzer a1(1);
    StageReport rep = runStage(db, a1, 2, cancel, nullptr, 2);
    CHECK(rep.processed == 3 && rep.failed == 1 && a1.calls == 3);
    CHECK(tagsOf(db, ids["/p/b.jpg"]).size() == 1);
    CHECK(tagsOf(db, ids["/p/b.jpg"])[0].source == stage::kTagger);
    rep = runStage(db, a1, 2, cancel);
    CHECK(rep.processed == 0 && a1.calls == 3);
    FakeAnalyzer a2(2);
    rep = runStage(db, a2, 2, cancel);
    CHECK(rep.processed == 3);
    CHECK(tagsOf(db, ids["/p/b.jpg"]).size() == 1);  // 同じ出どころのタグは置き換わる

    // 手動で確定したタグは解析器に上書きされない
    {
        Stmt st(db.handle(), "UPDATE tags SET manual=1, confidence=1.0 WHERE image_id=?");
        st.bind(1, ids["/p/b.jpg"]);
        st.step();
    }
    replaceTags(db, ids["/p/b.jpg"], stage::kTagger, {{"len9", "general", "", 0.1, false}});
    for (const auto& t : tagsOf(db, ids["/p/b.jpg"]))
        if (t.manual) CHECK(t.confidence == 1.0);

    // 消えたファイル（フォルダの下だけ）。段階・タグも一緒に消える
    std::unordered_map<std::string, int64_t> alive = {{"/p/a.jpg", ids["/p/a.jpg"]}};
    CHECK(removeMissing(db, {"/other"}, alive) == 0);
    CHECK(removeMissing(db, {"/p"}, alive) == 2);
    {
        Stmt st(db.handle(), "SELECT COUNT(*) FROM tags WHERE image_id=?");
        st.bind(1, ids["/p/b.jpg"]);
        CHECK(st.step() && st.i64(0) == 0);
    }

    // GUI と indexer のやりとり
    setRoots(db, {"D:\\写真", "E:\\Backup"});
    CHECK((getRoots(db) == std::vector<std::string>{"D:\\写真", "E:\\Backup"}));
    setRoots(db, {});
    CHECK(getRoots(db).empty());
    addCommand(db, "rescan");
    addCommand(db, "quit");
    CHECK((takeCommands(db) == std::vector<std::string>{"rescan", "quit"}));
    CHECK(takeCommands(db).empty());

    // 前の版のキャッシュの取り込み
    Cache c;
    Entry e;
    e.path = "/q/x.png";
    e.size = 5;
    e.mtime = 6;
    e.f.ok = true;
    e.f.dv = 42;
    c.entries[e.path] = e;
    CHECK(importCache(db, c, kHashVersion) == 1);
    CHECK(importCache(db, c, kHashVersion) == 0);
    CHECK(loadHashedImages(db, kHashVersion).at("/q/x.png").e.f.dv == 42);
    db.close();

    // 新しい版の imgdupe で作った DB は開かない
    {
        Db d2;
        CHECK(d2.open(path));
        d2.exec("PRAGMA user_version=999");
    }
    Db d3;
    CHECK(!d3.open(path));
    CHECK(!d3.error().empty());
    fs::remove_all(dir);
}

static void testPack() {
    const fs::path dir = fs::temp_directory_path() / "imgdupe-pack-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    auto ids = upsertScanned(db, {{"/p/1.jpg", 1, 1}, {"/p/2.jpg", 2, 2}, {"/p/3.jpg", 3, 3}});
    ThumbPack pack(dir.string(), 200);  // 小さくして、2 つ目のパックに移るところも試す
    ThumbData a;
    a.width = 160;
    a.height = 120;
    a.bytes.assign(100, 7);
    ThumbData b = a;
    b.bytes.assign(150, 9);
    b.format = ThumbFormat::WebP;
    CHECK(pack.put(db, ids["/p/1.jpg"], a));
    CHECK(pack.put(db, ids["/p/2.jpg"], b));  // 200 を超えるので次のパックへ
    CHECK(fs::exists(pack.packPath(0)) && fs::exists(pack.packPath(1)));
    ThumbData got;
    CHECK(pack.get(db, ids["/p/1.jpg"], got) && got.bytes == a.bytes && got.width == 160 && got.height == 120);
    CHECK(pack.get(db, ids["/p/2.jpg"], got) && got.bytes == b.bytes && got.format == ThumbFormat::WebP);
    CHECK(!pack.get(db, ids["/p/3.jpg"], got));
    CHECK(pack.garbageBytes(db) == 0);
    // 作り直すと古い分は使われない部分になる
    a.bytes.assign(50, 1);
    CHECK(pack.put(db, ids["/p/1.jpg"], a));
    CHECK(pack.get(db, ids["/p/1.jpg"], got) && got.bytes == a.bytes);
    CHECK(pack.garbageBytes(db) == 100 + 21);
    // パックが壊れていたら読めない（落ちない）
    {
        FILE* f = std::fopen(pack.packPath(1).c_str(), "r+b");
        std::fputc('X', f);
        std::fclose(f);
    }
    CHECK(!pack.get(db, ids["/p/2.jpg"], got));

    // 解析器の store がパックに書く
    class ThumbFake : public Analyzer {
    public:
        explicit ThumbFake(ThumbPack& p) : pack_(p) {}
        std::string stageName() const override { return stage::kThumb; }
        int version() const override { return 1; }
        AnalyzerOutput analyze(const AnalyzerInput& in) override {
            AnalyzerOutput out;
            out.data.assign(in.path.begin(), in.path.end());
            out.width = out.height = 8;
            return out;
        }
        bool store(Db& d, int64_t id, AnalyzerOutput& out) override {
            ThumbData t;
            t.width = static_cast<uint16_t>(out.width);
            t.height = static_cast<uint16_t>(out.height);
            t.bytes = out.data;
            return pack_.put(d, id, t);
        }

    private:
        ThumbPack& pack_;
    };
    ThumbPack big(dir.string());
    ThumbFake tf(big);
    std::atomic<bool> cancel{false};
    CHECK(runStage(db, tf, 2, cancel).processed == 3);
    CHECK(big.get(db, ids["/p/3.jpg"], got) && std::string(got.bytes.begin(), got.bytes.end()) == "/p/3.jpg");
    db.close();

    // 版 1 の DB を開くと、版 2（thumbs テーブル）へ移行する
    {
        Db d;
        CHECK(d.open((dir / "old.db").string()));
        d.exec("DROP TABLE thumbs; PRAGMA user_version=1;");
    }
    Db d2;
    CHECK(d2.open((dir / "old.db").string()));
    CHECK(d2.schemaVersion() == kSchemaVersion);
    Stmt st(d2.handle(), "SELECT COUNT(*) FROM sqlite_master WHERE name='thumbs'");
    CHECK(st.step() && st.i64(0) == 1);
    fs::remove_all(dir);
}

static void testIndexer() {
    const fs::path root = fs::temp_directory_path() / "imgdupe-index-test";
    fs::remove_all(root);
    const fs::path pics = root / "pics";
    writeRaw(pics / "a.jpg", pattern(1));
    writeRaw(pics / "a_noisy.png", noisy(pattern(1), 3, 5));
    writeRaw(pics / "b.jpg", pattern(2));
    std::ofstream(pics / "broken.jpg") << "x";

    int decodes = 0;
    const Callbacks base = fakeCallbacks(decodes);
    std::atomic<int> encodes{0};
    IndexCallbacks cb;
    cb.scan = base.scan;
    cb.decode = base.decode;
    cb.encodeThumb = [&encodes](const std::string& path, int side, std::vector<uint8_t>& bytes, int& w, int& h) {
        encodes.fetch_add(1);
        if (path.find("broken") != std::string::npos) return false;
        bytes.assign(path.begin(), path.end());
        w = h = side;
        return true;
    };
    std::atomic<int> metas{0};
    cb.readMeta = [&metas](const std::string& path, FileMeta& out) {
        metas.fetch_add(1);
        out.title = "title of " + path.substr(path.find_last_of('/') + 1);
        return true;
    };
    IndexOptions opt;
    opt.roots = {pics.string()};
    opt.dbFile = (root / "imgdupe.db").string();
    opt.packDir = root.string();
    opt.threads = 2;
    IndexProgress prog;
    std::atomic<bool> cancel{false};

    // GPU の側（タガー）も同時に流す: 推論を差し替えた解析器（まとめて 2 枚ずつ）
    struct FakeGpu : Analyzer {
        std::string stageName() const override { return stage::kTagger; }
        int version() const override { return 7; }
        AnalyzerOutput analyze(const AnalyzerInput&) override { return {}; }
        size_t batchSize() const override { return 2; }
        std::shared_ptr<void> prepare(const AnalyzerInput& in) override {
            return in.path.find("broken") != std::string::npos ? nullptr : std::make_shared<std::string>(in.path);
        }
        void inferBatch(const std::vector<std::shared_ptr<void>>& prepared, std::vector<AnalyzerOutput>& outs) override {
            for (size_t k = 0; k < prepared.size(); ++k) outs[k].tags = {{"gpu_tag", "general", "tagger", 0.9, false}};
        }
    };
    cb.taggerVersion = [] { return 7; };
    cb.makeTagger = [](std::string&) { return std::shared_ptr<Analyzer>(std::make_shared<FakeGpu>()); };
    IndexReport r = indexOnce(opt, cb, prog, cancel);
    CHECK(r.files == 4 && r.added == 4 && r.removed == 0);
    CHECK(r.hashed == 4 && decodes == 4);
    CHECK(r.thumbs == 3 && r.failed == 2 && encodes == 4);  // 読めない画像はサムネイルとタガーで 1 つずつ
    CHECK(r.metas == 4 && metas == 4);
    CHECK(prog.phase == static_cast<int>(IndexPhase::Done));
    CHECK(r.tagged == 3);  // 読めない 1 枚は失敗
    CHECK(prog.gpuPhase == static_cast<int>(IndexPhase::Idle));
    CHECK((prog.doneMask & (1u << static_cast<int>(IndexPhase::Tagger))) && (prog.doneMask & (1u << static_cast<int>(IndexPhase::Thumb))));

    // 2 回目は何もしない。消したファイルは DB から消える
    fs::remove(pics / "b.jpg");
    decodes = 0;
    encodes = 0;
    r = indexOnce(opt, cb, prog, cancel);
    CHECK(r.added == 0 && r.removed == 1 && r.hashed == 0 && decodes == 0 && encodes == 0);

    // indexer が済ませた分は、検索でも読み直さない
    RunOptions ro;
    ro.roots = opt.roots;
    ro.dbFile = opt.dbFile;
    ro.threads = 2;
    Progress p2;
    const Result res = run(ro, base, p2, cancel);
    CHECK(res.hashed == 0 && decodes == 0);
    CHECK(res.files.size() == 3);

    // サムネイルはパックから読める
    Db db;
    CHECK(db.open(opt.dbFile));
    ThumbPack pack(opt.packDir);
    ThumbData t;
    CHECK(pack.get(db, res.ids[0], t) && t.width == kThumbSide);
    fs::remove_all(root);
}

static void testScanTasks() {
    // 調べるフォルダの書き方にそろえる（同じ画像を別のパスで二重に登録しない）
    CHECK(spellLikeRoot("d:\\Pics\\a\\b.jpg", {"D:\\pics"}) == "D:\\pics\\a\\b.jpg");
    CHECK(spellLikeRoot("D:/pics/a", {"D:\\pics"}) == "D:\\pics\\a");
    CHECK(spellLikeRoot("D:\\pics", {"D:\\pics"}) == "D:\\pics");
    CHECK(spellLikeRoot("D:\\picsX\\a", {"D:\\pics"}) == "D:\\picsX\\a");
    CHECK(spellLikeRoot("E:\\x", {"D:\\pics"}) == "E:\\x");
    CHECK(spellLikeRoot("d:\\x", {"D:\\"}) == "D:\\x");
    {
        const fs::path dbp = fs::temp_directory_path() / "imgdupe-misspell.db";
        fs::remove(dbp);
        Db d;
        CHECK(d.open(dbp.string()));
        auto ids = upsertScanned(d, {{"D:\\pics\\a.jpg", 1, 1}, {"d:\\Pics\\a.jpg", 1, 1}, {"d:\\Pics\\only.jpg", 1, 1},
                                     {"E:\\b.jpg", 1, 1}});
        setManualTag(d, ids["d:\\Pics\\a.jpg"], "miku", true);
        CHECK(removeMisspelled(d, {"D:\\pics"}) == 1);  // 正しい書き方の同じ画像があるものだけ
        CHECK(imageCount(d) == 3);
        bool moved = false;
        for (const auto& t : tagsOf(d, ids["D:\\pics\\a.jpg"])) moved |= t.tag == "miku" && t.manual;
        CHECK(moved);
        d.close();
        fs::remove(dbp);
    }
    const fs::path root = fs::temp_directory_path() / "imgdupe-task-test";
    fs::remove_all(root);
    const fs::path pics = root / "pics", extra = root / "extra";
    writeRaw(pics / "a.jpg", pattern(1));
    writeRaw(pics / "b.jpg", pattern(2));
    writeRaw(extra / "c.jpg", pattern(3));
    writeRaw(extra / "d.jpg", pattern(4));

    Db db;
    CHECK(db.open((root / "imgdupe.db").string()));
    // 並び: 足した順、top は一番上
    const int64_t t1 = addScanTask(db, "フォルダ: extra", {extra.string()}, false);
    const int64_t t2 = addScanTask(db, "ファイル: a.jpg", {(pics / "a.jpg").string()}, true);
    const int64_t t3 = addScanTask(db, "3", {"/nowhere"}, false);
    auto order = [&] {
        std::vector<int64_t> v;
        for (const auto& t : scanTasks(db)) v.push_back(t.id);
        return v;
    };
    CHECK((order() == std::vector<int64_t>{t2, t1, t3}));
    CHECK(scanTasks(db)[1].paths.size() == 1 && scanTasks(db)[1].label == "フォルダ: extra");
    moveScanTask(db, t3, TaskMove::Top);
    CHECK((order() == std::vector<int64_t>{t3, t2, t1}));
    moveScanTask(db, t3, TaskMove::Down);
    CHECK((order() == std::vector<int64_t>{t2, t3, t1}));
    moveScanTask(db, t2, TaskMove::Bottom);
    CHECK((order() == std::vector<int64_t>{t3, t1, t2}));
    setScanTaskOrder(db, {t2, t3});
    CHECK((order() == std::vector<int64_t>{t2, t3, t1}));
    removeScanTask(db, t3);
    CHECK((order() == std::vector<int64_t>{t2, t1}));

    // タスクだけの回: ファイル 1 つとフォルダだけを調べ、全部済ませてタスクを消す（roots の pics/b.jpg は触らない）
    int decodes = 0;
    const Callbacks base = fakeCallbacks(decodes);
    IndexCallbacks cb;
    cb.scan = [&base](const std::string& r, std::vector<ScannedFile>& out) {
        if (fs::is_regular_file(r)) {
            out.push_back({r, static_cast<uint64_t>(fs::file_size(r)),
                           static_cast<int64_t>(fs::last_write_time(r).time_since_epoch().count())});
            return;
        }
        if (fs::exists(r)) base.scan(r, out);
    };
    cb.decode = base.decode;
    IndexOptions opt;
    opt.roots = {pics.string()};
    opt.dbFile = (root / "imgdupe.db").string();
    opt.packDir = root.string();
    opt.thumbnails = false;
    opt.threads = 2;
    opt.fullPass = false;
    std::atomic<bool> preempt{false};
    opt.preempt = &preempt;
    IndexProgress prog;
    std::atomic<bool> cancel{false};
    IndexReport r = indexOnce(opt, cb, prog, cancel);
    CHECK(r.tasks == 2 && r.files == 3 && r.hashed == 3 && decodes == 3);
    CHECK(scanTasks(db).empty());
    CHECK(imageCount(db) == 3);
    CHECK(prog.task == -1);

    // フォルダ全体: 残りの b.jpg だけ
    decodes = 0;
    opt.fullPass = true;
    r = indexOnce(opt, cb, prog, cancel);
    CHECK(r.tasks == 0 && r.hashed == 1 && decodes == 1 && imageCount(db) == 4);

    // yield が立っていれば、段階はすぐ譲る
    {
        Stmt st(db.handle(), "DELETE FROM stages");
        st.step();
    }
    preempt = true;
    HashAnalyzer hash(cb);
    const StageReport sr = runStage(db, hash, 1, cancel, nullptr, 256, nullptr, &preempt);
    CHECK(sr.yielded && sr.cancelled && sr.processed == 0);
    db.close();
    fs::remove_all(root);
}

static std::vector<uint8_t> pngChunk(const std::string& type, const std::string& data) {
    std::vector<uint8_t> c;
    const uint32_t n = static_cast<uint32_t>(data.size());
    for (int i = 3; i >= 0; --i) c.push_back(static_cast<uint8_t>(n >> (8 * i)));
    c.insert(c.end(), type.begin(), type.end());
    c.insert(c.end(), data.begin(), data.end());
    c.insert(c.end(), {0, 0, 0, 0});  // CRC（読まない）
    return c;
}

static void testMeta() {
    // ファイル名の ID
    auto ids = parseFilenameIds("D:\\pixiv\\98765432_p3_master1200.jpg");
    CHECK(ids.size() == 1 && ids[0].site == "pixiv" && ids[0].postId == "98765432" && ids[0].page == 3);
    ids = parseFilenameIds("/x/user-1789012345678901234-img1.png");
    CHECK(ids.size() == 1 && ids[0].site == "x" && ids[0].postId == "1789012345678901234");
    ids = parseFilenameIds("D41D8CD98F00B204E9800998ECF8427E.jpg");
    CHECK(ids.size() == 1 && ids[0].site == "md5" && ids[0].postId == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(parseFilenameIds("IMG_20240501_123456.jpg").empty());
    CHECK(parseFilenameIds("123_p0.jpg").empty());  // 短すぎる数字は pixiv にしない

    // PNG のテキスト（tEXt は Latin-1、iTXt は UTF-8）とアニメーション
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto add = [&](const std::vector<uint8_t>& c) { png.insert(png.end(), c.begin(), c.end()); };
    add(pngChunk("IHDR", std::string(13, '\0')));
    add(pngChunk("tEXt", std::string("parameters\0masterpiece, 1girl", 29)));
    add(pngChunk("iTXt", std::string("Title\0\0\0\0\0", 10) + "夏の空"));
    add(pngChunk("acTL", std::string(8, '\0')));
    add(pngChunk("IEND", ""));
    PngInfo pi;
    CHECK(parsePng(png.data(), png.size(), pi));
    CHECK(pi.animated && pi.texts.size() == 2);
    CHECK(pi.texts[0].first == "parameters" && pi.texts[0].second == "masterpiece, 1girl");
    CHECK(pi.texts[1].first == "Title" && pi.texts[1].second == "夏の空");
    PngInfo cut;
    CHECK(parsePng(png.data(), 40, cut));  // 途中で切れていても落ちない
    const uint8_t jpg[] = {0xFF, 0xD8, 0xFF};
    CHECK(!parsePng(jpg, 3, cut));

    // JPEG の EXIF（IFD0 と Exif IFD、Windows の XPKeywords）と XMP（優先される）
    {
        std::vector<uint8_t> tiff = {'I', 'I', 42, 0, 8, 0, 0, 0};
        auto p16 = [&](uint16_t v) { tiff.insert(tiff.end(), {uint8_t(v), uint8_t(v >> 8)}); };
        auto p32 = [&](uint32_t v) { p16(uint16_t(v)); p16(uint16_t(v >> 16)); };
        // 値の置き場（IFD の後ろ）を先に決める
        const std::string make = "Canon", model = "Canon EOS R5", date = "2024:05:01 12:34:56";
        std::vector<uint8_t> xpkw;  // "風景;海" を UTF-16LE で
        for (char16_t c : std::u16string(u"風景;海")) xpkw.insert(xpkw.end(), {uint8_t(c), uint8_t(c >> 8)});
        xpkw.insert(xpkw.end(), {0, 0});
        const uint32_t ifd0 = 8, ifd0Count = 4, exifIfd = ifd0 + 2 + ifd0Count * 12 + 4, exifCount = 2;
        uint32_t data = exifIfd + 2 + exifCount * 12 + 4;
        const uint32_t makeAt = data, modelAt = makeAt + uint32_t(make.size() + 1), xpAt = modelAt + uint32_t(model.size() + 1),
                       dateAt = xpAt + uint32_t(xpkw.size());
        p16(ifd0Count);
        p16(0x010F); p16(2); p32(uint32_t(make.size() + 1)); p32(makeAt);
        p16(0x0110); p16(2); p32(uint32_t(model.size() + 1)); p32(modelAt);
        p16(0x8769); p16(4); p32(1); p32(exifIfd);
        p16(0x9C9E); p16(1); p32(uint32_t(xpkw.size())); p32(xpAt);
        p32(0);
        p16(exifCount);
        p16(0x8827); p16(3); p32(1); p16(400); p16(0);
        p16(0x9003); p16(2); p32(uint32_t(date.size() + 1)); p32(dateAt);
        p32(0);
        for (const std::string* str : {&make, &model}) {
            tiff.insert(tiff.end(), str->begin(), str->end());
            tiff.push_back(0);
        }
        tiff.insert(tiff.end(), xpkw.begin(), xpkw.end());
        tiff.insert(tiff.end(), date.begin(), date.end());
        tiff.push_back(0);

        auto segment = [](std::vector<uint8_t>& out, const std::string& head, const std::vector<uint8_t>& body) {
            const size_t len = head.size() + body.size() + 2;
            out.insert(out.end(), {0xFF, 0xE1, uint8_t(len >> 8), uint8_t(len)});
            out.insert(out.end(), head.begin(), head.end());
            out.insert(out.end(), body.begin(), body.end());
        };
        std::vector<uint8_t> jpg = {0xFF, 0xD8};
        segment(jpg, std::string("Exif\0\0", 6), tiff);
        FileMeta fm;
        CHECK(parseJpegMeta(jpg.data(), jpg.size(), fm));
        CHECK(fm.camera == "Canon EOS R5" && fm.iso == 400 && fm.taken == 1714566896);
        CHECK((fm.keywords == std::vector<std::string>{"風景", "海"}));
        CHECK(fm.rating == -1);

        const std::string xmp =
            "<x:xmpmeta><rdf:RDF><rdf:Description xmp:Rating=\"4\"><dc:subject><rdf:Bag><rdf:li>夏</rdf:li>"
            "<rdf:li>空 &amp; 海</rdf:li></rdf:Bag></dc:subject><dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">夕焼け"
            "</rdf:li></rdf:Alt></dc:title></rdf:Description></rdf:RDF></x:xmpmeta>";
        segment(jpg, std::string("http://ns.adobe.com/xap/1.0/") + std::string(1, '\0'),
                std::vector<uint8_t>(xmp.begin(), xmp.end()));
        jpg.insert(jpg.end(), {0xFF, 0xDA, 0, 2});
        FileMeta fx;
        CHECK(parseJpegMeta(jpg.data(), jpg.size(), fx));
        CHECK((fx.keywords == std::vector<std::string>{"夏", "空 & 海"}));  // XMP を優先
        CHECK(fx.rating == 4 && fx.title == "夕焼け" && fx.camera == "Canon EOS R5");
        FileMeta cut;
        CHECK(parseJpegMeta(jpg.data(), 30, cut));  // 途中で切れていても落ちない

        FileMeta a, b;
        a.title = "keep";
        b.title = "other";
        b.camera = "X";
        b.rating = 2;
        fillMissing(a, b);
        CHECK(a.title == "keep" && a.camera == "X" && a.rating == 2);
    }

    std::string host, ref;
    parseZoneIdentifier("[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=https://www.pixiv.net/\r\nHostUrl=https://i.pximg.net/a.png\r\n",
                        host, ref);
    CHECK(host == "https://i.pximg.net/a.png" && ref == "https://www.pixiv.net/");
    CHECK(fileTimeToUnix(116444736000000000ull + 10000000ull * 86400) == 86400);
    CHECK(parseExifDate("2024:05:01 12:34:56") == 1714566896);
    CHECK(parseExifDate("garbage") == 0);
    {
        int y, mo, d, h, mi, se;
        unixToCivil(1714566896, y, mo, d, h, mi, se);
        CHECK(y == 2024 && mo == 5 && d == 1 && h == 12 && mi == 34 && se == 56);
        unixToCivil(civilToUnix(1999, 12, 31, 23, 59, 59), y, mo, d, h, mi, se);
        CHECK(y == 1999 && mo == 12 && d == 31 && h == 23 && se == 59);
        unixToCivil(civilToUnix(1960, 2, 29), y, mo, d, h, mi, se);  // 1970 年より前
        CHECK(y == 1960 && mo == 2 && d == 29);
    }

    // DB に書いて、全文検索・タグ・出典で引ける
    const fs::path dir = fs::temp_directory_path() / "imgdupe-meta-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    auto m = upsertScanned(db, {{"/p/98765432_p0.png", 1, 1}, {"/p/b.jpg", 2, 2}});
    FileMeta fm;
    fm.format = "PNG";
    fm.title = "夏の空と海";
    fm.keywords = {"風景", "海"};
    fm.rating = 4;
    fm.pngText = "masterpiece, 1girl, blue sky";
    fm.zoneUrl = "https://i.pximg.net/a.png";
    fm.fileId = 0x1234;
    storeMeta(db, m["/p/98765432_p0.png"], "/p/98765432_p0.png", fm);
    storeMeta(db, m["/p/b.jpg"], "/p/b.jpg", FileMeta{});
    CHECK((searchMetaText(db, "空と海") == std::vector<int64_t>{m["/p/98765432_p0.png"]}));
    CHECK((searchMetaText(db, "blue sky") == std::vector<int64_t>{m["/p/98765432_p0.png"]}));
    CHECK((searchMetaText(db, "海") == std::vector<int64_t>{m["/p/98765432_p0.png"]}));  // 2 文字以下
    CHECK(searchMetaText(db, "冬").empty());
    const auto tags = tagsOf(db, m["/p/98765432_p0.png"]);
    CHECK(tags.size() == 2 && tags[0].source == kSourceEmbedded && tags[0].confidence == kEmbeddedTagConfidence);
    {
        Stmt st(db.handle(), "SELECT site,post_id FROM sources WHERE image_id=? ORDER BY site");
        st.bind(1, m["/p/98765432_p0.png"]);
        CHECK(st.step() && st.text(0) == "download");
        CHECK(st.step() && st.text(0) == "pixiv" && st.text(1) == "98765432");
        Stmt r(db.handle(), "SELECT rating,format FROM file_meta WHERE image_id=?");
        r.bind(1, m["/p/98765432_p0.png"]);
        CHECK(r.step() && r.i64(0) == 4 && r.text(1) == "PNG");
    }
    // 書き直すと置き換わる。画像を消すと索引からも消える
    fm.title = "冬の山";
    storeMeta(db, m["/p/98765432_p0.png"], "/p/98765432_p0.png", fm);
    CHECK(searchMetaText(db, "空と海").empty());
    CHECK(searchMetaText(db, "冬の山").size() == 1);
    removeMissing(db, {"/p"}, {});
    CHECK(searchMetaText(db, "冬の山").empty());
    db.close();

    // 版 2 の DB は版 3（全文検索）へ移行する
    {
        Db d;
        CHECK(d.open((dir / "v2.db").string()));
        d.exec("DROP TRIGGER images_delete_fts; DROP TABLE meta_fts; PRAGMA user_version=2;");
    }
    Db d2;
    CHECK(d2.open((dir / "v2.db").string()));
    CHECK(d2.schemaVersion() == kSchemaVersion);
    CHECK(searchMetaText(d2, "abc").empty());
    fs::remove_all(dir);
}

static void testSearch() {
    std::vector<std::string> inc, exc;
    parseTagQuery("hatsune_miku  -1boy\u3000風景,-", inc, exc);
    CHECK((inc == std::vector<std::string>{"hatsune_miku", "風景"}));
    CHECK((exc == std::vector<std::string>{"1boy"}));
    CHECK(std::abs(combineConfidence({0.5, 0.5}) - 0.75) < 1e-9);
    CHECK(combineConfidence({}) == 0);

    const fs::path dir = fs::temp_directory_path() / "imgdupe-search-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    auto id = upsertScanned(db, {{"/p/a.jpg", 100, 10}, {"/p/b.jpg", 200, 20}, {"/p/c.png", 300, 30}, {"/q/d.jpg", 400, 40}});
    const int64_t a = id["/p/a.jpg"], b = id["/p/b.jpg"], c = id["/p/c.png"], d = id["/q/d.jpg"];
    auto tag = [&](int64_t img, const std::string& t, const std::string& src, double conf, bool manual = false) {
        Stmt st(db.handle(), "INSERT INTO tags(image_id,tag,kind,confidence,source,manual) VALUES(?,?,?,?,?,?)");
        st.bind(1, img).bind(2, t).bind(3, std::string("character")).bind(4, conf).bind(5, src);
        st.bind(6, static_cast<int64_t>(manual));
        st.step();
    };
    tag(a, "hatsune_miku", "tagger", 0.9);
    tag(b, "hatsune_miku", "tagger", 0.5);
    tag(b, "hatsune_miku", "folder", 0.5);  // 合成すると 0.75
    tag(c, "Hatsune_Miku", "tagger", 0.3);
    tag(c, "1boy", "tagger", 0.8);
    tag(d, "hatsune_miku", "tagger", 0.95);
    tag(d, "hatsune_miku", "manual", 0.0, true);  // 手動で「違う」と確定
    {
        Stmt st(db.handle(), "INSERT INTO tag_dict(tag,alias_of) VALUES('初音ミク','hatsune_miku')");
        st.step();
    }
    FileMeta fa;
    fa.rating = 5;
    fa.taken = 1714566896;
    fa.title = "夏の空と海";
    fa.format = "JPEG";
    storeMeta(db, a, "/p/a.jpg", fa);
    FileMeta fc;
    fc.rating = 2;
    fc.format = "PNG";
    fc.zoneUrl = "https://i.pximg.net/x.png";
    storeMeta(db, c, "/p/c.png", fc);
    {
        Stmt st(db.handle(), "UPDATE images SET width=4000,height=3000 WHERE id=?");
        st.bind(1, b);
        st.step();
    }
    auto ids = [&](SearchQuery q) { return search(db, q).ids; };
    using V = std::vector<int64_t>;

    SearchQuery q;
    q.tags = {"hatsune_miku"};
    q.minConfidence = 0.7;
    CHECK((ids(q) == V{a, b}));                 // b は合成で 0.75。d は手動で否定
    q.sources = {"tagger"};
    CHECK((ids(q) == V{a}));                    // 出どころを絞ると b は 0.5
    q.sources.clear();
    q.excludeSources = {"folder"};
    CHECK((ids(q) == V{a}));                    // 除外でも同じ
    q.excludeSources.clear();
    q.minConfidence = 0.2;
    CHECK((ids(q) == V{a, b, c}));              // 大文字小文字は区別しない
    q.tags = {"初音ミク"};
    CHECK((ids(q) == V{a, b, c}));              // 別名
    q.excludeTags = {"1boy"};
    CHECK((ids(q) == V{a, b}));
    q.sort = SortKey::Confidence;
    q.descending = true;
    CHECK((ids(q) == V{a, b}));                 // 0.9 > 0.75
    q.limit = 1;
    SearchResult r = search(db, q);
    CHECK(r.ids.size() == 1 && r.total == 2);

    SearchQuery m;
    CHECK(search(db, m).total == 4);
    m.minRating = 3;
    CHECK((ids(m) == V{a}));
    m = SearchQuery{};
    m.noRating = true;
    CHECK((ids(m) == V{b, d}));
    m = SearchQuery{};
    m.takenFrom = 1714500000;
    m.takenTo = 1714600000;
    CHECK((ids(m) == V{a}));
    m = SearchQuery{};
    m.noTaken = true;
    CHECK(ids(m).size() == 3);
    m = SearchQuery{};
    m.minWidth = 3000;
    CHECK((ids(m) == V{b}));
    m = SearchQuery{};
    m.minSize = 150;
    m.maxSize = 350;
    CHECK((ids(m) == V{b, c}));
    m = SearchQuery{};
    m.format = "PNG";
    CHECK((ids(m) == V{c}));
    m = SearchQuery{};
    m.pathContains = "/Q/";
    CHECK((ids(m) == V{d}));
    m = SearchQuery{};
    m.downloadContains = "PXIMG";
    CHECK((ids(m) == V{c}));
    m = SearchQuery{};
    m.text = "空と海";
    CHECK((ids(m) == V{a}));
    m.text = "海";
    CHECK((ids(m) == V{a}));
    m = SearchQuery{};
    m.sort = SortKey::Size;
    m.descending = true;
    CHECK((ids(m) == V{d, c, b, a}));
    m.sort = SortKey::Rating;
    m.descending = true;
    CHECK((ids(m) == V{a, c, b, d}));  // 星なしは最後（パス順）

    // 確認キュー: 手動で確定していないもの、迷っている順（0.5 に近い順）
    SearchQuery rq;
    rq.tags = {"hatsune_miku"};
    rq.minConfidence = 0.2;
    rq.review = true;
    CHECK((ids(rq) == V{c, b}));  // c 0.3、b 0.75（a 0.9 は確度が高い、d は手動で確定済み）
    setManualTag(db, b, "初音ミク", true);  // 別名で確定しても同じタグ
    CHECK((ids(rq) == V{c}));
    rq.review = false;
    rq.minConfidence = 0.99;
    CHECK((ids(rq) == V{b}));
    setManualTag(db, b, "hatsune_miku", false);  // 確定し直すと前の確定は置き換わる
    CHECK(ids(rq).empty());
    {
        Stmt st(db.handle(), "SELECT COUNT(*) FROM tags WHERE image_id=? AND manual=1");
        st.bind(1, b);
        CHECK(st.step() && st.i64(0) == 1);
    }
    clearManualTag(db, b, "hatsune_miku");
    rq.minConfidence = 0.7;
    CHECK((ids(rq) == V{a, b}));
    setManualTag(db, c, "new_tag", true);  // 無かったタグも付けられる
    SearchQuery nt;
    nt.tags = {"new_tag"};
    CHECK((ids(nt) == V{c}));

    // 取り消し: 変える前の手動の確定に戻す
    {
        TagHistory h(db);
        h.touch(a, "hatsune_miku");
        setManualTag(db, a, "hatsune_miku", false);
        h.touch(c, "new_tag");
        setManualTag(db, c, "new_tag", false);
        CHECK(h.save("違う 2 枚") > 0);
        TagHistory empty(db);
        CHECK(empty.save("nothing") == 0);
        SearchQuery hq;
        hq.tags = {"hatsune_miku"};
        hq.minConfidence = 0.7;
        CHECK((ids(hq) == V{b}));
        std::string desc;
        CHECK(undoLastTagEdit(db, desc) && desc == "違う 2 枚");
        CHECK((ids(hq) == V{a, b}));  // a は手動の確定が無い状態に戻る
        CHECK((ids(nt) == V{c}));      // c は「正しい」に戻る
        CHECK(!undoLastTagEdit(db, desc));
    }
    // 未分類: 確度 0.5 以上のタグが 1 つも無い画像
    {
        SearchQuery uq;
        uq.unclassified = true;
        uq.minConfidence = 0.5;
        // a: miku 0.9、b: miku 0.75、c: 1boy 0.8 / new_tag 1.0（手動）、d: miku 手動 0
        CHECK((ids(uq) == V{d}));
        uq.excludeSources = {"tagger"};
        CHECK((ids(uq) == V{a, d}));  // タガーを除くと a は無い、b はフォルダとで 0.5（>= 0.5）、c は手動
    }

    // 条件の JSON（スマートフォルダ）
    SearchQuery full;
    full.tags = {"初音ミク", "say \"hi\""};
    full.excludeTags = {"1boy"};
    full.minConfidence = 0.35;
    full.excludeSources = {"tagger"};
    full.text = "夏\n空";
    full.minRating = 0;
    full.noRating = true;
    full.takenFrom = 1714500000;
    full.takenTo = 1714600000;
    full.noTaken = true;
    full.minWidth = 3000;
    full.minHeight = 2000;
    full.minSize = 150;
    full.maxSize = 5000000000LL;
    full.format = "PNG";
    full.animatedOnly = true;
    full.pathContains = "\\p\\";
    full.downloadContains = "pixiv";
    full.sort = SortKey::Taken;
    full.descending = true;
    full.review = true;
    full.unclassified = true;
    SearchQuery back;
    CHECK(queryFromJson(queryToJson(full), back));
    CHECK(back.tags == full.tags && back.excludeTags == full.excludeTags && back.excludeSources == full.excludeSources);
    CHECK(std::abs(back.minConfidence - 0.35) < 1e-9 && back.text == full.text && back.minRating == 0 && back.noRating);
    CHECK(back.takenFrom == full.takenFrom && back.takenTo == full.takenTo && back.noTaken && back.minWidth == 3000);
    CHECK(back.minHeight == 2000 && back.minSize == 150 && back.maxSize == 5000000000LL && back.format == "PNG");
    CHECK(back.animatedOnly && back.pathContains == full.pathContains && back.downloadContains == "pixiv");
    CHECK(back.sort == SortKey::Taken && back.descending && back.sources.empty() && back.limit == 0 && back.review && back.unclassified);
    SearchQuery def;
    CHECK(queryFromJson(queryToJson(SearchQuery{}), def) && def.minRating == -1 && def.minConfidence == 0.5 &&
          def.sort == SortKey::Relevance && !def.descending);
    CHECK(!queryFromJson("[1]", def) && !queryFromJson("{", def));

    SearchQuery miku;
    miku.tags = {"hatsune_miku"};
    miku.minConfidence = 0.7;
    const int64_t s1 = saveSmartFolder(db, "ミク", miku);
    const int64_t s2 = saveSmartFolder(db, "png", m);
    CHECK(s1 && s2 && s1 != s2);
    miku.minConfidence = 0.2;
    CHECK(saveSmartFolder(db, "ミク", miku) == s1);  // 同じ名前は上書き
    auto folders = smartFolders(db);
    CHECK(folders.size() == 2 && folders[0].name == "png" && folders[1].name == "ミク");
    CHECK((ids(folders[1].query) == V{a, b, c}));      // 保存した条件で、今の DB を探す
    CHECK(saveSmartFolder(db, "PNG", m) == s2);          // 大文字小文字は区別しない
    CHECK(!renameSmartFolder(db, s1, "Png") && renameSmartFolder(db, s1, "初音ミク"));
    // 並べ替え・書き出し・読み込み
    setSmartFolderOrder(db, {s1, s2});
    folders = smartFolders(db);
    CHECK(folders.size() == 2 && folders[0].id == s1 && folders[1].id == s2);
    const int64_t s3 = saveSmartFolder(db, "a", m);  // 並べていないものは後ろ
    CHECK(smartFolders(db).back().id == s3);
    deleteSmartFolder(db, s3);
    {
        std::vector<SmartFolder> back;
        CHECK(importSmartFolders(exportSmartFolders(smartFolders(db)), back));
        CHECK(back.size() == 2 && back[0].name == "初音ミク" && back[0].query.tags == std::vector<std::string>{"hatsune_miku"});
        CHECK(std::abs(back[0].query.minConfidence - 0.2) < 1e-9);
        CHECK(queryToJson(back[1].query) == queryToJson(m));
        std::vector<SmartFolder> none;
        CHECK(!importSmartFolders("not json", none) && importSmartFolders(exportSmartFolders({}), none) && none.empty());
    }
    deleteSmartFolder(db, s2);
    folders = smartFolders(db);
    CHECK(folders.size() == 1 && folders[0].name == "初音ミク" && folders[0].id == s1);

    ImageInfo info;
    CHECK(imageInfo(db, a, info) && info.path == "/p/a.jpg" && info.rating == 5 && info.title == "夏の空と海");
    const auto srcs = tagSources(db);
    CHECK(!srcs.empty() && srcs[0].first == "tagger");
    fs::remove_all(dir);
}

static void testLinks() {
    CHECK(safeFolderName("a/b:c*?") == "a_b_c__");
    CHECK(safeFolderName("  . ") == "検索結果");
    CHECK(safeFolderName("夏 ") == "夏");
    CHECK(driveOf("D:\\x\\y.jpg") == "D:" && driveOf("\\\\srv\\share\\a.jpg") == "\\\\srv\\share");

    const fs::path dir = fs::temp_directory_path() / "imgdupe-links-test";
    fs::remove_all(dir);
    fs::create_directories(dir / "src" / "a");
    fs::create_directories(dir / "src" / "b");
    auto write = [](const fs::path& p, const std::string& s) {
        FILE* f = std::fopen(p.string().c_str(), "wb");
        std::fwrite(s.data(), 1, s.size(), f);
        std::fclose(f);
    };
    const std::string p1 = (dir / "src" / "a" / "x.jpg").string(), p2 = (dir / "src" / "b" / "x.jpg").string(),
                      p3 = (dir / "src" / "a" / "y.jpg").string(), p4 = (dir / "src" / "a" / "gone.jpg").string();
    write(p1, "one");
    write(p2, "two");
    write(p3, "three");
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    auto id = upsertScanned(db, {{p1, 3, 1}, {p2, 3, 2}, {p3, 5, 3}, {p4, 1, 4}});
    CHECK(vfolderRoot(db, "D:") == "D:\\_vfolders" && vfolderRoot(db, "/") == "/_vfolders");
    const std::string root = (dir / "vf").string();
    setVfolderRoot(db, "/", root + "/");
    CHECK(vfolderRoot(db, "/") == root);
    CHECK((customVfolderRoots(db) == std::vector<std::string>{root}));

    // 展開: 同じ名前のファイルは ID を頭に付ける。無い画像は失敗に数える
    ExpandResult e = expandLinks(db, {id[p1], id[p2], id[p3], id[p4]}, "ミク/1", true);
    CHECK(e.created == 3 && e.failed == 1 && e.firstFailure == p4);
    const fs::path out = fs::path(root) / "ミク_1";
    CHECK((e.folders == std::vector<std::string>{out.string()}));
    CHECK(fs::exists(out / "x.jpg") && fs::exists(out / (std::to_string(id[p2]) + "_x.jpg")) && fs::exists(out / "y.jpg"));
    FileIdentity a, b;
    CHECK(fileIdentity(p1, a) && fileIdentity((out / "x.jpg").string(), b) && a.same(b) && a.links == 2);
    auto folders = linkFolders(db);
    CHECK(folders.size() == 1 && folders[0].dir == out.string() && folders[0].count == 3 && folders[0].temporary == 3);

    // 同じ名前でもう一度 = 作り直し（今の結果だけになる）
    e = expandLinks(db, {id[p3]}, "ミク/1", true);
    CHECK(e.created == 1 && e.removed == 3 && !fs::exists(out / "x.jpg") && fs::exists(out / "y.jpg"));
    CHECK(fs::exists(p1) && fs::exists(p2));  // 元画像はそのまま

    // 固定にしたものは、一時リンクの後片付けでは消えない
    e = expandLinks(db, {id[p1], id[p2]}, "keep", true);
    const fs::path keep = fs::path(root) / "keep";
    setFolderTemporary(db, keep.string(), false);
    RemoveResult rr = removeLinks(db, "", true);
    CHECK(rr.removed == 1 && !fs::exists(out));  // 空になったフォルダも消す
    CHECK(fs::exists(keep / "x.jpg") && fs::exists(fs::path(root)));
    folders = linkFolders(db);
    CHECK(folders.size() == 1 && folders[0].temporary == 0 && folders[0].count == 2);

    // 別のファイルに置き換わったリンクは消さない（記録だけ消す）
    fs::remove(keep / "x.jpg");
    write(keep / "x.jpg", "user file");
    // 元画像が消えて、データがリンクにしか無いものは消さずに知らせる
    fs::remove(p2);
    CHECK((orphanLinks(db) == std::vector<std::string>{(keep / (std::to_string(id[p2]) + "_x.jpg")).string()}));
    rr = removeLinks(db, keep.string(), false);
    CHECK(rr.removed == 0 && rr.gone == 1 && rr.kept == 1);
    CHECK(fs::exists(keep / "x.jpg") && fs::exists(keep / (std::to_string(id[p2]) + "_x.jpg")));
    CHECK(linkFolders(db).size() == 1 && linkFolders(db)[0].count == 1);
    // リンクが消えていたら記録だけ消す
    fs::remove(keep / (std::to_string(id[p2]) + "_x.jpg"));
    rr = removeLinks(db, "", false);
    CHECK(rr.gone == 1 && linkFolders(db).empty());
    fs::remove_all(dir);
}

static void testNames() {
    // gallery-dl の pixiv
    SidecarInfo s;
    CHECK(parseSidecarJson(R"({"category":"pixiv","id":98765432,"title":" 夏の海 ","num":0,
        "tags":["初音ミク","VOCALOID",{"name":"海","translated_name":"sea"}],
        "user":{"id":1234,"name":"作者さん","account":"sakusha"}})", s));
    CHECK(s.site == "pixiv" && s.postId == "98765432" && s.authorId == "1234" && s.title == "夏の海");
    CHECK(s.url == "https://www.pixiv.net/artworks/98765432");
    CHECK(s.tags.size() == 4 && s.tags[0].tag == "作者さん" && s.tags[0].kind == "artist" && s.tags[0].source == "meta");
    CHECK(s.tags[3].tag == "海" && s.tags[3].kind == "general" && std::abs(s.tags[3].confidence - 0.95) < 1e-9);
    // twitter
    CHECK(parseSidecarJson(R"({"category":"twitter","tweet_id":1790000000000000001,"id":5,
        "author":{"id":42,"name":"screen","nick":"表示名"},"hashtags":["miku","miku"]})", s));
    CHECK(s.postId == "1790000000000000001" && s.url == "https://x.com/screen/status/1790000000000000001");
    CHECK(s.tags.size() == 2 && s.tags[0].tag == "表示名" && s.tags[1].tag == "miku");
    // danbooru
    CHECK(parseSidecarJson(R"({"category":"danbooru","id":7,"tag_string_artist":"some_artist",
        "tag_string_character":"hatsune_miku","tag_string_copyright":"vocaloid","tag_string_general":"1girl ^_^ "})", s));
    CHECK(s.url == "https://danbooru.donmai.us/posts/7" && s.tags.size() == 5);
    CHECK(s.tags[0].kind == "artist" && s.tags[1].kind == "character" && s.tags[1].tag == "hatsune_miku");
    CHECK(s.tags[4].tag == "^_^");
    CHECK(!parseSidecarJson("[]", s) && !parseSidecarJson("{oops", s));

    // フォルダ名
    auto names = [](const std::string& p) {
        std::vector<std::string> v;
        for (const auto& t : folderTags(p)) v.push_back(t.kind + ":" + t.tag);
        return v;
    };
    using V = std::vector<std::string>;
    CHECK((names("D:\\Pics\\初音ミク\\a.jpg") == V{"general:初音ミク"}));
    CHECK((names("D:\\Pics\\[作者名] 夏の本\\001.jpg") == V{"artist:作者名", "title:夏の本"}));
    CHECK((names("D:\\【サークル】\\001.jpg") == V{"artist:サークル"}));
    // [サークル名(作者名)]タイトル(イベント名・原作名)
    CHECK((names("D:\\[ABCサークル(山田太郎)]夏の本(C102)\\001.jpg") ==
           V{"circle:ABCサークル", "artist:山田太郎", "title:夏の本", "event:C102"}));
    CHECK((names("D:\\(C99) [ABC (山田、佐藤)] 夏の本 (東方Project) [DL版]\\001.jpg") ==
           V{"circle:ABC", "artist:山田", "artist:佐藤", "title:夏の本", "event:C99", "copyright:東方Project"}));
    CHECK((names("D:\\【ABC（山田）】夏の本（東方Project）（例大祭20）\\01\\001.jpg") ==  // 全角、中のフォルダ（01 は数字だけなので使わない）
           V{"circle:ABC", "artist:山田", "title:夏の本", "event:例大祭20", "copyright:東方Project"}));
    // 本のページの役目（名前順。数字は数として）
    {
        const std::vector<std::string> pages = {"10.jpg", "2.jpg", "1.jpg", "cover_b.png"};
        CHECK(pageRole("1.jpg", pages) == "表紙" && pageRole("2.jpg", pages) == "本文" && pageRole("10.jpg", pages) == "本文" &&
              pageRole("cover_b.png", pages) == "背表紙" && pageRole("x.jpg", pages).empty());
        CHECK(pageRole("a.jpg", {"a.jpg"}) == "表紙");
        const fs::path dbp = fs::temp_directory_path() / "imgdupe-book.db";
        fs::remove(dbp);
        Db d;
        CHECK(d.open(dbp.string()));
        auto ids = upsertScanned(d, {{"D:\\[A(B)]T\\1.jpg", 1, 1}, {"D:\\[A(B)]T\\2.jpg", 1, 1}, {"D:\\[A(B)]T\\10.jpg", 1, 1},
                                     {"D:\\[A(B)]T\\sub\\0.jpg", 1, 1}, {"D:\\[A(B)]T2\\0.jpg", 1, 1}});
        NamesAnalyzer na;
        std::atomic<bool> stop{false};
        runStage(d, na, 1, stop);
        auto role = [&](const char* p) {
            for (const auto& t : tagsOf(d, ids[p]))
                if (t.kind == "page") return t.tag;
            return std::string();
        };
        CHECK(role("D:\\[A(B)]T\\1.jpg") == "表紙" && role("D:\\[A(B)]T\\2.jpg") == "本文" && role("D:\\[A(B)]T\\10.jpg") == "背表紙");
        CHECK(role("D:\\[A(B)]T\\sub\\0.jpg").empty() && role("D:\\[A(B)]T2\\0.jpg") == "表紙");  // 中のフォルダは付けない、別の本
        d.close();
        fs::remove(dbp);
        CHECK(naturalCompare("img2.jpg", "img10.jpg") < 0 && naturalCompare("A.jpg", "a.jpg") != 0 && naturalCompare("007", "7") != 0);
    }
    {
        DoujinName d;
        CHECK(parseDoujinName("[サークル(作者)]タイトル(2023冬)(オリジナル)", d) && d.circle == "サークル" && d.authors.size() == 1 &&
              d.title == "タイトル" && d.events == std::vector<std::string>{"2023冬"} && d.parodies == std::vector<std::string>{"オリジナル"});
        CHECK(!parseDoujinName("夏の本", d) && !parseDoujinName("[]本", d) && !parseDoujinName("[ABC本", d));
        CHECK(looksLikeEvent("C102") && looksLikeEvent("コミックマーケット103") && looksLikeEvent("COMIC1☆22") && !looksLikeEvent("東方Project"));
        CHECK(folderTags("D:\\[A(B)]T(C1)\\x.jpg").at(0).confidence > 0.9);
    }
    CHECK(names("C:\\Users\\me\\Downloads\\a.jpg").empty());
    CHECK(names("D:\\Photos\\2024-05\\a.jpg").empty());
    CHECK(names("E:\\DCIM\\100MEDIA\\a.jpg").empty());
    CHECK(names("D:\\a.jpg").empty());
    CHECK((names("/home/u/landscape_/b.png") == V{"general:landscape"}));
    CHECK(folderTags("D:\\Miku\\a.jpg").at(0).source == "folder");

    // 段階 names: json を読み、タグ・出典・タイトルを書く
    const fs::path dir = fs::temp_directory_path() / "imgdupe-names-test";
    fs::remove_all(dir);
    fs::create_directories(dir / "初音ミク");
    const std::string img = (dir / "初音ミク" / "98765432_p0.jpg").string();
    const std::string img2 = (dir / "初音ミク" / "b.png").string();
    {
        std::ofstream(img) << "x";
        std::ofstream(img2) << "y";
        std::ofstream(img + ".json") << R"({"category":"pixiv","id":98765432,"title":"夏の海","tags":["blue_sky"],"user":{"id":1,"name":"作者"}})";
        std::ofstream((dir / "初音ミク" / "b.json").string()) << R"({"category":"gelbooru","id":3,"tags":"a b"})";
    }
    CHECK(findSidecar(img) == img + ".json" && findSidecar(img2) == (dir / "初音ミク" / "b.json").string());
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    auto id = upsertScanned(db, {{img, 1, 1}, {img2, 1, 2}});
    {
        Stmt st(db.handle(), "INSERT INTO file_meta(image_id) VALUES(?)");
        st.bind(1, id[img]);
        st.step();
    }
    NamesAnalyzer na;
    std::atomic<bool> cancel{false};
    StageReport rep = runStage(db, na, 2, cancel);
    CHECK(rep.processed == 2 && rep.failed == 0);
    int metaTags = 0, folderTagsN = 0;
    for (const auto& t : tagsOf(db, id[img])) {
        if (t.source == "meta") ++metaTags;
        if (t.source == "folder" && t.tag == "初音ミク" && std::abs(t.confidence - 0.70) < 1e-9) ++folderTagsN;
    }
    CHECK(metaTags == 2 && folderTagsN == 1);
    {
        Stmt st(db.handle(), "SELECT author_id, url FROM sources WHERE image_id=? AND site='pixiv' AND post_id='98765432'");
        st.bind(1, id[img]);
        CHECK(st.step() && st.text(0) == "1" && st.text(1) == "https://www.pixiv.net/artworks/98765432");
        Stmt t(db.handle(), "SELECT title FROM file_meta WHERE image_id=?");
        t.bind(1, id[img]);
        CHECK(t.step() && t.text(0) == "夏の海");
    }
    SearchQuery q;
    q.text = "夏の海";
    CHECK((search(db, q).ids == std::vector<int64_t>{id[img]}));  // json のタイトルも全文検索に入る
    q = SearchQuery{};
    q.tags = {"作者"};
    q.minConfidence = 0.9;
    CHECK((search(db, q).ids == std::vector<int64_t>{id[img]}));
    q.tags = {"初音ミク"};
    CHECK(search(db, q).ids.empty());  // フォルダ名は 0.70
    q.minConfidence = 0.6;
    CHECK(search(db, q).ids.size() == 2);
    fs::remove_all(dir);
}

static void testModels() {
    const std::string h1(64, 'a'), h2(64, 'b');
    const std::string json = R"({"version":1,"components":[
      {"id":"onnxruntime","name":"ONNX Runtime","version":"1.24.4","files":[
        {"path":"onnxruntime.dll","url":"https://example.com/o.dll","sha256":")" + h1 + R"(","size":100},
        {"path":"DirectML.dll","url":"https://example.com/d.dll","sha256":")" + h1 + R"(","size":50}]},
      {"id":"wd14","name":"WD14","version":"abc","requires":["onnxruntime"],"files":[
        {"path":"wd14/model.onnx","url":"https://example.com/m.onnx","sha256":")" + std::string(64, 'B') + R"(","size":1000}]}]})";
    Manifest m;
    CHECK(parseManifest(json, m) && m.components.size() == 2);
    CHECK(m.find("wd14") && m.find("wd14")->totalSize() == 1000 && m.find("wd14")->files[0].sha256 == h2);
    CHECK(m.find("onnxruntime")->totalSize() == 150 && !m.find("x"));

    auto ids = [](const std::vector<const Component*>& v) {
        std::vector<std::string> out;
        for (auto* c : v) out.push_back(c->id);
        return out;
    };
    using V = std::vector<std::string>;
    Installed none;
    CHECK((ids(planInstall(m, none, {"wd14"})) == V{"onnxruntime", "wd14"}));  // 要る部品を先に
    CHECK(planUpdate(m, none).empty());
    Installed in;
    in["onnxruntime"] = {"1.24.4", {"onnxruntime.dll", "DirectML.dll"}};
    in["wd14"] = {"old", {"wd14/model-old.onnx", "wd14/selected_tags.csv"}};
    CHECK((ids(planUpdate(m, in)) == V{"wd14"}));  // 版が変わったものだけ
    CHECK((ids(planInstall(m, in, {"wd14", "nothing"})) == V{"wd14"}));
    CHECK((staleFiles(in["wd14"], *m.find("wd14")) == V{"wd14/model-old.onnx", "wd14/selected_tags.csv"}));
    in.erase("onnxruntime");
    CHECK((ids(planUpdate(m, in)) == V{"onnxruntime", "wd14"}));  // 要る部品が足りない

    Installed back;
    CHECK(parseInstalled(installedToJson(in), back) && back.size() == 1 && back["wd14"].version == "old" &&
          back["wd14"].files.size() == 2);

    // 危ないもの
    CHECK(safeRelativePath("a/b.onnx") && !safeRelativePath("../x") && !safeRelativePath("a/../b") &&
          !safeRelativePath("C:\\x") && !safeRelativePath("/etc/x") && !safeRelativePath("a//b"));
    auto bad = [&](const std::string& path, const std::string& url, const std::string& sha) {
        Manifest x;
        return !parseManifest(R"({"components":[{"id":"a","version":"1","files":[{"path":")" + path + R"(","url":")" + url +
                                  R"(","sha256":")" + sha + R"("}]}]})",
                              x);
    };
    CHECK(bad("../a", "https://x/a", h1) && bad("a", "http://x/a", h1) && bad("a", "https://x/a", "abc"));
    CHECK(!bad("a", "http://127.0.0.1:8000/a", h1));  // 手元のテスト用サーバーだけ http 可
    CHECK(!parseManifest("[]", m) && !parseManifest(R"({"components":[{"id":"a","version":"1","files":[]}]})", m));
}

static void testTagger() {
    std::vector<TaggerLabel> labels;
    CHECK(parseSelectedTags("tag_id,name,category,count\r\n9999999,general,9,1\n9999998,sensitive,9,1\n1,1girl,0,5\n"
                            "2,hatsune_miku,4,3\n3,\"a,b\",0,1\n4,highres,5,1\n5,vocaloid,3,1\n", labels));
    CHECK(labels.size() == 7 && labels[4].name == "a,b" && labels[3].category == 4);
    CHECK(!parseSelectedTags("x,y\n1,2\n", labels));
    parseSelectedTags("tag_id,name,category,count\n0,general,9,1\n0,sensitive,9,1\n1,1girl,0,5\n2,hatsune_miku,4,3\n"
                      "3,a,0,1\n4,highres,5,1\n5,vocaloid,3,1\n", labels);
    const float probs[] = {0.1f, 0.8f, 0.95f, 0.9f, 0.1f, 0.99f, 0.5f};
    const auto tags = taggerTags(probs, 7, labels);
    std::vector<std::string> got;
    for (const auto& t : tags) got.push_back(t.kind + ":" + t.tag + ":" + t.source);
    CHECK((got == std::vector<std::string>{"general:1girl:tagger", "character:hatsune_miku:tagger", "copyright:vocaloid:tagger",
                                           "rating:sensitive:tagger"}));  // 0.2 未満とメタは無し、レーティングは 1 つ
    CHECK(taggerStageVersion("abc") > 0 && taggerStageVersion("abc") != taggerStageVersion("abd"));

    // 前処理: 2×1 の画像（左: 不透明の赤、右: 透明）→ 4×4。白で正方形に広げ、透明は白、BGR
    const uint8_t px[] = {0, 0, 255, 255, /* 透明 */ 0, 0, 0, 0};
    std::vector<float> in(4 * 4 * 3);
    prepareTaggerInput(px, 2, 1, 4, in.data());
    auto at = [&](int x, int y, int c) { return in[(y * 4 + x) * 3 + c]; };
    // 2×2 に広げると (2-1)/2 = 0 なので画像は上の行、下の行が白の余白（wd-tagger と同じ）
    CHECK(at(0, 0, 0) == 0 && at(0, 0, 1) == 0 && at(0, 0, 2) == 255);        // 赤（B=0, G=0, R=255）
    CHECK(at(0, 3, 0) == 255 && at(0, 3, 1) == 255 && at(0, 3, 2) == 255);  // 下の余白は白
    CHECK(at(3, 0, 0) == 255 && at(3, 0, 2) == 255);                          // 透明は白
    std::vector<float> same(2 * 2 * 3);
    const uint8_t sq[] = {10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255};
    prepareTaggerInput(sq, 2, 2, 2, same.data());
    CHECK(same[0] == 10 && same[2] == 30 && same[9] == 100 && same[11] == 120);  // 大きさが同じならそのまま

    // 段階 tagger: 推論を差し替えて、タグが DB に入ることを確かめる
    struct Fake : TaggerModel {
        int inputSide() const override { return 4; }
        bool run(const std::vector<float>& input, std::vector<float>& out, std::string&) override {
            out = {0.0f, 0.9f, input[0] > 128 ? 0.9f : 0.1f, 0.3f, 0.0f, 0.0f, 0.0f};
            return true;
        }
    };
    const fs::path dir = fs::temp_directory_path() / "imgdupe-tagger-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    auto id = upsertScanned(db, {{"/a.png", 1, 1}, {"/broken.png", 1, 2}});
    TaggerAnalyzer ta(std::make_shared<Fake>(), labels, taggerStageVersion("m1"),
                      [](const std::string& path, int box, std::vector<uint8_t>& bgra, int& w, int& h) {
                          if (path == "/broken.png") return false;
                          w = h = box;
                          bgra.assign(static_cast<size_t>(box) * box * 4, 255);  // 白
                          return true;
                      });
    std::atomic<bool> cancel{false};
    StageReport rep = runStage(db, ta, 2, cancel);
    CHECK(rep.processed == 2 && rep.failed == 1);
    std::vector<std::string> t;
    for (const auto& x : tagsOf(db, id["/a.png"])) t.push_back(x.tag);
    std::sort(t.begin(), t.end());
    CHECK((t == std::vector<std::string>{"1girl", "hatsune_miku", "sensitive"}));  // 白い画像 → 1girl 0.9、レーティング 1 つ
    // 同じ版ではやり直さない、版が変わればやり直す
    CHECK(runStage(db, ta, 1, cancel).processed == 0);
    TaggerAnalyzer ta2(std::make_shared<Fake>(), labels, taggerStageVersion("m2"),
                       [](const std::string&, int box, std::vector<uint8_t>& bgra, int& w, int& h) {
                           w = h = box;
                           bgra.assign(static_cast<size_t>(box) * box * 4, 255);
                           return true;
                       });
    CHECK(runStage(db, ta2, 1, cancel).processed == 2);

    // まとめて推論する（GPU）: 前処理はいくつものスレッドで、推論は 3 枚ずつ。結果が画像ごとに正しく戻ること
    struct FakeBatch : Fake {
        std::atomic<size_t> calls{0}, maxN{0}, images{0};
        size_t maxBatch() const override { return 3; }
        bool runBatch(const std::vector<float>& input, size_t n, std::vector<float>& out, std::string& e) override {
            ++calls;
            images += n;
            maxN = std::max<size_t>(maxN, n);
            const size_t one = input.size() / n;
            out.clear();
            for (size_t k = 0; k < n; ++k) {
                std::vector<float> in(input.begin() + static_cast<std::ptrdiff_t>(one * k), input.begin() + static_cast<std::ptrdiff_t>(one * (k + 1))), o;
                run(in, o, e);
                out.insert(out.end(), o.begin(), o.end());
            }
            return true;
        }
    };
    std::vector<ScannedFile> many;
    for (int i = 0; i < 20; ++i) many.push_back({"/w" + std::to_string(i) + ".png", 1, 1});
    for (int i = 0; i < 5; ++i) many.push_back({"/k" + std::to_string(i) + ".png", 1, 1});  // 黒い画像
    many.push_back({"/broken.png", 1, 2});
    const auto ids = upsertScanned(db, many);
    auto fb = std::make_shared<FakeBatch>();
    TaggerAnalyzer tb(fb, labels, taggerStageVersion("m3"),
                      [](const std::string& path, int box, std::vector<uint8_t>& bgra, int& w, int& h) {
                          if (path == "/broken.png") return false;
                          w = h = box;
                          bgra.assign(static_cast<size_t>(box) * box * 4, path[1] == 'k' ? 0 : 255);
                          if (path[1] == 'k')
                              for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;  // 不透明の黒
                          return true;
                      });
    CHECK(tb.batchSize() == 3);
    rep = runStage(db, tb, 4, cancel);
    CHECK(rep.processed == ids.size() + 1 && rep.failed == 1);  // 前からある /a.png も
    CHECK(fb->maxN == 3 && fb->images == ids.size() && fb->calls < fb->images);  // 読めない画像は推論に回さない
    // 前処理が 1 スレッドの時は burst 枚たまってから続けて流す（結果は同じ）
    {
        Stmt st(db.handle(), "DELETE FROM stages WHERE stage='tagger'");
        st.step();
    }
    fb->calls = 0;
    fb->images = 0;
    fb->maxN = 0;
    rep = runStage(db, tb, 1, cancel, nullptr, 256, nullptr, nullptr, 12);
    CHECK(rep.processed == ids.size() + 1 && rep.failed == 1 && fb->images == ids.size() && fb->maxN == 3);
    auto hasGirl = [&](const std::string& p) {  // 白い画像だけ 1girl
        for (const auto& x : tagsOf(db, ids.at(p)))
            if (x.tag == "1girl") return true;
        return false;
    };
    CHECK(hasGirl("/w0.png") && hasGirl("/w19.png") && !hasGirl("/k0.png") && !hasGirl("/k4.png"));
    fs::remove_all(dir);
}

static void testVectors() {
    for (float f : {0.0f, 1.0f, -2.5f, 0.333f, 65504.0f, 1e-5f, -1e-7f}) {
        const float back = halfToFloat(floatToHalf(f));
        CHECK(std::abs(back - f) <= std::abs(f) * 1e-3f + 1e-7f);
    }
    CHECK(floatToHalf(1.0f) == 0x3c00 && floatToHalf(-2.0f) == 0xc000 && std::isinf(halfToFloat(floatToHalf(1e6f))));

    const fs::path dir = fs::temp_directory_path() / "imgdupe-vectors-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "v.bin").string();
    auto unit = [](std::vector<float> v) {
        normalizeVector(v);
        return v;
    };
    bool created = false;
    {
        VectorWriter w;
        CHECK(w.open(path, 4, "m1", &created) && created);
        CHECK(w.append(1, unit({1, 0, 0, 0}).data()));
        CHECK(w.append(2, unit({0, 1, 0, 0}).data()));
        CHECK(w.append(3, unit({1, 1, 0, 0}).data()));
    }
    {
        VectorWriter w;
        CHECK(w.open(path, 4, "m1", &created) && !created);  // 同じ版なら足していく
        CHECK(w.append(2, unit({0, 0, 1, 0}).data()));       // 同じ画像は後のものを使う
        CHECK(w.append(4, unit({-1, 0, 0, 0}).data()));
    }
    VectorIndex idx;
    CHECK(idx.load(path) && idx.dim() == 4 && idx.version() == "m1" && idx.size() == 4 && idx.records() == 5);
    std::vector<float> v;
    CHECK(idx.get(2, v) && std::abs(v[2] - 1) < 1e-3 && !idx.get(9, v));
    const auto q = unit({1, 0.1f, 0, 0});
    auto hits = idx.topK(q.data(), 2);
    CHECK(hits.size() == 2 && hits[0].first == 1 && hits[1].first == 3);
    hits = idx.topK(q.data(), 10, [](int64_t id) { return id != 1; }, 3);
    CHECK(hits.size() == 3 && hits[0].first == 3 && hits.back().first == 4);  // 1 を除いた全部、内積の順

    // 書きかけのレコードは読まない。次に書く時に切り詰める
    {
        FILE* f = std::fopen(path.c_str(), "ab");
        std::fwrite("\x05\0\0", 1, 3, f);
        std::fclose(f);
    }
    idx.close();
    CHECK(idx.load(path) && idx.size() == 4);
    {
        VectorWriter w;
        CHECK(w.open(path, 4, "m1", &created) && !created);
        CHECK(w.append(5, unit({0, 0, 0, 1}).data()));
    }
    idx.close();
    CHECK(idx.load(path) && idx.size() == 5 && idx.get(5, v) && std::abs(v[3] - 1) < 1e-3);

    // 作り直し: 消えた画像と古いレコードを捨てる
    CHECK(compactVectors(path, [](int64_t id) { return id != 4; }));
    idx.close();
    CHECK(idx.load(path) && idx.size() == 4 && idx.records() == 4 && !idx.has(4) && idx.get(2, v) && std::abs(v[2] - 1) < 1e-3);
    // 版が違えば作り直す
    {
        VectorWriter w;
        CHECK(w.open(path, 4, "m2", &created) && created);
    }
    idx.close();
    CHECK(idx.load(path) && idx.size() == 0 && idx.version() == "m2");
    idx.close();
    fs::remove_all(dir);
}

static void testClip() {
    WordPiece wp;
    CHECK(wp.load("[PAD]\n[UNK]\n[CLS]\n[SEP]\n夕\n焼\nけ\n##の\n写\n真\nsun\n##set\n,\nphoto\n"));
    using V = std::vector<std::string>;
    // 漢字は 1 文字ずつ、かなは続けて WordPiece、記号は分ける
    CHECK((wp.tokens("夕焼けの写真") == V{"夕", "焼", "け", "##の", "写", "真"}));
    CHECK((wp.tokens(" sunset,photo\t") == V{"sun", "##set", ",", "photo"}));
    CHECK((wp.tokens("Sunset xyz") == V{"[UNK]", "[UNK]"}));  // 大文字小文字はそのまま
    CHECK((wp.encode("夕焼け") == std::vector<int64_t>{2, 4, 5, 6, 3}));
    CHECK(wp.encode("夕 夕 夕 夕 夕", 4).size() == 4);  // [CLS] 夕 夕 [SEP]

    // safetensors の Dense（2×3）
    std::string st;
    const std::string header = R"({"linear.weight":{"dtype":"F32","shape":[2,3],"data_offsets":[0,24]}})";
    const uint64_t hl = header.size();
    st.append(reinterpret_cast<const char*>(&hl), 8);
    st += header;
    const float wts[6] = {1, 0, 0, 0, 2, 1};
    st.append(reinterpret_cast<const char*>(wts), 24);
    DenseLayer d;
    CHECK(d.load(st) && d.out == 2 && d.in == 3);
    const float x[3] = {3, 4, 5};
    const auto y = d.apply(x);
    CHECK(y.size() == 2 && y[0] == 3 && y[1] == 13);
    CHECK(!d.load("short"));

    const float tok[6] = {1, 2, 3, 4, 100, 100};  // 3 単語 × 2 次元、最後は mask で使わない
    const auto mp = meanPool(tok, 3, 2, {1, 1, 0});
    CHECK(mp.size() == 2 && mp[0] == 2 && mp[1] == 3);

    // 前処理: 4×2 の画像（左半分 赤、右半分 白）→ 2×2。短い辺（2）に合わせ、中央の 2×2 を切り抜く
    std::vector<uint8_t> px(4 * 2 * 4, 255);
    for (int y2 = 0; y2 < 2; ++y2)
        for (int x2 = 0; x2 < 2; ++x2) {
            uint8_t* p = &px[(y2 * 4 + x2) * 4];
            p[0] = 0, p[1] = 0, p[2] = 255;  // BGR = 赤
        }
    std::vector<float> in(3 * 2 * 2);
    prepareClipInput(px.data(), 4, 2, 2, in.data());
    const float r1 = (1.f - 0.48145466f) / 0.26862954f, g0 = (0.f - 0.4578275f) / 0.26130258f, g1 = (1.f - 0.4578275f) / 0.26130258f;
    CHECK(std::abs(in[0] - r1) < 1e-4);              // R の面、左上（赤の列）
    CHECK(std::abs(in[4 + 0] - g0) < 1e-4);          // G の面、左上は赤なので 0
    CHECK(std::abs(in[4 + 1] - g1) < 1e-4);          // G の面、右上は白
    CHECK(clipStageVersion("a") != clipStageVersion("b") && clipStageVersion("a") > 0);

    // 段階 clip: ベクトルがファイルに入る
    struct Fake : ClipModel {
        int imageSide() const override { return 4; }
        size_t dim() const override { return 3; }
        bool encodeImage(const std::vector<float>& p, std::vector<float>& out, std::string&) override {
            out = {p[0], 1.f, 0.f};
            return true;
        }
        bool encodeText(const std::string&, std::vector<float>& out, std::string&) override {
            out = {0, 1, 0};
            return true;
        }
    };
    const fs::path dir = fs::temp_directory_path() / "imgdupe-clip-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    auto id = upsertScanned(db, {{"/a.png", 1, 1}, {"/b.png", 1, 2}});
    auto writer = std::make_shared<VectorWriter>();
    CHECK(writer->open((dir / "v.bin").string(), 3, "m1"));
    ClipAnalyzer ca(std::make_shared<Fake>(), writer, clipStageVersion("m1"),
                    [](const std::string& path, int box, std::vector<uint8_t>& bgra, int& w, int& h) {
                        w = h = box;
                        bgra.assign(static_cast<size_t>(box) * box * 4, path == "/a.png" ? 255 : 0);
                        if (path != "/a.png")
                            for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;  // 黒
                        return true;
                    });
    std::atomic<bool> cancel{false};
    CHECK(runStage(db, ca, 2, cancel).processed == 2);
    writer->close();
    VectorIndex idx;
    CHECK(idx.load((dir / "v.bin").string()) && idx.size() == 2 && idx.has(id["/a.png"]) && idx.has(id["/b.png"]));
    std::vector<float> va, vb;
    idx.get(id["/a.png"], va);
    idx.get(id["/b.png"], vb);
    CHECK(va[0] > 0.5f && vb[0] < 0 && std::abs(va[0] * va[0] + va[1] * va[1] + va[2] * va[2] - 1) < 1e-2);  // 長さ 1
    // 似ている順（ほかの条件で絞る）
    {
        Stmt st(db.handle(), "INSERT INTO tags(image_id,tag,kind,confidence,source,manual) VALUES(?,'x','general',0.9,'tagger',0)");
        st.bind(1, id["/b.png"]);
        st.step();
    }
    const float qa[3] = {1, 0, 0};
    SearchQuery none;
    RankResult rr = rankByVector(db, none, idx, qa, 10);
    CHECK(rr.total == 2 && rr.ids.size() == 2 && rr.ids[0] == id["/a.png"] && rr.scores[0] > rr.scores[1]);
    SearchQuery withTag;
    withTag.tags = {"x"};
    rr = rankByVector(db, withTag, idx, qa, 10);
    CHECK(rr.total == 1 && rr.ids == std::vector<int64_t>{id["/b.png"]});
    rr = rankByVector(db, none, idx, qa, 10, id["/a.png"]);  // 似た画像を探す時は、元の画像を除く
    CHECK(rr.ids == std::vector<int64_t>{id["/b.png"]});
    CHECK(!hasConditions(none) && hasConditions(withTag));
    SearchQuery js;
    js.clipText = "夕焼けの海";
    js.similarTo = 42;
    SearchQuery jb;
    CHECK(queryFromJson(queryToJson(js), jb) && jb.clipText == "夕焼けの海" && jb.similarTo == 42);
    idx.close();
    fs::remove_all(dir);
}

static void testAnn() {
    const fs::path dir = fs::temp_directory_path() / "imgdupe-ann-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string vpath = (dir / "v.bin").string(), apath = (dir / "a.hnsw").string();
    // 20 個の「元の絵」と、それぞれの少しだけ違う版（差分）を 2 つずつ。元の絵どうしは遠い
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0, 1);
    const uint32_t dim = 32;
    std::vector<std::vector<float>> base(20);
    {
        VectorWriter w;
        CHECK(w.open(vpath, dim, "m1"));
        int64_t id = 1;
        for (int b = 0; b < 20; ++b) {
            base[b].resize(dim);
            for (auto& x : base[b]) x = nd(rng);
            for (int v = 0; v < 3; ++v) {
                std::vector<float> x = base[b];
                if (v)
                    for (auto& e : x) e += 0.15f * nd(rng);
                normalizeVector(x);
                w.append(id++, x.data());
            }
        }
    }
    VectorIndex vecs;
    CHECK(vecs.load(vpath) && vecs.size() == 60);
    std::atomic<bool> cancel{false};
    {
        AnnIndex ann;
        CHECK(ann.open(apath, dim, "m1") && ann.size() == 0);
        CHECK(ann.sync(vecs, [](int64_t) { return true; }, 2, cancel) && ann.size() == 60);
        // 近い順の 1 つ目は自分、次は同じ元の絵の差分
        auto nn = ann.knn(vecs.raw(4), 3);
        CHECK(nn.size() == 3 && nn[0].first == 4 && std::abs(nn[0].second - 1) < 1e-2 && (nn[1].first == 5 || nn[1].first == 6));
        auto groups = variantGroups(ann, vecs, 0.8, 5, 2, cancel);
        CHECK(groups.size() == 20);
        CHECK((groups[1] == std::vector<int64_t>{4, 5, 6}));
        CHECK(ann.save());
    }
    {
        AnnIndex ann;
        CHECK(ann.open(apath, dim, "m1") && ann.size() == 60);  // 保存したものを読む
        CHECK(!ann.sync(vecs, [](int64_t) { return true; }, 1, cancel));  // 変わらない
        CHECK(ann.sync(vecs, [](int64_t id) { return id != 5; }, 1, cancel) && ann.size() == 59 && !ann.has(5));  // 消えた画像
        auto groups = variantGroups(ann, vecs, 0.8, 5, 1, cancel);
        CHECK(groups.size() == 20 && (groups[1] == std::vector<int64_t>{4, 6}));
        CHECK(variantGroups(ann, vecs, 0.9999, 5, 1, cancel).empty());  // 厳しくすればまとまらない
    }
    {
        AnnIndex ann;
        CHECK(ann.open(apath, dim, "m2") && ann.size() == 0);  // 版が違えば作り直す
    }
    // DB に入れる
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    std::vector<ScannedFile> files;
    for (int i = 1; i <= 6; ++i) files.push_back({"/x/" + std::to_string(i) + ".png", 1, i});
    auto ids = upsertScanned(db, files);
    replaceVariantGroups(db, {{ids["/x/1.png"], ids["/x/2.png"]}, {ids["/x/4.png"], ids["/x/5.png"], ids["/x/6.png"]}});
    replaceVariantGroups(db, {{ids["/x/1.png"], ids["/x/3.png"]}});  // 入れ替え
    {
        Stmt st(db.handle(), "SELECT COUNT(*) FROM groups WHERE kind='variant'");
        CHECK(st.step() && st.i64(0) == 1);
        Stmt m(db.handle(), "SELECT COUNT(*) FROM group_members m JOIN groups g ON g.id=m.group_id WHERE g.kind='variant'");
        CHECK(m.step() && m.i64(0) == 2);
    }
    // 重複の結果に差分を足す: 今回のフォルダにある画像だけ、同じ重複のグループの中だけのものは出さない
    {
        const int64_t other = upsertScanned(db, {{"/elsewhere/9.png", 1, 9}})["/elsewhere/9.png"];  // 今回のフォルダの外
        replaceVariantGroups(db, {{ids["/x/1.png"], ids["/x/2.png"], other},
                                  {ids["/x/3.png"], ids["/x/4.png"]},
                                  {ids["/x/5.png"], ids["/x/6.png"]}});
        Result r;
        for (int i2 = 1; i2 <= 6; ++i2) {
            Entry e;
            e.path = "/x/" + std::to_string(i2) + ".png";
            e.size = 100;
            r.files.push_back(e);
            r.ids.push_back(ids[e.path]);
        }
        Group dup;
        dup.kind = Kind::Exact;
        dup.members = {{4, 0}, {5, 0}};  // 5.png と 6.png は完全一致
        r.groups.push_back(dup);
        appendVariantGroups(db, r);
        CHECK(r.groups.size() == 3 && r.groups[1].kind == Kind::Variant && r.groups[1].members.size() == 2);
        CHECK(r.groups[2].members.size() == 2 && r.files[r.groups[2].members[0].index].path == "/x/3.png");
        appendVariantGroups(db, r);  // 2 回目は入れ替え
        CHECK(r.groups.size() == 3);
    }
    vecs.close();
    fs::remove_all(dir);
}

static void testDict() {
    std::vector<DictEntry> d;
    std::string err;
    CHECK(parseDictionary("\xEF\xBB\xBF# c\n[テニスをしている]\nwd14: tennis_racket, tennis_ball\nclip: \"a person, playing tennis\", テニスをする人\n"
                          "\n[剣]\n種類: item\nwd14:sword\n",
                          d, err));
    CHECK(d.size() == 2 && d[0].name == "テニスをしている" && d[0].kind == "action");
    CHECK((d[0].wd14 == std::vector<std::string>{"tennis_racket", "tennis_ball"}));
    CHECK((d[0].clip == std::vector<std::string>{"a person, playing tennis", "テニスをする人"}));
    CHECK(d[1].kind == "item" && d[1].wd14 == std::vector<std::string>{"sword"});
    CHECK(!parseDictionary("wd14: a\n", d, err) && err.find("1 行目") == 0);
    CHECK(!parseDictionary("[a]\nfoo: b\n", d, err) && err.find("2 行目") == 0);
    CHECK(!parseDictionary("[ ]\n", d, err));
    CHECK(parseDictionary(defaultDictionary(), d, err) && d.empty());  // 例はコメント
    CHECK(dictionaryHash("a") != dictionaryHash("b") && dictionaryHash("a").size() == 16);
    CHECK(std::abs(clipProbability(0.27) - 0.5) < 1e-9 && clipProbability(0.35) > 0.9 && clipProbability(0.2) < 0.1);

    const fs::path dir = fs::temp_directory_path() / "imgdupe-dict-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    auto id = upsertScanned(db, {{"/a.png", 1, 1}, {"/b.png", 1, 2}, {"/c.png", 1, 3}});
    auto tag = [&](int64_t img, const std::string& t, double conf) {
        Stmt st(db.handle(), "INSERT INTO tags(image_id,tag,kind,confidence,source,manual) VALUES(?,?,'general',?,'tagger',0)");
        st.bind(1, img).bind(2, t).bind(3, conf);
        st.step();
    };
    tag(id["/a.png"], "tennis_racket", 0.6);
    tag(id["/a.png"], "tennis_ball", 0.8);
    tag(id["/b.png"], "tennis_ball", 0.3);
    tag(id["/c.png"], "sword", 0.9);
    parseDictionary("[テニスをしている]\nwd14: tennis_racket, tennis_ball\n[剣]\n種類: item\nwd14: sword\n", d, err);
    applyDictionaryTags(db, d);
    applyDictionaryTags(db, d);  // 何度当てても同じ
    auto conf = [&](int64_t img, const std::string& t, const char* source) {
        Stmt st(db.handle(), "SELECT confidence, kind FROM tags WHERE image_id=? AND tag=? AND source=?");
        st.bind(1, img).bind(2, t).bind(3, std::string(source));
        return st.step() ? st.f64(0) : -1.0;
    };
    CHECK(std::abs(conf(id["/a.png"], "テニスをしている", "dict") - 0.8) < 1e-9);  // 一番高いもの
    CHECK(std::abs(conf(id["/b.png"], "テニスをしている", "dict") - 0.3) < 1e-9);
    CHECK(conf(id["/c.png"], "テニスをしている", "dict") < 0 && conf(id["/c.png"], "剣", "dict") > 0.8);
    // 辞書から消した項目のタグも消える
    parseDictionary("[剣]\nwd14: sword\n", d, err);
    applyDictionaryTags(db, d);
    CHECK(conf(id["/a.png"], "テニスをしている", "dict") < 0 && conf(id["/c.png"], "剣", "dict") > 0.8);

    // clip の行
    const std::string vpath = (dir / "v.bin").string();
    {
        VectorWriter w;
        CHECK(w.open(vpath, 2, "m"));
        const float a[2] = {1, 0}, b[2] = {0.6f, 0.8f}, c[2] = {0, 1};
        w.append(id["/a.png"], a);
        w.append(id["/b.png"], b);
        w.append(id["/c.png"], c);
    }
    VectorIndex idx;
    CHECK(idx.load(vpath));
    // 文章のベクトル (1, 0) → a は 1.0、b は 0.6 → どちらも 0.27 を大きく超える。c は 0
    auto m = clipMatches(idx, {{1, 0}}, 0.2);
    CHECK(m.size() == 2 && m[0].first == id["/a.png"] && m[1].first == id["/b.png"] && m[0].second > 0.99f);
    m = clipMatches(idx, {{1, 0}, {0, 1}}, 0.2);  // 2 つの文章 → どちらかに近ければ
    CHECK(m.size() == 3);
    parseDictionary("[外]\nclip: outdoor\n", d, err);
    replaceClipDictTags(db, d, {m});
    CHECK(conf(id["/c.png"], "外", "clip") > 0.99 && conf(id["/a.png"], "剣", "dict") < 0);
    replaceClipDictTags(db, d, {{}});
    CHECK(conf(id["/c.png"], "外", "clip") < 0);
    idx.close();
    fs::remove_all(dir);
}

static void testRefs() {
    CHECK(std::abs(referenceProbability(0.80) - 0.5) < 1e-9 && referenceProbability(0.9) > 0.9 && referenceProbability(0.7) < 0.1);
    const fs::path dir = fs::temp_directory_path() / "imgdupe-refs-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    // 見本: miku は手動で 3 枚、rin は refs フォルダに 1 枚。len は手動 2 枚（足りない）
    std::vector<ScannedFile> files;
    for (int i = 1; i <= 8; ++i) files.push_back({"/img/" + std::to_string(i) + ".png", 1, i});
    files.push_back({"D:\\data\\refs\\rin\\r1.png", 1, 9});
    files.push_back({"D:\\data\\refs\\top.png", 1, 10});
    auto ids = upsertScanned(db, files);
    auto I = [&](int n) { return ids["/img/" + std::to_string(n) + ".png"]; };
    for (int n : {1, 2, 3}) setManualTag(db, I(n), "miku", true);
    for (int n : {4, 5}) setManualTag(db, I(n), "len", true);
    setManualTag(db, I(6), "miku", false);  // 「違う」は見本にしない
    auto sets = collectRefSets(db, "D:\\data\\refs\\");
    CHECK(sets.size() == 2 && sets[0].tag == "miku" && sets[0].ids.size() == 3 && sets[1].tag == "rin" &&
          sets[1].ids == std::vector<int64_t>{ids["D:\\data\\refs\\rin\\r1.png"]} && sets[1].kind == "character");
    CHECK(collectRefSets(db, "").size() == 1);
    CHECK(refStateHash(sets, 5) != refStateHash(sets, 6));

    // ベクトル: 1〜3 と 7 は似ている（miku）、8 と見本の rin が似ている
    const std::string vpath = (dir / "v.bin").string();
    {
        VectorWriter w;
        CHECK(w.open(vpath, 3, "m"));
        auto put = [&](int64_t id, std::vector<float> v) {
            normalizeVector(v);
            w.append(id, v.data());
        };
        put(I(1), {1, 0.1f, 0});
        put(I(2), {1, -0.1f, 0});
        put(I(3), {1, 0, 0.1f});
        put(I(7), {1, 0.05f, 0.05f});
        put(I(6), {0.2f, 1, 0});
        put(I(8), {0, 0.1f, 1});
        put(ids["D:\\data\\refs\\rin\\r1.png"], {0, 0, 1});
    }
    VectorIndex vecs;
    CHECK(vecs.load(vpath));
    CHECK(applyReferences(db, vecs, sets) == 2);
    auto conf = [&](int64_t img, const std::string& t) {
        Stmt st(db.handle(), "SELECT confidence FROM tags WHERE image_id=? AND tag=? AND source='reference'");
        st.bind(1, img).bind(2, t);
        return st.step() ? st.f64(0) : -1.0;
    };
    CHECK(conf(I(7), "miku") > 0.9 && conf(I(6), "miku") < 0 && conf(I(8), "rin") > 0.9 && conf(I(8), "miku") < 0);
    // 検索では、手動の「違う」が見本との類似より強い
    SearchQuery q;
    q.tags = {"miku"};
    const auto found = search(db, q).ids;
    CHECK(std::find(found.begin(), found.end(), I(7)) != found.end() && std::find(found.begin(), found.end(), I(6)) == found.end());
    // 見本が無くなれば消える
    CHECK(applyReferences(db, vecs, {}) == 0 && conf(I(7), "miku") < 0);
    vecs.close();
    fs::remove_all(dir);
}

static void testTagRelations() {
    std::vector<TagRelation> rels;
    std::string err;
    CHECK(parseTagRelations("# c\n初音ミク = hatsune_miku\n hatsune_miku > vocaloid \n\nvocaloid>music\n", rels, err));
    CHECK(rels.size() == 3 && rels[0].kind == "alias" && rels[0].tag == "初音ミク" && rels[1].kind == "parent" &&
          rels[1].other == "vocaloid");
    CHECK(!parseTagRelations("a\n", rels, err) && err.find("1 行目") == 0);
    CHECK(!parseTagRelations("a = b = c\n", rels, err));
    std::vector<TagRelation> back;
    parseTagRelations("初音ミク = hatsune_miku\nhatsune_miku > vocaloid\n", rels, err);
    CHECK(parseTagRelations(formatTagRelations(rels), back, err) && back.size() == 2 && back[1].other == "vocaloid");
    std::vector<std::pair<std::string, std::string>> pairs;
    CHECK(parseDanbooruPairs(R"([{"antecedent_name":"miku","consequent_name":"hatsune_miku","status":"active"},
                                 {"antecedent_name":"x","consequent_name":"y","status":"deleted"}])", pairs));
    CHECK(pairs.size() == 1 && pairs[0].first == "miku");
    CHECK(!parseDanbooruPairs("{}", pairs));

    const fs::path dir = fs::temp_directory_path() / "imgdupe-tagrel-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    auto id = upsertScanned(db, {{"/a.png", 1, 1}, {"/b.png", 1, 2}, {"/c.png", 1, 3}});
    auto tag = [&](int64_t img, const std::string& t, double conf, const char* source = "tagger") {
        Stmt st(db.handle(), "INSERT INTO tags(image_id,tag,kind,confidence,source,manual) VALUES(?,?,'character',?,?,0)");
        st.bind(1, img).bind(2, t).bind(3, conf).bind(4, std::string(source));
        st.step();
    };
    tag(id["/a.png"], "hatsune_miku", 0.9);
    tag(id["/b.png"], "初音ミク", 0.6, "folder");  // 別名で付いている
    tag(id["/c.png"], "hatsune_miku", 0.1);         // 確度が低すぎる
    replaceRelations(db, "user", rels);
    replaceRelations(db, "danbooru", {{"miku", "hatsune_miku", "alias"}, {"vocaloid", "music", "parent"}});
    const RelationCounts rc = relationCounts(db);
    CHECK(rc.userAlias == 1 && rc.userParent == 1 && rc.danbooruAlias == 1 && rc.danbooruParent == 1);
    CHECK(loadRelations(db, "user").size() == 2);
    // 検索の別名にも入る（自分で書いたものと Danbooru のもの）
    SearchQuery q;
    q.tags = {"miku"};
    q.minConfidence = 0.5;
    CHECK(search(db, q).ids.size() == 2);
    // 親子: a（0.9）と b（別名で 0.6）に vocaloid と、その親の music。c は付かない
    CHECK(applyParents(db) == 4);
    auto conf = [&](int64_t img, const std::string& t) {
        Stmt st(db.handle(), "SELECT confidence FROM tags WHERE image_id=? AND tag=? AND source='parent'");
        st.bind(1, img).bind(2, t);
        return st.step() ? st.f64(0) : -1.0;
    };
    CHECK(std::abs(conf(id["/a.png"], "vocaloid") - 0.9) < 1e-9 && std::abs(conf(id["/b.png"], "music") - 0.6) < 1e-9);
    CHECK(conf(id["/c.png"], "vocaloid") < 0);
    // 関係を消せば、付けたタグも消える
    replaceRelations(db, "user", {});
    CHECK(applyParents(db) == 0 && conf(id["/a.png"], "vocaloid") < 0);
    fs::remove_all(dir);
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

static void testLookup() {
    // 応答の解析
    LookupPost post;
    CHECK(parseDanbooruPost(R"([{"id":123,"tag_string_artist":"foo_artist","tag_string_character":"hatsune_miku",
        "tag_string_copyright":"vocaloid","tag_string_general":"1girl long_hair 1girl","tag_string_meta":"highres"}])", post));
    CHECK(post.found && post.site == "danbooru" && post.postId == "123" && post.url == "https://danbooru.donmai.us/posts/123" &&
          post.tags.size() == 5 && post.tags[0].tag == "foo_artist" && post.tags[0].kind == "artist" && post.tags[1].kind == "character");
    CHECK(parseDanbooruPost("[]", post) && !post.found);
    CHECK(parseDanbooruPost(R"({"id":7,"tag_string_general":"a"})", post) && post.found && post.postId == "7" && post.tags.size() == 1);
    CHECK(!parseDanbooruPost("<html>", post));
    CHECK(parsePixivIllust(R"({"error":false,"message":"","body":{"illustId":"555","illustTitle":"t","userId":"9","userName":"作者",
        "tags":{"tags":[{"tag":"オリジナル","translation":{"en":"original"}},{"tag":"女の子"}]}}})", post));
    CHECK(post.found && post.postId == "555" && post.authorId == "9" && post.title == "t" && post.tags.size() == 4 &&
          post.tags[0].tag == "作者" && post.tags[0].kind == "artist" && post.tags[2].tag == "original");
    CHECK(parsePixivIllust(R"({"error":true,"message":"消された","body":[]})", post) && !post.found);
    SauceResult sr;
    CHECK(parseSauceNao(R"({"header":{"status":0,"short_remaining":3,"long_remaining":99},"results":[
        {"header":{"similarity":"95.1"},"data":{"ext_urls":["https://x"],"member_name":"m"}},
        {"header":{"similarity":"88.0"},"data":{"pixiv_id":555,"member_name":"作者","ext_urls":["https://www.pixiv.net/artworks/555"]}},
        {"header":{"similarity":"40"},"data":{"danbooru_id":1}}]})", sr));
    CHECK(sr.status == 0 && sr.shortRemaining == 3 && sr.longRemaining == 99 && sr.found && sr.best.pixivId == "555" &&
          sr.best.similarity == 88.0 && sr.best.author == "作者");
    CHECK(parseSauceNao(R"({"header":{"status":-1,"message":"bad key"}})", sr) && sr.status == -1 && !sr.found && sr.message == "bad key");
    std::string ct;
    const std::string mp = buildMultipart("file", "a.jpg", "image/jpeg", "DATA", ct);
    CHECK(ct.find("multipart/form-data; boundary=") == 0 && mp.find("filename=\"a.jpg\"") != std::string::npos &&
          mp.find("\r\n\r\nDATA\r\n--") != std::string::npos);

    // DB と流れ（偽の通信）
    const fs::path dir = fs::temp_directory_path() / "imgdupe-lookup-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    std::vector<ScannedFile> files;
    for (int i = 1; i <= 7; ++i) files.push_back({"/img/" + std::to_string(i) + ".png", 1, i});
    auto ids = upsertScanned(db, files);
    auto I = [&](int n) { return ids["/img/" + std::to_string(n) + ".png"]; };
    auto src = [&](int n, const char* site, const std::string& post) {
        Stmt st(db.handle(), "INSERT INTO sources(image_id,site,post_id) VALUES(?,?,?)");
        st.bind(1, I(n)).bind(2, std::string(site)).bind(3, post);
        st.step();
    };
    const std::string md5a(32, 'a'), md5b(32, 'b');
    src(1, "pixiv", "555");
    src(2, "pixiv", "555");  // 同じ投稿の別のページ（照会は 1 回）
    src(3, "md5", md5a);
    src(4, "md5", md5b);     // Danbooru に無い → 後で逆画像検索の対象
    src(5, "pixiv", "777");
    replaceTags(db, I(5), kSourceMeta, {TagRow{"known", "general", kSourceMeta, 0.95, false}});  // json で分かっている
    // 6: 出典なし（逆画像検索）、7: 写真（送らない）
    {
        ThumbPack pack(dir.string());
        db.begin();
        for (int n : {4, 6, 7}) {
            ThumbData t;
            t.width = t.height = 1;
            t.bytes = {0xFF, 0xD8, static_cast<uint8_t>(n)};
            CHECK(pack.put(db, I(n), t));
        }
        Stmt st(db.handle(), "INSERT INTO file_meta(image_id, camera) VALUES(?, 'Canon')");
        st.bind(1, I(7));
        st.step();
        db.commit();
    }
    auto pending = pendingIdLookups(db, 100);
    CHECK(pending.size() == 4);  // 5 は除く

    std::vector<std::string> gets;
    std::vector<std::string> posts;
    int sleptMs = 0;
    LookupNet net;
    net.get = [&](const std::string& url, const std::vector<std::string>&) {
        gets.push_back(url);
        HttpResult r;
        r.ok = true;
        r.status = 200;
        if (url == "https://www.pixiv.net/ajax/illust/555")
            r.body = R"({"error":false,"body":{"illustId":"555","userId":"9","userName":"作者","tags":{"tags":[{"tag":"女の子"}]}}})";
        else if (url == "https://danbooru.donmai.us/posts.json?tags=md5%3A" + md5a + "&limit=1")
            r.body = R"([{"id":42,"tag_string_artist":"art","tag_string_general":"1girl"}])";
        else if (url == "https://danbooru.donmai.us/posts.json?tags=md5%3A" + md5b + "&limit=1")
            r.body = "[]";
        else
            r.status = 404;
        return r;
    };
    net.post = [&](const std::string& url, const std::vector<std::string>& headers, const std::string& body) {
        posts.push_back(url);
        HttpResult r;
        r.ok = true;
        r.status = 200;
        CHECK(headers.size() == 1 && headers[0].find("multipart/form-data") != std::string::npos && body.find("\xFF\xD8") != std::string::npos);
        if (body.find(std::string("\xFF\xD8") + static_cast<char>(6)) != std::string::npos)
            r.body = R"({"header":{"status":0,"short_remaining":3,"long_remaining":50},"results":[
                {"header":{"similarity":"91"},"data":{"pixiv_id":"555","member_name":"作者"}}]})";
        else
            r.body = R"({"header":{"status":0,"short_remaining":3,"long_remaining":50},"results":[]})";
        return r;
    };
    net.thumbnail = [&](int64_t id, std::string& jpeg) {
        ThumbPack pack(dir.string());
        ThumbData t;
        if (!pack.get(db, id, t)) return false;
        jpeg.assign(t.bytes.begin(), t.bytes.end());
        return true;
    };
    net.sleep = [&](int ms) { sleptMs += ms; };
    int64_t clock = 86400 * 100 + 5;
    net.now = [&] { return clock; };

    LookupOptions opt;
    opt.idLookup = true;
    std::atomic<bool> cancel{false};
    LookupReport rep = runLookups(db, opt, net, cancel);
    CHECK(rep.looked == 4 && rep.found == 3 && rep.reverseSent == 0 && rep.error.empty());
    CHECK(gets.size() == 3);  // 555 は 1 回
    CHECK(sleptMs >= 1000);   // pixiv 1 回・Danbooru 2 回 → Danbooru の間で 1 秒待つ
    auto tagSet = [&](int n, const char* source) {
        std::set<std::string> out;
        for (const auto& t : tagsOf(db, I(n)))
            if (t.source == source) out.insert(t.tag);
        return out;
    };
    CHECK(tagSet(1, kSourceIdLookup) == std::set<std::string>({"作者", "女の子"}) && tagSet(2, kSourceIdLookup).size() == 2);
    CHECK(tagSet(3, kSourceIdLookup) == std::set<std::string>({"art", "1girl"}) && tagSet(4, kSourceIdLookup).empty());
    {
        Stmt st(db.handle(), "SELECT author_id, url, checked FROM sources WHERE image_id=? AND site='pixiv'");
        st.bind(1, I(1));
        CHECK(st.step() && st.text(0) == "9" && st.text(1) == "https://www.pixiv.net/artworks/555" && st.i64(2) == clock);
        Stmt d(db.handle(), "SELECT post_id FROM sources WHERE image_id=? AND site='danbooru'");
        d.bind(1, I(3));
        CHECK(d.step() && d.text(0) == "42");
    }
    CHECK(pendingIdLookups(db, 100).empty());
    // 逆画像検索の対象: 4（Danbooru に無かった MD5）と 6。7 は写真、1〜3 は出典あり、5 は json あり
    auto rev = pendingReverseLookups(db, 100);
    CHECK(rev.size() == 2 && rev[0] == I(6) && rev[1] == I(4));

    // SauceNAO（1 日 1 回まで → 6 だけ送る）
    gets.clear();
    opt.reverse = true;
    opt.sauceKey = "k ey";
    opt.sauceDailyLimit = 1;
    rep = runLookups(db, opt, net, cancel);
    CHECK(rep.reverseSent == 1 && rep.reverseFound == 1 && posts.size() == 1 &&
          posts[0] == "https://saucenao.com/search.php?output_type=2&numres=5&db=999&api_key=k%20ey");
    CHECK(gets.size() == 1 && gets[0] == "https://www.pixiv.net/ajax/illust/555");
    CHECK(tagSet(6, kSourceReverse) == std::set<std::string>({"作者", "女の子"}));
    CHECK(sauceSentToday(db, clock) == 1 && sauceSentToday(db, clock + 86400) == 0);
    rep = runLookups(db, opt, net, cancel);
    CHECK(rep.reverseSent == 0 && posts.size() == 1);  // 今日の分は使い切った
    clock += 86400;
    rep = runLookups(db, opt, net, cancel);
    CHECK(rep.reverseSent == 1 && rep.reverseFound == 0 && posts.size() == 2);  // 4 を送った（見つからない）
    CHECK(pendingReverseLookups(db, 100).empty());

    // API キーの誤り
    net.post = [&](const std::string&, const std::vector<std::string>&, const std::string&) {
        HttpResult r;
        r.ok = true;
        r.status = 200;
        r.body = R"({"header":{"status":-2,"message":"Invalid key"}})";
        return r;
    };
    resetLookups(db, {I(6)});
    CHECK(tagSet(6, kSourceReverse).empty() && pendingReverseLookups(db, 100).size() == 0);  // 6 には pixiv の出典が残る
    CHECK(pendingIdLookups(db, 100).size() == 1);  // その pixiv をもう一度照会する
    resetLookups(db, {I(4)});
    clock += 86400;
    rep = runLookups(db, opt, net, cancel);
    CHECK(rep.error.find("API キー") != std::string::npos && rep.reverseSent == 0);

    // 通信できない時は印を付けない
    net.get = [&](const std::string&, const std::vector<std::string>&) {
        HttpResult r;
        r.error = "timeout";
        return r;
    };
    opt.reverse = false;
    resetLookups(db, {I(1), I(3)});
    rep = runLookups(db, opt, net, cancel);
    CHECK(rep.looked == 0 && !rep.error.empty() && pendingIdLookups(db, 100).size() == 2);
}

static void testStats() {
    const fs::path dir = fs::temp_directory_path() / "imgdupe-stats-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    // FILETIME: 2024-01-15 と 2024-03-10（UTC）
    auto ft = [](int64_t unix) { return (unix + 11644473600LL) * 10000000LL; };
    const int64_t jan = 1705276800, mar = 1710028800;
    std::vector<ScannedFile> files = {{"/a/1.png", 100, ft(jan)}, {"/a/2.jpg", 200, ft(jan)}, {"/a/3.jpg", 300, ft(mar)},
                                      {"/a/4.gif", 400, ft(mar)}};
    auto ids = upsertScanned(db, files);
    auto I = [&](int n) {
        for (auto& [p, id] : ids)
            if (p.find("/" + std::to_string(n) + ".") != std::string::npos) return id;
        return int64_t(0);
    };
    {
        // 4 はコピーされたもの: 作成日時が更新日時より新しい → 更新日時（3 月）。1 は作成日時が古い（2023-12）→ 作成日時
        Stmt st(db.handle(), "INSERT INTO file_meta(image_id, format, created) VALUES(?,?,?)");
        st.bind(1, I(1)).bind(2, std::string("png")).bind(3, int64_t(1702000000));
        st.step();
        st.reset();
        st.bind(1, I(4)).bind(2, std::string("GIF")).bind(3, mar + 86400 * 40);
        st.step();
    }
    auto months = monthlyCounts(db, false);
    CHECK(months.size() == 3 && months[0].key == "2023-12" && months[0].count == 1 && months[1].key == "2024-01" &&
          months[1].count == 1 && months[1].bytes == 200 && months[2].key == "2024-03" && months[2].count == 2);
    auto formats = formatCounts(db);
    CHECK(formats.size() == 3 && formats[0].key == "JPG" && formats[0].count == 2 && formats[1].key == "GIF" && formats[2].key == "PNG");

    // タグ: miku は 1・2・3（3 は手動で「違う」）、別名 初音ミク（4）も miku にまとめる。rin は確度が低い
    replaceTags(db, I(1), "tagger", {TagRow{"miku", "character", "tagger", 0.9, false}, TagRow{"rin", "character", "tagger", 0.3, false}});
    replaceTags(db, I(2), "tagger", {TagRow{"miku", "character", "tagger", 0.6, false}});
    replaceTags(db, I(3), "tagger", {TagRow{"miku", "character", "tagger", 0.8, false}});
    replaceTags(db, I(4), "meta", {TagRow{"初音ミク", "character", "meta", 0.95, false}});
    replaceTags(db, I(2), "meta", {TagRow{"miku", "character", "meta", 0.95, false}, TagRow{"someone", "artist", "meta", 0.95, false}});
    setManualTag(db, I(3), "miku", false);
    db.exec("INSERT INTO tag_dict(tag, alias_of) VALUES('初音ミク', 'miku')");
    auto chars = tagCounts(db, {"character"}, 0.5, 10);
    CHECK(chars.size() == 1 && chars[0].key == "miku" && chars[0].count == 3 && chars[0].bytes == 700);
    auto artists = tagCounts(db, {"artist"}, 0.5, 10);
    CHECK(artists.size() == 1 && artists[0].key == "someone" && artists[0].count == 1);
    CHECK(tagCounts(db, {"character", "artist"}, 0.2, 10).size() == 3);

    // 概要
    {
        db.exec("INSERT INTO groups(id, kind) VALUES(1, 'exact'), (2, 'similar')");
        Stmt st(db.handle(), "INSERT INTO group_members(group_id, image_id, representative) VALUES(?,?,?)");
        for (auto [g, n, rep] : std::vector<std::tuple<int, int, int>>{{1, 1, 1}, {1, 2, 0}, {2, 3, 1}, {2, 4, 0}}) {
            st.bind(1, int64_t(g)).bind(2, I(n)).bind(3, int64_t(rep));
            st.step();
            st.reset();
        }
        Stmt src(db.handle(), "INSERT INTO sources(image_id, site, post_id, url) VALUES(?, 'pixiv', '1', 'https://x')");
        src.bind(1, I(1));
        src.step();
    }
    setStage(db, I(1), "hash", StageState::Done, 1);
    setStage(db, I(2), "hash", StageState::Failed, 1);
    StatsSummary s = statsSummary(db);
    CHECK(s.images == 4 && s.bytes == 1000 && s.exactGroups == 1 && s.exactExtra == 1 && s.exactExtraBytes == 200 &&
          s.similarGroups == 1 && s.similarExtraBytes == 400 && s.tagged == 4 && s.withSource == 1);
    CHECK(s.stages.size() == 1 && s.stages[0].name == "hash" && s.stages[0].done == 1 && s.stages[0].failed == 1);
}

static void testLinkCheck() {
    CHECK(linkStatusFromResponse("pixiv", 404, "") == 0 && linkStatusFromResponse("pixiv", 403, "") == -1 &&
          linkStatusFromResponse("pixiv", 200, R"({"error":false,"body":{}})") == 1 &&
          linkStatusFromResponse("pixiv", 200, R"({"error":true,"body":[]})") == 0 && linkStatusFromResponse("pixiv", 200, "<html>") == -1 &&
          linkStatusFromResponse("danbooru", 200, R"({"id":1,"is_deleted":true})") == 0 &&
          linkStatusFromResponse("danbooru", 200, R"({"id":1,"is_deleted":false})") == 1 && linkStatusFromResponse("x", 200, "{}") == 1 &&
          linkStatusFromResponse("x", 503, "") == -1);
    CHECK(sourcePageUrl("pixiv", "5", "") == "https://www.pixiv.net/artworks/5" && sourcePageUrl("x", "7", "") == "https://x.com/i/status/7" &&
          sourcePageUrl("download", "https://cdn/a.jpg", "https://page") == "https://page" && sourcePageUrl("md5", "abc", "").empty());

    const fs::path dir = fs::temp_directory_path() / "imgdupe-link-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    std::vector<ScannedFile> files;
    for (int i = 1; i <= 4; ++i) files.push_back({"/img/" + std::to_string(i) + ".png", 1, i});
    auto ids = upsertScanned(db, files);
    auto I = [&](int n) { return ids["/img/" + std::to_string(n) + ".png"]; };
    auto src = [&](int n, const char* site, const std::string& post, const std::string& url = "") {
        Stmt st(db.handle(), "INSERT INTO sources(image_id,site,post_id,url) VALUES(?,?,?,?)");
        st.bind(1, I(n)).bind(2, std::string(site)).bind(3, post).bind(4, url);
        st.step();
    };
    src(1, "pixiv", "100");
    src(2, "pixiv", "100");  // 同じ投稿は 1 回
    src(2, "danbooru", "200");
    src(3, "x", "300");
    src(4, "md5", std::string(32, 'a'));  // 確かめない
    src(4, "download", "https://cdn.example/a.jpg", "https://example/page");
    const int64_t day = 86400;
    int64_t clock = 1000 * day;
    auto pending = pendingLinkChecks(db, clock, 30, 100);
    CHECK(pending.size() == 3);

    std::vector<std::string> gets;
    LookupNet net;
    net.get = [&](const std::string& url, const std::vector<std::string>&) {
        gets.push_back(url);
        HttpResult r;
        r.ok = true;
        r.status = 200;
        if (url.find("/ajax/illust/100") != std::string::npos) r.status = 404;
        else if (url.find("/posts/200.json") != std::string::npos) r.body = R"({"id":200,"is_deleted":false})";
        else if (url.find("oembed") != std::string::npos) r.status = 403;  // 鍵のかかったアカウントなど
        return r;
    };
    net.sleep = [](int) {};
    net.now = [&] { return clock; };
    LookupOptions opt;
    opt.linkCheck = true;
    std::atomic<bool> cancel{false};
    LookupReport rep = runLookups(db, opt, net, cancel);
    CHECK(rep.linksChecked == 3 && rep.linksDead == 1 && gets.size() == 3);
    CHECK(std::find(gets.begin(), gets.end(), "https://publish.twitter.com/oembed?url=https%3A%2F%2Ftwitter.com%2Fi%2Fstatus%2F300") !=
          gets.end());
    auto links = sourceLinks(db, I(2));
    CHECK(links.size() == 2 && links[0].site == "danbooru" && links[0].status == 1 && links[1].site == "pixiv" && links[1].status == 0 &&
          links[1].url == "https://www.pixiv.net/artworks/100");
    CHECK(sourceLinks(db, I(3)).size() == 1 && sourceLinks(db, I(3))[0].status == -1);
    CHECK(sourceLinks(db, I(4)).size() == 1 && sourceLinks(db, I(4))[0].url == "https://example/page");
    // リンク切れで検索・統計
    SearchQuery q;
    q.deadLinks = true;
    CHECK(search(db, q).ids == std::vector<int64_t>({I(1), I(2)}));
    SearchQuery q2;
    CHECK(queryFromJson(queryToJson(q), q2) && q2.deadLinks && hasConditions(q2));
    CHECK(statsSummary(db).deadLinks == 2);
    // 30 日たつまでは確かめない
    clock += 29 * day;
    CHECK(pendingLinkChecks(db, clock, 30, 100).empty());
    clock += 2 * day;
    CHECK(pendingLinkChecks(db, clock, 30, 100).size() == 3);
    // 通信できなければ印を付けない
    net.get = [&](const std::string&, const std::vector<std::string>&) {
        HttpResult r;
        r.error = "timeout";
        return r;
    };
    rep = runLookups(db, opt, net, cancel);
    CHECK(rep.linksChecked == 0 && !rep.error.empty() && pendingLinkChecks(db, clock, 30, 100).size() == 3);
}

static void testQuality() {
    // 白い雑音（画素ごとに違う値）: 元の大きさなら細かさ 1、半分の大きさから双一次で 2 倍にすると半分くらい
    std::mt19937 rng(7);
    auto noise = [&](int n) {
        GrayImage g;
        g.width = g.height = n;
        g.px.resize(static_cast<size_t>(n) * n);
        for (auto& p : g.px) p = static_cast<uint8_t>(rng() % 256);
        return g;
    };
    auto upscale2 = [](const GrayImage& s) {
        GrayImage g;
        g.width = s.width * 2;
        g.height = s.height * 2;
        g.px.resize(static_cast<size_t>(g.width) * g.height);
        for (int y = 0; y < g.height; ++y)
            for (int x = 0; x < g.width; ++x) {
                const float u = std::clamp((x + 0.5f) / 2 - 0.5f, 0.0f, s.width - 1.0f), v = std::clamp((y + 0.5f) / 2 - 0.5f, 0.0f, s.height - 1.0f);
                const int x0 = static_cast<int>(u), y0 = static_cast<int>(v), x1 = std::min(x0 + 1, s.width - 1), y1 = std::min(y0 + 1, s.height - 1);
                const float fx = u - x0, fy = v - y0;
                auto P = [&](int xx, int yy) { return static_cast<float>(s.px[static_cast<size_t>(yy) * s.width + xx]); };
                const float a = P(x0, y0) * (1 - fx) + P(x1, y0) * fx, b = P(x0, y1) * (1 - fx) + P(x1, y1) * fx;
                g.px[static_cast<size_t>(y) * g.width + x] = static_cast<uint8_t>(std::lround(a * (1 - fy) + b * fy));
            }
        return g;
    };
    auto blur = [](const GrayImage& s, int r) {
        GrayImage g = s;
        for (int y = 0; y < s.height; ++y)
            for (int x = 0; x < s.width; ++x) {
                int sum = 0, n = 0;
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        const int xx = std::clamp(x + dx, 0, s.width - 1), yy = std::clamp(y + dy, 0, s.height - 1);
                        sum += s.px[static_cast<size_t>(yy) * s.width + xx];
                        ++n;
                    }
                g.px[static_cast<size_t>(y) * s.width + x] = static_cast<uint8_t>(sum / n);
            }
        return g;
    };
    const GrayImage native = noise(512);
    QualityResult a = measureQuality(native);
    QualityResult b = measureQuality(upscale2(noise(256)));
    QualityResult c = measureQuality(blur(blur(native, 2), 2));
    std::printf("quality: native %.2f %.2f / up2 %.2f %.2f / blur %.2f %.2f\n", a.detail, a.sharpness, b.detail, b.sharpness, c.detail, c.sharpness);
    CHECK(a.detail > 0.9 && a.effectiveSide > 460 && a.sharpness > kBlurryBelow);
    CHECK(b.detail > 0 && b.detail < kSoftBelow && b.effectiveSide < 400);
    CHECK(c.detail > 0 && c.detail < kSoftBelow && c.sharpness < kBlurryBelow);
    CHECK(measureQuality(native, 0.5).effectiveSide == 1024);  // 縮めて読んだ時は元の大きさで
    GrayImage flat;
    flat.width = flat.height = 64;
    flat.px.assign(64 * 64, 128);
    CHECK(measureQuality(flat).detail < 0 && measureQuality(flat).sharpness < 0 && measureQuality(flat).effectiveSide == 64);
    // 見栄え: 重みの向きのベクトルは点数が高く、逆向きは低い
    std::vector<float> v(kAestheticWeights, kAestheticWeights + 512);
    normalizeVector(v);
    const double hi = aestheticScore(v.data());
    for (float& x : v) x = -x;
    CHECK(hi > 20 && aestheticScore(v.data()) < -10);
    CHECK(std::abs(hi + aestheticScore(v.data()) - 2 * kAestheticBias) < 1e-3);

    // 段階 quality・見栄え・検索
    const fs::path dir = fs::temp_directory_path() / "imgdupe-quality-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    std::vector<ScannedFile> files = {{"/q/native.png", 1, 1}, {"/q/up.png", 1, 2}, {"/q/blur.png", 1, 3}, {"/q/bad.png", 1, 4}};
    auto ids = upsertScanned(db, files);
    std::map<std::string, GrayImage> imgs = {{"/q/native.png", native}, {"/q/up.png", upscale2(noise(256))}, {"/q/blur.png", blur(blur(native, 2), 2)}};
    QualityAnalyzer qa(
        [&](const std::string& path, int maxSide, std::vector<uint8_t>& bgra, int& w, int& h, int& ow, int& oh) {
            auto it = imgs.find(path);
            if (it == imgs.end()) return false;
            CHECK(maxSide == kQualityDecodeSide);
            w = ow = it->second.width;
            h = oh = it->second.height;
            bgra.resize(static_cast<size_t>(w) * h * 4);
            for (size_t i = 0; i < it->second.px.size(); ++i) {
                bgra[i * 4] = bgra[i * 4 + 1] = bgra[i * 4 + 2] = it->second.px[i];
                bgra[i * 4 + 3] = 255;
            }
            return true;
        },
        {}, {});
    std::atomic<bool> cancel{false};
    StageReport sr = runStage(db, qa, 2, cancel);
    CHECK(sr.processed == 4 && sr.failed == 1);
    SearchQuery q;
    q.quality = QualityFilter::Good;
    CHECK(search(db, q).ids == std::vector<int64_t>({ids["/q/native.png"]}));
    q.quality = QualityFilter::Soft;
    CHECK(search(db, q).ids.size() == 2);
    q.quality = QualityFilter::Blurry;
    CHECK(search(db, q).ids == std::vector<int64_t>({ids["/q/blur.png"]}));
    q.quality = QualityFilter::LowRes;
    CHECK(search(db, q).ids.size() == 3);  // どれも 800 未満
    SearchQuery q2;
    CHECK(queryFromJson(queryToJson(q), q2) && q2.quality == QualityFilter::LowRes && hasConditions(q2));
    ImageInfo info;
    CHECK(imageInfo(db, ids["/q/native.png"], info) && info.detail > 0.9 && info.effective > 460 && info.aesthetic < 0);
    // 見栄え（CLIP のベクトルから）
    const std::string vpath = (dir / "v.bin").string();
    {
        VectorWriter w;
        CHECK(w.open(vpath, 512, "m"));
        std::vector<float> good(kAestheticWeights, kAestheticWeights + 512), bad = good;
        normalizeVector(good);
        for (float& x : bad) x = -x;
        normalizeVector(bad);
        w.append(ids["/q/native.png"], good.data());
        w.append(ids["/q/up.png"], bad.data());
    }
    VectorIndex vecs;
    CHECK(vecs.load(vpath));
    CHECK(applyAesthetic(db, vecs) == 2 && applyAesthetic(db, vecs) == 0);
    SearchQuery s;
    s.sort = SortKey::Aesthetic;
    s.descending = true;
    auto sorted = search(db, s).ids;
    CHECK(sorted.size() == 4 && sorted[0] == ids["/q/native.png"] && sorted[1] == ids["/q/up.png"]);
    s.sort = SortKey::Effective;
    CHECK(search(db, s).ids[0] == ids["/q/native.png"]);
}

static void testResolveRedirect() {
    const std::string b = "https://huggingface.co/a/b/resolve/123/model.onnx?x=1";
    CHECK(resolveRedirect(b, "https://cdn.example/x") == "https://cdn.example/x");
    CHECK(resolveRedirect(b, "/api/resolve-cache/models/a/b/123/model.onnx?etag=%22e%22") ==
          "https://huggingface.co/api/resolve-cache/models/a/b/123/model.onnx?etag=%22e%22");
    CHECK(resolveRedirect(b, "//cas.example/p") == "https://cas.example/p");
    CHECK(resolveRedirect(b, "other.onnx") == "https://huggingface.co/a/b/resolve/123/other.onnx");
    CHECK(resolveRedirect(b, "?y=2") == "https://huggingface.co/a/b/resolve/123/model.onnx?y=2");
    CHECK(resolveRedirect("http://127.0.0.1:8765", "/m.json") == "http://127.0.0.1:8765/m.json");
    CHECK(resolveRedirect(b, "/go?to=https://x") == "https://huggingface.co/go?to=https://x");
}

static void testSameImage() {
    const fs::path dir = fs::temp_directory_path() / "imgdupe-same-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    std::vector<ScannedFile> files = {{"D:\\Pics\\a\\1.jpg", 100, 1}, {"D:\\Pics\\a\\2.jpg", 100, 2}, {"D:\\Pics\\b\\3.png", 300, 3},
                                      {"D:\\Pics\\b\\4.png", 100, 4}, {"D:\\Pics\\b\\sub\\5.png", 100, 5}, {"D:\\Other\\6.jpg", 100, 6},
                                      {"D:\\Pics\\a\\7.jpg", 100, 7}};
    auto ids = upsertScanned(db, files);
    auto feat = [&](const std::string& p, uint64_t dh, uint64_t dv, int contrast, const char* sha) {
        Stmt st(db.handle(), "UPDATE images SET dh=?, dv=?, color=?, contrast=?, readable=1, width=10, height=20, sha=? WHERE id=?");
        st.bind(1, static_cast<int64_t>(dh)).bind(2, static_cast<int64_t>(dv)).bind(3, int64_t(0x808080)).bind(4, int64_t(contrast));
        if (sha) {
            Sha256 h{};
            h.fill(static_cast<uint8_t>(sha[0]));
            st.bindBlob(5, h.data(), h.size());
        }
        st.bind(6, ids[p]);
        st.step();
        setStage(db, ids[p], "hash", StageState::Done, kHashVersion);
    };
    feat("D:\\Pics\\a\\1.jpg", 0xF0F0, 0xFF00, 40, "A");
    feat("D:\\Pics\\a\\2.jpg", 0xF0F0, 0xFF00, 40, nullptr);   // SHA が DB に無い（ファイルから求める）
    feat("D:\\Pics\\b\\3.png", 0xF0F1, 0xFF00, 40, "B");       // 大きさが違う・見た目は近い（距離 1）
    feat("D:\\Pics\\b\\4.png", 0xF0F0, 0xFF00 ^ 0xFFF, 40, "C");  // 大きさは同じだが SHA が違う・距離 12
    feat("D:\\Pics\\b\\sub\\5.png", 0, 0, 2, "D");               // 単色に近い
    feat("D:\\Other\\6.jpg", 0xF0F0 ^ 0x7, 0xFF00, 40, "E");   // 距離 3
    // 7 はまだハッシュしていない
    SameImageIndex index;
    CHECK(index.refresh(db, kHashVersion) && !index.refresh(db, kHashVersion) && index.size() == 6);
    SameQuery q;
    q.f.ok = true;
    q.f.dh = 0xF0F0;
    q.f.dv = 0xFF00;
    q.f.r = q.f.g = q.f.b = 0x80;
    q.f.contrast = 40;
    q.size = 100;
    q.hasSha = true;
    q.sha.fill('A');
    q.selfPath = "d:/pics/a/1.jpg";  // 大文字小文字・区切りが違っても自分は除く
    int shaCalls = 0;
    auto shaOf = [&](const std::string& path, Sha256& out) {
        ++shaCalls;
        out.fill(path == "D:\\Pics\\a\\2.jpg" ? 'A' : 'Z');
        return true;
    };
    auto m = index.find(db, q, shaOf);
    CHECK(m.size() == 3 && m[0].path == "D:\\Pics\\a\\2.jpg" && m[0].exact && m[1].path == "D:\\Pics\\b\\3.png" && m[1].distance == 1 &&
          !m[1].exact && m[2].path == "D:\\Other\\6.jpg" && m[2].distance == 3 && m[0].width == 10);
    CHECK(shaCalls == 2);  // 2.jpg と、SHA の無い 7.jpg
    q.threshold = 12;
    CHECK(index.find(db, q, shaOf).size() == 4);
    q.f.ok = false;  // 特徴量が無ければ完全一致だけ
    CHECK(index.find(db, q, shaOf).size() == 1);
    auto folders = groupSameByFolder(m);
    CHECK(folders.size() == 3 && folders[0].folder == "D:\\Pics\\a" && folders[0].exact == 1 && folders[1].folder == "D:\\Pics\\b");

    // インデックスの状態
    const std::vector<std::string> roots = {"D:\\Pics\\"};
    auto st = fileIndexStates(db, "d:\\pics\\a", roots, kHashVersion);
    CHECK(st.size() == 3 && st["1.jpg"] == IndexState::Indexed && st["7.jpg"] == IndexState::Pending && !st.count("3.png"));
    CHECK(fileIndexStates(db, "D:\\Pics\\b", roots, kHashVersion).size() == 2);  // sub の下は入らない
    CHECK(fileIndexStates(db, "D:\\Other", {}, kHashVersion).size() == 1);
    {
        // 名前を渡すと、その名前だけ引く（同じ結果）
        const std::vector<std::string> names = {"1.jpg", "7.jpg", "3.png", "none.jpg"};
        auto byName = fileIndexStates(db, "d:\\pics\\a", roots, kHashVersion, &names);
        CHECK(byName.size() == 2 && byName["1.jpg"] == IndexState::Indexed && byName["7.jpg"] == IndexState::Pending);
    }
    CHECK(folderIndexState("d:\\pics\\b\\sub", roots) == IndexState::Indexed && folderIndexState("D:\\Pics", roots) == IndexState::Indexed &&
          folderIndexState("D:\\", roots) == IndexState::Partial && folderIndexState("D:\\Other", roots) == IndexState::None &&
          folderIndexState("D:\\Pictures", roots) == IndexState::None);
    CHECK(findImageId(db, "d:\\pics\\a\\1.jpg", roots) == ids["D:\\Pics\\a\\1.jpg"] && findImageId(db, "D:/Other/6.jpg", {}) == ids["D:\\Other\\6.jpg"] &&
          findImageId(db, "D:\\nothing.jpg", roots) == 0);
    CHECK(folderHasIndexed(db, "D:\\Other") && folderHasIndexed(db, "D:\\Pics\\b\\") && !folderHasIndexed(db, "D:\\Pic"));
}

static void testFreeSearch() {
    const fs::path dir = fs::temp_directory_path() / "imgdupe-free-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    std::vector<ScannedFile> files = {{"C:\\p\\miku_live.jpg", 1, 1}, {"C:\\p\\a.jpg", 1, 2}, {"C:\\p\\b.jpg", 1, 3},
                                      {"C:\\miku\\c.jpg", 1, 4}, {"C:\\p\\d.jpg", 1, 5}};
    auto ids = upsertScanned(db, files);
    auto I = [&](const char* p) { return ids[p]; };
    replaceTags(db, I("C:\\p\\a.jpg"), "tagger", {TagRow{"hatsune_miku", "character", "tagger", 0.9, false},
                                                   TagRow{"stage", "general", "tagger", 0.8, false}});
    replaceTags(db, I("C:\\p\\b.jpg"), "tagger", {TagRow{"miku", "character", "tagger", 0.6, false}});
    replaceTags(db, I("C:\\p\\d.jpg"), "tagger", {TagRow{"miku", "character", "tagger", 0.95, false}});
    setManualTag(db, I("C:\\p\\d.jpg"), "miku", false);  // 手動で「違う」
    {
        Stmt st(db.handle(), "INSERT INTO file_meta(image_id, title) VALUES(?, ?)");
        st.bind(1, I("C:\\p\\a.jpg")).bind(2, std::string("ライブのステージ"));
        st.step();
        Stmt f(db.handle(), "INSERT INTO meta_fts(rowid, title) VALUES(?, ?)");
        f.bind(1, I("C:\\p\\a.jpg")).bind(2, std::string("ライブのステージ"));
        f.step();
    }
    SearchQuery q;
    q.freeText = "miku";
    auto r = search(db, q).ids;
    // a（語を含むタグ 0.9×0.7=0.63）、b（タグそのもの 0.6）、miku_live.jpg（ファイル名 0.5）。c はフォルダの名前だけ、d は手動で「違う」
    CHECK(r.size() == 3 && r[0] == I("C:\\p\\a.jpg") && r[1] == I("C:\\p\\b.jpg") && r[2] == I("C:\\p\\miku_live.jpg"));
    q.descending = true;  // 関連度の低い順
    CHECK(search(db, q).ids.front() == I("C:\\p\\miku_live.jpg"));
    q.descending = false;
    q.freeScope = FreeScope::Tags;
    CHECK(search(db, q).ids.size() == 2);
    q.freeScope = FreeScope::Title;
    CHECK(search(db, q).ids.empty());
    q.freeText = "ステージ";  // タイトル（3 文字以上は全文検索の索引）
    CHECK(search(db, q).ids == std::vector<int64_t>({I("C:\\p\\a.jpg")}));
    q.freeScope = FreeScope::All;
    q.freeText = "miku stage";  // どちらにも合う
    CHECK(search(db, q).ids == std::vector<int64_t>({I("C:\\p\\a.jpg")}));
    q.freeText = "miku -stage";
    CHECK(search(db, q).ids.size() == 2);
    q.freeText = "ライブ";  // 2 文字以下ではないが 3 文字（タイトル）
    CHECK(search(db, q).ids.size() == 1);
    q.freeText = "";
    q.sort = SortKey::FileName;
    r = search(db, q).ids;
    CHECK(r.size() == 5 && r[0] == I("C:\\p\\a.jpg") && r[3] == I("C:\\p\\d.jpg") && r[4] == I("C:\\p\\miku_live.jpg"));
    // ファイルのタグに限る（出どころ embedded のタグだけ）
    replaceTags(db, I("C:\\p\\b.jpg"), "embedded", {TagRow{"stage", "general", "embedded", 1.0, false}});
    {
        SearchQuery f;
        f.tags = {"stage"};
        CHECK(search(db, f).ids.size() == 2);  // a（タガー）と b（ファイル）
        f.sources = {"embedded"};
        CHECK(search(db, f).ids == std::vector<int64_t>({I("C:\\p\\b.jpg")}));
    }
    SearchQuery q2;
    q.freeText = "x";
    q.freeScope = FreeScope::Title;
    q.dateField = DateField::Modified;
    CHECK(queryFromJson(queryToJson(q), q2) && q2.freeText == "x" && q2.freeScope == FreeScope::Title &&
          q2.dateField == DateField::Modified && q2.sort == SortKey::FileName && hasConditions(q2));
    // 更新日時で絞る（images.mtime は FILETIME。ここでは 1〜5 なので 1601 年）
    SearchQuery d;
    d.dateField = DateField::Modified;
    d.takenTo = -11644473600LL;  // 1601-01-01 00:00:00
    CHECK(search(db, d).ids.size() == 5);
    d.takenTo = -11644473601LL;
    CHECK(search(db, d).ids.empty());
    // 選んだ画像だけ（スマートフォルダに保存した選択）。ほかの条件とも組み合わせる
    SearchQuery s;
    s.ids = {I("C:\\p\\b.jpg"), I("C:\\p\\d.jpg"), I("C:\\p\\a.jpg")};
    s.sort = SortKey::FileName;
    CHECK(hasConditions(s) && search(db, s).ids == std::vector<int64_t>({I("C:\\p\\a.jpg"), I("C:\\p\\b.jpg"), I("C:\\p\\d.jpg")}));
    s.freeText = "miku";
    CHECK(search(db, s).ids.size() == 2);
    SearchQuery s2;
    CHECK(queryFromJson(queryToJson(s), s2) && s2.ids == s.ids);
    // ファイル名に限る（タグ・タイトルは見ない）
    SearchQuery n;
    n.freeText = "miku";
    n.freeScope = FreeScope::Name;
    CHECK(search(db, n).ids == std::vector<int64_t>({I("C:\\p\\miku_live.jpg")}));
    SearchQuery n2;
    CHECK(queryFromJson(queryToJson(n), n2) && n2.freeScope == FreeScope::Name);
    // フォルダを名前で
    const auto folders = indexedFolders(db);
    CHECK(folders == std::vector<std::string>({"C:\\miku", "C:\\p"}));
    CHECK(matchFolders(folders, "MIKU", 10) == std::vector<std::string>({"C:\\miku"}));
    CHECK(matchFolders(folders, "miku -mi", 10).empty() && matchFolders(folders, "", 10).empty());
    CHECK(matchFolders({"D:\\a\\ab", "D:\\a\\abcdef", "D:\\ab\\x"}, "ab", 10) ==
          std::vector<std::string>({"D:\\a\\ab", "D:\\a\\abcdef"}));
}

static void testTagNames() {
    const fs::path dir = fs::temp_directory_path() / "imgdupe-tagnames-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    const auto parsed = parseTagNames("\xEF\xBB\xBF# コメント\nlong_hair\tロングヘア\r\n\nbad line\nshort_hair\tショートヘア\nblue_eyes\t青い目\n");
    CHECK(parsed.size() == 3 && parsed[0].first == "long_hair" && parsed[0].second == "ロングヘア");
    const std::string text = "long_hair\tロングヘア\nshort_hair\tショートヘア\nblue_eyes\t青い目\nhatsune_miku\t初音ミク\n";
    CHECK(installTagNames(db, text));
    CHECK(!installTagNames(db, text));  // 同じ内容なら入れ直さない
    auto names = loadTagNames(db);
    CHECK(names.size() == 4 && names["long_hair"] == "ロングヘア");
    setUserTagName(db, "Long_Hair", "長髪");  // 自分で付けた名前が優先（大文字小文字は区別しない）
    names = loadTagNames(db);
    CHECK(names["long_hair"] == "長髪");
    CHECK(tagsNamed(db, "長髪") == std::vector<std::string>({"Long_Hair"}));
    CHECK(tagsNamed(db, "ロングヘア").empty());  // 自分で付けた名前があるので、同梱の訳では合わせない
    CHECK(tagsWithNameContaining(db, "ヘア", 10) == std::vector<std::string>({"short_hair"}));
    // 同梱の訳を入れ直しても、自分で付けた名前は残る
    CHECK(installTagNames(db, text + "red_eyes\t赤い目\n"));
    CHECK(loadTagNames(db)["long_hair"] == "長髪" && loadTagNames(db)["red_eyes"] == "赤い目");
    setUserTagName(db, "long_hair", "");
    CHECK(loadTagNames(db)["long_hair"] == "ロングヘア");

    // 検索: 表示名でもそのタグとして探せる
    std::vector<ScannedFile> files = {{"C:\\p\\a.jpg", 1, 1}, {"C:\\p\\b.jpg", 1, 2}, {"C:\\p\\c.jpg", 1, 3}};
    auto ids = upsertScanned(db, files);
    replaceTags(db, ids["C:\\p\\a.jpg"], "tagger", {TagRow{"long_hair", "general", "tagger", 0.9, false}});
    replaceTags(db, ids["C:\\p\\b.jpg"], "tagger", {TagRow{"short_hair", "general", "tagger", 0.9, false}});
    replaceTags(db, ids["C:\\p\\c.jpg"], "tagger", {TagRow{"hatsune_miku", "character", "tagger", 0.9, false}});
    SearchQuery q;
    q.freeText = "ロングヘア";
    CHECK(search(db, q).ids == std::vector<int64_t>({ids["C:\\p\\a.jpg"]}));
    q.freeText = "ヘア";  // 表示名に語を含むタグ
    CHECK(search(db, q).ids.size() == 2);
    q.freeText = "初音ミク";
    q.freeScope = FreeScope::Tags;
    CHECK(search(db, q).ids == std::vector<int64_t>({ids["C:\\p\\c.jpg"]}));
    SearchQuery t;
    t.tags = {"ショートヘア"};
    CHECK(search(db, t).ids == std::vector<int64_t>({ids["C:\\p\\b.jpg"]}));

    // 同梱の訳: 読めて、タグも表示名も重ならない
    std::ifstream f("data/tags_ja.tsv", std::ios::binary);
    const std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto builtin = parseTagNames(all);
    std::set<std::string> tags, jas;
    for (const auto& [tag, ja] : builtin) {
        tags.insert(tag);
        jas.insert(ja);
    }
    CHECK(builtin.size() > 10000 && tags.size() == builtin.size() && jas.size() == builtin.size());
}

static bool hasInferred(const std::vector<InferredTag>& v, const std::string& kind, const std::string& tag) {
    for (const auto& t : v)
        if (t.kind == kind && t.tag == tag) return true;
    return false;
}

static void testPathInfer() {
    // 形
    CHECK(nameShape("12345 yamada").key == "D S");
    CHECK(nameShape("山田 太郎 (pixiv 12345)").key == "S ( pixiv D )");
    const NameShape sh = nameShape("山田 太郎 (pixiv 12345)");
    CHECK(sh.slots.size() == 2 && sh.slots[0].value == "山田 太郎" && sh.slots[1].value == "12345" && sh.slots[1].label == "pixiv");
    CHECK(nameShape("taro_yamada").key == "S" && nameShape("pixiv_12345").key == "pixiv D" && nameShape("12345_taro").key == "D S");
    CHECK(nameShape("山田【＠yamada_t】").key == "S [ @ ]" && nameShape("山田【＠yamada_t】").slots[1].value == "yamada_t");
    CHECK(nameShape("（pixiv：１２３）").key == "( pixiv D )" && nameShape("（pixiv：１２３）").slots[0].label == "pixiv");
    // 名前だけで
    auto a = inferFromName("山田 太郎 (pixiv 12345)", "");
    CHECK(hasInferred(a, "account", "pixiv:12345") && hasInferred(a, "artist", "山田 太郎") && a.size() == 2);
    a = inferFromName("山田【@yamada_t】", "");
    CHECK(hasInferred(a, "account", "x:@yamada_t") && hasInferred(a, "artist", "山田"));
    a = inferFromName("12345 sakusha", "pixiv");  // gallery-dl の pixiv
    CHECK(hasInferred(a, "account", "pixiv:12345") && hasInferred(a, "artist", "sakusha"));
    a = inferFromName("some_account", "x");
    CHECK(a.size() == 1 && hasInferred(a, "account", "x:@some_account"));
    CHECK(inferFromName("夏休み 2024", "").empty() && inferFromName("[サークル(作者)]本", "").empty());
    CHECK(inferFromName("12345", "").empty());  // 何の数字か分からない
    CHECK(accountUrl("pixiv:12345") == "https://www.pixiv.net/users/12345" && accountUrl("x:@abc") == "https://x.com/abc");
    CHECK(accountUrl("fanbox:abc") == "https://abc.fanbox.cc/" && accountUrl("foo") == "");

    // 兄弟の形と中の画像から
    InferInput in;
    // D:\絵\<pixiv の ID> <名前>\ … 中の画像の作者の ID が 1 つ分かっている
    for (const char* d : {"D:\\絵\\111 あ", "D:\\絵\\222 い", "D:\\絵\\333 う", "D:\\絵\\444 え"}) in.dirs[d].images = 3;
    in.dirs["D:\\絵\\111 あ"].accounts.insert("pixiv:111");
    // D:\作者\<名前>\ … 1 つの名前が json で分かった作者
    for (const char* d : {"D:\\作者\\たろう", "D:\\作者\\はなこ", "D:\\作者\\じろう", "D:\\作者\\たろう\\作品A"}) in.dirs[d].images = 2;
    in.dirs["D:\\作者\\はなこ"].artists.insert("はなこ");
    in.knownArtists.insert("はなこ");
    // 教えた形: D:\本\ の下の "名前 - 数字" は作者と fantia
    for (const char* d : {"D:\\本\\あいう - 77", "D:\\本\\かき - 88"}) in.dirs[d].images = 1;
    in.rules.push_back({0, "D:\\本", "S - D", {"artist", "fantia"}, "あいう - 77"});
    // 作者の名前とアカウントの対応（json から）で、名前だけのフォルダにアカウントを足す
    in.pairs.emplace_back("pixiv:999", "じろう");
    const auto r = inferFolders(in);
    CHECK(hasInferred(r.at("D:\\絵\\111 あ"), "account", "pixiv:111") && hasInferred(r.at("D:\\絵\\111 あ"), "artist", "あ"));
    CHECK(hasInferred(r.at("D:\\絵\\444 え"), "account", "pixiv:444") && hasInferred(r.at("D:\\絵\\444 え"), "artist", "え"));
    CHECK(hasInferred(r.at("D:\\作者\\たろう"), "artist", "たろう") && hasInferred(r.at("D:\\作者\\はなこ"), "artist", "はなこ"));
    CHECK(hasInferred(r.at("D:\\作者\\たろう\\作品A"), "artist", "たろう"));  // 上のフォルダから受け継ぐ
    CHECK(hasInferred(r.at("D:\\作者\\じろう"), "account", "pixiv:999"));
    CHECK(hasInferred(r.at("D:\\本\\かき - 88"), "artist", "かき") && hasInferred(r.at("D:\\本\\かき - 88"), "account", "fantia:88"));
    // 手がかりの無い名前だけのフォルダは何も付けない
    InferInput none;
    for (const char* d : {"D:\\x\\風景", "D:\\x\\猫", "D:\\x\\空"}) none.dirs[d].images = 1;
    CHECK(inferFolders(none).empty());

    // DB で: 書いて、変わっていなければ何もしない
    const fs::path dbPath = fs::temp_directory_path() / "imgdupe-infer-test.db";
    fs::remove(dbPath);
    {
        Db db;
        CHECK(db.open(dbPath.string()));
        for (const char* p : {"D:\\pixiv\\123 taro\\1_p0.jpg", "D:\\pixiv\\123 taro\\2_p0.jpg", "D:\\other\\a.jpg"}) {
            Stmt st(db.handle(), "INSERT INTO images(path) VALUES(?)");
            st.bind(1, std::string(p));
            st.step();
        }
        InferSummary s1 = runFolderInference(db);
        CHECK(!s1.skipped && s1.images == 2 && s1.tagged == 2);
        const auto tags = tagsOf(db, 1);
        bool acc = false, art = false;
        for (const auto& t : tags) {
            acc = acc || (t.kind == "account" && t.tag == "pixiv:123" && t.source == "infer");
            art = art || (t.kind == "artist" && t.tag == "taro");
        }
        CHECK(acc && art);
        CHECK(storedInferTags(db, "D:\\pixiv\\123 taro\\3_p0.jpg").size() == 2);
        CHECK(runFolderInference(db).skipped);
        // 教えた形を足すと、やり直す
        InferRule rule{0, "", "S", {"artist"}, "other"};
        CHECK(saveInferRule(db, rule) > 0 && loadInferRules(db).size() == 1);
        InferSummary s2 = runFolderInference(db);
        CHECK(!s2.skipped);
        bool other = false;
        for (const auto& t : tagsOf(db, 3)) other = other || (t.kind == "artist" && t.tag == "other");
        CHECK(other);
        deleteInferRule(db, loadInferRules(db)[0].id);
        CHECK(loadInferRules(db).empty());
        runFolderInference(db);
        CHECK(tagsOf(db, 3).empty());
    }
    fs::remove(dbPath);
}

static void testLogs() {
    const std::string line = logs::formatLine(0, "解析", logs::Level::Warn, "a\nb");
    CHECK(line.find("[解析] 注意: a b") != std::string::npos && line.size() >= 2 && line.substr(line.size() - 2) == "\r\n");
    const fs::path dir = fs::temp_directory_path() / "imgdupe-log-test";
    fs::remove_all(dir);
    logs::open(dir.string(), "t", "画面", 300, 2);  // 300 バイトで回し、2 つまで残す
    for (int i = 0; i < 20; ++i) logs::info("行 " + std::to_string(i) + " " + std::string(30, 'x'));
    logs::close();
    CHECK(fs::exists(dir / "t.log") && fs::exists(dir / "t.1.log") && fs::exists(dir / "t.2.log") && !fs::exists(dir / "t.3.log"));
    CHECK(fs::file_size(dir / "t.log") <= 300);
    std::ifstream in(dir / "t.log");
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(all.find("行 19 ") != std::string::npos);  // 新しい行は今のファイルに
    fs::remove_all(dir);
}

static void testQueryStore() {
    CHECK(queryImageName(1700000000, 3, ".bmp") == "q_1700000000_3.bmp");
    CHECK(queryImageTime("q_1700000000_3.bmp") == 1700000000 && queryImageTime("x.bmp") == 0 && queryImageTime("q_abc_1.png") == 0);
    const int64_t now = 1700100000;
    std::vector<std::string> names = {"keep.txt"};
    for (int i = 0; i < 12; ++i) names.push_back(queryImageName(now - 100 * i, 0, ".png"));  // 新しい順に 12 枚
    names.push_back(queryImageName(now - 2 * 86400, 0, ".png"));                         // 2 日前
    auto gone = queryImagesToRemove(names, now);
    std::sort(gone.begin(), gone.end());
    std::vector<std::string> want = {queryImageName(now - 1000, 0, ".png"), queryImageName(now - 1100, 0, ".png"),
                                     queryImageName(now - 2 * 86400, 0, ".png")};
    std::sort(want.begin(), want.end());
    CHECK(gone == want);  // 11・12 枚目と 1 日より古いもの。ほかの名前は触らない
    // DIB → BMP: 2×1 の 24bit（行は 4 バイトずつ）
    std::vector<uint8_t> dib(40 + 8, 0);
    dib[0] = 40;
    dib[4] = 2;
    dib[8] = 1;
    dib[12] = 1;
    dib[14] = 24;
    const auto bmp = bmpFromDib(dib.data(), dib.size());
    CHECK(bmp.size() == 14 + 48 && bmp[0] == 'B' && bmp[1] == 'M' && bmp[10] == 54 && bmp[2] == 62);
    dib[16] = 3;  // BI_BITFIELDS: 色のマスク 12 バイトの後ろ
    dib.resize(40 + 12 + 8);
    CHECK(bmpFromDib(dib.data(), dib.size())[10] == 66);
    CHECK(bmpFromDib(dib.data(), 10).empty());
}

static void testClusterResults() {
    const fs::path dir = fs::temp_directory_path() / "imgdupe-cluster-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Db db;
    CHECK(db.open((dir / "t.db").string()));
    std::vector<ScannedFile> files;
    for (int i = 0; i < 6; ++i) files.push_back({"C:\\p\\" + std::to_string(i) + ".jpg", 100 + i, 1});
    auto ids = upsertScanned(db, files);
    auto I = [&](int i) { return ids["C:\\p\\" + std::to_string(i) + ".jpg"]; };
    auto group = [&](const char* kind, std::vector<int> members) {
        Stmt g(db.handle(), "INSERT INTO groups(kind, created) VALUES(?, 0)");
        g.bind(1, std::string(kind));
        g.step();
        Stmt last(db.handle(), "SELECT last_insert_rowid()");
        last.step();
        const int64_t gid = last.i64(0);
        for (int m : members) {
            Stmt st(db.handle(), "INSERT INTO group_members(group_id, image_id) VALUES(?, ?)");
            st.bind(1, gid).bind(2, I(m));
            st.step();
        }
    };
    group("exact", {1, 3});
    group("variant", {3, 5});  // 1・3・5 は 1 つの束に
    group("query", {0, 2});     // 1 枚で検索のまとまりは使わない
    const std::vector<int64_t> res = {I(0), I(5), I(1), I(2), I(3), I(4)};
    const auto c = clusterResults(db, res);
    CHECK(c.size() == 4 && c[0] == std::vector<size_t>({0}) && c[1] == std::vector<size_t>({1, 2, 4}) && c[2] == std::vector<size_t>({3}) &&
          c[3] == std::vector<size_t>({5}));
    CHECK(imageCount(db) == 6);
    setStage(db, I(0), stage::kHash, StageState::Done, kHashVersion);
    setStage(db, I(1), stage::kHash, StageState::Failed, kHashVersion);
    setStage(db, I(2), stage::kHash, StageState::Done, kHashVersion + 1);  // 別の版は数えない
    const StageCount sc = stageCount(db, stage::kHash, kHashVersion);
    CHECK(sc.done == 1 && sc.failed == 1);
    const auto props = imageProperties(db, I(2));
    CHECK(!props.empty() && props[0].first == "パス" && props[0].second == "C:\\p\\2.jpg");
    bool exact = false;
    for (const auto& [k, v] : imageProperties(db, I(1))) exact = exact || (k == "完全一致" && v == "1 枚");
    CHECK(exact);
}

int main() {
    testSha256();
    testFeatures();
    testOrientPixels();
    testShrinker();
    testSimilarMatchesBruteForce();
    testRun();
    testDb();
    testPack();
    testIndexer();
    testScanTasks();
    testMeta();
    testSearch();
    testLinks();
    testNames();
    testModels();
    testTagger();
    testVectors();
    testClip();
    testAnn();
    testDict();
    testRefs();
    testTagRelations();
    testLookup();
    testStats();
    testLinkCheck();
    testQuality();
    testResolveRedirect();
    testSameImage();
    testFreeSearch();
    testQueryStore();
    testLogs();
    testPathInfer();
    testTagNames();
    testClusterResults();
    testExifOrientation();
    testUpdateLogic();
    testKeys();
    testMisc();
    if (failures) {
        std::fprintf(stderr, "%d 件失敗\n", failures);
        return 1;
    }
    std::puts("すべて成功");
    return 0;
}
