// タグ辞書（仕様「タグ辞書」）: 別名をまとめる、親子関係で自動付与する、Danbooru のタグ関係を取り込む。
// 関係は tag_relations（出どころ user = 自分で書いたもの / danbooru = 取り込んだもの）。検索が使う tag_dict の別名はここから作る。
// 親子関係は統合の段階で当てる（子のタグが付いた画像に親のタグを、出どころ parent で付ける）。
// OS に依存しない（Danbooru からの取得は呼ぶ側）。
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "db.hpp"

namespace imgdupe {

constexpr const char* kSourceParent = "parent";

struct TagRelation {
    std::string tag, other;
    std::string kind;  // "alias"（tag は other の別名）/ "parent"（tag が付けば other も）
};

// 自分で書く形: 「初音ミク = hatsune_miku」（別名）、「hatsune_miku > vocaloid」（親子）、# はコメント
bool parseTagRelations(const std::string& text, std::vector<TagRelation>& out, std::string& error);
std::string formatTagRelations(const std::vector<TagRelation>& rels);  // 上の形で書き出す（説明のコメント付き）

std::vector<TagRelation> loadRelations(Db& db, const std::string& source);
// source の関係を入れ替え、tag_dict の別名を作り直す
void replaceRelations(Db& db, const std::string& source, const std::vector<TagRelation>& rels);

// Danbooru の tag_aliases.json / tag_implications.json の 1 ページ（[{"antecedent_name":..,"consequent_name":..}]）
bool parseDanbooruPairs(const std::string& json, std::vector<std::pair<std::string, std::string>>& out);

// 親子関係を当てる（出どころ parent のタグを入れ替える。親の親までたどる。確度は子の中で一番高いもの）。付けたタグの数
size_t applyParents(Db& db, int depth = 3);

struct RelationCounts {
    size_t userAlias = 0, userParent = 0, danbooruAlias = 0, danbooruParent = 0;
};
RelationCounts relationCounts(Db& db);

}  // namespace imgdupe
