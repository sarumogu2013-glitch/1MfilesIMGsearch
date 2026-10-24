// OS に依存しない部分のテスト（./build.sh test）
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <random>

#include "analyzer.hpp"
#include "commands.hpp"
#include "db.hpp"
#include "indexer.hpp"
#include "meta.hpp"
#include "search.hpp"
#include "links.hpp"
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

    IndexReport r = indexOnce(opt, cb, prog, cancel);
    CHECK(r.files == 4 && r.added == 4 && r.removed == 0);
    CHECK(r.hashed == 4 && decodes == 4);
    CHECK(r.thumbs == 3 && r.failed == 1 && encodes == 4);
    CHECK(r.metas == 4 && metas == 4);
    CHECK(prog.phase == static_cast<int>(IndexPhase::Done));

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
    SearchQuery back;
    CHECK(queryFromJson(queryToJson(full), back));
    CHECK(back.tags == full.tags && back.excludeTags == full.excludeTags && back.excludeSources == full.excludeSources);
    CHECK(std::abs(back.minConfidence - 0.35) < 1e-9 && back.text == full.text && back.minRating == 0 && back.noRating);
    CHECK(back.takenFrom == full.takenFrom && back.takenTo == full.takenTo && back.noTaken && back.minWidth == 3000);
    CHECK(back.minHeight == 2000 && back.minSize == 150 && back.maxSize == 5000000000LL && back.format == "PNG");
    CHECK(back.animatedOnly && back.pathContains == full.pathContains && back.downloadContains == "pixiv");
    CHECK(back.sort == SortKey::Taken && back.descending && back.sources.empty() && back.limit == 0);
    SearchQuery def;
    CHECK(queryFromJson(queryToJson(SearchQuery{}), def) && def.minRating == -1 && def.minConfidence == 0.5 &&
          def.sort == SortKey::Path && !def.descending);
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
    testOrientPixels();
    testShrinker();
    testSimilarMatchesBruteForce();
    testRun();
    testDb();
    testPack();
    testIndexer();
    testMeta();
    testSearch();
    testLinks();
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
