// imgdupe の Windows 用 GUI（Win32 API + WIC）。--cli を付けると画面を出さずに CSV / HTML を書き出す。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>

#include "core.hpp"
#include "image_win.hpp"
#include "resource.h"
#include "update_win.hpp"
#include "version.hpp"
#include "win_util.hpp"

using namespace imgdupe;
using winutil::Com;
using winutil::narrow;
using winutil::widen;

namespace {

// ---------------------------------------------------------------- 定数

constexpr UINT WM_APP_DONE = WM_APP + 1;   // lParam: Outcome*
constexpr UINT WM_APP_THUMB = WM_APP + 2;  // lParam: ThumbResult*
constexpr UINT WM_APP_CHECKED = WM_APP + 3;     // wParam: CheckMode, lParam: upd::CheckResult*
constexpr UINT WM_APP_DOWNLOADED = WM_APP + 4;  // wParam: CheckMode, lParam: upd::DownloadResult*
constexpr UINT_PTR kProgressTimer = 1;
constexpr UINT_PTR kExitTimer = 2;
constexpr UINT kExitCheckTimeoutMs = 20000;  // 終了時の確認を待つのはここまで

enum CheckMode : WPARAM { kManual = 0, kOnExit = 1 };

enum : int {
    IDC_FOLDERS = 100,
    IDC_ADD,
    IDC_REMOVE,
    IDC_THRESHOLD,
    IDC_THRESHOLD_LABEL,
    IDC_EXACT,
    IDC_FLAT,
    IDC_START,
    IDC_QUERY,
    IDC_EXPORT,
    IDC_PROGRESS,
    IDC_STATUS,
    IDC_GROUPS,
    IDC_THUMBS,
    IDC_INFO,
    IDC_FOLDERS_LABEL,
    IDC_THRESHOLD_CAPTION,
    IDC_TOOLBAR,
    ID_OPEN = 200,
    ID_EXPLORER,
    ID_COPY_PATH,
    ID_HELP,
    ID_RELEASES,
    ID_CHECK_UPDATE,
    ID_ABOUT,
    ID_EXIT,
};

constexpr int kDefaultThreshold = 10;
constexpr int kMaxThreshold = 20;

// ---------------------------------------------------------------- 状態

struct Outcome {
    Result result;
    bool query = false;
    std::wstring error;
};

struct ThumbResult {
    unsigned gen;
    int item;
    HBITMAP bmp;
};

struct App {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND folders = nullptr, add = nullptr, remove = nullptr, foldersLabel = nullptr;
    HWND thresholdCaption = nullptr, threshold = nullptr, thresholdLabel = nullptr, exact = nullptr, flat = nullptr;
    HWND toolbar = nullptr;
    HWND progress = nullptr, status = nullptr;
    HWND groups = nullptr, thumbs = nullptr, info = nullptr;
    HFONT font = nullptr;
    double scale = 1.0;

    std::wstring dataDir;  // 設定とキャッシュを置く場所

    // 検索
    std::thread worker;
    std::atomic<bool> cancel{false};
    bool running = false;
    bool runningQuery = false;
    Progress prog;
    ULONGLONG startTick = 0;

    // アップデート
    bool updating = false;                   // 確認・ダウンロード中
    bool closing = false;                    // 終了時のアップデート確認中（ウィンドウは隠してある）

    // 結果
    Result result;
    std::vector<std::string> resultRoots;  // result.files を作った時のフォルダ
    bool haveFiles = false;
    int curGroup = -1;

    // サムネイル
    int thumbSize = 160;
    HIMAGELIST images = nullptr;
    std::vector<int> imageOf;     // 項目 → 画像リストの番号（-1: まだ）
    std::vector<char> requested;  // 読み込みを頼んだか
    std::thread thumbThread;
    std::mutex thumbMutex;
    std::condition_variable thumbCv;
    std::deque<std::pair<int, std::wstring>> thumbQueue;  // (項目, パス)
    unsigned thumbGen = 0;
    bool thumbQuit = false;
};

App app;

int px(int v) { return static_cast<int>(v * app.scale + 0.5); }

// ---------------------------------------------------------------- ツールバー

int stopImage = 0, startImage = 0;  // 検索開始ボタンの絵（検索中は中止の絵にする）

void tbEnable(int id, bool on) { SendMessageW(app.toolbar, TB_ENABLEBUTTON, id, MAKELPARAM(on ? TRUE : FALSE, 0)); }

void tbSetText(int id, const wchar_t* text) {
    TBBUTTONINFOW bi{sizeof(bi)};
    bi.dwMask = TBIF_TEXT;
    bi.pszText = const_cast<wchar_t*>(text);
    SendMessageW(app.toolbar, TB_SETBUTTONINFOW, id, reinterpret_cast<LPARAM>(&bi));
}

// ---------------------------------------------------------------- 設定

std::wstring exeDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/"));
}

// exe の隣に書ければそこ（持ち運べる）。書けなければ %LOCALAPPDATA%\imgdupe
std::wstring findDataDir() {
    const std::wstring dir = exeDir();
    const std::wstring probe = dir + L"\\imgdupe.write-test.tmp";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return dir;
    }
    PWSTR local = nullptr;
    std::wstring out = dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        out = std::wstring(local) + L"\\imgdupe";
        CreateDirectoryW(out.c_str(), nullptr);
    }
    CoTaskMemFree(local);
    return out;
}

std::wstring iniPath() { return app.dataDir + L"\\imgdupe.ini"; }
std::wstring cachePath() { return app.dataDir + L"\\imgdupe_cache.bin"; }

int iniInt(const wchar_t* key, int def) { return GetPrivateProfileIntW(L"imgdupe", key, def, iniPath().c_str()); }
void iniSet(const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(L"imgdupe", key, v.c_str(), iniPath().c_str());
}

std::vector<std::wstring> folderList() {
    std::vector<std::wstring> out;
    const int n = ListBox_GetCount(app.folders);
    for (int i = 0; i < n; ++i) {
        std::wstring s(ListBox_GetTextLen(app.folders, i), L'\0');
        ListBox_GetText(app.folders, i, &s[0]);
        out.push_back(s.c_str());
    }
    return out;
}

void saveSettings() {
    const auto f = folderList();
    WritePrivateProfileSectionW(L"folders", L"\0", iniPath().c_str());
    for (size_t i = 0; i < f.size(); ++i)
        WritePrivateProfileStringW(L"folders", std::to_wstring(i + 1).c_str(), f[i].c_str(), iniPath().c_str());
    iniSet(L"threshold", std::to_wstring(SendMessageW(app.threshold, TBM_GETPOS, 0, 0)));
    iniSet(L"exactOnly", Button_GetCheck(app.exact) == BST_CHECKED ? L"1" : L"0");
    iniSet(L"includeFlat", Button_GetCheck(app.flat) == BST_CHECKED ? L"1" : L"0");
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (GetWindowPlacement(app.hwnd, &wp)) {
        const RECT& r = wp.rcNormalPosition;
        iniSet(L"window", std::to_wstring(r.left) + L"," + std::to_wstring(r.top) + L"," + std::to_wstring(r.right) +
                              L"," + std::to_wstring(r.bottom) + L"," +
                              std::to_wstring(wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0));
    }
}

std::vector<std::wstring> loadFolders() {
    std::vector<std::wstring> out;
    for (int i = 1;; ++i) {
        wchar_t buf[4096];
        GetPrivateProfileStringW(L"folders", std::to_wstring(i).c_str(), L"", buf, 4096, iniPath().c_str());
        if (!buf[0]) break;
        out.emplace_back(buf);
    }
    return out;
}

// ---------------------------------------------------------------- 小物

