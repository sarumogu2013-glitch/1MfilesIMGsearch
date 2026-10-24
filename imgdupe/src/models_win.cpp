#include "models_win.hpp"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>

#include "update_win.hpp"
#include "version.hpp"
#include "win_util.hpp"

namespace models {

using winutil::narrow;
using winutil::widen;

namespace {

std::wstring join(const std::wstring& dir, const std::string& rel) {
    std::wstring r = widen(rel);
    for (auto& c : r)
        if (c == L'/') c = L'\\';
    return dir + L"\\" + r;
}

void makeParentDirs(const std::wstring& path) {
    const size_t sep = path.find_last_of(L'\\');
    if (sep != std::wstring::npos) SHCreateDirectoryExW(nullptr, path.substr(0, sep).c_str(), nullptr);
}

bool readFile(const std::wstring& path, std::string& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

}  // namespace

std::wstring manifestUrl() {
    wchar_t buf[2048];
    const DWORD n = GetEnvironmentVariableW(L"IMGDUPE_MODELS_URL", buf, 2048);
    if (n > 0 && n < 2048) return buf;
    return L"https://github.com/" APP_REPOSITORY L"/releases/latest/download/models.json";
}

ManifestResult fetchManifest() {
    ManifestResult r;
    upd::FetchResult f = upd::fetchText(manifestUrl());
    if (!f.ok) {
        r.error = L"モデルの一覧を取得できませんでした: " + f.error;
        return r;
    }
    if (!imgdupe::parseManifest(f.body, r.manifest)) {
        r.error = L"モデルの一覧（models.json）を読めませんでした";
        return r;
    }
    r.ok = true;
    return r;
}

std::wstring modelsDir(const std::wstring& dataDir) { return dataDir + L"\\" + widen(imgdupe::kModelsDir); }

imgdupe::Installed loadInstalled(const std::wstring& dataDir) {
    imgdupe::Installed in;
    std::string text;
    if (readFile(modelsDir(dataDir) + L"\\" + widen(imgdupe::kInstalledFile), text)) imgdupe::parseInstalled(text, in);
    return in;
}

bool saveInstalled(const std::wstring& dataDir, const imgdupe::Installed& in) {
    const std::wstring dir = modelsDir(dataDir);
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\" + widen(imgdupe::kInstalledFile);
    const std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const std::string text = imgdupe::installedToJson(in);
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring install(const std::wstring& dataDir, const std::vector<const imgdupe::Component*>& comps, const Progress& progress) {
    uint64_t total = 0, before = 0;
    for (const auto* c : comps) total += c->totalSize();
    const std::wstring dir = modelsDir(dataDir);
    imgdupe::Installed installed = loadInstalled(dataDir);
    for (const auto* c : comps) {
        const std::wstring name = widen(c->name);
        for (const auto& f : c->files) {
            const std::wstring path = join(dir, f.path);
            makeParentDirs(path);
            upd::DownloadResult r = upd::downloadFile(widen(f.url), path, f.sha256, f.size, [&](uint64_t got) {
                return !progress || progress(name, before + got, total);
            });
            if (!r.ok) return name + L": " + r.error;
            before += f.size;
        }
        // 前の版にしか無いファイルを消してから、入れた版を記録する
        auto it = installed.find(c->id);
        if (it != installed.end())
            for (const auto& stale : imgdupe::staleFiles(it->second, *c)) DeleteFileW(join(dir, stale).c_str());
        imgdupe::InstalledComponent ic;
        ic.version = c->version;
        for (const auto& f : c->files) ic.files.push_back(f.path);
        installed[c->id] = std::move(ic);
        if (!saveInstalled(dataDir, installed)) return L"入れた部品の記録（installed.json）を書けませんでした";
    }
    return {};
}

void removeAll(const std::wstring& dataDir) {
    const imgdupe::Installed installed = loadInstalled(dataDir);
    const std::wstring dir = modelsDir(dataDir);
    for (const auto& [id, c] : installed)
        for (const auto& f : c.files)
            if (imgdupe::safeRelativePath(f)) DeleteFileW(join(dir, f).c_str());
    DeleteFileW((dir + L"\\" + widen(imgdupe::kInstalledFile)).c_str());
    // 空になったフォルダ（models\wd14 など）も消す
    for (const auto& [id, c] : installed)
        for (const auto& f : c.files) {
            std::wstring p = join(dir, f);
            for (size_t sep; (sep = p.find_last_of(L'\\')) != std::wstring::npos && p.size() > dir.size();) {
                p.resize(sep);
                if (p.size() <= dir.size() || !RemoveDirectoryW(p.c_str())) break;
            }
        }
    RemoveDirectoryW(dir.c_str());
}

std::wstring sizeText(uint64_t bytes) {
    wchar_t buf[32];
    if (bytes >= 1000ull * 1000 * 1000) swprintf(buf, 32, L"%.1f GB", bytes / 1e9);
    else if (bytes >= 1000ull * 1000) swprintf(buf, 32, L"%.0f MB", bytes / 1e6);
    else swprintf(buf, 32, L"%.0f KB", bytes / 1e3);
    return buf;
}

}  // namespace models
