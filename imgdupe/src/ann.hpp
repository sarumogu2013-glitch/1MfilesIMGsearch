// CLIP のベクトルの近似最近傍探索（仕様「似た画像の検索・差分グループ: hnswlib で索引を作る」）と差分グループ。
// 索引のベクトルは fp16 のまま持つ（100 万枚で約 1.2 GB）。新しい画像だけを足し、消えた画像は消した印を付ける。
// OS に依存しない。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core.hpp"
#include "db.hpp"
#include "vectors.hpp"

namespace imgdupe {

constexpr double kDefaultVariantSimilarity = 0.92;  // これ以上似ていれば差分（表情違い・服違いなど）とみなす

class AnnIndex {
public:
    AnnIndex();
    ~AnnIndex();
    AnnIndex(const AnnIndex&) = delete;
    AnnIndex& operator=(const AnnIndex&) = delete;

    // path（と path + ".meta"）を読む。版か次元が違う・無い時は空から作る
    bool open(const std::string& path, uint32_t dim, const std::string& version);
    bool save();
    size_t size() const;  // 消した印の付いていない数
    bool has(int64_t id) const;

    // vecs にあって索引に無い画像を足し、alive が false の画像に消した印を付ける。変わったら true
    bool sync(const VectorIndex& vecs, const std::function<bool(int64_t)>& alive, unsigned threads,
              const std::atomic<bool>& cancel, Progress* progress = nullptr);
    // fp16 のベクトルに近い順に k 個（似ている度合い = 内積）
    std::vector<std::pair<int64_t, float>> knn(const uint16_t* q, size_t k) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 差分グループ: お互いに近い k 個に入っていて、似ている度合いが minSim 以上の 2 枚をつなぐ（Union-Find）。
// 2 枚以上のまとまりを返す（似ている順ではなく、画像 ID の順）
std::vector<std::vector<int64_t>> variantGroups(const AnnIndex& ann, const VectorIndex& vecs, double minSim, size_t k,
                                                unsigned threads, const std::atomic<bool>& cancel, Progress* progress = nullptr);

// groups の kind 'variant' を入れ替える
void replaceVariantGroups(Db& db, const std::vector<std::vector<int64_t>>& groups);

}  // namespace imgdupe
