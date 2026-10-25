// 統計（仕様「統計画面」: 作者別・キャラ別の枚数、月ごとの増え方）。OS に依存しない。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db.hpp"

namespace imgdupe {

struct StatsSummary {
    int64_t images = 0, bytes = 0;
    int64_t exactGroups = 0, exactExtra = 0, exactExtraBytes = 0;     // 完全一致: グループの数、代表以外の枚数と大きさ（消せば空く量）
    int64_t similarGroups = 0, similarExtra = 0, similarExtraBytes = 0;
    int64_t tagged = 0;      // 検索用のタグ（どの出どころでも）が付いている画像
    int64_t withSource = 0;  // 出典（sources）が分かっている画像
    int64_t deadLinks = 0;   // 出典の投稿が消えている画像
    int64_t blurry = 0, soft = 0;  // 品質: ぼやけている・細かさが足りない（拡大された）画像
    struct Stage {
        std::string name;
        int64_t done = 0, failed = 0;
    };
    std::vector<Stage> stages;  // 段階ごとの済んだ数（版は問わない）
};
StatsSummary statsSummary(Db& db);

struct StatRow {
    std::string key;
    int64_t count = 0, bytes = 0;
};

// 種類（artist / character / copyright など。いくつか渡せばまとめて）ごとのタグの枚数（多い順）。
// どれかの出どころの確度が minConfidence 以上で、手動で「違う」と確定していない画像を数える。別名は元の名前にまとめる
std::vector<StatRow> tagCounts(Db& db, const std::vector<std::string>& kinds, double minConfidence, size_t limit);

// 月ごとの枚数（"2024-05" の古い順）。月は、ファイルの作成日時と更新日時の古い方（コピーすると作成日時が新しくなるので）
std::vector<StatRow> monthlyCounts(Db& db, bool localTime = true);

// 形式ごとの枚数（多い順。形式が分からなければ拡張子）
std::vector<StatRow> formatCounts(Db& db);

}  // namespace imgdupe
