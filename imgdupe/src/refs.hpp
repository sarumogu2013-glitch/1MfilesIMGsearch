// 見本画像（仕様「見本画像」「確定した結果を見本画像に追加し、判定精度を上げる」）。
// 見本: (1) 手動で「正しい」と確定したタグ（同じタグに 3 枚以上）、(2) <データのフォルダ>\refs\<名前>\ に置いた画像。
// 名前ごとに見本の CLIP のベクトルを平均して長さ 1 にし（代表のベクトル）、それに近い画像に、その名前のタグを付ける（出どころ reference）。
// OS に依存しない。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db.hpp"
#include "vectors.hpp"

namespace imgdupe {

constexpr const char* kSourceReference = "reference";
constexpr const char* kRefsDir = "refs";
constexpr size_t kMinManualReferences = 3;

struct RefSet {
    std::string tag, kind;
    std::vector<int64_t> ids;  // 見本の画像
};

// refsPrefix: "<dataDir>\refs\"（区切りまで。空ならフォルダの見本は使わない）
std::vector<RefSet> collectRefSets(Db& db, const std::string& refsPrefix);

// 代表のベクトルとの内積 → 確度（0.80 で 0.5、傾き 25）
double referenceProbability(double sim);

// 見本と CLIP のベクトルが変わったかを見るための値
std::string refStateHash(const std::vector<RefSet>& sets, size_t vectorRecords);

// 出どころ reference のタグを全部入れ替える（確度 0.2 以上）。代表のベクトルを作れた名前の数を返す
size_t applyReferences(Db& db, const VectorIndex& vecs, const std::vector<RefSet>& sets, unsigned threads = 0);

}  // namespace imgdupe
