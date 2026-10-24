// 類似検索用のベクトル（CLIP など）のファイル（仕様「100万枚への対応」: SQLite ではなく専用のファイルに置き、メモリマップで読む）。
// 形式: "IVEC" | 次元（u32） | 版の長さ（u32） | 版（UTF-8） | レコード…（画像 ID（i64） | fp16 × 次元）。
// 書く側（indexer）は後ろに足すだけ。同じ画像が何度も出てきたら最後のものを使う。読む側は大きさが変わったら読み直す。
// OS に依存しない（メモリマップは platform.hpp）。
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "platform.hpp"

namespace imgdupe {

uint16_t floatToHalf(float f);
float halfToFloat(uint16_t h);

// ベクトルを長さ 1 にする（0 ならそのまま）
void normalizeVector(std::vector<float>& v);

class VectorWriter {
public:
    ~VectorWriter() { close(); }
    // 版か次元が違うファイルは作り直す（モデルが変わったら全部やり直しなので）
    // created: 新しく作った（前のベクトルが無い）なら true
    bool open(const std::string& path, uint32_t dim, const std::string& version, bool* created = nullptr);
    bool append(int64_t imageId, const float* v);
    void flush();
    void close();

private:
    FILE* f_ = nullptr;
    uint32_t dim_ = 0;
    std::vector<uint16_t> buf_;
};

class VectorIndex {
public:
    // 開く（無い・壊れている時は false）。もう開いていて大きさが変わっていなければ何もしない
    bool load(const std::string& path);
    void close();
    uint32_t dim() const { return dim_; }
    const std::string& version() const { return version_; }
    size_t size() const { return ids_.size(); }      // 画像の数（同じ画像は 1 つ）
    size_t records() const { return records_; }     // ファイルの中のレコードの数（作り直しの目安）
    bool has(int64_t id) const { return pos_.count(id) != 0; }
    const std::vector<int64_t>& ids() const { return ids_; }  // 最初に出てきた順
    const uint16_t* raw(int64_t id) const;  // fp16 のまま（無ければ nullptr）。並びはそろっていないことがある
    bool get(int64_t id, std::vector<float>& out) const;
    // q（長さ 1）と内積の大きい順に k 個。accept が false を返す画像は除く
    std::vector<std::pair<int64_t, float>> topK(const float* q, size_t k, const std::function<bool(int64_t)>& accept = {},
                                                unsigned threads = 0) const;

private:
    const uint16_t* vec(size_t i) const;
    MappedFile map_;
    std::string path_;
    uint64_t mappedSize_ = 0;
    uint32_t dim_ = 0;
    std::string version_;
    size_t header_ = 0, records_ = 0;
    std::vector<int64_t> ids_;
    std::vector<uint64_t> offsets_;  // ids_ と同じ並び（レコードの先頭からの位置）
    std::unordered_map<int64_t, size_t> pos_;
};

// keep に入っている画像の最後のレコードだけを残して作り直す（消えた画像・古いレコードを捨てる）。成功したら true
bool compactVectors(const std::string& path, const std::function<bool(int64_t)>& keep);

}  // namespace imgdupe
