// 段階 names（仕様「4. メタデータ・名前解析」のうち json とフォルダ名）。
// - gallery-dl などが画像の横に書く json（"画像.jpg.json" か "画像.json"）から、作者・タグ・タイトル・投稿 ID を読む（出どころ meta、0.95）
// - 画像のあるフォルダの名前をタグにする（出どころ folder、0.70）。"[作者] 作品名" の形なら作者を取り出す
// OS に依存しない。
#pragma once

#include <string>
#include <vector>

#include "analyzer.hpp"

namespace imgdupe {

constexpr int kNamesVersion = 1;
constexpr const char* kStageNames = "names";
constexpr const char* kSourceMeta = "meta";
constexpr const char* kSourceFolder = "folder";
constexpr double kMetaTagConfidence = 0.95;
constexpr double kFolderTagConfidence = 0.70;

struct SidecarInfo {
    std::vector<TagRow> tags;  // 作者（kind artist）、キャラ、作品、一般タグ
    std::string title;
    std::string site, postId, authorId, url;
};

// json の中身を読む（gallery-dl の pixiv・twitter・danbooru 系などの形）。json として読めなければ false
bool parseSidecarJson(const std::string& json, SidecarInfo& out);
// 画像の横の json のパス（無ければ空）
std::string findSidecar(const std::string& imagePath);

// フォルダの名前から取れるタグ（よくある名前・日付・数字だけのものは使わない）
std::vector<TagRow> folderTags(const std::string& imagePath);

class NamesAnalyzer : public Analyzer {
public:
    std::string stageName() const override { return kStageNames; }
    int version() const override { return kNamesVersion; }
    AnalyzerOutput analyze(const AnalyzerInput& in) override;
    bool store(Db& db, int64_t imageId, AnalyzerOutput& out) override;
};

}  // namespace imgdupe
