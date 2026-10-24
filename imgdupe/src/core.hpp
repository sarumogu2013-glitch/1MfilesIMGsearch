// imgdupe の OS に依存しない部分（特徴量・比較・グループ分け・キャッシュ・結果の出力）。
// パスはすべて UTF-8 の std::string で持つ。画像の読み込みとフォルダの走査は呼び出し側が渡す。
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace imgdupe {

// 特徴量を作る縮小画像の一辺（RGB, 上から下へ）
constexpr int kThumb = 32;

// 画像 1 枚の特徴量
struct Features {
    bool ok = false;        // 読めたか
    uint64_t dh = 0;        // 横方向の dHash（隣の画素より明るいか、9x8 → 64bit）
    uint64_t dv = 0;        // 縦方向の dHash（8x9 → 64bit）
    uint8_t r = 0, g = 0, b = 0;  // 平均の色
    uint8_t contrast = 0;   // 明るさの標準偏差（0〜127）。小さいほど単色に近い
    uint32_t width = 0, height = 0;
};

// rgb: kThumb*kThumb*3 バイト。orientation は EXIF の向き（1〜8、それ以外は 1 とみなす）。
Features computeFeatures(const uint8_t* rgb, int orientation);

// 大きな画像を n×n に面積平均で縮める（行を少しずつ渡せるので、画像全体をメモリに置かなくてよい）。
// 縮小の仕方を自分で決めておくと、Windows の版やデコーダーの違いで特徴量が変わらない
class Shrinker {
public:
    Shrinker(uint32_t width, uint32_t height, int n = kThumb);
    // y 行目から count 行分。1 画素 3 バイト（bgr なら B,G,R の順）
    void addRows(const uint8_t* px, size_t stride, uint32_t y, uint32_t count, bool bgr);
    std::vector<uint8_t> finish() const;  // n×n×3 の RGB

private:
    struct Span {
        uint16_t begin, end;  // 入力の 1 列（1 行）が足し込まれる出力の範囲
    };
    static std::vector<Span> spans(uint32_t size, int n);
    int n_;
    std::vector<Span> xs_, ys_;
    std::vector<uint64_t> sum_;
    std::vector<uint32_t> count_;
};

// EXIF の向きに合わせて正方形の RGB 画像を回す（n×n）
void applyOrientation(std::vector<uint8_t>& rgb, int n, int orientation);

// JPEG の先頭部分（EXIF を含む APP1 まで）から向き（1〜8）を読む。無ければ 0
int jpegExifOrientation(const uint8_t* data, size_t len);

int popcount64(uint64_t x);

// 見た目の距離（横と縦の dHash のハミング距離の和、0〜128）
inline int distance(const Features& a, const Features& b) {
    return popcount64(a.dh ^ b.dh) + popcount64(a.dv ^ b.dv);
}

using Sha256 = std::array<uint8_t, 32>;
Sha256 sha256(const void* data, size_t len);
// ファイルの SHA-256。読めなければ false
bool sha256File(const std::string& path, Sha256& out);
std::string toHex(const Sha256& h);

// ---------------------------------------------------------------- キャッシュ

struct Entry {
    std::string path;
    uint64_t size = 0;
    int64_t mtime = 0;
    Features f;
    bool hasSha = false;
    Sha256 sha{};
};

// path → Entry。新規・変更されたファイルだけ計算し直すために使う
class Cache {
public:
    // 読めない・形式が違う時は空のまま false
    bool load(const std::string& file);
    // 一時ファイルに書いてから置き換える
    bool save(const std::string& file) const;

    std::unordered_map<std::string, Entry> entries;
};

// ---------------------------------------------------------------- 比較

struct SimilarOptions {
    int threshold = 10;     // distance() がこれ以下なら同じ見た目
    int colorTolerance = 40;  // 平均の色の差（各チャンネル）がこれより大きければ別の画像
    unsigned threads = 0;   // 0 なら CPU の数
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(double)> progress;  // 0〜1
};

struct Similar {
    // 同じ見た目のまとまり（2 枚以上）。中身は items の添字
    std::vector<std::vector<uint32_t>> clusters;
    // 各画像の、まとまりの中で一番近い相手との距離（まとまりに入らなければ 255）
    std::vector<uint8_t> nearest;
};

// 似た画像のペアを探してつなげる（Union-Find）。
// 「横 + 縦 ≤ しきい値」なら横か縦の少なくとも一方は しきい値/2 以下なので、
// ハッシュを小さな塊に分け、塊が一致するものどうしだけを比べる（Multi-Index Hashing）。
Similar findSimilar(const std::vector<Features>& items, const SimilarOptions& opt);

// 総当たり版（テスト用）
Similar findSimilarBruteForce(const std::vector<Features>& items, const SimilarOptions& opt);

// ---------------------------------------------------------------- 全体の流れ

struct ScannedFile {
    std::string path;
    uint64_t size = 0;
    int64_t mtime = 0;
};

enum class Stage { Scan, Hash, Exact, Similar, Done };

struct Progress {
    std::atomic<int> stage{static_cast<int>(Stage::Scan)};
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{0};
};

enum class Kind { Exact, Similar, Query };

struct Member {
    uint32_t index;  // Result::files の添字
    uint8_t dist;    // 見た目の距離（完全一致は 0）
};

struct Group {
    Kind kind = Kind::Exact;
    std::vector<Member> members;  // 距離の近い順、同じなら大きい順
    uint64_t totalSize = 0;
    uint64_t reclaim = 0;  // 一番大きいもの以外の合計（消せば空く容量）
};

struct RunOptions {
    std::vector<std::string> roots;
    int threshold = 10;
    int colorTolerance = 40;
    bool exactOnly = false;
    bool featuresOnly = false;  // ハッシュ化までで終える（1 枚で検索する時）
    bool includeFlat = false;  // 単色に近い画像も見た目で比べる
    int flatContrast = 6;      // contrast がこれ未満なら単色に近いとみなす
    unsigned threads = 0;
    std::string dbFile;        // 画像の情報を残す DB（SQLite）。空ならメモリの中だけ
    std::string legacyCacheFile;  // 前の版のキャッシュ（imgdupe_cache.bin）。あれば最初の 1 回だけ DB に取り込む
};

struct Result {
    std::vector<Entry> files;   // 見つかった画像（読めなかったものも含む）
    std::vector<int64_t> ids;   // files と同じ並びの、DB の画像 ID
    std::vector<Group> groups;
    uint64_t unreadable = 0;
    uint64_t flatSkipped = 0;
    uint64_t hashed = 0;        // 今回ハッシュ化した枚数
    bool cancelled = false;
    double secHash = 0, secCompare = 0;
};

struct Callbacks {
    // フォルダ以下の画像を列挙する（out に追加）。file を見つけるたびに Progress::total を増やしてよい
    std::function<void(const std::string& root, std::vector<ScannedFile>& out)> scan;
    // 画像を読んで特徴量を作る（複数のスレッドから同時に呼ばれる）
    std::function<Features(const std::string& path)> decode;
    // 各ワーカースレッドの始めと終わりに呼ぶ（COM の初期化など）。無くてもよい
    std::function<void()> threadInit, threadExit;
};

// 走査 → ハッシュ化（キャッシュ利用）→ 完全一致 → 見た目一致
Result run(const RunOptions& opt, const Callbacks& cb, Progress& prog, const std::atomic<bool>& cancel);

// 1 枚の画像と似た画像を result.files から探す（距離の近い順）
Group query(const Result& result, const Features& q, int threshold, int colorTolerance);

// 画像の拡張子か（小文字・大文字は区別しない）
bool isImageExt(const std::string& path);

// ---------------------------------------------------------------- 出力

std::string fileUri(const std::string& path);  // file:/// 形式（UTF-8 をパーセントエンコード）
std::string kindName(Kind k);
std::string formatSize(uint64_t bytes);  // "1.2 MB" など
bool writeCsv(const Result& r, const std::string& file);
bool writeHtml(const Result& r, const std::string& file);

// 並べ替え・容量の計算（run と query の後で使う）
void finishGroup(Group& g, const std::vector<Entry>& files);

}  // namespace imgdupe
