// エクスプローラーの画面で使う、DB への問い合わせ（OS に依存しない）。
// - 同じ画像を探す: 1 枚の画像（DB に無くてもよい）の特徴量から、DB の画像のうち完全一致（大きさと SHA-256 が同じ）と
//   見た目一致（dHash の距離がしきい値以下・平均の色が近い）を探す。dHash は全部の画像をメモリに読んでおき、総当たりで比べる
//   （100 万枚で数 ms。読み直すのは画像の数が変わった時だけ）
// - インデックスの状態: フォルダの中の画像が DB にあるか（ハッシュまで済んでいるか）、フォルダが調べる対象（roots）に入っているか
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core.hpp"
#include "db.hpp"

namespace imgdupe {

struct SameMatch {
    int64_t id = 0;
    std::string path;
    uint64_t size = 0;
    uint32_t width = 0, height = 0;
    bool exact = false;  // 中身がまったく同じ
    int distance = 0;    // 見た目の距離（完全一致は 0）
};

struct SameQuery {
    Features f;                // 調べる画像の特徴量（f.ok でなければ見た目一致は探さない）
    uint64_t size = 0;         // ファイルの大きさ（0 なら完全一致は探さない）
    bool hasSha = false;       // sha が分かっているか
    Sha256 sha{};
    std::string selfPath;      // 調べる画像そのもの（DB にあれば結果から除く）
    int threshold = 10;
    int colorTolerance = 40;
    int flatContrast = 6;      // これ未満（単色に近い）なら見た目一致は探さない
};

class SameImageIndex {
public:
    // 画像の数・最大の ID が変わっていれば読み直す（読み直したら true）。別のスレッドから呼んでよい
    bool refresh(Db& db, int hashVersion);
    // 完全一致を先に、続いて見た目の近い順。shaOf: DB に SHA-256 が無い候補のファイルの SHA を求める（無ければ候補を比べない）
    std::vector<SameMatch> find(Db& db, const SameQuery& q,
                                const std::function<bool(const std::string& path, Sha256& out)>& shaOf) const;
    size_t size() const;

private:
    struct Row {
        int64_t id;
        uint64_t dh, dv;
        uint8_t r, g, b, contrast;
    };
    mutable std::mutex m_;
    std::vector<Row> rows_;
    int64_t count_ = -1, maxId_ = -1;
};

// 結果をフォルダごとにまとめる（一致の多い順）
struct SameFolder {
    std::string folder;
    size_t exact = 0, similar = 0;
    std::vector<size_t> matches;  // 結果の添字
};
std::vector<SameFolder> groupSameByFolder(const std::vector<SameMatch>& matches);

// 検索の結果を、重複（完全一致・見た目一致。最後の「重複を探す」の結果）と差分（CLIP）のまとまりでまとめる。
// ids の添字の束を返す（束の並びは、束の中で一番前の画像の順。束の中も ids の順）。まとまりに入らない画像は 1 つだけの束
std::vector<std::vector<size_t>> clusterResults(Db& db, const std::vector<int64_t>& ids);

// 画像のプロパティ（プレビューの「プロパティ」に出す。名前と値の組。分からないものは出さない）。
// timeText: Unix 秒を文字にする（画面ではその地域の時刻。無ければ UTC）
std::vector<std::pair<std::string, std::string>> imageProperties(Db& db, int64_t id,
                                                                 const std::function<std::string(int64_t)>& timeText = nullptr);

// インデックスの状態
enum class IndexState { None = 0, Pending = 1, Indexed = 2, Partial = 3 };

// フォルダ dir（区切りで終わらない）の直下のファイルの状態（名前 → Indexed（読めなかったものも）/ Pending。DB に無いものは入らない）。
// roots と大文字小文字だけが違う時は、roots の書き方に直して探す
// names を渡すと、その名前だけを 1 つずつ引く（ドライブの直下など、下に画像の多いフォルダで下の階層全部を読まないように）
std::unordered_map<std::string, IndexState> fileIndexStates(Db& db, const std::string& dir, const std::vector<std::string>& roots,
                                                            int hashVersion, const std::vector<std::string>* names = nullptr);
// フォルダの状態: roots の中なら Indexed、中に roots があれば Partial、どちらでもなければ None
IndexState folderIndexState(const std::string& dir, const std::vector<std::string>& roots);
// パスの画像の ID（無ければ 0）。roots と大文字小文字だけが違う・区切りが / と \ で違うものも探す
int64_t findImageId(Db& db, const std::string& path, const std::vector<std::string>& roots);
// フォルダの下（いくつ下でも）の画像が DB にあるか（roots から外したフォルダや --index で作ったもの）
bool folderHasIndexed(Db& db, const std::string& dir);

// ---------------------------------------------------------------- 画像で探す（クリップボード・ドロップ・URL の画像）

// 一時に置く画像の名前: q_<追加した Unix 秒>_<番号><拡張子>
std::string queryImageName(int64_t addedUnix, int seq, const std::string& ext);
int64_t queryImageTime(const std::string& name);  // この形の名前でなければ 0
// 消す名前: maxAge 秒より古いもの、新しい順に maxCount 個を超えるもの（「10 枚まで、か 1 日分の少ない方」）。
// この形でない名前は触らない
std::vector<std::string> queryImagesToRemove(const std::vector<std::string>& names, int64_t now, size_t maxCount = 10,
                                             int64_t maxAge = 24 * 60 * 60);
// クリップボードの DIB（CF_DIB: BITMAPINFOHEADER から始まる）を BMP のファイルの中身にする。読めなければ空
std::vector<uint8_t> bmpFromDib(const uint8_t* dib, size_t size);

}  // namespace imgdupe
