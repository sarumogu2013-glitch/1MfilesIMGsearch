#include "update_logic.hpp"

#include "json.hpp"

#include <algorithm>
#include <cstdint>

namespace imgdupe {

Version parseVersion(const std::string& version) {
    long long parts[3] = {0, 0, 0};
    size_t i = 0;
    while (i < version.size() && !(version[i] >= '0' && version[i] <= '9')) ++i;  // "v" / "ver" を飛ばす
    for (int k = 0; k < 3 && i < version.size(); ++k) {
        size_t digits = 0;
        while (i < version.size() && version[i] >= '0' && version[i] <= '9') {
            if (digits++ < 15) parts[k] = parts[k] * 10 + (version[i] - '0');  // 桁あふれ防止
            ++i;
        }
        if (!digits || i >= version.size() || version[i] != '.') break;
        ++i;
    }
    return {parts[0], parts[1], parts[2]};
}

VersionCompare compareVersion(const std::string& current, const std::string& latestTag) {
    if (current == latestTag) return VersionCompare::Latest;
    const Version c = parseVersion(current), l = parseVersion(latestTag);
    const long long cv[3] = {c.major, c.minor, c.patch}, lv[3] = {l.major, l.minor, l.patch};
    for (int k = 0; k < 3; ++k) {
        if (lv[k] > cv[k]) return VersionCompare::RemoteNewer;
        if (lv[k] < cv[k]) return VersionCompare::LocalNewer;
    }
    return VersionCompare::Latest;
}

bool parseLatestRelease(const std::string& json, const std::string& assetName, ReleaseInfo& out) {
    Json root;
    if (!parseJson(json, root) || root.type != Json::Object) return false;
    out = ReleaseInfo{};
    out.tag = root.getString("tag_name");
    if (const Json* assets = root.get("assets"); assets && assets->type == Json::Array) {
        for (const Json& a : assets->arr) {
            if (a.getString("name") != assetName) continue;
            out.hasAsset = true;
            out.assetUrl = a.getString("url");
            const std::string digest = a.getString("digest");
            if (digest.compare(0, 7, "sha256:") == 0) {
                out.sha256 = digest.substr(7);
                std::transform(out.sha256.begin(), out.sha256.end(), out.sha256.begin(),
                               [](char ch) { return (ch >= 'A' && ch <= 'F') ? static_cast<char>(ch - 'A' + 'a') : ch; });
            }
            break;
        }
    }
    return !out.tag.empty();
}

std::string httpErrorMessage(int status) {
    if (status == 200) return {};
    if (status == 404) return "リリースが見つかりません";
    if (status == 403 || status == 429) return "GitHub に拒否されました（回数制限の可能性があります。しばらくしてから試してください）";
    return "GitHub の応答が異常です (HTTP " + std::to_string(status) + ")";
}

}  // namespace imgdupe
