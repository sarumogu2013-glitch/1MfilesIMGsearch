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
