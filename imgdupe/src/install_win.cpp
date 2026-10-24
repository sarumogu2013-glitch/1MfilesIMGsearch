#include "install_win.hpp"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "win_util.hpp"

namespace install {

namespace {

constexpr const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\imgdupe";
constexpr const wchar_t* kShortcutName = L"imgdupe.lnk";

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

void setString(HKEY key, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
}

void setDword(HKEY key, const wchar_t* name, DWORD v) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
}

std::wstring getString(HKEY key, const wchar_t* name) {
    wchar_t buf[1024];
    DWORD size = sizeof buf, type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) != ERROR_SUCCESS || type != REG_SZ)
        return {};
    return std::wstring(buf, wcsnlen(buf, size / sizeof(wchar_t)));
}

bool makeShortcut(const std::wstring& exe, const std::wstring& lnk) {
    winutil::Com<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())))) return false;
    link->SetPath(exe.c_str());
    const size_t sep = exe.find_last_of(L'\\');
    if (sep != std::wstring::npos) link->SetWorkingDirectory(exe.substr(0, sep).c_str());
    link->SetDescription(L"重複画像検索・画像管理");
    winutil::Com<IPersistFile> file;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(file.put())))) return false;
    return SUCCEEDED(file->Save(lnk.c_str(), TRUE));
}

}  // namespace

std::wstring defaultDir() {
    const std::wstring base = knownFolder(FOLDERID_UserProgramFiles);  // %LOCALAPPDATA%\Programs
    if (!base.empty()) return base + L"\\imgdupe";
    return knownFolder(FOLDERID_LocalAppData) + L"\\Programs\\imgdupe";
}

std::wstring installedDir() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
    std::wstring dir = getString(key, L"InstallLocation");
    RegCloseKey(key);
    return dir;
}

void registerUninstall(const std::wstring& dir, const std::wstring& version, unsigned long estimatedKb) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    const std::wstring exe = dir + L"\\imgdupe.exe";
    setString(key, L"DisplayName", L"imgdupe（重複画像検索・画像管理）");
    setString(key, L"DisplayVersion", version);
    setString(key, L"Publisher", L"imgdupe");
    setString(key, L"InstallLocation", dir);
    setString(key, L"DisplayIcon", exe);
    setString(key, L"UninstallString", L"\"" + exe + L"\" --uninstall");
    setString(key, L"URLInfoAbout", L"https://github.com/sarumogu2013-glitch/1MfilesIMGsearch");
    setDword(key, L"NoModify", 1);
    setDword(key, L"NoRepair", 1);
    setDword(key, L"EstimatedSize", estimatedKb);
    RegCloseKey(key);
}

void unregisterUninstall() { RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey); }

void syncDisplayVersion(const std::wstring& exe, const std::wstring& version) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS) return;
    const std::wstring dir = getString(key, L"InstallLocation");
    if (!dir.empty() && lstrcmpiW((dir + L"\\imgdupe.exe").c_str(), exe.c_str()) == 0 && getString(key, L"DisplayVersion") != version)
        setString(key, L"DisplayVersion", version);
    RegCloseKey(key);
}

bool createShortcuts(const std::wstring& exe, bool startMenu, bool desktop) {
    bool ok = true;
    if (startMenu) ok &= makeShortcut(exe, knownFolder(FOLDERID_Programs) + L"\\" + kShortcutName);
    if (desktop) ok &= makeShortcut(exe, knownFolder(FOLDERID_Desktop) + L"\\" + kShortcutName);
    return ok;
}

void removeShortcuts() {
    DeleteFileW((knownFolder(FOLDERID_Programs) + L"\\" + kShortcutName).c_str());
    DeleteFileW((knownFolder(FOLDERID_Desktop) + L"\\" + kShortcutName).c_str());
}

}  // namespace install
