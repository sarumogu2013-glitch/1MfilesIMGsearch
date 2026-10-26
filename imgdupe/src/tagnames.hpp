// タグの表示名（日本語）。タグそのもの（英語）は DB でもファイルでも変えず、画面に出す時だけ置き換える。
// - 同梱の訳（data/tags_ja.tsv。exe に埋め込む）は、内容が変わった時だけ DB の tag_names（出どころ builtin）に入れ直す
// - 自分で付けた名前（出どころ user）は同梱の訳より優先する
// - 検索では表示名でもそのタグとして探せる（search.cpp の aliasesOf・partialTags）
// OS に依存しない。
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "db.hpp"

namespace imgdupe {

constexpr const char* kTagNameBuiltin = "builtin";
constexpr const char* kTagNameUser = "user";

// 「タグ<TAB>表示名」の行（# で始まる行と空の行は飛ばす）
std::vector<std::pair<std::string, std::string>> parseTagNames(const std::string& text);
// 同梱の訳を入れる（前回入れたものと同じ内容なら何もしない。入れたら true）
bool installTagNames(Db& db, const std::string& text);
// タグ（ASCII を小文字にしたもの）→ 表示名。自分で付けたものが優先
std::unordered_map<std::string, std::string> loadTagNames(Db& db);
// 自分で付けた表示名（空なら消して、同梱の訳に戻す）
void setUserTagName(Db& db, const std::string& tag, const std::string& name);
// 表示名がそのまま合うタグ（同じ表示名のタグが複数あればすべて）
std::vector<std::string> tagsNamed(Db& db, const std::string& name);
// 表示名に語を含むタグ（多すぎる時は limit 個まで）
std::vector<std::string> tagsWithNameContaining(Db& db, const std::string& word, size_t limit);
// 表示名の表のキーの形（ASCII を小文字に）
std::string tagNameKey(const std::string& tag);

}  // namespace imgdupe
