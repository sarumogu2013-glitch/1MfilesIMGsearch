// アップデート確認のうち OS に依存しない部分（版の比較・GitHub API 応答の解析）。
#pragma once

#include <string>

namespace imgdupe {

struct Version {
    long long major = 0, minor = 0, patch = 0;
};

// "v1.2.3" / "1.2" / "ver0.3.10" を読む。数字が無ければ 0.0.0（"dev" → 0.0.0）。
Version parseVersion(const std::string& version);

enum class VersionCompare {
    Latest,       // タグと今の版が同じ
    RemoteNewer,  // 新しい版がある
    LocalNewer,   // 今の版の方が新しい（開発版など）
};

VersionCompare compareVersion(const std::string& current, const std::string& latestTag);

struct ReleaseInfo {
    std::string tag;        // tag_name
    bool hasAsset = false;  // assetName の添付ファイルがあるか
    std::string assetUrl;   // API の asset URL
    std::string sha256;     // digest が "sha256:..." の時の 16 進表記（小文字）。無ければ空
};

// releases/latest の JSON を解析する。JSON として読めなければ false。
bool parseLatestRelease(const std::string& json, const std::string& assetName, ReleaseInfo& out);

// HTTP 状態ごとのエラー文（UTF-8）。200 の時は空文字。
std::string httpErrorMessage(int status);

}  // namespace imgdupe
