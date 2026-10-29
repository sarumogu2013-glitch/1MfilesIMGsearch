// 段階 names（仕様「4. メタデータ・名前解析」のうち json とフォルダ名）。
// - gallery-dl などが画像の横に書く json（"画像.jpg.json" か "画像.json"）から、作者・タグ・タイトル・投稿 ID を読む（出どころ meta、0.95）
// - 画像のあるフォルダの名前をタグにする（出どころ folder、0.70）。
//   "[サークル名(作者名)]タイトル(イベント名・原作名)" の形（同人誌などでよくある形）なら、サークル・作者・タイトル・イベント・原作を
//   それぞれのタグにする（0.95。中のフォルダの画像には、上へたどって最初に見つかったこの形のフォルダのもの）
// OS に依存しない。
#pragma once

#include <string>
#include <vector>

#include "analyzer.hpp"

namespace imgdupe {

constexpr int kNamesVersion = 3;  // 2: サークル・作者・タイトル・イベント・原作を分け、本のページ（表紙・本文・背表紙）
                                  // 3: 作者・アカウントを読み取れる形のフォルダ名は、名前そのものをタグにしない
constexpr const char* kStageNames = "names";
constexpr const char* kSourceMeta = "meta";
constexpr const char* kSourceFolder = "folder";
constexpr double kMetaTagConfidence = 0.95;
constexpr double kFolderTagConfidence = 0.70;
constexpr double kDoujinTagConfidence = 0.95;  // 自分で付けた "[サークル(作者)]タイトル(イベント)" の形の名前

// "[サークル名(作者名)]タイトル(イベント名等)" を分けたもの
struct DoujinName {
    std::string circle;
    std::vector<std::string> authors;
    std::string title;
    std::vector<std::string> events;    // C102、例大祭 など
    std::vector<std::string> parodies;  // 原作名（東方Project など。イベントらしくない丸かっこ）
};
// 先頭の (イベント) は何個でも、次に [サークル(作者)] か [作者]（必須）、続いてタイトル、後ろの (…) は何個でも、[DL版] などは読み飛ばす。
// 全角の 【】［］（） も同じ。この形でなければ false
bool parseDoujinName(const std::string& name, DoujinName& out);
// イベントの名前らしいか（C102・コミケ・例大祭・COMIC1・コミティア・年の数字 など）
bool looksLikeEvent(const std::string& text);

// 本のページの役目（"[サークル(作者)]タイトル" の形のフォルダの直下の画像だけ）: 名前順（数字は数として。2 < 10）で
// 1 枚目は「表紙」、最後は「背表紙」、ほかは「本文」（1 枚だけなら表紙）。name が siblings（同じフォルダの画像の名前）に無ければ空
std::string pageRole(const std::string& name, const std::vector<std::string>& siblings);
// 名前の比べ方（大文字・小文字を区別せず、続いた数字は数として）。負なら a が先
int naturalCompare(const std::string& a, const std::string& b);

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
