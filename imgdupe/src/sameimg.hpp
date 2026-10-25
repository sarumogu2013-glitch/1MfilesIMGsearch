// エクスプローラーの画面で使う、DB への問い合わせ（OS に依存しない）。
// - 同じ画像を探す: 1 枚の画像（DB に無くてもよい）の特徴量から、DB の画像のうち完全一致（大きさと SHA-256 が同じ）と
//   見た目一致（dHash の距離がしきい値以下・平均の色が近い）を探す。dHash は全部の画像をメモリに読んでおき、総当たりで比べる
//   （100 万枚で数 ms。読み直すのは画像の数が変わった時だけ）
// - インデックスの状態: フォルダの中の画像が DB にあるか（ハッシュまで済んでいるか）、フォルダが調べる対象（roots）に入っているか
#pragma once

#include <cstdint>
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

// インデックスの状態
enum class IndexState { None = 0, Pending = 1, Indexed = 2, Partial = 3 };

// フォルダ dir（区切りで終わらない）の直下のファイルの状態（名前 → Indexed（読めなかったものも）/ Pending。DB に無いものは入らない）。
// roots と大文字小文字だけが違う時は、roots の書き方に直して探す
std::unordered_map<std::string, IndexState> fileIndexStates(Db& db, const std::string& dir, const std::vector<std::string>& roots,
                                                            int hashVersion);
// フォルダの状態: roots の中なら Indexed、中に roots があれば Partial、どちらでもなければ None
IndexState folderIndexState(const std::string& dir, const std::vector<std::string>& roots);
// フォルダの下（いくつ下でも）の画像が DB にあるか（roots から外したフォルダや --index で作ったもの）
bool folderHasIndexed(Db& db, const std::string& dir);

}  // namespace imgdupe