std::wstring fileName(const std::string& path) {
    const std::wstring w = widen(path);
    const size_t p = w.find_last_of(L"\\/");
    return p == std::wstring::npos ? w : w.substr(p + 1);
}

std::wstring parentDir(const std::string& path) {
    const std::wstring w = widen(path);
    const size_t p = w.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"" : w.substr(0, p);
}

std::wstring withCommas(uint64_t v) {
    std::wstring s = std::to_wstring(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), L",");
    return s;
}

std::wstring sizeText(uint64_t bytes) { return widen(formatSize(bytes)); }

std::wstring thresholdText(int t) {
    const wchar_t* word = t <= 4 ? L"ほぼ同じ" : t <= 10 ? L"標準" : t <= 15 ? L"ゆるい" : L"かなりゆるい";
    return std::to_wstring(t) + L"（" + word + L"）";
}

void setText(HWND h, const std::wstring& s) { SetWindowTextW(h, s.c_str()); }

std::wstring stripTrailingSlash(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

void addFolder(const std::wstring& raw) {
    const std::wstring p = stripTrailingSlash(raw);
    const DWORD attr = GetFileAttributesW(p.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;
    for (const auto& f : folderList())
        if (lstrcmpiW(f.c_str(), p.c_str()) == 0) return;
    ListBox_AddString(app.folders, p.c_str());
}

void pickFolder() {
    Com<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.put()))))
        return;
    DWORD opt = 0;
    dlg->GetOptions(&opt);
    dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT);
    dlg->SetTitle(L"検索するフォルダを選ぶ");
    if (FAILED(dlg->Show(app.hwnd))) return;
    Com<IShellItemArray> items;
    if (FAILED(dlg->GetResults(items.put()))) return;
    DWORD n = 0;
    items->GetCount(&n);
    for (DWORD i = 0; i < n; ++i) {
        Com<IShellItem> item;
        PWSTR p = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            addFolder(p);
            CoTaskMemFree(p);
        }
    }
}

std::wstring pickImage() {
    Com<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.put()))))
        return L"";
    const COMDLG_FILTERSPEC types[] = {
        {L"画像", L"*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.bmp;*.dib;*.gif;*.tif;*.tiff;*.webp;*.heic;*.heif;*.avif;*.jxr;*.wdp"},
        {L"すべてのファイル", L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(L"この画像に似た画像を探す");
    if (FAILED(dlg->Show(app.hwnd))) return L"";
    Com<IShellItem> item;
    PWSTR p = nullptr;
    if (FAILED(dlg->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return L"";
    std::wstring out(p);
    CoTaskMemFree(p);
    return out;
}

// 拡張子なしの保存先を選ぶ（.csv と .html を書く）
std::wstring pickSaveBase() {
    Com<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.put()))))
        return L"";
    const COMDLG_FILTERSPEC types[] = {{L"CSV と HTML", L"*.csv"}};
    dlg->SetFileTypes(1, types);
    dlg->SetDefaultExtension(L"csv");
    dlg->SetFileName(L"imgdupe_result");
    dlg->SetTitle(L"結果を保存（CSV と、サムネイル付きの HTML を書き出します）");
    if (FAILED(dlg->Show(app.hwnd))) return L"";
    Com<IShellItem> item;
    PWSTR p = nullptr;
    if (FAILED(dlg->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return L"";
    std::wstring out(p);
    CoTaskMemFree(p);
    const size_t dot = out.find_last_of(L'.');
    const size_t sep = out.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (sep == std::wstring::npos || dot > sep)) out.resize(dot);
    return out;
}

// ---------------------------------------------------------------- サムネイル

void thumbLoop() {
    imgwin::threadBegin();
    for (;;) {
        std::pair<int, std::wstring> job;
        unsigned gen;
        {
            std::unique_lock<std::mutex> lock(app.thumbMutex);
            app.thumbCv.wait(lock, [] { return app.thumbQuit || !app.thumbQueue.empty(); });
            if (app.thumbQuit) break;
            job = std::move(app.thumbQueue.front());
            app.thumbQueue.pop_front();
            gen = app.thumbGen;
        }
        HBITMAP bmp = imgwin::loadThumbnail(job.second, app.thumbSize, GetSysColor(COLOR_WINDOW));
        auto* r = new ThumbResult{gen, job.first, bmp};
        if (!PostMessageW(app.hwnd, WM_APP_THUMB, 0, reinterpret_cast<LPARAM>(r))) {
            if (bmp) DeleteObject(bmp);
            delete r;
        }
    }
    imgwin::threadEnd();
}

HBITMAP solidBitmap(int size, COLORREF fill, COLORREF frame) {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, size, size);
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT r{0, 0, size, size};
    HBRUSH b = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
    FillRect(dc, &r, b);
    DeleteObject(b);
    InflateRect(&r, -size / 8, -size / 8);
    b = CreateSolidBrush(fill);
    FillRect(dc, &r, b);
    DeleteObject(b);
    b = CreateSolidBrush(frame);
    FrameRect(dc, &r, b);
    DeleteObject(b);
    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return bmp;
}

// 画像リストを空にして、0: 読み込み中, 1: 読めない を入れ直す
void resetImages() {
    if (!app.images) {
        app.images = ImageList_Create(app.thumbSize, app.thumbSize, ILC_COLOR32, 16, 64);
        ListView_SetImageList(app.thumbs, app.images, LVSIL_NORMAL);
    }
    ImageList_RemoveAll(app.images);
    HBITMAP loading = solidBitmap(app.thumbSize, RGB(236, 238, 242), RGB(210, 214, 220));
    HBITMAP broken = solidBitmap(app.thumbSize, RGB(250, 225, 225), RGB(220, 120, 120));
    ImageList_Add(app.images, loading, nullptr);
    ImageList_Add(app.images, broken, nullptr);
    DeleteObject(loading);
    DeleteObject(broken);
}

const Group* currentGroup() {
    if (app.curGroup < 0 || app.curGroup >= static_cast<int>(app.result.groups.size())) return nullptr;
    return &app.result.groups[app.curGroup];
}

const Entry* memberEntry(int item, uint8_t* dist = nullptr) {
    const Group* g = currentGroup();
    if (!g || item < 0 || item >= static_cast<int>(g->members.size())) return nullptr;
    if (dist) *dist = g->members[item].dist;
    return &app.result.files[g->members[item].index];
}

void showInfo(int item) {
    uint8_t dist = 0;
    const Entry* e = memberEntry(item, &dist);
    for (int id : {ID_OPEN, ID_EXPLORER, ID_COPY_PATH}) tbEnable(id, e != nullptr);
    if (!e) {
        const Group* g = currentGroup();
        setText(app.info, g ? L"画像をダブルクリックすると開きます。右クリックで「エクスプローラーで表示」などができます。" : L"");
        return;
    }
    setText(app.info, widen(e->path) + L"\r\n" + std::to_wstring(e->f.width) + L" × " + std::to_wstring(e->f.height) +
                          L"　" + sizeText(e->size) + L"　距離 " + std::to_wstring(dist));
}

void selectGroup(int g) {
    app.curGroup = g;
    {
        std::lock_guard<std::mutex> lock(app.thumbMutex);
        ++app.thumbGen;
        app.thumbQueue.clear();
    }
    const Group* grp = currentGroup();
    const int n = grp ? static_cast<int>(grp->members.size()) : 0;
    app.imageOf.assign(n, -1);
    app.requested.assign(n, 0);
    resetImages();
    ListView_SetItemCountEx(app.thumbs, n, 0);
    ListView_SetItemState(app.thumbs, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(app.thumbs, 0, FALSE);
    InvalidateRect(app.thumbs, nullptr, TRUE);
    showInfo(-1);
}

void requestThumb(int item) {
    if (item < 0 || item >= static_cast<int>(app.requested.size()) || app.requested[item]) return;
    const Entry* e = memberEntry(item);
    if (!e) return;
    app.requested[item] = 1;
    {
        std::lock_guard<std::mutex> lock(app.thumbMutex);
        app.thumbQueue.emplace_front(item, widen(e->path));  // 後から頼んだ（今見えている）ものを先に
    }
    app.thumbCv.notify_one();
}

// ---------------------------------------------------------------- 結果の表示

void showResult(Result&& r, bool query) {
    app.result = std::move(r);
    ListView_SetItemCountEx(app.groups, static_cast<int>(app.result.groups.size()), 0);
    InvalidateRect(app.groups, nullptr, TRUE);
    selectGroup(-1);
    if (!app.result.groups.empty()) {
        ListView_SetItemState(app.groups, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(app.groups, 0, FALSE);
    }
    tbEnable(IDC_EXPORT, !app.result.groups.empty());

    std::wstring s;
    if (query) {
        const size_t hits = app.result.groups.empty() ? 0 : app.result.groups[0].members.size();
        s = L"似た画像 " + withCommas(hits) + L" 枚（" + withCommas(app.result.files.size()) + L" 枚の中から）";
    } else {
        size_t exact = 0, similar = 0;
        for (const auto& g : app.result.groups) (g.kind == Kind::Exact ? exact : similar)++;
        uint64_t reclaim = 0;
        for (const auto& g : app.result.groups) reclaim += g.reclaim;
        s = L"画像 " + withCommas(app.result.files.size()) + L" 枚: 完全一致 " + withCommas(exact) + L" グループ・見た目一致 " +
            withCommas(similar) + L" グループ（重複を消せば約 " + sizeText(reclaim) + L" 空きます）";
        wchar_t t[96];
        if (app.result.hashed)
            swprintf(t, 96, L"　新しく読み込んだ画像 %llu 枚（%.0f 秒）", static_cast<unsigned long long>(app.result.hashed),
                     app.result.secHash);
        else
            swprintf(t, 96, L"　前回の読み込み結果を使用");
        s += t;
    }
    if (app.result.unreadable) s += L"　読めない画像 " + withCommas(app.result.unreadable) + L" 枚";
    if (app.result.flatSkipped) s += L"　単色に近いため比べなかった画像 " + withCommas(app.result.flatSkipped) + L" 枚";
    setText(app.status, s);
}

// ---------------------------------------------------------------- 検索

void setRunning(bool on) {
    app.running = on;
    tbSetText(IDC_START, on ? L"検索を中止" : L"検索開始");
    SendMessageW(app.toolbar, TB_CHANGEBITMAP, IDC_START, MAKELPARAM(on ? stopImage : startImage, 0));
    for (HWND h : {app.add, app.remove, app.folders, app.threshold, app.exact, app.flat}) EnableWindow(h, !on);
    for (int id : {IDC_ADD, IDC_QUERY}) tbEnable(id, !on);
    tbEnable(IDC_EXPORT, !on && !app.result.groups.empty());
    SendMessageW(app.progress, PBM_SETPOS, 0, 0);
    if (on) {
        app.startTick = GetTickCount64();
        SetTimer(app.hwnd, kProgressTimer, 200, nullptr);
    } else {
        KillTimer(app.hwnd, kProgressTimer);
    }
}

void startSearch(const std::wstring& queryPath) {
    const auto folders = folderList();
    if (folders.empty()) {
        MessageBoxW(app.hwnd, L"検索するフォルダを追加してください（ウィンドウにドラッグ＆ドロップしても追加できます）。",
                    L"imgdupe", MB_ICONINFORMATION);
        return;
    }
    saveSettings();
    if (app.worker.joinable()) app.worker.join();

    RunOptions opt;
    for (const auto& f : folders) opt.roots.push_back(narrow(f));
    opt.threshold = static_cast<int>(SendMessageW(app.threshold, TBM_GETPOS, 0, 0));
    opt.exactOnly = Button_GetCheck(app.exact) == BST_CHECKED;
    opt.includeFlat = Button_GetCheck(app.flat) == BST_CHECKED;
    opt.cacheFile = narrow(cachePath());
    const bool query = !queryPath.empty();
    opt.featuresOnly = query;

    // 同じフォルダでもう読み込んであれば、1 枚で検索する時は走査を省く
    Result previous;
    const bool reuse = query && app.haveFiles && app.resultRoots == opt.roots;
    if (reuse) previous.files = app.result.files;

    app.cancel = false;
    app.prog.stage = static_cast<int>(Stage::Scan);
    app.prog.done = 0;
    app.prog.total = 0;
    app.runningQuery = query;
    setRunning(true);

    app.worker = std::thread([opt, queryPath, query, reuse, previous = std::move(previous)]() mutable {
        auto* out = new Outcome;
        out->query = query;
        try {
            Callbacks cb;
            cb.scan = [](const std::string& root, std::vector<ScannedFile>& files) {
                imgwin::scanFolder(widen(root), files, app.prog.total, app.cancel);
            };
            cb.decode = [](const std::string& path) { return imgwin::decodeFeatures(widen(path)); };
            cb.threadInit = imgwin::threadBegin;
            cb.threadExit = imgwin::threadEnd;
            Result r = reuse ? std::move(previous) : run(opt, cb, app.prog, app.cancel);
            if (query && !r.cancelled) {
                imgwin::threadBegin();
                const Features q = imgwin::decodeFeatures(queryPath);
                imgwin::threadEnd();
                if (!q.ok) {
                    out->error = L"画像を読めませんでした: " + queryPath;
                } else {
                    r.groups.clear();
                    Group g = imgdupe::query(r, q, opt.threshold, opt.colorTolerance);
                    if (!g.members.empty()) r.groups.push_back(std::move(g));
                }
            }
            out->result = std::move(r);
        } catch (const std::exception& e) {
            out->error = L"エラー: " + widen(e.what());
        }
        PostMessageW(app.hwnd, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(out));
    });
}

void updateProgress() {
    const auto stage = static_cast<Stage>(app.prog.stage.load());
    const uint64_t done = app.prog.done, total = app.prog.total;
    const double sec = (GetTickCount64() - app.startTick) / 1000.0;
    std::wstring s;
    int pos = 0;
    switch (stage) {
        case Stage::Scan:
            s = L"フォルダを調べています… 画像 " + withCommas(total) + L" 枚";
            SendMessageW(app.progress, PBM_SETMARQUEE, TRUE, 30);
            break;
        case Stage::Hash: {
            static ULONGLONG t0 = 0;
            static uint64_t d0 = 0;
            static double rate = 0;
            const ULONGLONG now = GetTickCount64();
            if (done < d0 || now - t0 > 1000) {
                if (t0 && done >= d0 && now > t0) rate = (done - d0) * 1000.0 / (now - t0);
                t0 = now;
                d0 = done;
            }
            s = L"画像を読み込んでいます " + withCommas(done) + L" / " + withCommas(total) + L" 枚";
            if (rate > 0 && done < total) {
                wchar_t t[96];
                const double left = (total - done) / rate;
                swprintf(t, 96, L"（%.0f 枚/秒・残り約 %.0f 分）", rate, left / 60 + 0.5);
                s += t;
            }
            pos = total ? static_cast<int>(done * 1000 / total) : 0;
            break;
        }
        case Stage::Exact:
            s = L"大きさが同じファイルの中身を比べています " + withCommas(done) + L" / " + withCommas(total);
            pos = total ? static_cast<int>(done * 1000 / total) : 0;
            break;
        case Stage::Similar:
            s = L"見た目を比べています…";
            pos = static_cast<int>(done);
            break;
        case Stage::Done:
            s = L"まとめています…";
            pos = 1000;
            break;
    }
    if (stage != Stage::Scan) {
        LONG_PTR style = GetWindowLongPtrW(app.progress, GWL_STYLE);
        if (style & PBS_MARQUEE) {
            SendMessageW(app.progress, PBM_SETMARQUEE, FALSE, 0);
            SetWindowLongPtrW(app.progress, GWL_STYLE, style & ~PBS_MARQUEE);
            SendMessageW(app.progress, PBM_SETRANGE32, 0, 1000);
        }
        SendMessageW(app.progress, PBM_SETPOS, pos, 0);
    } else {
        LONG_PTR style = GetWindowLongPtrW(app.progress, GWL_STYLE);
        if (!(style & PBS_MARQUEE)) {
            SetWindowLongPtrW(app.progress, GWL_STYLE, style | PBS_MARQUEE);
            SendMessageW(app.progress, PBM_SETMARQUEE, TRUE, 30);
        }
    }
    wchar_t t[32];
    swprintf(t, 32, L"　%d:%02d", static_cast<int>(sec) / 60, static_cast<int>(sec) % 60);
    setText(app.status, s + t);
}

void onDone(Outcome* out) {
    std::unique_ptr<Outcome> o(out);
    if (app.worker.joinable()) app.worker.join();
    LONG_PTR style = GetWindowLongPtrW(app.progress, GWL_STYLE);
    if (style & PBS_MARQUEE) {
        SendMessageW(app.progress, PBM_SETMARQUEE, FALSE, 0);
        SetWindowLongPtrW(app.progress, GWL_STYLE, style & ~PBS_MARQUEE);
        SendMessageW(app.progress, PBM_SETRANGE32, 0, 1000);
    }
    setRunning(false);
    if (!o->error.empty()) {
        setText(app.status, o->error);
        MessageBoxW(app.hwnd, o->error.c_str(), L"imgdupe", MB_ICONWARNING);
        return;
    }
    if (o->result.cancelled) {
        setText(app.status, L"中止しました（読み込んだ分はキャッシュに残したので、次はその続きから始まります）");
        return;
    }
    app.resultRoots.clear();
    for (const auto& f : folderList()) app.resultRoots.push_back(narrow(f));
    app.haveFiles = true;
    showResult(std::move(o->result), o->query);
}

// ---------------------------------------------------------------- アップデート

std::wstring today() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[16];
    swprintf(buf, 16, L"%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    return buf;
}

void startCheck(CheckMode mode) {
    if (app.updating) return;
    app.updating = true;
    tbEnable(ID_CHECK_UPDATE, false);
    HWND hwnd = app.hwnd;
    std::thread([hwnd, mode] {
        auto* r = new upd::CheckResult(upd::checkLatest());
        if (!PostMessageW(hwnd, WM_APP_CHECKED, mode, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void startDownload(CheckMode mode, const imgdupe::ReleaseInfo& info) {
    app.updating = true;
    tbEnable(ID_CHECK_UPDATE, false);
    setText(app.status, L"新しい版 " + widen(info.tag) + L" をダウンロードしています…");
    HWND hwnd = app.hwnd;
    std::thread([hwnd, mode, info] {
        auto* r = new upd::DownloadResult(upd::downloadAndReplace(info));
        if (!PostMessageW(hwnd, WM_APP_DOWNLOADED, mode, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

// 終了する時、その日初めてならアップデートを確認する（imgdupe.ini の autoUpdateCheck=0 で止められる）
bool shouldCheckOnExit() {
    wchar_t last[32];
    GetPrivateProfileStringW(L"imgdupe", L"lastUpdateCheck", L"", last, 32, iniPath().c_str());
    return iniInt(L"autoUpdateCheck", 1) && today() != last && std::string(IMGDUPE_VERSION) != "dev";
}

void onChecked(CheckMode mode, const upd::CheckResult& r) {
    app.updating = false;
    tbEnable(ID_CHECK_UPDATE, true);
    const std::wstring err = r.error();
    const auto cmp = err.empty() ? compareVersion(IMGDUPE_VERSION, r.info.tag) : VersionCompare::Latest;
    const std::wstring cur = widen(IMGDUPE_VERSION);
    const std::wstring versions = L"新しい版: " + widen(r.info.tag) + L"\r\n今の版: " + cur;
    if (mode == kOnExit) {
        // 新しい版がある時だけ聞く。確認できなかった時は何も言わずに終わる
        KillTimer(app.hwnd, kExitTimer);
        if (err.empty() && cmp == VersionCompare::RemoteNewer &&
            MessageBoxW(nullptr,
                        (L"新しい版があります。\r\n\r\n" + versions +
                         L"\r\n\r\n終了する前に更新しますか？\r\n（次に起動した時から新しい版になります。設定とキャッシュはそのまま使えます）")
                            .c_str(),
                        L"imgdupe のアップデート", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES) {
            startDownload(kOnExit, r.info);
            return;
        }
        DestroyWindow(app.hwnd);
        return;
    }
    if (!err.empty()) {
        MessageBoxW(app.hwnd, (L"アップデートを確認できませんでした。\r\n\r\n" + err + L"\r\n\r\n今の版: " + cur).c_str(),
                    L"imgdupe のアップデート", MB_ICONWARNING);
        return;
    }
    if (cmp != VersionCompare::RemoteNewer) {
        MessageBoxW(app.hwnd,
                    (L"最新の版です。\r\n\r\n今の版: " + cur + L"\r\n最新のリリース: " + widen(r.info.tag)).c_str(),
                    L"imgdupe のアップデート", MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxW(app.hwnd,
                    (L"新しい版があります。\r\n\r\n" + versions +
                     L"\r\n\r\n更新して再起動しますか？\r\n（設定とキャッシュはそのまま使えます）")
                        .c_str(),
                    L"imgdupe のアップデート", MB_YESNO | MB_ICONQUESTION) == IDYES)
        startDownload(kManual, r.info);
}

void onDownloaded(CheckMode mode, const upd::DownloadResult& r) {
    app.updating = false;
    tbEnable(ID_CHECK_UPDATE, true);
    if (mode == kOnExit) {
        if (!r.ok)
            MessageBoxW(nullptr, (L"更新できませんでした。\r\n\r\n" + r.error).c_str(), L"imgdupe のアップデート",
                        MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        DestroyWindow(app.hwnd);
        return;
    }
    if (!r.ok) {
        setText(app.status, L"更新できませんでした");
        MessageBoxW(app.hwnd, (L"更新できませんでした。\r\n\r\n" + r.error).c_str(), L"imgdupe のアップデート",
                    MB_ICONWARNING);
        return;
    }
    // 新しい exe を起動して、今のウィンドウを閉じる
    saveSettings();
    std::wstring cmd = L"\"" + upd::exePath() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        MessageBoxW(app.hwnd, L"更新しました。次に起動した時から新しい版になります。", L"imgdupe のアップデート",
                    MB_ICONINFORMATION);
    }
    DestroyWindow(app.hwnd);
}

// ---------------------------------------------------------------- 操作

void openItem(int item) {
    const Entry* e = memberEntry(item);
    if (e) ShellExecuteW(app.hwnd, L"open", widen(e->path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void showInExplorer(int item) {
    const Entry* e = memberEntry(item);
    if (!e) return;
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(widen(e->path).c_str());
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

void copyPath(int item) {
    const Entry* e = memberEntry(item);
    if (!e || !OpenClipboard(app.hwnd)) return;
    const std::wstring w = widen(e->path);
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (mem) {
        std::memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

void exportResult() {
    if (app.result.groups.empty()) return;
    const std::wstring base = pickSaveBase();
    if (base.empty()) return;
    const bool ok = writeCsv(app.result, narrow(base + L".csv")) && writeHtml(app.result, narrow(base + L".html"));
    if (!ok) {
        MessageBoxW(app.hwnd, L"保存できませんでした。", L"imgdupe", MB_ICONWARNING);
        return;
    }
    if (MessageBoxW(app.hwnd,
                    (L"保存しました。\r\n" + base + L".csv\r\n" + base +
                     L".html\r\n\r\nHTML（サムネイル付きの一覧）をブラウザで開きますか？")
                        .c_str(),
                    L"imgdupe", MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(app.hwnd, L"open", (base + L".html").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void showHelp() {
    MessageBoxW(app.hwnd,
                L"1. 「フォルダを追加」で検索するフォルダを選びます（ウィンドウへのドラッグ＆ドロップでも追加できます。複数可）。\r\n"
                L"2. 「検索開始」を押すと、同じ画像のまとまり（グループ）を左に出します。\r\n"
                L"　・完全一致: 中身がまったく同じファイル\r\n"
                L"　・見た目一致: 縮小・再圧縮・形式の違い（JPEG と PNG など）があっても見た目が同じ画像\r\n"
                L"3. グループを選ぶと、右にサムネイルを出します。ダブルクリックで開き、右クリックで「エクスプローラーで表示」などができます。\r\n"
                L"4. 「結果を保存」で CSV と、サムネイル付きの HTML を書き出します。\r\n\r\n"
                L"・しきい値: 小さいほど厳しく（ほぼ同じものだけ）、大きいほどゆるく（似ているものまで）まとめます。\r\n"
                L"・単色に近い画像（真っ白・真っ黒など）は見分けにくく、別の画像がまとまりやすいので、ふだんは見た目では比べません。\r\n"
                L"・「1 枚で検索」で、選んだ画像に似た画像を探します。\r\n"
                L"・2 回目からは読み込んだ結果（キャッシュ）を使うので、増えた・変わった画像だけを読み込みます。\r\n"
                L"・このツールは画像を読むだけで、移動や削除はしません。",
                L"imgdupe の使い方", MB_ICONINFORMATION);
}

// ---------------------------------------------------------------- 画面

void layout() {
    RECT rc;
    GetClientRect(app.hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int m = px(8), btnW = px(150), btnH = px(28), rowH = px(26);
    SendMessageW(app.toolbar, TB_AUTOSIZE, 0, 0);
    RECT tr;
    GetWindowRect(app.toolbar, &tr);
    int y = (tr.bottom - tr.top) + px(4);
    MoveWindow(app.foldersLabel, m, y, W - 2 * m, px(18), TRUE);
    y += px(20);
    const int listH = px(70);
    MoveWindow(app.folders, m, y, W - 3 * m - btnW, listH, TRUE);
    MoveWindow(app.add, W - m - btnW, y, btnW, btnH, TRUE);
    MoveWindow(app.remove, W - m - btnW, y + btnH + px(4), btnW, btnH, TRUE);
    y += listH + px(8);

    int x = m;
    MoveWindow(app.thresholdCaption, x, y + px(5), px(64), px(18), TRUE);
    x += px(66);
    MoveWindow(app.threshold, x, y, px(170), rowH + px(4), TRUE);
    x += px(172);
    MoveWindow(app.thresholdLabel, x, y + px(5), px(110), px(18), TRUE);
    x += px(112);
    MoveWindow(app.exact, x, y + px(2), px(110), rowH, TRUE);
    x += px(114);
    MoveWindow(app.flat, x, y + px(2), px(200), rowH, TRUE);
    y += rowH + px(10);

    MoveWindow(app.progress, m, y + px(3), px(220), px(16), TRUE);
    MoveWindow(app.status, m + px(228), y + px(1), W - 2 * m - px(228), px(20), TRUE);
    y += px(26);

    const int infoH = px(40);
    const int listW = std::max(px(260), (W - 3 * m) * 2 / 5);
    const int bottom = H - m - infoH - px(4);
    MoveWindow(app.groups, m, y, listW, std::max(0, bottom - y), TRUE);
    MoveWindow(app.thumbs, 2 * m + listW, y, std::max(0, W - 3 * m - listW), std::max(0, bottom - y), TRUE);
    MoveWindow(app.info, m, bottom + px(4), W - 2 * m, infoH, TRUE);
}

HWND makeControl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0) {
    HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, app.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.inst, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
    return h;
}

void addColumn(HWND lv, int i, const wchar_t* title, int width, int fmt = LVCFMT_LEFT) {
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    c.fmt = fmt;
    c.cx = px(width);
    c.pszText = const_cast<wchar_t*>(title);
    ListView_InsertColumn(lv, i, &c);
}

bool fontExists(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    bool found = false;
    EnumFontFamiliesExW(
        dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
            *reinterpret_cast<bool*>(p) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

// 記号フォントの文字を、アルファ付きの 32bpp アイコンにする（Massiguraedit と同じ作り）
HBITMAP makeGlyphIcon(int size, const wchar_t* face, const std::wstring& text, double scale, COLORREF color) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    std::memset(bits, 0, static_cast<size_t>(size) * size * 4);
    HFONT font = CreateFontW(-static_cast<int>(size * scale), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             ANTIALIASED_QUALITY, 0, face);
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT r{0, 0, size, size};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    // 白で描いた明るさをアルファにして、指定色で塗る（乗算済み）
    auto* px = static_cast<uint8_t*>(bits);
    for (int i = 0; i < size * size; ++i) {
        const uint8_t a = std::max(px[i * 4], std::max(px[i * 4 + 1], px[i * 4 + 2]));
        px[i * 4 + 0] = static_cast<uint8_t>(GetBValue(color) * a / 255);
        px[i * 4 + 1] = static_cast<uint8_t>(GetGValue(color) * a / 255);
        px[i * 4 + 2] = static_cast<uint8_t>(GetRValue(color) * a / 255);
        px[i * 4 + 3] = a;
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    SelectObject(dc, oldBmp);
    DeleteDC(dc);
    return bmp;
}

struct ToolButton {
    int id;
    wchar_t glyph;         // Segoe Fluent Icons / Segoe MDL2 Assets の文字
    const wchar_t* text;   // フォントが無い時の代わり
    const wchar_t* tip;    // ツールチップ
    bool enabled;
};
const ToolButton kToolButtons[] = {
    {IDC_ADD, 0xE8F4, L"追", L"フォルダを追加", true},
    {IDC_START, 0xE721, L"検", L"検索開始", true},
    {IDC_QUERY, 0xE8B9, L"1", L"1 枚で検索", true},
    {IDC_EXPORT, 0xE74E, L"保", L"結果を保存", false},
    {0, 0, nullptr, nullptr, false},
    {ID_OPEN, 0xE8E5, L"開", L"開く", false},
    {ID_EXPLORER, 0xE838, L"場", L"エクスプローラーで表示", false},
    {ID_COPY_PATH, 0xE8C8, L"複", L"パスをコピー", false},
    {0, 0, nullptr, nullptr, false},
    {ID_CHECK_UPDATE, 0xE895, L"更", L"アップデートを確認", true},
    {ID_HELP, 0xE897, L"?", L"使い方", true},
};

void createToolbar() {
    app.toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                                  WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_TOP | CCS_NODIVIDER,
                                  0, 0, 0, 0, app.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TOOLBAR)),
                                  app.inst, nullptr);
    SendMessageW(app.toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

    const int size = px(20);
    const wchar_t* face = fontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons"
                          : fontExists(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets"
                                                              : nullptr;
    HIMAGELIST images = ImageList_Create(size, size, ILC_COLOR32, 0, 16);
    auto add = [&](wchar_t glyph, const std::wstring& text) {
        HBITMAP bmp = (face && glyph) ? makeGlyphIcon(size, face, std::wstring(1, glyph), 0.8, RGB(40, 40, 40))
                                      : makeGlyphIcon(size, L"MS UI Gothic", text, text.size() > 1 ? 0.5 : 0.75,
                                                      RGB(40, 40, 40));
        const int n = ImageList_Add(images, bmp, nullptr);
        DeleteObject(bmp);
        return n;
    };

    // 文字はツールチップだけに出す（MIXEDBUTTONS で BTNS_SHOWTEXT を付けなければ、ボタンには絵だけ）
    SendMessageW(app.toolbar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS);
    std::vector<TBBUTTON> buttons;
    for (const ToolButton& d : kToolButtons) {
        TBBUTTON b{};
        if (!d.id) {
            b.fsStyle = BTNS_SEP;
        } else {
            b.iBitmap = add(d.glyph, d.text);
            if (d.id == IDC_START) startImage = b.iBitmap;
            b.idCommand = d.id;
            b.fsState = d.enabled ? TBSTATE_ENABLED : 0;
            b.fsStyle = BTNS_BUTTON;
            b.iString = reinterpret_cast<INT_PTR>(d.tip);
        }
        buttons.push_back(b);
    }
    stopImage = add(0xE71A, L"止");
    SendMessageW(app.toolbar, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images));
    SendMessageW(app.toolbar, TB_ADDBUTTONSW, buttons.size(), reinterpret_cast<LPARAM>(buttons.data()));
    SendMessageW(app.toolbar, TB_AUTOSIZE, 0, 0);
}

void createControls() {
    createToolbar();
    app.foldersLabel = makeControl(WC_STATICW, L"検索するフォルダ（ウィンドウにドラッグ＆ドロップしても追加できます）", 0,
                                   IDC_FOLDERS_LABEL);
    app.folders = makeControl(WC_LISTBOXW, L"", WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT,
                              IDC_FOLDERS, WS_EX_CLIENTEDGE);
    app.add = makeControl(WC_BUTTONW, L"フォルダを追加…", WS_TABSTOP, IDC_ADD);
    app.remove = makeControl(WC_BUTTONW, L"選んだフォルダを外す", WS_TABSTOP, IDC_REMOVE);
    app.thresholdCaption = makeControl(WC_STATICW, L"しきい値", 0, IDC_THRESHOLD_CAPTION);
    app.threshold = makeControl(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, IDC_THRESHOLD);
    SendMessageW(app.threshold, TBM_SETRANGE, TRUE, MAKELPARAM(0, kMaxThreshold));
    SendMessageW(app.threshold, TBM_SETTICFREQ, 5, 0);
    app.thresholdLabel = makeControl(WC_STATICW, L"", 0, IDC_THRESHOLD_LABEL);
    app.exact = makeControl(WC_BUTTONW, L"完全一致だけ", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_EXACT);
    app.flat = makeControl(WC_BUTTONW, L"単色に近い画像も比べる", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_FLAT);
    app.progress = makeControl(PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_PROGRESS);
    SendMessageW(app.progress, PBM_SETRANGE32, 0, 1000);
    app.status = makeControl(WC_STATICW, L"フォルダを追加して「検索開始」を押してください。", SS_ENDELLIPSIS | SS_NOPREFIX,
                             IDC_STATUS);

    app.groups = makeControl(WC_LISTVIEWW, L"",
                             WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS, IDC_GROUPS,
                             WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(app.groups, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    addColumn(app.groups, 0, L"#", 56, LVCFMT_RIGHT);
    addColumn(app.groups, 1, L"種類", 80);
    addColumn(app.groups, 2, L"枚数", 48, LVCFMT_RIGHT);
    addColumn(app.groups, 3, L"消せば空く", 84, LVCFMT_RIGHT);
    addColumn(app.groups, 4, L"場所", 400);

    app.thumbs = makeControl(WC_LISTVIEWW, L"",
                             WS_TABSTOP | LVS_ICON | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_AUTOARRANGE,
                             IDC_THUMBS, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(app.thumbs, LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
    ListView_SetIconSpacing(app.thumbs, app.thumbSize + px(20), app.thumbSize + px(44));
    resetImages();

    app.info = makeControl(WC_STATICW, L"", SS_NOPREFIX | SS_PATHELLIPSIS, IDC_INFO);
}

std::wstring groupFolderText(const Group& g) {
    // 全員が同じフォルダならそのフォルダ、違えば先頭のフォルダ + 「ほか」
    const std::wstring first = parentDir(app.result.files[g.members[0].index].path);
    for (const auto& m : g.members)
        if (parentDir(app.result.files[m.index].path) != first) return first + L" ほか";
    return first;
}

void copyOut(LVITEMW& item, const std::wstring& s) {
    if (item.pszText && item.cchTextMax > 0) lstrcpynW(item.pszText, s.c_str(), item.cchTextMax);
}

LRESULT onNotify(NMHDR* nm) {
    if (nm->hwndFrom == app.groups) {
        switch (nm->code) {
            case LVN_GETDISPINFOW: {
                auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
                const int i = di->item.iItem;
                if (i < 0 || i >= static_cast<int>(app.result.groups.size()) || !(di->item.mask & LVIF_TEXT)) break;
                const Group& g = app.result.groups[i];
                switch (di->item.iSubItem) {
                    case 0: copyOut(di->item, std::to_wstring(i + 1)); break;
                    case 1: copyOut(di->item, widen(kindName(g.kind))); break;
                    case 2: copyOut(di->item, withCommas(g.members.size())); break;
                    case 3: copyOut(di->item, sizeText(g.reclaim)); break;
                    case 4: copyOut(di->item, groupFolderText(g)); break;
                }
                break;
            }
            case LVN_ITEMCHANGED: {
                auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
                if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED))
                    selectGroup(lv->iItem);
                else if (lv->iItem == -1 && !(lv->uNewState & LVIS_SELECTED) && ListView_GetSelectedCount(app.groups) == 0)
                    selectGroup(-1);
                break;
            }
        }
    } else if (nm->hwndFrom == app.thumbs) {
        switch (nm->code) {
            case LVN_GETDISPINFOW: {
                auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
                const int i = di->item.iItem;
                uint8_t dist = 0;
                const Entry* e = memberEntry(i, &dist);
                if (!e) break;
                if (di->item.mask & LVIF_TEXT)
                    copyOut(di->item, L"距離" + std::to_wstring(dist) + L"  " + sizeText(e->size) + L"  " +
                                          std::to_wstring(e->f.width) + L"×" + std::to_wstring(e->f.height) + L"  " +
                                          fileName(e->path));
                if (di->item.mask & LVIF_IMAGE) {
                    if (app.imageOf[i] < 0) requestThumb(i);
                    di->item.iImage = app.imageOf[i] < 0 ? 0 : app.imageOf[i];
                }
                break;
            }
            case LVN_GETINFOTIPW: {
                auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(nm);
                uint8_t dist = 0;
                const Entry* e = memberEntry(tip->iItem, &dist);
                if (e && tip->pszText && tip->cchTextMax > 0) {
                    const std::wstring s = widen(e->path) + L"\n" + std::to_wstring(e->f.width) + L" × " +
                                           std::to_wstring(e->f.height) + L"　" + sizeText(e->size) + L"　距離 " +
                                           std::to_wstring(dist);
                    lstrcpynW(tip->pszText, s.c_str(), tip->cchTextMax);
                }
                break;
            }
            case LVN_ITEMCHANGED: {
                auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
                if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED)) showInfo(lv->iItem);
                break;
            }
            case NM_DBLCLK: {
                auto* ia = reinterpret_cast<NMITEMACTIVATE*>(nm);
                openItem(ia->iItem);
                break;
            }
            case NM_RCLICK: {
                auto* ia = reinterpret_cast<NMITEMACTIVATE*>(nm);
                if (ia->iItem < 0) break;
                ListView_SetItemState(app.thumbs, ia->iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, ID_OPEN, L"開く(&O)");
                AppendMenuW(menu, MF_STRING, ID_EXPLORER, L"エクスプローラーで表示(&E)");
                AppendMenuW(menu, MF_STRING, ID_COPY_PATH, L"パスをコピー(&C)");
                POINT pt;
                GetCursorPos(&pt);
                const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, app.hwnd, nullptr);
                DestroyMenu(menu);
                if (cmd == ID_OPEN) openItem(ia->iItem);
                if (cmd == ID_EXPLORER) showInExplorer(ia->iItem);
                if (cmd == ID_COPY_PATH) copyPath(ia->iItem);
                break;
            }
        }
    }
    return 0;
}

void createMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDC_ADD, L"フォルダを追加(&A)…");
    AppendMenuW(file, MF_STRING, IDC_EXPORT, L"結果を保存(&S)…");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_EXIT, L"終了(&X)");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"ファイル(&F)");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_HELP, L"使い方(&H)");
    AppendMenuW(help, MF_STRING, ID_CHECK_UPDATE, L"アップデートを確認(&U)");
    AppendMenuW(help, MF_STRING, ID_RELEASES, L"最新版のページを開く(&R)");
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help, MF_STRING, ID_ABOUT, L"バージョン情報(&A)");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"ヘルプ(&H)");
    SetMenu(app.hwnd, bar);
}

void shutdown() {
    app.cancel = true;
    if (app.worker.joinable()) app.worker.join();
    {
        std::lock_guard<std::mutex> lock(app.thumbMutex);
        app.thumbQuit = true;
        app.thumbQueue.clear();
    }
    app.thumbCv.notify_all();
    if (app.thumbThread.joinable()) app.thumbThread.join();
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            app.hwnd = hwnd;
            createMenu();
            createControls();
            DragAcceptFiles(hwnd, TRUE);
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {px(900), px(520)};
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            if (!app.running) {
                const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i) {
                    wchar_t buf[MAX_PATH * 4];
                    if (DragQueryFileW(drop, i, buf, static_cast<UINT>(std::size(buf)))) addFolder(buf);
                }
                saveSettings();
            }
            DragFinish(drop);
            return 0;
        }
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lp) == app.threshold)
                setText(app.thresholdLabel, thresholdText(static_cast<int>(SendMessageW(app.threshold, TBM_GETPOS, 0, 0))));
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_ADD:
                    if (!app.running) {
                        pickFolder();
                        saveSettings();
                    }
                    break;
                case IDC_REMOVE: {
                    const int n = ListBox_GetSelCount(app.folders);
                    if (n > 0) {
                        std::vector<int> sel(n);
                        ListBox_GetSelItems(app.folders, n, sel.data());
                        for (int k = n - 1; k >= 0; --k) ListBox_DeleteString(app.folders, sel[k]);
                        saveSettings();
                    }
                    break;
                }
                case IDC_START:
                    if (app.updating && !app.running) break;  // 更新のダウンロード中
                    if (app.running) {
                        app.cancel = true;
                        setText(app.status, L"中止しています…");
                    } else {
                        startSearch(L"");
                    }
                    break;
                case IDC_QUERY: {
                    if (app.running) break;
                    const std::wstring p = pickImage();
                    if (!p.empty()) startSearch(p);
                    break;
                }
                case IDC_EXPORT:
                    if (!app.running) exportResult();
                    break;
                case ID_OPEN: openItem(ListView_GetNextItem(app.thumbs, -1, LVNI_SELECTED)); break;
                case ID_EXPLORER: showInExplorer(ListView_GetNextItem(app.thumbs, -1, LVNI_SELECTED)); break;
                case ID_COPY_PATH: copyPath(ListView_GetNextItem(app.thumbs, -1, LVNI_SELECTED)); break;
                case ID_CHECK_UPDATE:
                    if (app.running)
                        MessageBoxW(hwnd, L"検索が終わってから確認してください。", L"imgdupe", MB_ICONINFORMATION);
                    else
                        startCheck(kManual);
                    break;
                case ID_HELP: showHelp(); break;
                case ID_RELEASES: ShellExecuteW(hwnd, L"open", APP_RELEASES_URL, nullptr, nullptr, SW_SHOWNORMAL); break;
                case ID_ABOUT:
                    MessageBoxW(hwnd,
                                (L"imgdupe " + widen(IMGDUPE_VERSION) + L"\r\n重複画像検索\r\n\r\n"
                                 L"github.com/" APP_REPOSITORY L"\r\n\r\n"
                                 L"設定・キャッシュ: " + app.dataDir)
                                    .c_str(),
                                L"バージョン情報", MB_ICONINFORMATION);
                    break;
                case ID_EXIT: SendMessageW(hwnd, WM_CLOSE, 0, 0); break;
            }
            return 0;
        case WM_NOTIFY:
            return onNotify(reinterpret_cast<NMHDR*>(lp));
        case WM_TIMER:
            if (wp == kProgressTimer) updateProgress();
            if (wp == kExitTimer) DestroyWindow(hwnd);  // 確認に時間がかかりすぎたら、そのまま終わる
            return 0;
        case WM_APP_DONE:
            if (app.closing) {  // 終了しようとして中止した検索
                delete reinterpret_cast<Outcome*>(lp);
                if (app.worker.joinable()) app.worker.join();
                return 0;
            }
            onDone(reinterpret_cast<Outcome*>(lp));
            return 0;
        case WM_APP_CHECKED: {
            std::unique_ptr<upd::CheckResult> r(reinterpret_cast<upd::CheckResult*>(lp));
            onChecked(static_cast<CheckMode>(wp), *r);
            return 0;
        }
        case WM_APP_DOWNLOADED: {
            std::unique_ptr<upd::DownloadResult> r(reinterpret_cast<upd::DownloadResult*>(lp));
            onDownloaded(static_cast<CheckMode>(wp), *r);
            return 0;
        }
        case WM_APP_THUMB: {
            std::unique_ptr<ThumbResult> r(reinterpret_cast<ThumbResult*>(lp));
            bool current;
            {
                std::lock_guard<std::mutex> lock(app.thumbMutex);
                current = r->gen == app.thumbGen;
            }
            if (current && r->item < static_cast<int>(app.imageOf.size())) {
                app.imageOf[r->item] = r->bmp ? ImageList_Add(app.images, r->bmp, nullptr) : 1;
                if (app.imageOf[r->item] < 0) app.imageOf[r->item] = 1;
                ListView_RedrawItems(app.thumbs, r->item, r->item);
            }
            if (r->bmp) DeleteObject(r->bmp);
            return 0;
        }
        case WM_CLOSE:
            if (app.closing) return 0;
            if (app.updating) {
                MessageBoxW(hwnd, L"アップデートの処理中です。少し待ってから閉じてください。", L"imgdupe", MB_ICONINFORMATION);
                return 0;
            }
            if (app.running &&
                MessageBoxW(hwnd, L"検索中です。中止して終了しますか？\r\n（読み込んだ分はキャッシュに残します）", L"imgdupe",
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            saveSettings();
            if (shouldCheckOnExit()) {
                // その日初めての終了: ウィンドウを隠して、裏でアップデートを確認してから終わる
                app.closing = true;
                app.cancel = true;
                iniSet(L"lastUpdateCheck", today());
                ShowWindow(hwnd, SW_HIDE);
                startCheck(kOnExit);
                SetTimer(hwnd, kExitTimer, kExitCheckTimeoutMs, nullptr);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            shutdown();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- コマンドライン

class Console {
public:
    Console() {
        attached_ = AttachConsole(ATTACH_PARENT_PROCESS);
        out_ = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode;
        isConsole_ = out_ && out_ != INVALID_HANDLE_VALUE && GetConsoleMode(out_, &mode);
    }
    void print(const std::wstring& s) {
        if (!out_ || out_ == INVALID_HANDLE_VALUE) return;
        DWORD n;
        const std::wstring line = s + L"\r\n";
        if (isConsole_) {
            WriteConsoleW(out_, line.c_str(), static_cast<DWORD>(line.size()), &n, nullptr);
        } else {
            const std::string u = narrow(line);
            WriteFile(out_, u.data(), static_cast<DWORD>(u.size()), &n, nullptr);
        }
    }

private:
    bool attached_ = false, isConsole_ = false;
    HANDLE out_ = nullptr;
};

const wchar_t kCliUsage[] =
    L"使い方: imgdupe.exe --cli <フォルダ>... [-t しきい値(0-20, 既定10)] [-o 出力名(拡張子なし)] [--exact-only]\r\n"
    L"                    [--include-flat] [-q 画像] [--db キャッシュ] [-j スレッド数]\r\n"
    L"  <出力名>.csv と <出力名>.html を書き出します（既定: imgdupe_result）。";

int runCli(int argc, wchar_t** argv) {
    Console con;
    RunOptions opt;
    opt.cacheFile = narrow(app.dataDir + L"\\imgdupe_cache.bin");
    std::wstring out = L"imgdupe_result", queryPath;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"-t") opt.threshold = std::clamp(_wtoi(next().c_str()), 0, 64);
        else if (a == L"-o") out = next();
        else if (a == L"--exact-only") opt.exactOnly = true;
        else if (a == L"--include-flat") opt.includeFlat = true;
        else if (a == L"-q") queryPath = next();
        else if (a == L"--db") opt.cacheFile = narrow(next());
        else if (a == L"-j") opt.threads = static_cast<unsigned>(std::max(1, _wtoi(next().c_str())));
        else if (a == L"-h" || a == L"--help") {
            con.print(kCliUsage);
            return 0;
        } else opt.roots.push_back(narrow(stripTrailingSlash(a)));
    }
    if (opt.roots.empty()) {
        con.print(kCliUsage);
        return 2;
    }
    opt.featuresOnly = !queryPath.empty();
    Callbacks cb;
    std::atomic<bool> cancel{false};
    Progress prog;
    cb.scan = [&](const std::string& root, std::vector<ScannedFile>& files) {
        imgwin::scanFolder(widen(root), files, prog.total, cancel);
    };
    cb.decode = [](const std::string& path) { return imgwin::decodeFeatures(widen(path)); };
    cb.threadInit = imgwin::threadBegin;
    cb.threadExit = imgwin::threadEnd;
    Result r = run(opt, cb, prog, cancel);
    con.print(L"画像 " + withCommas(r.files.size()) + L" 枚（今回読み込んだのは " + withCommas(r.hashed) + L" 枚、読めない画像 " +
              withCommas(r.unreadable) + L" 枚）");
    if (!queryPath.empty()) {
        imgwin::threadBegin();
        const Features q = imgwin::decodeFeatures(queryPath);
        imgwin::threadEnd();
        if (!q.ok) {
            con.print(L"画像を読めません: " + queryPath);
            return 1;
        }
        Group g = query(r, q, opt.threshold, opt.colorTolerance);
        con.print(withCommas(g.members.size()) + L" 枚ヒット");
        for (size_t k = 0; k < g.members.size() && k < 50; ++k)
            con.print(L"  距離" + std::to_wstring(g.members[k].dist) + L"  " + widen(r.files[g.members[k].index].path));
        r.groups.clear();
        if (!g.members.empty()) r.groups.push_back(std::move(g));
    } else {
        size_t exact = 0;
        for (const auto& g : r.groups) exact += g.kind == Kind::Exact;
        con.print(L"完全一致グループ: " + withCommas(exact));
        if (!opt.exactOnly) {
            wchar_t t[64];
            swprintf(t, 64, L"（比較 %.1f 秒）", r.secCompare);
            con.print(L"見た目一致グループ: " + withCommas(r.groups.size() - exact) + t);
            if (r.flatSkipped)
                con.print(L"単色に近いため見た目では比べなかった画像: " + withCommas(r.flatSkipped) + L" 枚");
        }
    }
    const bool ok = writeCsv(r, narrow(out + L".csv")) && writeHtml(r, narrow(out + L".html"));
    con.print(ok ? L"結果: " + out + L".csv / " + out + L".html" : L"結果を書き出せませんでした");
    return ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    app.inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    app.dataDir = findDataDir();
    upd::cleanupOldExe();

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && lstrcmpiW(argv[1], L"--cli") == 0) {
        const int code = runCli(argc, argv);
        LocalFree(argv);
        CoUninitialize();
        return code;
    }
    // exe にフォルダをドロップして起動した時は、そのフォルダをすぐ検索する
    std::vector<std::wstring> argFolders;
    for (int i = 1; argv && i < argc; ++i) argFolders.emplace_back(argv[i]);
    if (argv) LocalFree(argv);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    HDC screen = GetDC(nullptr);
    app.scale = GetDeviceCaps(screen, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, screen);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    app.font = CreateFontIndirectW(&ncm.lfMessageFont);
    app.thumbSize = px(160);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ImgdupeMain";
    RegisterClassExW(&wc);

    const std::wstring title = L"imgdupe - 重複画像検索 " + widen(IMGDUPE_VERSION);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                px(1180), px(780), nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    // 前回の設定（起動時にフォルダを渡されたら、そちらを使う）
    for (const auto& f : argFolders) addFolder(f);
    const bool autoStart = ListBox_GetCount(app.folders) > 0;
    if (!autoStart)
        for (const auto& f : loadFolders()) addFolder(f);
    const int t = std::clamp(iniInt(L"threshold", kDefaultThreshold), 0, kMaxThreshold);
    SendMessageW(app.threshold, TBM_SETPOS, TRUE, t);
    setText(app.thresholdLabel, thresholdText(t));
    Button_SetCheck(app.exact, iniInt(L"exactOnly", 0) ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(app.flat, iniInt(L"includeFlat", 0) ? BST_CHECKED : BST_UNCHECKED);
    wchar_t win[128];
    GetPrivateProfileStringW(L"imgdupe", L"window", L"", win, 128, iniPath().c_str());
    int l, tp, r, b, maxed = 0;
    if (swscanf(win, L"%d,%d,%d,%d,%d", &l, &tp, &r, &b, &maxed) >= 4 && r - l > 200 && b - tp > 200) {
        WINDOWPLACEMENT wp{sizeof(wp)};
        wp.rcNormalPosition = {l, tp, r, b};
        wp.showCmd = maxed ? SW_SHOWMAXIMIZED : show;
        SetWindowPlacement(hwnd, &wp);
    } else {
        ShowWindow(hwnd, show);
    }
    app.thumbThread = std::thread(thumbLoop);
    UpdateWindow(hwnd);
    if (autoStart) startSearch(L"");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // Enter はダイアログの既定ボタンではなく、選んだ画像を開くのに使う
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd == app.thumbs) {
            openItem(ListView_GetNextItem(app.thumbs, -1, LVNI_SELECTED));
            continue;
        }
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
