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
#include <optional>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include "commands.hpp"
#include "core.hpp"
#include "db.hpp"
#include "image_win.hpp"
#include "indexer.hpp"
#include "indexer_win.hpp"
#include "install_win.hpp"
#include "keymap_win.hpp"
#include "ann.hpp"
#include "clip_win.hpp"
#include "tagger_win.hpp"
#include "models_win.hpp"
#include "resource.h"
#include "search.hpp"
#include "links.hpp"
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
constexpr UINT WM_APP_SEARCHED = WM_APP + 5;  // lParam: SearchDone*
constexpr UINT WM_APP_EXPANDED = WM_APP + 6;  // lParam: ExpandDone*
constexpr UINT WM_APP_MODELS = WM_APP + 7;     // lParam: models::ManifestResult*（AI の部品の一覧が届いた）
constexpr UINT WM_APP_MODEL_PROGRESS = WM_APP + 8;  // lParam: std::wstring*
constexpr UINT WM_APP_MODELS_DONE = WM_APP + 9;     // lParam: std::wstring*（失敗の理由。空なら成功）
constexpr UINT_PTR kProgressTimer = 1;
constexpr UINT_PTR kSearchTimer = 3;
constexpr UINT_PTR kExitTimer = 2;
constexpr UINT kExitCheckTimeoutMs = 20000;  // 終了時の確認を待つのはここまで

enum CheckMode : WPARAM { kManual = 0, kOnExit = 1 };

enum : int {
    IDC_FOLDERS = 100,
    IDC_THRESHOLD,
    IDC_THRESHOLD_LABEL,
    IDC_EXACT,
    IDC_FLAT,
    IDC_VARIANTS,
    IDC_PROGRESS,
    IDC_STATUS,
    IDC_GROUPS,
    IDC_THUMBS,
    IDC_INFO,
    IDC_FOLDERS_LABEL,
    IDC_THRESHOLD_CAPTION,
    IDC_TOOLBAR,
    IDC_TABS,
    // 検索の画面
    IDC_S_TAGS = 600,
    IDC_S_CONF,
    IDC_S_CONF_VALUE,
    IDC_S_SOURCES,
    IDC_S_TEXT,
    IDC_S_RATING,
    IDC_S_FORMAT,
    IDC_S_FROM,
    IDC_S_TO,
    IDC_S_NOTAKEN,
    IDC_S_MINW,
    IDC_S_MINKB,
    IDC_S_MAXKB,
    IDC_S_PATH,
    IDC_S_DOWNLOAD,
    IDC_S_SORT,
    IDC_S_DESC,
    IDC_S_CLEAR,
    IDC_S_COUNT,
    IDC_S_RESULTS,
    IDC_S_LABEL,
    IDC_S_SMART,
    IDC_S_SMART_SAVE,
    IDC_S_SMART_DELETE,
    IDC_S_EXPAND,
    IDC_S_REVIEW,
    IDC_S_YES,
    IDC_S_NO,
    IDC_S_BULK,
    IDC_S_UNCLASSIFIED,
    IDC_S_CLIP,
};  // メニュー・ツールバー・キーで呼ぶ機能の ID は commands.hpp の CMD_*

constexpr int kDefaultThreshold = 10;
constexpr int kMaxThreshold = 20;

// ---------------------------------------------------------------- 状態

struct Outcome {
    Result result;
    bool query = false;
    std::wstring error;
};

struct ThumbResult {
    int view;
    unsigned gen;
    int item;
    HBITMAP bmp;
};

// サムネイルを並べる一覧（重複のグループの中身・検索結果）ごとの画像リスト
enum : int { kDupView = 0, kSearchView = 1, kViewCount = 2 };
struct ThumbView {
    HWND lv = nullptr;
    HIMAGELIST images = nullptr;
    std::vector<int> imageOf;     // 項目 → 画像リストの番号（-1: まだ）
    std::vector<char> requested;  // 読み込みを頼んだか
    unsigned gen = 0;             // 中身を入れ替えるたびに増やす（古い読み込みの結果は捨てる）
};

struct App {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND folders = nullptr, add = nullptr, remove = nullptr, foldersLabel = nullptr;
    HWND thresholdCaption = nullptr, threshold = nullptr, thresholdLabel = nullptr, exact = nullptr, flat = nullptr, variants = nullptr;
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
    bool modelsDownloading = false;          // AI の部品をダウンロード中
    bool closeAfterModels = false;           // ダウンロードを中止したら閉じる
    std::atomic<bool> modelsCancel{false};
    bool closing = false;                    // 終了時のアップデート確認中（ウィンドウは隠してある）

    // 画面の切り替え（0: 重複を探す、1: 検索）
    HWND tabs = nullptr;
    int tab = 0;
    std::vector<HWND> dupPage;  // 重複を探す画面だけのコントロール

    // 検索の画面
    struct SearchCtl {
        HWND tags, conf, confValue, sources, text, clip, rating, format, from, to, noTaken, unclassified, minW, minKb, maxKb, path, download,
            sort, desc, clear, count, results, smart, smartSave, smartDelete, expand, review, yes, no, bulk;
        std::vector<HWND> labels;  // 見出し（並べる順）
    } s{};
    std::vector<HWND> searchPage;
    std::vector<int64_t> searchIds;
    size_t searchTotal = 0;
    unsigned searchGen = 0;
    bool searchShown = false;  // 一度でも検索の画面を開いたか
    std::unordered_map<int, ImageInfo> infoCache;  // 一覧の項目 → 画像の情報
    std::unique_ptr<Db> uiDb;  // 画面の表示に使う DB の接続（UI のスレッドだけで使う）
    std::vector<std::string> sourceNames;    // 出どころの一覧の並び
    std::set<std::string> uncheckedSources;  // 外した出どころ
    bool fillingSources = false;
    std::vector<SmartFolder> smartList;  // スマートフォルダの一覧（コンボボックスの 1 番目から）
    bool expanding = false;              // フォルダに展開している最中
    int64_t similarTo = 0;               // 「似た画像を探す」の元の画像（0 = 使っていない）
    std::vector<float> searchScores;     // 内容（CLIP）で探した時の、各画像の似ている度合い
    std::wstring similarLabel;           // その時に「内容」の欄に出している文字
    std::mutex clipMutex;
    std::shared_ptr<ClipModel> clipText;  // 言葉をベクトルにするモデル（最初に使う時に読む）
    std::wstring searchNotice;           // 次の検索結果の件数の横に一度だけ出す（「元に戻しました」など）
    int reselect = -1;                   // 次の検索結果で選ぶ項目（タグを確定した後、同じ位置＝次の画像を選ぶ）

    // メニューとショートカットキー
    HMENU menuBar = nullptr;
    KeyMap keys;
    std::vector<int> keyTmp = std::vector<int>(8, 0);  // ショートカットキーの設定画面の一時置き場

    // 結果
    Result result;
    std::vector<std::string> resultRoots;  // result.files を作った時のフォルダ
    bool haveFiles = false;
    bool resultIsQuery = false;  // 1 枚で検索した結果を出している
    int curGroup = -1;

    // サムネイル
    int thumbSize = 160;
    ThumbView views[kViewCount];
    std::thread thumbThread;
    std::thread expandThread;
    std::mutex thumbMutex;
    std::condition_variable thumbCv;
    struct ThumbJob {
        int view;
        int item;
        std::wstring path;
        int64_t imageId;
    };
    std::deque<ThumbJob> thumbQueue;
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
std::wstring cachePath() { return app.dataDir + L"\\imgdupe_cache.bin"; }  // 前の版のキャッシュ（DB に取り込む）
std::wstring dbPath() { return app.dataDir + L"\\imgdupe.db"; }

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

void saveSearchSettings();

void saveSettings() {
    if (app.s.tags) saveSearchSettings();
    const auto f = folderList();
    indexer::setRoots(app.dataDir, f);  // バックグラウンドの解析も同じフォルダを見る
    WritePrivateProfileSectionW(L"folders", L"\0", iniPath().c_str());
    for (size_t i = 0; i < f.size(); ++i)
        WritePrivateProfileStringW(L"folders", std::to_wstring(i + 1).c_str(), f[i].c_str(), iniPath().c_str());
    iniSet(L"threshold", std::to_wstring(SendMessageW(app.threshold, TBM_GETPOS, 0, 0)));
    iniSet(L"exactOnly", Button_GetCheck(app.exact) == BST_CHECKED ? L"1" : L"0");
    iniSet(L"includeFlat", Button_GetCheck(app.flat) == BST_CHECKED ? L"1" : L"0");
    iniSet(L"showVariants", Button_GetCheck(app.variants) == BST_CHECKED ? L"1" : L"0");
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
    // indexer が作ったサムネイル（パックファイル）があればそれを使い、無ければ元の画像から作る
    Db db;
    const bool haveDb = db.open(narrow(dbPath()));
    ThumbPack pack(narrow(app.dataDir));
    for (;;) {
        App::ThumbJob job;
        unsigned gen;
        {
            std::unique_lock<std::mutex> lock(app.thumbMutex);
            app.thumbCv.wait(lock, [] { return app.thumbQuit || !app.thumbQueue.empty(); });
            if (app.thumbQuit) break;
            job = std::move(app.thumbQueue.front());
            app.thumbQueue.pop_front();
            gen = app.views[job.view].gen;
        }
        HBITMAP bmp = nullptr;
        ThumbData t;
        // パックのサムネイルが表示より小さければ、ぼやけるので元の画像から作る
        if (haveDb && job.imageId && pack.get(db, job.imageId, t) &&
            std::max(t.width, t.height) >= std::min(app.thumbSize, kThumbSide - 1))
            bmp = imgwin::thumbFromBytes(t.bytes, app.thumbSize, GetSysColor(COLOR_WINDOW));
        if (!bmp) bmp = imgwin::loadThumbnail(job.path, app.thumbSize, GetSysColor(COLOR_WINDOW));
        auto* r = new ThumbResult{job.view, gen, job.item, bmp};
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

// 一覧の中身を n 件に入れ替える（画像リストを空にして、0: 読み込み中, 1: 読めない を入れ直す）
void resetView(int view, int n) {
    ThumbView& v = app.views[view];
    {
        std::lock_guard<std::mutex> lock(app.thumbMutex);
        ++v.gen;
        app.thumbQueue.erase(std::remove_if(app.thumbQueue.begin(), app.thumbQueue.end(),
                                            [view](const App::ThumbJob& j) { return j.view == view; }),
                             app.thumbQueue.end());
    }
    if (!v.images) {
        v.images = ImageList_Create(app.thumbSize, app.thumbSize, ILC_COLOR32, 16, 64);
        ListView_SetImageList(v.lv, v.images, LVSIL_NORMAL);
    }
    ImageList_RemoveAll(v.images);
    HBITMAP loading = solidBitmap(app.thumbSize, RGB(236, 238, 242), RGB(210, 214, 220));
    HBITMAP broken = solidBitmap(app.thumbSize, RGB(250, 225, 225), RGB(220, 120, 120));
    ImageList_Add(v.images, loading, nullptr);
    ImageList_Add(v.images, broken, nullptr);
    DeleteObject(loading);
    DeleteObject(broken);
    v.imageOf.assign(n, -1);
    v.requested.assign(n, 0);
    ListView_SetItemCountEx(v.lv, n, 0);
    ListView_SetItemState(v.lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (n) ListView_EnsureVisible(v.lv, 0, FALSE);
    InvalidateRect(v.lv, nullptr, TRUE);
}

// 一覧の数だけを n 件に増やす（表示中の項目とサムネイルはそのまま）
void extendView(int view, int n) {
    ThumbView& v = app.views[view];
    if (n <= static_cast<int>(v.imageOf.size())) return;
    v.imageOf.resize(n, -1);
    v.requested.resize(n, 0);
    ListView_SetItemCountEx(v.lv, n, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
}

// 見えている項目のサムネイルを頼む（後から頼んだ = 今見えているものを先に読む）
void requestThumb(int view, int item, const std::wstring& path, int64_t imageId) {
    ThumbView& v = app.views[view];
    if (item < 0 || item >= static_cast<int>(v.requested.size()) || v.requested[item]) return;
    v.requested[item] = 1;
    {
        std::lock_guard<std::mutex> lock(app.thumbMutex);
        app.thumbQueue.push_front({view, item, path, imageId});
    }
    app.thumbCv.notify_one();
}

// GETDISPINFO の画像の番号（まだなら読み込みを頼む）
int thumbImage(int view, int item, const std::wstring& path, int64_t imageId) {
    ThumbView& v = app.views[view];
    if (item < 0 || item >= static_cast<int>(v.imageOf.size())) return 0;
    if (v.imageOf[item] < 0) requestThumb(view, item, path, imageId);
    return v.imageOf[item] < 0 ? 0 : v.imageOf[item];
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
    for (int id : {CMD_OPEN, CMD_EXPLORER, CMD_COPY_PATH}) tbEnable(id, e != nullptr);
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
    const Group* grp = currentGroup();
    resetView(kDupView, grp ? static_cast<int>(grp->members.size()) : 0);
    showInfo(-1);
}

int64_t memberImageId(int item) {
    const Group* g = currentGroup();
    if (!g || item < 0 || item >= static_cast<int>(g->members.size())) return 0;
    const uint32_t index = g->members[item].index;
    return index < app.result.ids.size() ? app.result.ids[index] : 0;
}

// ---------------------------------------------------------------- 結果の表示

Db* uiDb();

void showResult(Result&& r, bool query) {
    app.result = std::move(r);
    app.resultIsQuery = query;
    // 差分（CLIP）のグループを DB から足す（重複の後ろ）。外していれば取り除く
    if (!query) {
        if (Button_GetCheck(app.variants) == BST_CHECKED) {
            if (Db* db = uiDb()) {
                try {
                    appendVariantGroups(*db, app.result);
                } catch (const std::exception&) {
                }
            }
        } else {
            auto& gs = app.result.groups;
            gs.erase(std::remove_if(gs.begin(), gs.end(), [](const Group& g) { return g.kind == Kind::Variant; }), gs.end());
        }
    }
    ListView_SetItemCountEx(app.groups, static_cast<int>(app.result.groups.size()), 0);
    InvalidateRect(app.groups, nullptr, TRUE);
    selectGroup(-1);
    if (!app.result.groups.empty()) {
        ListView_SetItemState(app.groups, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(app.groups, 0, FALSE);
    }
    tbEnable(CMD_EXPORT, !app.result.groups.empty());

    std::wstring s;
    if (query) {
        const size_t hits = app.result.groups.empty() ? 0 : app.result.groups[0].members.size();
        s = L"似た画像 " + withCommas(hits) + L" 枚（" + withCommas(app.result.files.size()) + L" 枚の中から）";
    } else {
        size_t exact = 0, similar = 0, variant = 0;
        for (const auto& g : app.result.groups) (g.kind == Kind::Exact ? exact : g.kind == Kind::Variant ? variant : similar)++;
        uint64_t reclaim = 0;
        for (const auto& g : app.result.groups) reclaim += g.reclaim;
        s = L"画像 " + withCommas(app.result.files.size()) + L" 枚: 完全一致 " + withCommas(exact) + L" グループ・見た目一致 " +
            withCommas(similar) + L" グループ" + (variant ? L"・差分（AI） " + withCommas(variant) + L" グループ" : L"") +
            L"（重複を消せば約 " + sizeText(reclaim) + L" 空きます）";
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
    tbSetText(CMD_SEARCH, on ? L"検索を中止" : L"検索開始");
    SendMessageW(app.toolbar, TB_CHANGEBITMAP, CMD_SEARCH, MAKELPARAM(on ? stopImage : startImage, 0));
    for (HWND h : {app.add, app.remove, app.folders, app.threshold, app.exact, app.flat, app.variants}) EnableWindow(h, !on);
    for (int id : {CMD_ADD_FOLDER, CMD_QUERY}) tbEnable(id, !on);
    tbEnable(CMD_EXPORT, !on && !app.result.groups.empty());
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
    opt.dbFile = narrow(dbPath());
    opt.legacyCacheFile = narrow(cachePath());
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
    tbEnable(CMD_CHECK_UPDATE, false);
    HWND hwnd = app.hwnd;
    std::thread([hwnd, mode] {
        auto* r = new upd::CheckResult(upd::checkLatest());
        if (!PostMessageW(hwnd, WM_APP_CHECKED, mode, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

// ---------------------------------------------------------------- AI の部品（ONNX Runtime・モデル）

enum class ModelsAsk { UpdatesOnly, Offer };  // アップデートの確認から / メニュー「AI 解析の部品」から
ModelsAsk modelsAsk = ModelsAsk::UpdatesOnly;

void startModelsCheck(ModelsAsk ask) {
    if (app.modelsDownloading) {
        MessageBoxW(app.hwnd, L"AI の部品をダウンロードしています。終わるまでお待ちください。", L"imgdupe", MB_ICONINFORMATION);
        return;
    }
    modelsAsk = ask;
    HWND hwnd = app.hwnd;
    std::thread([hwnd] {
        auto* r = new models::ManifestResult(models::fetchManifest());
        if (!PostMessageW(hwnd, WM_APP_MODELS, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

std::wstring componentLines(const std::vector<const imgdupe::Component*>& comps, uint64_t& total) {
    std::wstring lines;
    total = 0;
    for (const auto* c : comps) {
        lines += L"・" + widen(c->name) + L"（" + widen(c->version) + L"、約 " + models::sizeText(c->totalSize()) + L"）\r\n";
        total += c->totalSize();
    }
    return lines;
}

void startModelsDownload(std::vector<const imgdupe::Component*> comps, imgdupe::Manifest manifest) {
    app.modelsDownloading = true;
    app.modelsCancel = false;
    tbEnable(CMD_CHECK_UPDATE, false);
    HWND hwnd = app.hwnd;
    const std::wstring dir = app.dataDir;
    // comps は manifest の中を指すので、manifest ごとスレッドに渡す
    auto shared = std::make_shared<imgdupe::Manifest>(std::move(manifest));
    std::vector<std::string> ids;
    for (const auto* c : comps) ids.push_back(c->id);
    std::thread([hwnd, dir, shared, ids] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::vector<const imgdupe::Component*> list;
        for (const auto& id : ids)
            if (const auto* c = shared->find(id)) list.push_back(c);
        DWORD last = 0;
        const std::wstring err = models::install(dir, list, [&](const std::wstring& name, uint64_t done, uint64_t total) {
            const DWORD now = GetTickCount();
            if (now - last > 300 || done == total) {
                last = now;
                auto* text = new std::wstring(name + L" をダウンロードしています… " + models::sizeText(done) + L" / " +
                                              models::sizeText(total) + L"（" + std::to_wstring(total ? done * 100 / total : 0) + L"%）");
                if (!PostMessageW(hwnd, WM_APP_MODEL_PROGRESS, 0, reinterpret_cast<LPARAM>(text))) delete text;
            }
            return !app.modelsCancel.load();
        });
        CoUninitialize();
        auto* r = new std::wstring(err);
        if (!PostMessageW(hwnd, WM_APP_MODELS_DONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void onModelsChecked(const models::ManifestResult& r) {
    if (!r.ok) {
        if (modelsAsk == ModelsAsk::Offer)
            MessageBoxW(app.hwnd, r.error.c_str(), L"AI 解析の部品", MB_ICONWARNING);
        return;  // アップデートの確認からなら、本体の結果を出した後なので何も言わない
    }
    const imgdupe::Installed installed = models::loadInstalled(app.dataDir);
    uint64_t total = 0;
    const auto updates = imgdupe::planUpdate(r.manifest, installed);
    if (!updates.empty()) {
        const std::wstring lines = componentLines(updates, total);
        if (MessageBoxW(app.hwnd,
                        (L"AI 解析の部品に新しい版があります。\r\n\r\n" + lines + L"\r\n合計 約 " + models::sizeText(total) +
                         L"。ダウンロードして入れ替えますか？")
                            .c_str(),
                        L"AI 解析の部品", MB_YESNO | MB_ICONQUESTION) == IDYES)
            startModelsDownload(updates, r.manifest);
        return;
    }
    if (modelsAsk == ModelsAsk::UpdatesOnly) return;
    // まだ入れていない部品（ほかの部品から要るだけのものは出さない）
    std::vector<std::string> wanted;
    for (const auto& c : r.manifest.components) {
        bool dependency = false;
        for (const auto& o : r.manifest.components)
            for (const auto& q : o.requires_)
                if (q == c.id) dependency = true;
        if (!dependency) wanted.push_back(c.id);
    }
    const auto missing = imgdupe::planInstall(r.manifest, installed, wanted);
    if (missing.empty()) {
        std::wstring lines;
        for (const auto& [id, c] : installed)
            if (const auto* m = r.manifest.find(id)) lines += L"・" + widen(m->name) + L"（" + widen(c.version) + L"）\r\n";
        MessageBoxW(app.hwnd, (L"AI 解析の部品はすべて最新です。\r\n\r\n" + lines).c_str(), L"AI 解析の部品", MB_ICONINFORMATION);
        return;
    }
    const std::wstring lines = componentLines(missing, total);
    if (MessageBoxW(app.hwnd,
                    (L"次の部品を入れます。\r\n\r\n" + lines + L"\r\n合計 約 " + models::sizeText(total) +
                     L"。ダウンロードしますか？\r\n（入れる場所: " + models::modelsDir(app.dataDir) + L"）")
                        .c_str(),
                    L"AI 解析の部品", MB_YESNO | MB_ICONQUESTION) == IDYES)
        startModelsDownload(missing, r.manifest);
}

void restartIndexerAfterUpdate();

void onModelsDone(const std::wstring& err) {
    app.modelsDownloading = false;
    tbEnable(CMD_CHECK_UPDATE, true);
    SetWindowTextW(app.hwnd, (L"imgdupe - 重複画像検索 " + widen(IMGDUPE_VERSION)).c_str());
    if (app.closeAfterModels) {
        PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
        return;
    }
    if (!err.empty()) {
        setText(app.status, L"AI 解析の部品を入れられませんでした");
        MessageBoxW(app.hwnd, (L"AI 解析の部品を入れられませんでした。\r\n\r\n" + err).c_str(), L"AI 解析の部品", MB_ICONWARNING);
        return;
    }
    restartIndexerAfterUpdate();  // 新しいモデルで解析し直す
    setText(app.status, L"AI 解析の部品を入れました");
    MessageBoxW(app.hwnd, L"AI 解析の部品を入れました。", L"AI 解析の部品", MB_ICONINFORMATION);
}

void startDownload(CheckMode mode, const imgdupe::ReleaseInfo& info) {
    app.updating = true;
    tbEnable(CMD_CHECK_UPDATE, false);
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
    tbEnable(CMD_CHECK_UPDATE, true);
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
        startModelsCheck(ModelsAsk::UpdatesOnly);  // 本体が最新なら、AI の部品の版も確かめる
        return;
    }
    if (MessageBoxW(app.hwnd,
                    (L"新しい版があります。\r\n\r\n" + versions +
                     L"\r\n\r\n更新して再起動しますか？\r\n（設定とキャッシュはそのまま使えます。AI 解析の部品の更新は、"
                     L"再起動した後にもう一度「アップデートを確認」で確かめます）")
                        .c_str(),
                    L"imgdupe のアップデート", MB_YESNO | MB_ICONQUESTION) == IDYES)
        startDownload(kManual, r.info);
}

// 更新した後、動いているバックグラウンドの解析を新しい版で起動し直す
void restartIndexerAfterUpdate() {
    if (!indexer::isRunning()) return;
    indexer::requestQuit(app.dataDir);
    indexer::start(true);  // 新しい版は、古い版が終わるのを待ってから始まる
}

void onDownloaded(CheckMode mode, const upd::DownloadResult& r) {
    app.updating = false;
    tbEnable(CMD_CHECK_UPDATE, true);
    if (r.ok) restartIndexerAfterUpdate();
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

void openPath(const std::wstring& path) {
    if (!path.empty()) ShellExecuteW(app.hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void showPathInExplorer(const std::wstring& path) {
    if (path.empty()) return;
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

void copyText(const std::wstring& w) {
    if (w.empty() || !OpenClipboard(app.hwnd)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (mem) {
        std::memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

std::wstring memberPath(int item) {
    const Entry* e = memberEntry(item);
    return e ? widen(e->path) : L"";
}

const ImageInfo* resultInfo(int item);

// 今の画面で選んでいる画像のパス
std::wstring selectedPath() {
    if (app.tab == 0) return memberPath(ListView_GetNextItem(app.thumbs, -1, LVNI_SELECTED));
    const ImageInfo* info = resultInfo(ListView_GetNextItem(app.s.results, -1, LVNI_SELECTED));
    return info ? widen(info->path) : L"";
}

// 画像の右クリックのメニュー
void findSimilar(int64_t imageId, const std::wstring& name);

// imageId があれば「似た画像を探す」も出す（検索の画面）
void imageContextMenu(const std::wstring& path, int64_t imageId = 0) {
    constexpr int kSimilar = 0x7f00;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CMD_OPEN, L"開く(&O)");
    AppendMenuW(menu, MF_STRING, CMD_EXPLORER, L"エクスプローラーで表示(&E)");
    AppendMenuW(menu, MF_STRING, CMD_COPY_PATH, L"パスをコピー(&C)");
    if (imageId) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kSimilar, L"似た画像を探す(&S)");
    }
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, app.hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == CMD_OPEN) openPath(path);
    if (cmd == CMD_EXPLORER) showPathInExplorer(path);
    if (cmd == CMD_COPY_PATH) copyText(path);
    if (cmd == kSimilar) {
        const size_t sep = path.find_last_of(L"\\/");
        findSimilar(imageId, sep == std::wstring::npos ? path : path.substr(sep + 1));
    }
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
                L"・キーの割り当ては「その他 → ショートカットキーの設定」（Ctrl+K）で変えられます。\r\n"
                L"・2 回目からは読み込んだ結果（キャッシュ）を使うので、増えた・変わった画像だけを読み込みます。\r\n"
                L"・このツールは画像を読むだけで、移動や削除はしません。",
                L"imgdupe の使い方", MB_ICONINFORMATION);
}

// ---------------------------------------------------------------- 検索の画面

HWND makeControl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0);
void addColumn(HWND lv, int i, const wchar_t* title, int width, int fmt = LVCFMT_LEFT);
void copyOut(LVITEMW& item, const std::wstring& s);

void applyQueryToUi(const SearchQuery& q);
void expandLinksUi();
void markSelected(bool correct);
void bulkTagUi();
void showLinksWindow();
void cleanupTemporaryLinks();
struct ExpandDone {
    ExpandResult result;
    std::wstring name, error;
};
void onExpanded(ExpandDone* p);
void saveSmartFolderUi();
void deleteSmartFolderUi();
void refreshSmartFolders(const std::wstring& select);

struct SearchDone {
    unsigned gen;
    SearchResult result;
    std::wstring error;
    bool rest = false;  // 2 回目（先頭の後の残り全部）
    bool clip = false;  // 内容（CLIP）で似ている順に上位だけ
    std::vector<float> scores;
};

constexpr size_t kFirstResults = 2000;  // まず先頭だけ並べて見せる（残りは裏で）

Db* uiDb() {
    if (!app.uiDb) {
        app.uiDb = std::make_unique<Db>();
        if (!app.uiDb->open(narrow(dbPath()))) app.uiDb.reset();
    }
    return app.uiDb.get();
}

// 出どころの表示名
std::wstring sourceLabel(const std::string& s) {
    static const std::pair<const char*, const wchar_t*> names[] = {
        {"manual", L"手動で確認"}, {"embedded", L"ファイル埋め込み"}, {"meta", L"メタデータ"}, {"id_lookup", L"ID 照会"},
        {"reverse", L"逆画像検索"}, {"folder", L"フォルダ名"}, {"same_folder", L"同じフォルダからの推定"},
        {"tagger", L"タガー（WD14）"}, {"clip", L"CLIP"}, {"reference", L"見本画像との類似"}, {"ocr", L"OCR"}};
    for (const auto& [k, v] : names)
        if (s == k) return v;
    return widen(s);
}

std::wstring editText(HWND h) {
    std::wstring t(static_cast<size_t>(GetWindowTextLengthW(h)), L'\0');
    if (!t.empty()) GetWindowTextW(h, &t[0], static_cast<int>(t.size()) + 1);
    return t;
}

// 出どころの一覧を DB から作り直す（外したものは外したまま）
void refreshSources() {
    Db* db = uiDb();
    app.fillingSources = true;
    ListView_DeleteAllItems(app.s.sources);
    app.sourceNames.clear();
    if (db) {
        try {
            for (const auto& [name, count] : tagSources(*db)) {
                const std::wstring text = sourceLabel(name) + L"（" + withCommas(static_cast<uint64_t>(count)) + L"）";
                LVITEMW it{};
                it.mask = LVIF_TEXT;
                it.iItem = static_cast<int>(app.sourceNames.size());
                it.pszText = const_cast<wchar_t*>(text.c_str());
                const int i = ListView_InsertItem(app.s.sources, &it);
                ListView_SetCheckState(app.s.sources, i, app.uncheckedSources.count(name) ? FALSE : TRUE);
                app.sourceNames.push_back(name);
            }
        } catch (const std::exception&) {
        }
    }
    if (app.sourceNames.empty()) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.pszText = const_cast<wchar_t*>(L"（タグはまだありません）");
        ListView_InsertItem(app.s.sources, &it);
    }
    app.fillingSources = false;
}

std::optional<int64_t> pickerDate(HWND h, bool endOfDay) {
    SYSTEMTIME st{};
    if (DateTime_GetSystemtime(h, &st) != GDT_VALID) return std::nullopt;
    return civilToUnix(st.wYear, st.wMonth, st.wDay, endOfDay ? 23 : 0, endOfDay ? 59 : 0, endOfDay ? 59 : 0);
}

SearchQuery searchQueryFromUi() {
    SearchQuery q;
    parseTagQuery(narrow(editText(app.s.tags)), q.tags, q.excludeTags);
    q.minConfidence = SendMessageW(app.s.conf, TBM_GETPOS, 0, 0) / 100.0;
    q.excludeSources.assign(app.uncheckedSources.begin(), app.uncheckedSources.end());
    q.text = narrow(editText(app.s.text));
    if (app.similarTo) q.similarTo = app.similarTo;
    else q.clipText = narrow(editText(app.s.clip));
    const int rating = ComboBox_GetCurSel(app.s.rating);  // 0: 指定なし、1〜5: ★n 以上、6: 星なし
    if (rating >= 1 && rating <= 5) q.minRating = rating;
    if (rating == 6) q.noRating = true;
    const int format = ComboBox_GetCurSel(app.s.format);
    if (format > 0) {
        std::wstring f(static_cast<size_t>(ComboBox_GetLBTextLen(app.s.format, format)), L'\0');
        ComboBox_GetLBText(app.s.format, format, &f[0]);
        if (f == L"アニメーション") q.animatedOnly = true;
        else q.format = narrow(f.c_str());
    }
    if (auto t = pickerDate(app.s.from, false)) q.takenFrom = *t;
    if (auto t = pickerDate(app.s.to, true)) q.takenTo = *t;
    q.noTaken = Button_GetCheck(app.s.noTaken) == BST_CHECKED;
    q.unclassified = Button_GetCheck(app.s.unclassified) == BST_CHECKED;
    q.minWidth = _wtoi(editText(app.s.minW).c_str());
    q.minSize = static_cast<int64_t>(_wtoi(editText(app.s.minKb).c_str())) * 1024;
    q.maxSize = static_cast<int64_t>(_wtoi(editText(app.s.maxKb).c_str())) * 1024;
    q.pathContains = narrow(editText(app.s.path));
    q.downloadContains = narrow(editText(app.s.download));
    static const SortKey sorts[] = {SortKey::Path,      SortKey::Modified, SortKey::Created, SortKey::Taken, SortKey::Resolution,
                                    SortKey::Size,      SortKey::Rating,   SortKey::Title,   SortKey::Confidence};
    const int sort = std::max(0, ComboBox_GetCurSel(app.s.sort));
    q.sort = sorts[std::min<int>(sort, static_cast<int>(std::size(sorts)) - 1)];
    q.descending = Button_GetCheck(app.s.desc) == BST_CHECKED;
    q.review = Button_GetCheck(app.s.review) == BST_CHECKED;
    return q;
}

// 条件が変わったら、少し待ってから探す（スライダーを動かしている間や、文字を打っている間は探さない）
void scheduleSearch(UINT delayMs) {
    if (app.tab != 1) return;
    SetTimer(app.hwnd, kSearchTimer, delayMs, nullptr);
}

void runSearch() {
    KillTimer(app.hwnd, kSearchTimer);
    SearchQuery q = searchQueryFromUi();
    const unsigned gen = ++app.searchGen;
    setText(app.s.count, L"検索中…");
    const std::string db = narrow(dbPath());
    HWND hwnd = app.hwnd;
    const std::wstring dataDir = app.dataDir;
    std::thread([q, gen, db, hwnd, dataDir]() mutable {
        // 1 回目: 先頭だけ（DB は上位だけを並べるので速い）と全部の数。多ければ 2 回目に残りを全部
        auto* done = new SearchDone{gen, {}, L""};
        bool more = false;
        Db d;
        try {
            if (!d.open(db)) throw std::runtime_error(d.error());
            if (!q.clipText.empty() || q.similarTo) {
                // 内容（CLIP）: ほかの条件に合う画像を、似ている順に上位だけ
                done->clip = true;
                VectorIndex idx;
                if (!idx.load(narrow(clip::vectorPath(dataDir))))
                    throw std::runtime_error("CLIP のベクトルがまだありません（AI 解析の部品の CLIP を入れると、バックグラウンドの解析で作ります）");
                std::vector<float> qv;
                if (q.similarTo) {
                    if (!idx.get(q.similarTo, qv)) throw std::runtime_error("この画像の CLIP のベクトルがまだありません（バックグラウンドの解析で作ります）");
                } else {
                    std::shared_ptr<ClipModel> model;
                    {
                        std::lock_guard<std::mutex> lock(app.clipMutex);
                        if (!app.clipText) {
                            std::wstring err;
                            app.clipText = clip::loadText(dataDir, err);
                            if (!app.clipText) throw std::runtime_error(narrow(err));
                        }
                        model = app.clipText;
                    }
                    std::string err;
                    if (!model->encodeText(q.clipText, qv, err)) throw std::runtime_error("言葉をベクトルにできませんでした: " + err);
                }
                if (qv.size() != idx.dim()) throw std::runtime_error("CLIP のモデルとベクトルの次元が合いません");
                const RankResult rr = rankByVector(d, q, idx, qv.data(), kFirstResults, q.similarTo);
                done->result.ids = rr.ids;
                done->result.total = rr.total;
                done->scores = rr.scores;
            } else {
                q.limit = kFirstResults;
                done->result = search(d, q);
                more = done->result.total > done->result.ids.size();
            }
        } catch (const std::exception& e) {
            done->error = widen(e.what());
        }
        if (!PostMessageW(hwnd, WM_APP_SEARCHED, 0, reinterpret_cast<LPARAM>(done))) delete done;
        if (!more) return;
        auto* rest = new SearchDone{gen, {}, L"", true};
        try {
            q.limit = 0;
            rest->result = search(d, q);
        } catch (const std::exception& e) {
            rest->error = widen(e.what());
        }
        if (!PostMessageW(hwnd, WM_APP_SEARCHED, 0, reinterpret_cast<LPARAM>(rest))) delete rest;
    }).detach();
}

void showSearchInfo(int item);

void onSearched(SearchDone* p) {
    std::unique_ptr<SearchDone> done(p);
    if (done->gen != app.searchGen) return;  // 古い条件の結果
    if (!done->error.empty()) {
        setText(app.s.count, L"検索できませんでした: " + done->error);
        return;
    }
    if (done->rest) {
        // 先頭は同じ並びなので、表示とサムネイルはそのままにして、残りを足す
        app.searchIds = std::move(done->result.ids);
        extendView(kSearchView, static_cast<int>(app.searchIds.size()));
        setText(app.s.count, withCommas(app.searchTotal) + L" 件");
        return;
    }
    app.searchIds = std::move(done->result.ids);
    app.searchTotal = done->result.total;
    app.searchScores = std::move(done->scores);
    app.infoCache.clear();
    resetView(kSearchView, static_cast<int>(app.searchIds.size()));
    const bool review = Button_GetCheck(app.s.review) == BST_CHECKED;
    std::wstring count;
    if (!app.searchTotal) count = review ? L"確かめる画像はありません" : L"見つかりません";
    else if (done->clip)
        count = withCommas(app.searchTotal) + L" 件のうち、" + (app.similarTo ? L"似ている" : L"内容が近い") + L"順に " +
                withCommas(app.searchIds.size()) + L" 件";
    else
        count = withCommas(app.searchTotal) + L" 件" +
                (app.searchTotal > app.searchIds.size() ? L"（残りを読み込んでいます…）" : review ? L"（迷っている順）" : L"");
    setText(app.s.count, count + (app.searchNotice.empty() ? L"" : L"　" + app.searchNotice));
    app.searchNotice.clear();
    showSearchInfo(-1);
    if (app.reselect >= 0 && !app.searchIds.empty()) {
        const int item = std::min(app.reselect, static_cast<int>(app.searchIds.size()) - 1);
        ListView_SetItemState(app.s.results, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(app.s.results, item, FALSE);
    }
    app.reselect = -1;
}

const ImageInfo* resultInfo(int item) {
    if (item < 0 || item >= static_cast<int>(app.searchIds.size())) return nullptr;
    auto it = app.infoCache.find(item);
    if (it != app.infoCache.end()) return &it->second;
    Db* db = uiDb();
    ImageInfo info;
    if (!db) return nullptr;
    try {
        if (!imageInfo(*db, app.searchIds[item], info)) return nullptr;
    } catch (const std::exception&) {
        return nullptr;
    }
    if (app.infoCache.size() > 20000) app.infoCache.clear();
    return &app.infoCache.emplace(item, std::move(info)).first->second;
}

std::wstring dateText(int64_t t) {
    int y, mo, d, h, mi, se;
    unixToCivil(t, y, mo, d, h, mi, se);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
    return buf;
}

std::wstring stars(int rating) { return rating > 0 ? std::wstring(static_cast<size_t>(rating), L'★') : L""; }

void showSearchInfo(int item) {
    const ImageInfo* info = resultInfo(item);
    for (int id : {CMD_OPEN, CMD_EXPLORER, CMD_COPY_PATH}) tbEnable(id, info != nullptr);
    if (!info) {
        setText(app.info, app.searchIds.empty() ? L""
                                                : L"画像をダブルクリックすると開きます。右クリックで「エクスプローラーで表示」などができます。");
        return;
    }
    std::wstring line = std::to_wstring(info->width) + L" × " + std::to_wstring(info->height) + L"　" +
                        sizeText(static_cast<uint64_t>(info->size));
    if (!info->format.empty()) line += L"　" + widen(info->format);
    if (info->rating > 0) line += L"　" + stars(info->rating);
    if (info->taken) line += L"　撮影 " + dateText(info->taken);
    if (!info->title.empty()) line += L"　「" + widen(info->title) + L"」";
    if (item >= 0 && item < static_cast<int>(app.searchScores.size())) {
        wchar_t sc[48];
        swprintf(sc, 48, L"　似ている度合い %.3f", app.searchScores[item]);
        line += sc;
    }
    // ファイルに書き込まれているタグ（実体。出どころ embedded）と、DB にだけある検索用のタグを分けて出す。
    // 検索用のタグは、出どころの確度を合成した値（手動で確定したものはその値）
    std::wstring fileTags, tags;
    if (Db* db = uiDb()) {
        try {
            struct Agg {
                std::vector<double> ps;
                std::wstring detail;
                int manual = -1;     // 手動で確定した値（-1 = なし）
                bool dbOnly = false;  // ファイル以外の出どころがある（検索用タグの行に出す）
            };
            std::vector<std::pair<std::string, Agg>> agg;
            for (const auto& t : tagsOf(*db, info->id)) {
                if (t.source == kSourceEmbedded) fileTags += (fileTags.empty() ? L"ファイルのタグ: " : L"、") + widen(t.tag);
                auto it = std::find_if(agg.begin(), agg.end(), [&](const auto& a) { return a.first == t.tag; });
                if (it == agg.end()) it = agg.insert(agg.end(), {t.tag, Agg{}});
                if (t.source != kSourceEmbedded) it->second.dbOnly = true;
                wchar_t c[16];
                swprintf(c, 16, L" %.2f", t.confidence);
                it->second.detail += (it->second.detail.empty() ? L"" : L"・") + sourceLabel(t.source) + c;
                if (t.manual) it->second.manual = t.confidence > 0.5 ? 1 : 0;
                else if (!app.uncheckedSources.count(t.source)) it->second.ps.push_back(t.confidence);
            }
            std::vector<std::pair<double, std::wstring>> lines;
            for (const auto& [tag, a] : agg) {
                if (!a.dbOnly) continue;  // ファイルのタグだけのものは上の行に出ている
                const double p = a.manual >= 0 ? a.manual : combineConfidence(a.ps);
                wchar_t c[16];
                swprintf(c, 16, L" %.2f", p);
                lines.emplace_back(p, widen(tag) + c + L"（" + a.detail + L"）");
            }
            std::stable_sort(lines.begin(), lines.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            for (size_t i = 0; i < lines.size() && i < 8; ++i) tags += (tags.empty() ? L"検索用タグ: " : L"　") + lines[i].second;
            if (lines.size() > 8) tags += L"　ほか " + std::to_wstring(lines.size() - 8) + L" 個";
        } catch (const std::exception&) {
        }
    }
    if (fileTags.empty()) fileTags = L"ファイルのタグ: なし";
    if (tags.empty()) tags = L"検索用タグ: なし";
    setText(app.info, widen(info->path) + L"\r\n" + line + L"\r\n" + fileTags + L"\r\n" + tags);
}

// 「似た画像を探す」: 内容の欄をその画像にして探す（ほかの条件はそのまま）
void findSimilar(int64_t imageId, const std::wstring& name) {
    app.similarTo = imageId;
    app.similarLabel = L"似た画像: " + name;
    SetWindowTextW(app.s.clip, app.similarLabel.c_str());  // EN_CHANGE が来ても同じ文字なので似た画像のまま
    scheduleSearch(10);
}

void clearSearchConditions() {
    app.similarTo = 0;
    for (HWND h : {app.s.tags, app.s.text, app.s.clip, app.s.minW, app.s.minKb, app.s.maxKb, app.s.path, app.s.download})
        SetWindowTextW(h, L"");
    SendMessageW(app.s.conf, TBM_SETPOS, TRUE, 50);
    setText(app.s.confValue, L"0.50");
    ComboBox_SetCurSel(app.s.rating, 0);
    ComboBox_SetCurSel(app.s.format, 0);
    DateTime_SetSystemtime(app.s.from, GDT_NONE, nullptr);
    DateTime_SetSystemtime(app.s.to, GDT_NONE, nullptr);
    Button_SetCheck(app.s.noTaken, BST_UNCHECKED);
    Button_SetCheck(app.s.unclassified, BST_UNCHECKED);
    Button_SetCheck(app.s.review, BST_UNCHECKED);
    app.uncheckedSources.clear();
    refreshSources();
    ComboBox_SetCurSel(app.s.smart, 0);
    EnableWindow(app.s.smartDelete, FALSE);
    scheduleSearch(10);
}

// 1 行の文字を入力してもらう（キャンセルなら nullopt）
std::optional<std::wstring> promptText(const std::wstring& title, const std::wstring& message, const std::wstring& initial) {
    struct State {
        HWND edit = nullptr;
        std::optional<std::wstring> result;
        bool done = false;
    } st;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            if (msg == WM_COMMAND && st) {
                if (LOWORD(wp) == IDOK) {
                    st->result = editText(st->edit);
                    st->done = true;
                } else if (LOWORD(wp) == IDCANCEL) {
                    st->done = true;
                }
                return 0;
            }
            if (msg == WM_CLOSE && st) {
                st->done = true;
                return 0;
            }
            return DefWindowProcW(h, msg, wp, lp);
        };
        wc.hInstance = app.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ImgdupePrompt";
        RegisterClassExW(&wc);
        registered = true;
    }
    const int w = px(380), h = px(150);
    RECT owner;
    GetWindowRect(app.hwnd, &owner);
    RECT frame{0, 0, w, h};
    AdjustWindowRectEx(&frame, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ImgdupePrompt", title.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               (owner.left + owner.right - fw) / 2, (owner.top + owner.bottom - fh) / 2, fw, fh, app.hwnd,
                               nullptr, app.inst, nullptr);
    if (!dlg) return std::nullopt;
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int cw, int ch,
                     DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cw, ch, dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
        return c;
    };
    const int m = px(12);
    child(WC_STATICW, message.c_str(), SS_NOPREFIX, -1, m, m, w - 2 * m, px(20));
    st.edit = child(WC_EDITW, initial.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 100, m, m + px(26), w - 2 * m, px(24),
                    WS_EX_CLIENTEDGE);
    child(WC_BUTTONW, L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, w - m - px(180), h - m - px(28), px(86), px(28));
    child(WC_BUTTONW, L"キャンセル", WS_TABSTOP, IDCANCEL, w - m - px(86), h - m - px(28), px(86), px(28));
    SendMessageW(st.edit, EM_SETSEL, 0, -1);
    EnableWindow(app.hwnd, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.edit);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            st.done = true;
            continue;
        }
        if (IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (msg.message == WM_QUIT) PostQuitMessage(static_cast<int>(msg.wParam));
    EnableWindow(app.hwnd, TRUE);
    DestroyWindow(dlg);
    SetActiveWindow(app.hwnd);
    return st.result;
}

// スマートフォルダの一覧を DB から作り直す（select の名前があれば選ぶ）
void refreshSmartFolders(const std::wstring& select) {
    std::wstring cur = select;
    if (cur.empty()) {
        const int i = ComboBox_GetCurSel(app.s.smart) - 1;
        if (i >= 0 && i < static_cast<int>(app.smartList.size())) cur = widen(app.smartList[i].name);
    }
    app.smartList.clear();
    if (Db* db = uiDb()) {
        try {
            app.smartList = smartFolders(*db);
        } catch (const std::exception&) {
        }
    }
    ComboBox_ResetContent(app.s.smart);
    ComboBox_AddString(app.s.smart, app.smartList.empty() ? L"（まだありません）" : L"（選ぶと条件を読み込みます）");
    int sel = 0;
    for (size_t i = 0; i < app.smartList.size(); ++i) {
        const std::wstring name = widen(app.smartList[i].name);
        ComboBox_AddString(app.s.smart, name.c_str());
        if (!cur.empty() && name == cur) sel = static_cast<int>(i) + 1;
    }
    ComboBox_SetCurSel(app.s.smart, sel);
    EnableWindow(app.s.smartDelete, sel > 0);
}

// 保存した条件を画面に戻す（画面に無い項目は使わない）
void applyQueryToUi(const SearchQuery& q) {
    std::wstring tags;
    for (const auto& t : q.tags) tags += (tags.empty() ? L"" : L" ") + widen(t);
    for (const auto& t : q.excludeTags) tags += (tags.empty() ? L"-" : L" -") + widen(t);
    SetWindowTextW(app.s.tags, tags.c_str());
    const int conf = std::clamp(static_cast<int>(q.minConfidence * 100 + 0.5), 0, 100);
    SendMessageW(app.s.conf, TBM_SETPOS, TRUE, conf);
    wchar_t v[16];
    swprintf(v, 16, L"%.2f", conf / 100.0);
    setText(app.s.confValue, v);
    app.uncheckedSources.clear();
    for (const auto& n : q.excludeSources) app.uncheckedSources.insert(n);
    if (!q.sources.empty())
        for (const auto& n : app.sourceNames)
            if (std::find(q.sources.begin(), q.sources.end(), n) == q.sources.end()) app.uncheckedSources.insert(n);
    refreshSources();
    SetWindowTextW(app.s.text, widen(q.text).c_str());
    app.similarTo = 0;
    if (q.similarTo) {
        ImageInfo info;
        Db* db = uiDb();
        std::wstring name = L"ID " + std::to_wstring(q.similarTo);
        if (db && imageInfo(*db, q.similarTo, info)) {
            const std::wstring p = widen(info.path);
            name = p.substr(p.find_last_of(L"\\/") + 1);
        }
        findSimilar(q.similarTo, name);
    } else {
        SetWindowTextW(app.s.clip, widen(q.clipText).c_str());
    }
    ComboBox_SetCurSel(app.s.rating, q.noRating ? 6 : q.minRating >= 1 && q.minRating <= 5 ? q.minRating : 0);
    int format = 0;
    if (q.animatedOnly) format = ComboBox_FindStringExact(app.s.format, -1, L"アニメーション");
    else if (!q.format.empty()) format = ComboBox_FindStringExact(app.s.format, -1, widen(q.format).c_str());
    ComboBox_SetCurSel(app.s.format, std::max(0, format));
    auto setDate = [](HWND h, int64_t t) {
        if (!t) {
            DateTime_SetSystemtime(h, GDT_NONE, nullptr);
            return;
        }
        int y, mo, d, hh, mi, ss;
        unixToCivil(t, y, mo, d, hh, mi, ss);
        SYSTEMTIME st{};
        st.wYear = static_cast<WORD>(y);
        st.wMonth = static_cast<WORD>(mo);
        st.wDay = static_cast<WORD>(d);
        DateTime_SetSystemtime(h, GDT_VALID, &st);
    };
    setDate(app.s.from, q.takenFrom);
    setDate(app.s.to, q.takenTo);
    Button_SetCheck(app.s.noTaken, q.noTaken ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(app.s.unclassified, q.unclassified ? BST_CHECKED : BST_UNCHECKED);
    auto setNum = [](HWND h, int64_t n) { SetWindowTextW(h, n > 0 ? std::to_wstring(n).c_str() : L""); };
    setNum(app.s.minW, q.minWidth);
    setNum(app.s.minKb, q.minSize / 1024);
    setNum(app.s.maxKb, q.maxSize / 1024);
    SetWindowTextW(app.s.path, widen(q.pathContains).c_str());
    SetWindowTextW(app.s.download, widen(q.downloadContains).c_str());
    static const SortKey sorts[] = {SortKey::Path,      SortKey::Modified, SortKey::Created, SortKey::Taken, SortKey::Resolution,
                                    SortKey::Size,      SortKey::Rating,   SortKey::Title,   SortKey::Confidence};
    for (size_t i = 0; i < std::size(sorts); ++i)
        if (sorts[i] == q.sort) ComboBox_SetCurSel(app.s.sort, static_cast<int>(i));
    Button_SetCheck(app.s.desc, q.descending ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(app.s.review, q.review ? BST_CHECKED : BST_UNCHECKED);
    scheduleSearch(10);
}

void saveSmartFolderUi() {
    Db* db = uiDb();
    if (!db) return;
    const int i = ComboBox_GetCurSel(app.s.smart) - 1;
    const std::wstring initial = i >= 0 && i < static_cast<int>(app.smartList.size()) ? widen(app.smartList[i].name) : L"";
    auto name = promptText(L"スマートフォルダに保存", L"名前（今の検索条件を保存します）", initial);
    if (!name) return;
    while (!name->empty() && iswspace(name->back())) name->pop_back();
    while (!name->empty() && iswspace(name->front())) name->erase(name->begin());
    if (name->empty()) return;
    const std::string n = narrow(*name);
    for (const auto& f : app.smartList) {
        if (_wcsicmp(widen(f.name).c_str(), name->c_str()) == 0 &&
            MessageBoxW(app.hwnd, (L"「" + *name + L"」の条件を今の条件で置き換えますか？").c_str(), L"imgdupe",
                        MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;
    }
    try {
        saveSmartFolder(*db, n, searchQueryFromUi());
    } catch (const std::exception& e) {
        MessageBoxW(app.hwnd, widen(e.what()).c_str(), L"imgdupe", MB_ICONWARNING);
        return;
    }
    refreshSmartFolders(*name);
}

void deleteSmartFolderUi() {
    Db* db = uiDb();
    const int i = ComboBox_GetCurSel(app.s.smart) - 1;
    if (!db || i < 0 || i >= static_cast<int>(app.smartList.size())) return;
    const std::wstring name = widen(app.smartList[i].name);
    if (MessageBoxW(app.hwnd, (L"スマートフォルダ「" + name + L"」を削除しますか？（画像は消えません）").c_str(), L"imgdupe",
                    MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    try {
        deleteSmartFolder(*db, app.smartList[i].id);
    } catch (const std::exception&) {
    }
    ComboBox_SetCurSel(app.s.smart, 0);
    refreshSmartFolders(L"");
}

// ---------------------------------------------------------------- ハードリンク展開

std::wstring trimmed(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    return s;
}

std::wstring localTimeText(int64_t t) {
    const time_t tt = static_cast<time_t>(t);
    struct tm lt {};
    if (localtime_s(&lt, &tt) != 0) return L"";
    wchar_t buf[32];
    swprintf(buf, 32, L"%04d-%02d-%02d %02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
    return buf;
}

std::atomic<bool> expandCancel{false};

void cleanupTemporaryLinks() {
    Db* db = uiDb();
    if (!db) return;
    try {
        removeLinks(*db, "", true);
    } catch (const std::exception&) {
    }
}

void expandLinksUi() {
    if (app.expanding) {
        MessageBoxW(app.hwnd, L"フォルダに展開しています。終わるまでお待ちください。", L"imgdupe", MB_ICONINFORMATION);
        return;
    }
    if (!app.searchTotal) {
        MessageBoxW(app.hwnd, L"展開する検索結果がありません。", L"imgdupe", MB_ICONINFORMATION);
        return;
    }
    const int i = ComboBox_GetCurSel(app.s.smart) - 1;
    const std::wstring initial = i >= 0 && i < static_cast<int>(app.smartList.size()) ? widen(app.smartList[i].name) : L"検索結果";
    auto input = promptText(L"フォルダに展開", L"フォルダの名前（画像のあるドライブごとに、_vfolders の下に作ります）", initial);
    if (!input || trimmed(*input).empty()) return;
    const std::string name = safeFolderName(narrow(trimmed(*input)));
    std::wstring msg = withCommas(app.searchTotal) + L" 枚を、ハードリンクで「" + widen(name) + L"」フォルダに並べます。\r\n\r\n" +
                       L"・容量は増えません。リンク側の画像を編集すると、元の画像も変わります。\r\n" +
                       L"・このソフトを終了すると消えます（残す時は「展開したフォルダ」で固定にします）。\r\n" +
                       L"・同じ名前で前に展開したフォルダは、今の結果で作り直します。";
    if (app.searchTotal > 10000) msg += L"\r\n\r\n枚数が多いので、少し時間がかかります。";
    if (MessageBoxW(app.hwnd, msg.c_str(), L"フォルダに展開", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return;

    SearchQuery q = searchQueryFromUi();
    const std::string db = narrow(dbPath());
    HWND hwnd = app.hwnd;
    app.expanding = true;
    expandCancel = false;
    setText(app.s.count, L"フォルダに展開しています…");
    EnableWindow(app.s.expand, FALSE);
    if (app.expandThread.joinable()) app.expandThread.join();
    app.expandThread = std::thread([q, db, name, hwnd]() mutable {
        auto* done = new ExpandDone;
        done->name = widen(name);
        try {
            Db d;
            if (!d.open(db)) throw std::runtime_error(d.error());
            q.limit = 0;
            done->result = expandLinks(d, search(d, q).ids, name, true, &expandCancel);
        } catch (const std::exception& e) {
            done->error = widen(e.what());
        }
        if (!PostMessageW(hwnd, WM_APP_EXPANDED, 0, reinterpret_cast<LPARAM>(done))) delete done;
    });
}

void onExpanded(ExpandDone* p) {
    std::unique_ptr<ExpandDone> done(p);
    app.expanding = false;
    EnableWindow(app.s.expand, TRUE);
    setText(app.s.count, withCommas(app.searchTotal) + L" 件");
    if (!done->error.empty()) {
        MessageBoxW(app.hwnd, (L"展開できませんでした: " + done->error).c_str(), L"imgdupe", MB_ICONWARNING);
        return;
    }
    const ExpandResult& r = done->result;
    std::wstring msg = withCommas(r.created) + L" 個のリンクを作りました。";
    for (const auto& f : r.folders) msg += L"\r\n" + widen(f);
    if (r.failed)
        msg += L"\r\n\r\n作れなかった画像: " + withCommas(r.failed) +
               L" 枚（元の画像が無い、ネットワークや FAT32 / exFAT のドライブなど）\r\n例: " + widen(r.firstFailure);
    if (!r.created) {
        MessageBoxW(app.hwnd, msg.c_str(), L"フォルダに展開", MB_ICONWARNING);
        return;
    }
    msg += L"\r\n\r\nフォルダを開きますか？";
    if (MessageBoxW(app.hwnd, msg.c_str(), L"フォルダに展開", MB_YESNO | MB_ICONINFORMATION) == IDYES)
        for (const auto& f : r.folders) openPath(widen(f));
}

// 展開したフォルダの一覧（固定にする・削除する・生成先を変える）
struct LinksWindow {
    HWND wnd = nullptr, list = nullptr, note = nullptr;
    std::vector<LinkFolder> folders;
    bool done = false;
} linksWin;

enum { IDC_L_LIST = 100, IDC_L_OPEN, IDC_L_KEEP, IDC_L_TEMP, IDC_L_DELETE, IDC_L_ROOT };

void refreshLinksWindow() {
    LinksWindow& w = linksWin;
    Db* db = uiDb();
    w.folders.clear();
    std::wstring note;
    if (db) {
        try {
            w.folders = linkFolders(*db);
            const auto orphans = orphanLinks(*db);
            if (!orphans.empty())
                note += L"元の画像が消えて、データがリンクにしか無いファイルが " + withCommas(orphans.size()) +
                        L" 個あります（リンクを消すまで容量は空きません）。例: " + widen(orphans[0]) + L"\r\n";
            const auto roots = customVfolderRoots(*db);
            note += L"生成先: 各ドライブの \\_vfolders";
            for (const auto& r : roots) note += L"、" + widen(r);
        } catch (const std::exception& e) {
            note = widen(e.what());
        }
    }
    setText(w.note, note);
    ListView_DeleteAllItems(w.list);
    for (size_t i = 0; i < w.folders.size(); ++i) {
        const LinkFolder& f = w.folders[i];
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = static_cast<int>(i);
        const std::wstring dir = widen(f.dir);
        it.pszText = const_cast<wchar_t*>(dir.c_str());
        ListView_InsertItem(w.list, &it);
        const std::wstring count = withCommas(f.count);
        const std::wstring kind = f.temporary == f.count ? L"一時" : f.temporary ? L"一部一時" : L"固定";
        const std::wstring when = f.created ? localTimeText(f.created) : L"";
        ListView_SetItemText(w.list, static_cast<int>(i), 1, const_cast<wchar_t*>(count.c_str()));
        ListView_SetItemText(w.list, static_cast<int>(i), 2, const_cast<wchar_t*>(kind.c_str()));
        ListView_SetItemText(w.list, static_cast<int>(i), 3, const_cast<wchar_t*>(when.c_str()));
    }
    if (!w.folders.empty()) ListView_SetItemState(w.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void linksCommand(int id) {
    LinksWindow& w = linksWin;
    Db* db = uiDb();
    if (id == IDCANCEL) {
        w.done = true;
        return;
    }
    if (!db) return;
    if (id == IDC_L_ROOT) {
        Com<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.put())))) return;
        DWORD opt = 0;
        dlg->GetOptions(&opt);
        dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"展開先を作るフォルダを選ぶ（その下に _vfolders を作ります）");
        Com<IShellItem> item;
        PWSTR p = nullptr;
        if (FAILED(dlg->Show(w.wnd)) || FAILED(dlg->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)))
            return;
        std::wstring base = p;
        CoTaskMemFree(p);
        while (base.size() > 3 && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
        const std::wstring root = base + (base.back() == L'\\' ? L"" : L"\\") + widen(kVfolderDirName);
        const std::string drive = driveOf(narrow(base));
        if (MessageBoxW(w.wnd, (widen(drive) + L" の画像は、これから\r\n" + root + L"\r\nの下に展開します。").c_str(), L"imgdupe",
                        MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;
        try {
            setVfolderRoot(*db, drive, narrow(root));
            imgwin::setScanExcludes(customVfolderRoots(*db));
        } catch (const std::exception& e) {
            MessageBoxW(w.wnd, widen(e.what()).c_str(), L"imgdupe", MB_ICONWARNING);
        }
        refreshLinksWindow();
        return;
    }
    const int sel = ListView_GetNextItem(w.list, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(w.folders.size())) return;
    const LinkFolder f = w.folders[sel];
    try {
        switch (id) {
            case IDC_L_OPEN: openPath(widen(f.dir)); return;
            case IDC_L_KEEP: setFolderTemporary(*db, f.dir, false); break;
            case IDC_L_TEMP: setFolderTemporary(*db, f.dir, true); break;
            case IDC_L_DELETE: {
                if (MessageBoxW(w.wnd, (L"「" + widen(f.dir) + L"」のリンク " + withCommas(f.count) + L" 個を削除しますか？\r\n（元の画像は消えません）").c_str(),
                                L"imgdupe", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                    return;
                const RemoveResult r = removeLinks(*db, f.dir, false);
                std::wstring msg;
                if (r.failed) msg += withCommas(r.failed) + L" 個は開かれていて消せませんでした（次に起動した時にまた消します）。\r\n";
                if (r.kept) msg += withCommas(r.kept) + L" 個は元の画像が消えていて、データがリンクにしか無いので残しました。\r\n";
                if (!msg.empty()) MessageBoxW(w.wnd, msg.c_str(), L"imgdupe", MB_ICONINFORMATION);
                break;
            }
            default: return;
        }
    } catch (const std::exception& e) {
        MessageBoxW(w.wnd, widen(e.what()).c_str(), L"imgdupe", MB_ICONWARNING);
    }
    refreshLinksWindow();
    if (sel < ListView_GetItemCount(w.list)) ListView_SetItemState(w.list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void showLinksWindow() {
    LinksWindow& w = linksWin;
    if (w.wnd) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            switch (msg) {
                case WM_COMMAND: linksCommand(LOWORD(wp)); return 0;
                case WM_CLOSE: linksWin.done = true; return 0;
                case WM_NOTIFY: {
                    auto* nm = reinterpret_cast<NMHDR*>(lp);
                    if (nm->idFrom == IDC_L_LIST && nm->code == NM_DBLCLK) linksCommand(IDC_L_OPEN);
                    return 0;
                }
            }
            return DefWindowProcW(h, msg, wp, lp);
        };
        wc.hInstance = app.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ImgdupeLinks";
        RegisterClassExW(&wc);
        registered = true;
    }
    const int cw = px(720), ch = px(400), m = px(10), bw = px(100), bh = px(28);
    RECT owner;
    GetWindowRect(app.hwnd, &owner);
    RECT frame{0, 0, cw, ch};
    AdjustWindowRectEx(&frame, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
    w = LinksWindow{};
    w.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ImgdupeLinks", L"展開したフォルダ", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            (owner.left + owner.right - fw) / 2, (owner.top + owner.bottom - fh) / 2, fw, fh, app.hwnd, nullptr,
                            app.inst, nullptr);
    if (!w.wnd) return;
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int cx, int cy, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy, w.wnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
        return c;
    };
    const int listW = cw - 3 * m - bw, noteH = px(40);
    w.list = child(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, IDC_L_LIST, m, m, listW,
                   ch - 3 * m - noteH, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(w.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const struct {
        const wchar_t* title;
        int width;
        int fmt;
    } cols[] = {{L"フォルダ", 330, LVCFMT_LEFT}, {L"数", 60, LVCFMT_RIGHT}, {L"種類", 60, LVCFMT_LEFT}, {L"作った日時", 120, LVCFMT_LEFT}};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = cols[i].fmt;
        c.cx = px(cols[i].width);
        c.pszText = const_cast<wchar_t*>(cols[i].title);
        ListView_InsertColumn(w.list, i, &c);
    }
    w.note = child(WC_STATICW, L"", SS_NOPREFIX, -1, m, ch - m - noteH, cw - 2 * m, noteH);
    const int bx = cw - m - bw;
    int by = m;
    for (auto [id, text] : std::initializer_list<std::pair<int, const wchar_t*>>{
             {IDC_L_OPEN, L"開く"}, {IDC_L_KEEP, L"固定にする"}, {IDC_L_TEMP, L"一時にする"}, {IDC_L_DELETE, L"削除"}}) {
        child(WC_BUTTONW, text, WS_TABSTOP, id, bx, by, bw, bh);
        by += bh + px(6);
    }
    by += px(12);
    child(WC_BUTTONW, L"生成先…", WS_TABSTOP, IDC_L_ROOT, bx, by, bw, bh);
    child(WC_BUTTONW, L"閉じる", WS_TABSTOP, IDCANCEL, bx, ch - 2 * m - noteH - bh, bw, bh);
    refreshLinksWindow();

    EnableWindow(app.hwnd, FALSE);
    ShowWindow(w.wnd, SW_SHOW);
    SetFocus(w.list);
    MSG msg;
    while (!w.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            w.done = true;
            continue;
        }
        if (IsDialogMessageW(w.wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (msg.message == WM_QUIT) PostQuitMessage(static_cast<int>(msg.wParam));
    EnableWindow(app.hwnd, TRUE);
    DestroyWindow(w.wnd);
    w.wnd = nullptr;
    SetActiveWindow(app.hwnd);
}

// ---------------------------------------------------------------- タグの確定（確認キュー・一括のタグ付け）

std::vector<int> selectedResults() {
    std::vector<int> out;
    for (int i = ListView_GetNextItem(app.s.results, -1, LVNI_SELECTED); i >= 0;
         i = ListView_GetNextItem(app.s.results, i, LVNI_SELECTED))
        if (i < static_cast<int>(app.searchIds.size())) out.push_back(i);
    return out;
}

// 選んだ画像のタグを書き換えて、検索し直す（同じ位置の画像を選び直す）
// edit(db, 画像 ID, history) は、タグを変える前に history.touch(画像 ID, タグ) を呼ぶ（取り消せるように）
template <typename F>
void editSelectedTags(const std::vector<int>& items, const std::string& description, F&& edit) {
    Db* db = uiDb();
    if (!db || items.empty()) return;
    try {
        db->begin();
        TagHistory history(*db);
        for (int i : items) edit(*db, app.searchIds[i], history);
        history.save(description);
        db->commit();
    } catch (const std::exception& e) {
        try {
            db->rollback();
        } catch (const std::exception&) {
        }
        MessageBoxW(app.hwnd, widen(e.what()).c_str(), L"imgdupe", MB_ICONWARNING);
        return;
    }
    app.reselect = items.front();
    refreshSources();
    scheduleSearch(10);
}

void markSelected(bool correct) {
    std::vector<std::string> tags, exclude;
    parseTagQuery(narrow(editText(app.s.tags)), tags, exclude);
    if (tags.empty()) {
        MessageBoxW(app.hwnd, L"「タグ」の欄に、確かめるタグを入れてください（1 つ目のタグを確定します）。", L"imgdupe",
                    MB_ICONINFORMATION);
        return;
    }
    const std::vector<int> items = selectedResults();
    if (items.empty()) return;
    const std::string tag = tags[0];
    const std::string desc = std::to_string(items.size()) + " 枚の " + tag + " を「" + (correct ? "正しい" : "違う") + "」に";
    editSelectedTags(items, desc, [&](Db& db, int64_t id, TagHistory& h) {
        h.touch(id, tag);
        setManualTag(db, id, tag, correct);
    });
}

void bulkTagUi() {
    const std::vector<int> items = selectedResults();
    if (items.empty()) {
        MessageBoxW(app.hwnd, L"タグを付ける画像を選んでください（Ctrl や Shift を押しながらクリックすると、いくつも選べます）。",
                    L"imgdupe", MB_ICONINFORMATION);
        return;
    }
    auto input = promptText(L"タグを付ける・外す",
                            withCommas(items.size()) + L" 枚に付けるタグ（空白で区切る。-タグ で「違う」と確定）", L"");
    if (!input) return;
    std::vector<std::string> add, remove;
    parseTagQuery(narrow(*input), add, remove);
    if (add.empty() && remove.empty()) return;
    editSelectedTags(items, std::to_string(items.size()) + " 枚に「" + narrow(trimmed(*input)) + "」",
                     [&](Db& db, int64_t id, TagHistory& h) {
                         for (const auto& t : add) {
                             h.touch(id, t);
                             setManualTag(db, id, t, true);
                         }
                         for (const auto& t : remove) {
                             h.touch(id, t);
                             setManualTag(db, id, t, false);
                         }
                     });
}

void undoTagEdit() {
    Db* db = uiDb();
    if (!db) return;
    std::string desc;
    bool done = false;
    try {
        db->begin();
        done = undoLastTagEdit(*db, desc);
        db->commit();
    } catch (const std::exception& e) {
        try {
            db->rollback();
        } catch (const std::exception&) {
        }
        MessageBoxW(app.hwnd, widen(e.what()).c_str(), L"imgdupe", MB_ICONWARNING);
        return;
    }
    if (!done) {
        MessageBoxW(app.hwnd, L"元に戻せるタグの操作はありません。", L"imgdupe", MB_ICONINFORMATION);
        return;
    }
    app.searchNotice = L"元に戻しました: " + widen(desc);
    app.reselect = std::max(0, ListView_GetNextItem(app.s.results, -1, LVNI_FOCUSED));
    refreshSources();
    scheduleSearch(10);
}

// ---------------------------------------------------------------- 設定の画面（imgdupe.ini と同じ内容を画面から変える）

struct SettingsWindow {
    HWND wnd = nullptr, autoUpdate = nullptr, autostart = nullptr, tagger = nullptr, clip = nullptr, gpu = nullptr, aiStatus = nullptr,
         variantSim = nullptr;
    bool done = false;
    int after = 0;  // 閉じた後に実行する機能（CMD_*。0 = なし）
} settingsWin;

enum { IDC_ST_AUTOUPDATE = 300, IDC_ST_AUTOSTART, IDC_ST_TAGGER, IDC_ST_CLIP, IDC_ST_VARIANT, IDC_ST_GPU, IDC_ST_MODELS, IDC_ST_LINKS, IDC_ST_KEYS, IDC_ST_INI };

// 画面の内容を imgdupe.ini などに書く
void applySettings() {
    SettingsWindow& w = settingsWin;
    iniSet(L"autoUpdateCheck", Button_GetCheck(w.autoUpdate) == BST_CHECKED ? L"1" : L"0");
    const bool autostart = Button_GetCheck(w.autostart) == BST_CHECKED;
    if (autostart != indexer::autoStartEnabled()) {
        indexer::setAutoStart(autostart);
        if (autostart && !indexer::isRunning()) {
            saveSettings();
            indexer::start();
        }
    }
    const int tagger = Button_GetCheck(w.tagger) == BST_CHECKED ? 1 : 0;
    const int clipOn = Button_GetCheck(w.clip) == BST_CHECKED ? 1 : 0;
    const int cpu = Button_GetCheck(w.gpu) == BST_CHECKED ? 0 : 1;
    const bool changed = tagger != iniInt(L"taggerEnabled", 1) || clipOn != iniInt(L"clipEnabled", 1) || cpu != iniInt(L"taggerCpu", 0);
    iniSet(L"taggerEnabled", std::to_wstring(tagger));
    iniSet(L"clipEnabled", std::to_wstring(clipOn));
    iniSet(L"taggerCpu", std::to_wstring(cpu));
    // 差分とみなす似ている度合い（0.80〜0.99。外れていたら既定に戻す）
    const double sim = _wtof(editText(w.variantSim).c_str());
    wchar_t v[16];
    swprintf(v, 16, L"%.2f", sim >= 0.8 && sim <= 0.99 ? sim : kDefaultVariantSimilarity);
    iniSet(L"variantSimilarity", v);
    if (changed) restartIndexerAfterUpdate();  // 動いているバックグラウンドの解析に新しい設定を使わせる
}

void settingsCommand(int id, int code) {
    SettingsWindow& w = settingsWin;
    switch (id) {
        case IDOK:
            applySettings();
            w.done = true;
            break;
        case IDCANCEL: w.done = true; break;
        case IDC_ST_TAGGER:
        case IDC_ST_CLIP:
            if (code == BN_CLICKED)
                EnableWindow(w.gpu, Button_GetCheck(w.tagger) == BST_CHECKED || Button_GetCheck(w.clip) == BST_CHECKED);
            break;
        // 別の画面を開くものは、設定を保存して閉じてから開く
        case IDC_ST_MODELS: applySettings(); w.after = CMD_MODELS; w.done = true; break;
        case IDC_ST_LINKS: applySettings(); w.after = CMD_LINKS; w.done = true; break;
        case IDC_ST_KEYS: applySettings(); w.after = CMD_KEY_SETTINGS; w.done = true; break;
        case IDC_ST_INI:
            applySettings();
            ShellExecuteW(w.wnd, L"open", L"notepad.exe", (L"\"" + iniPath() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            break;
    }
}

void showSettingsWindow() {
    SettingsWindow& w = settingsWin;
    if (w.wnd) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            if (msg == WM_COMMAND) {
                settingsCommand(LOWORD(wp), HIWORD(wp));
                return 0;
            }
            if (msg == WM_CLOSE) {
                settingsWin.done = true;
                return 0;
            }
            return DefWindowProcW(h, msg, wp, lp);
        };
        wc.hInstance = app.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ImgdupeSettings";
        RegisterClassExW(&wc);
        registered = true;
    }
    const int cw = px(560), ch = px(440), m = px(12);
    RECT owner;
    GetWindowRect(app.hwnd, &owner);
    RECT frame{0, 0, cw, ch};
    AdjustWindowRectEx(&frame, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
    w = SettingsWindow{};
    w.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ImgdupeSettings", L"設定", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            (owner.left + owner.right - fw) / 2, (owner.top + owner.bottom - fh) / 2, fw, fh, app.hwnd, nullptr,
                            app.inst, nullptr);
    if (!w.wnd) return;
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int cx, int cy) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy, w.wnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
        return c;
    };
    const int gw = cw - 2 * m, in = m + px(12), iw = gw - px(24), rowH = px(22), bh = px(28);
    int y = m;
    auto group = [&](const wchar_t* title, int h) {
        child(WC_BUTTONW, title, BS_GROUPBOX, -1, m, y, gw, h);
        const int top = y + px(22);
        y += h + px(8);
        return top;
    };
    int gy = group(L"アップデート", px(52));
    w.autoUpdate = child(WC_BUTTONW, L"終了する時に新しい版を確かめる（1 日 1 回）", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_ST_AUTOUPDATE,
                         in, gy, iw, rowH);
    gy = group(L"バックグラウンドの解析", px(52));
    w.autostart = child(WC_BUTTONW, L"Windows と一緒に起動する", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_ST_AUTOSTART, in, gy, iw, rowH);
    gy = group(L"AI 解析", px(174));
    w.tagger = child(WC_BUTTONW, L"WD14 タガーで画像にタグを付ける（検索用のタグ。ファイルには書き込みません）",
                     WS_TABSTOP | BS_AUTOCHECKBOX, IDC_ST_TAGGER, in, gy, iw, rowH);
    w.clip = child(WC_BUTTONW, L"CLIP で画像の内容を調べる（似た画像を探す・言葉で探す）", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_ST_CLIP,
                   in, gy + rowH + px(2), iw, rowH);
    child(WC_STATICW, L"差分とみなす似ている度合い（0.80〜0.99。大きいほど厳しい）", SS_NOPREFIX, -1, in + px(18),
          gy + 2 * (rowH + px(2)) + px(4), iw - px(90), rowH);
    w.variantSim = child(WC_EDITW, L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, IDC_ST_VARIANT, in + iw - px(60),
                         gy + 2 * (rowH + px(2)), px(60), rowH);
    w.gpu = child(WC_BUTTONW, L"GPU（DirectML）を使う（調子が悪い時は外すと CPU だけで動きます）", WS_TABSTOP | BS_AUTOCHECKBOX,
                  IDC_ST_GPU, in, gy + 3 * (rowH + px(2)), iw, rowH);
    w.aiStatus = child(WC_STATICW, L"", SS_NOPREFIX, -1, in, gy + 4 * (rowH + px(2)) + px(6), iw - px(150), px(36));
    child(WC_BUTTONW, L"AI 解析の部品…", WS_TABSTOP, IDC_ST_MODELS, in + iw - px(140), gy + 4 * (rowH + px(2)) + px(4), px(140), bh);
    gy = group(L"そのほか", px(60));
    const int b3 = (iw - 2 * px(8)) / 3;
    child(WC_BUTTONW, L"ショートカットキー…", WS_TABSTOP, IDC_ST_KEYS, in, gy, b3, bh);
    child(WC_BUTTONW, L"展開したフォルダ…", WS_TABSTOP, IDC_ST_LINKS, in + b3 + px(8), gy, b3, bh);
    child(WC_BUTTONW, L"設定ファイルを開く", WS_TABSTOP, IDC_ST_INI, in + 2 * (b3 + px(8)), gy, b3, bh);
    child(WC_BUTTONW, L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, cw - m - px(208), ch - m - bh, px(100), bh);
    child(WC_BUTTONW, L"キャンセル", WS_TABSTOP, IDCANCEL, cw - m - px(100), ch - m - bh, px(100), bh);

    // 今の設定を出す
    Button_SetCheck(w.autoUpdate, iniInt(L"autoUpdateCheck", 1) ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(w.autostart, indexer::autoStartEnabled() ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(w.tagger, iniInt(L"taggerEnabled", 1) ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(w.clip, iniInt(L"clipEnabled", 1) ? BST_CHECKED : BST_UNCHECKED);
    setText(w.variantSim, [&] {
        wchar_t v[16];
        swprintf(v, 16, L"%.2f", clip::variantSimilarity(app.dataDir));
        return std::wstring(v);
    }());
    Button_SetCheck(w.gpu, iniInt(L"taggerCpu", 0) ? BST_UNCHECKED : BST_CHECKED);
    EnableWindow(w.gpu, Button_GetCheck(w.tagger) == BST_CHECKED || Button_GetCheck(w.clip) == BST_CHECKED);
    {
        const imgdupe::Installed installed = models::loadInstalled(app.dataDir);
        auto wd = installed.find("wd14");
        std::wstring status;
        if (installed.count("onnxruntime")) {
            if (wd != installed.end()) status += L"WD14 タガー（版 " + widen(wd->second.version) + L"）";
            if (auto c = installed.find("clip"); c != installed.end())
                status += (status.empty() ? L"" : L"、") + std::wstring(L"CLIP（版 ") + widen(c->second.version) + L"）";
        }
        setText(w.aiStatus, status.empty() ? L"AI 解析の部品はまだ入っていません" : status + L" が入っています");
    }

    EnableWindow(app.hwnd, FALSE);
    ShowWindow(w.wnd, SW_SHOW);
    SetFocus(w.autoUpdate);
    MSG msg;
    while (!w.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            w.done = true;
            continue;
        }
        if (IsDialogMessageW(w.wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (msg.message == WM_QUIT) PostQuitMessage(static_cast<int>(msg.wParam));
    EnableWindow(app.hwnd, TRUE);
    DestroyWindow(w.wnd);
    w.wnd = nullptr;
    SetActiveWindow(app.hwnd);
    if (w.after) PostMessageW(app.hwnd, WM_COMMAND, static_cast<WPARAM>(w.after), 0);
}

void layout();

void switchTab(int t) {
    app.tab = t;
    TabCtrl_SetCurSel(app.tabs, t);
    for (HWND h : app.dupPage) ShowWindow(h, t == 0 ? SW_SHOW : SW_HIDE);
    for (HWND h : app.searchPage) ShowWindow(h, t == 1 ? SW_SHOW : SW_HIDE);
    layout();
    if (t == 1) {
        refreshSources();
        refreshSmartFolders(L"");
        if (!app.searchShown) {
            app.searchShown = true;
            scheduleSearch(10);
        }
        showSearchInfo(ListView_GetNextItem(app.s.results, -1, LVNI_SELECTED));
        SetFocus(app.s.tags);
    } else {
        showInfo(ListView_GetNextItem(app.thumbs, -1, LVNI_SELECTED));
    }
}

void layoutSearch(int y, int W, int bottom) {
    const int m = px(8), panelW = px(300), rowH = px(24), labelH = px(18), gap = px(6);
    int x = m, cy = y;
    size_t li = 0;
    auto label = [&](int ly, int lx, int w) {
        if (li < app.s.labels.size()) MoveWindow(app.s.labels[li++], lx, ly, w, labelH, TRUE);
    };
    auto full = [&](HWND h, int height) {
        label(cy, x, panelW);
        cy += labelH + px(2);
        MoveWindow(h, x, cy, panelW, height, TRUE);
        cy += height + gap;
    };
    // スマートフォルダ
    label(cy, x, panelW);
    cy += labelH + px(2);
    const int bw = px(56);
    MoveWindow(app.s.smart, x, cy, panelW - 2 * (bw + px(4)), px(300), TRUE);
    MoveWindow(app.s.smartSave, x + panelW - 2 * bw - px(4), cy, bw, rowH, TRUE);
    MoveWindow(app.s.smartDelete, x + panelW - bw, cy, bw, rowH, TRUE);
    cy += rowH + gap;
    full(app.s.tags, rowH);
    // 確度
    label(cy, x, panelW);
    cy += labelH;
    MoveWindow(app.s.conf, x, cy, panelW - px(50), rowH + px(4), TRUE);
    MoveWindow(app.s.confValue, x + panelW - px(46), cy + px(4), px(46), labelH, TRUE);
    cy += rowH + px(4) + gap;
    full(app.s.sources, px(72));
    // 文字・内容（AI）
    {
        const int hw = (panelW - gap) / 2;
        label(cy, x, hw);
        label(cy, x + hw + gap, hw);
        cy += labelH + px(2);
        MoveWindow(app.s.text, x, cy, hw, rowH, TRUE);
        MoveWindow(app.s.clip, x + hw + gap, cy, hw, rowH, TRUE);
        cy += rowH + gap;
    }
    // 星・形式
    const int half = (panelW - gap) / 2;
    label(cy, x, half);
    label(cy, x + half + gap, half);
    cy += labelH + px(2);
    MoveWindow(app.s.rating, x, cy, half, px(240), TRUE);
    MoveWindow(app.s.format, x + half + gap, cy, half, px(240), TRUE);
    cy += rowH + gap;
    // 撮影日
    label(cy, x, panelW);
    cy += labelH + px(2);
    const int dw = (panelW - px(24)) / 2;
    MoveWindow(app.s.from, x, cy, dw, rowH, TRUE);
    label(cy + px(4), x + dw + px(4), px(16));
    MoveWindow(app.s.to, x + panelW - dw, cy, dw, rowH, TRUE);
    cy += rowH + px(2);
    MoveWindow(app.s.noTaken, x, cy, half, rowH, TRUE);
    MoveWindow(app.s.unclassified, x + half + gap, cy, half, rowH, TRUE);
    cy += rowH + gap;
    // 幅・ファイルサイズ
    label(cy, x, px(90));
    label(cy, x + px(100), panelW - px(100));
    cy += labelH + px(2);
    MoveWindow(app.s.minW, x, cy, px(80), rowH, TRUE);
    MoveWindow(app.s.minKb, x + px(100), cy, px(80), rowH, TRUE);
    label(cy + px(4), x + px(184), px(16));
    MoveWindow(app.s.maxKb, x + px(204), cy, px(80), rowH, TRUE);
    cy += rowH + gap;
    // パス・ダウンロード元
    label(cy, x, half);
    label(cy, x + half + gap, half);
    cy += labelH + px(2);
    MoveWindow(app.s.path, x, cy, half, rowH, TRUE);
    MoveWindow(app.s.download, x + half + gap, cy, half, rowH, TRUE);
    cy += rowH + gap;
    // 並べ替え
    label(cy, x, panelW);
    cy += labelH + px(2);
    MoveWindow(app.s.sort, x, cy, panelW - px(80), px(300), TRUE);
    MoveWindow(app.s.desc, x + panelW - px(72), cy, px(72), rowH, TRUE);

    // 右: 件数・条件をクリア・結果
    const int rx = 2 * m + panelW, rw = std::max(0, W - rx - m), cw = px(110), top = rowH + px(4);
    // 右から: 条件をクリア・フォルダに展開・タグ…・違う・正しい・確認キュー
    int bx = rx + rw;
    auto right = [&](HWND h, int w) {
        bx -= w;
        MoveWindow(h, bx, y, w, rowH, TRUE);
        bx -= gap;
    };
    right(app.s.clear, cw);
    right(app.s.expand, cw);
    bx -= gap;
    right(app.s.bulk, px(70));
    right(app.s.no, px(56));
    right(app.s.yes, px(56));
    right(app.s.review, px(96));
    MoveWindow(app.s.count, rx, y + (rowH - labelH) / 2, std::max(0, bx - rx), labelH, TRUE);
    MoveWindow(app.s.results, rx, y + top, rw, std::max(0, bottom - y - top), TRUE);
}

void createSearchControls() {
    auto label = [&](const wchar_t* text) {
        HWND h = makeControl(WC_STATICW, text, SS_NOPREFIX, IDC_S_LABEL);
        app.s.labels.push_back(h);
        app.searchPage.push_back(h);
    };
    auto edit = [&](int id, DWORD style = 0) {
        HWND h = makeControl(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL | style, id, WS_EX_CLIENTEDGE);
        app.searchPage.push_back(h);
        return h;
    };
    auto combo = [&](int id, std::initializer_list<const wchar_t*> items) {
        HWND h = makeControl(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, id);
        for (const wchar_t* t : items) ComboBox_AddString(h, t);
        ComboBox_SetCurSel(h, 0);
        app.searchPage.push_back(h);
        return h;
    };
    // 見出しの順は layoutSearch で並べる順と同じ
    label(L"スマートフォルダ（保存した条件）");
    app.s.smart = combo(IDC_S_SMART, {});
    app.s.smartSave = makeControl(WC_BUTTONW, L"保存…", WS_TABSTOP, IDC_S_SMART_SAVE);
    app.s.smartDelete = makeControl(WC_BUTTONW, L"削除", WS_TABSTOP, IDC_S_SMART_DELETE);
    app.searchPage.insert(app.searchPage.end(), {app.s.smartSave, app.s.smartDelete});
    label(L"タグ（空白で区切る。-タグ で除外）");
    app.s.tags = edit(IDC_S_TAGS);
    label(L"確度（右へ動かすほど確実なものだけ）");
    app.s.conf = makeControl(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, IDC_S_CONF);
    SendMessageW(app.s.conf, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(app.s.conf, TBM_SETPOS, TRUE, 50);
    app.s.confValue = makeControl(WC_STATICW, L"0.50", SS_RIGHT, IDC_S_CONF_VALUE);
    app.searchPage.insert(app.searchPage.end(), {app.s.conf, app.s.confValue});
    label(L"出どころ（チェックしたものだけで確度を計算）");
    app.s.sources = makeControl(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL,
                                IDC_S_SOURCES, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(app.s.sources, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    addColumn(app.s.sources, 0, L"", 270);
    app.searchPage.push_back(app.s.sources);
    label(L"文字（タイトルなど）");
    label(L"内容（AI。夕焼けの海 など）");
    app.s.text = edit(IDC_S_TEXT);
    app.s.clip = edit(IDC_S_CLIP);
    label(L"星");
    label(L"形式");
    app.s.rating = combo(IDC_S_RATING, {L"指定なし", L"★1 以上", L"★2 以上", L"★3 以上", L"★4 以上", L"★5", L"星なし"});
    app.s.format = combo(IDC_S_FORMAT, {L"すべて", L"JPEG", L"PNG", L"GIF", L"WebP", L"HEIF", L"BMP", L"TIFF", L"JPEG XR",
                                        L"アニメーション"});
    label(L"撮影日（チェックを入れた日だけ使う）");
    label(L"〜");
    app.s.from = makeControl(DATETIMEPICK_CLASSW, L"", WS_TABSTOP | DTS_SHORTDATEFORMAT | DTS_SHOWNONE, IDC_S_FROM);
    app.s.to = makeControl(DATETIMEPICK_CLASSW, L"", WS_TABSTOP | DTS_SHORTDATEFORMAT | DTS_SHOWNONE, IDC_S_TO);
    DateTime_SetSystemtime(app.s.from, GDT_NONE, nullptr);
    DateTime_SetSystemtime(app.s.to, GDT_NONE, nullptr);
    app.s.noTaken = makeControl(WC_BUTTONW, L"撮影日なし（EXIF なし）", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_S_NOTAKEN);
    app.s.unclassified = makeControl(WC_BUTTONW, L"未分類（タグなし）", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_S_UNCLASSIFIED);
    app.searchPage.insert(app.searchPage.end(), {app.s.from, app.s.to, app.s.noTaken, app.s.unclassified});
    label(L"幅（px）以上");
    label(L"ファイルサイズ（KB）");
    app.s.minW = edit(IDC_S_MINW, ES_NUMBER);
    label(L"〜");
    app.s.minKb = edit(IDC_S_MINKB, ES_NUMBER);
    app.s.maxKb = edit(IDC_S_MAXKB, ES_NUMBER);
    label(L"パス（一部）");
    label(L"ダウンロード元（URL）");
    app.s.path = edit(IDC_S_PATH);
    app.s.download = edit(IDC_S_DOWNLOAD);
    label(L"並べ替え");
    app.s.sort = combo(IDC_S_SORT, {L"パス", L"更新日時", L"作成日時", L"撮影日時", L"解像度", L"ファイルサイズ", L"星",
                                    L"タイトル", L"確度（1 つ目のタグ）"});
    app.s.desc = makeControl(WC_BUTTONW, L"降順", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_S_DESC);
    app.s.clear = makeControl(WC_BUTTONW, L"条件をクリア", WS_TABSTOP, IDC_S_CLEAR);
    app.s.expand = makeControl(WC_BUTTONW, L"フォルダに展開…", WS_TABSTOP, IDC_S_EXPAND);
    app.s.review = makeControl(WC_BUTTONW, L"確認キュー", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_S_REVIEW);
    app.s.yes = makeControl(WC_BUTTONW, L"正しい", WS_TABSTOP, IDC_S_YES);
    app.s.no = makeControl(WC_BUTTONW, L"違う", WS_TABSTOP, IDC_S_NO);
    app.s.bulk = makeControl(WC_BUTTONW, L"タグ…", WS_TABSTOP, IDC_S_BULK);
    app.searchPage.insert(app.searchPage.end(), {app.s.expand, app.s.review, app.s.yes, app.s.no, app.s.bulk});
    app.s.count = makeControl(WC_STATICW, L"", SS_NOPREFIX | SS_ENDELLIPSIS, IDC_S_COUNT);
    app.searchPage.insert(app.searchPage.end(), {app.s.desc, app.s.clear, app.s.count});

    app.s.results = makeControl(WC_LISTVIEWW, L"",
                                WS_TABSTOP | LVS_ICON | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_AUTOARRANGE,
                                IDC_S_RESULTS, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(app.s.results, LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
    ListView_SetIconSpacing(app.s.results, app.thumbSize + px(20), app.thumbSize + px(44));
    app.views[kSearchView].lv = app.s.results;
    resetView(kSearchView, 0);
    app.searchPage.push_back(app.s.results);
    for (HWND h : app.searchPage) ShowWindow(h, SW_HIDE);
}

// 検索の画面の通知（処理したら true）
bool onSearchNotify(NMHDR* nm) {
    if (nm->hwndFrom == app.s.results) {
        switch (nm->code) {
            case LVN_GETDISPINFOW: {
                auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
                const ImageInfo* info = resultInfo(di->item.iItem);
                if (!info) return true;
                if (di->item.mask & LVIF_TEXT) {
                    std::wstring t = stars(info->rating);
                    if (!t.empty()) t += L"  ";
                    t += sizeText(static_cast<uint64_t>(info->size)) + L"  " + std::to_wstring(info->width) + L"×" +
                         std::to_wstring(info->height) + L"  " + fileName(info->path);
                    copyOut(di->item, t);
                }
                if (di->item.mask & LVIF_IMAGE)
                    di->item.iImage = thumbImage(kSearchView, di->item.iItem, widen(info->path), info->id);
                return true;
            }
            case LVN_GETINFOTIPW: {
                auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(nm);
                const ImageInfo* info = resultInfo(tip->iItem);
                if (info && tip->pszText && tip->cchTextMax > 0) lstrcpynW(tip->pszText, widen(info->path).c_str(), tip->cchTextMax);
                return true;
            }
            case LVN_ITEMCHANGED: {
                auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
                if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED)) showSearchInfo(lv->iItem);
                return true;
            }
            case NM_DBLCLK: {
                const ImageInfo* info = resultInfo(reinterpret_cast<NMITEMACTIVATE*>(nm)->iItem);
                if (info) openPath(widen(info->path));
                return true;
            }
            case NM_RCLICK: {
                auto* ia = reinterpret_cast<NMITEMACTIVATE*>(nm);
                const ImageInfo* info = resultInfo(ia->iItem);
                if (!info) return true;
                ListView_SetItemState(app.s.results, ia->iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                imageContextMenu(widen(info->path), info->id);
                return true;
            }
        }
        return false;
    }
    if (nm->hwndFrom == app.s.sources && nm->code == LVN_ITEMCHANGED && !app.fillingSources) {
        auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
        if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) && lv->iItem >= 0 &&
            lv->iItem < static_cast<int>(app.sourceNames.size())) {
            const std::string& name = app.sourceNames[lv->iItem];
            if (ListView_GetCheckState(app.s.sources, lv->iItem)) app.uncheckedSources.erase(name);
            else app.uncheckedSources.insert(name);
            scheduleSearch(80);
        }
        return true;
    }
    if ((nm->hwndFrom == app.s.from || nm->hwndFrom == app.s.to) && nm->code == DTN_DATETIMECHANGE) {
        scheduleSearch(150);
        return true;
    }
    if (nm->hwndFrom == app.tabs && nm->code == TCN_SELCHANGE) {
        switchTab(TabCtrl_GetCurSel(app.tabs));
        return true;
    }
    return false;
}

// 検索の画面のコマンド（処理したら true）
bool onSearchCommand(WPARAM wp) {
    const int id = LOWORD(wp), code = HIWORD(wp);
    switch (id) {
        case IDC_S_CLIP:
            if (code == EN_CHANGE) {
                if (app.similarTo && editText(app.s.clip) != app.similarLabel) app.similarTo = 0;  // 書き換えたら言葉で探す
                scheduleSearch(500);  // 言葉のベクトル化は少し重いので、長めに待つ
            }
            return true;
        case IDC_S_TAGS: case IDC_S_TEXT: case IDC_S_MINW: case IDC_S_MINKB: case IDC_S_MAXKB: case IDC_S_PATH:
        case IDC_S_DOWNLOAD:
            if (code == EN_CHANGE) scheduleSearch(300);  // 打ち終わるのを少し待つ
            return true;
        case IDC_S_RATING: case IDC_S_FORMAT: case IDC_S_SORT:
            if (code == CBN_SELCHANGE) scheduleSearch(30);
            return true;
        case IDC_S_NOTAKEN: case IDC_S_DESC: case IDC_S_UNCLASSIFIED:
            scheduleSearch(30);
            return true;
        case IDC_S_CLEAR:
            clearSearchConditions();
            return true;
        case IDC_S_SMART:
            // 開いた一覧の中で動かしている間は読み込まない（選び終えた時か、閉じたままキーで変えた時）
            if (code == CBN_SELENDOK || (code == CBN_SELCHANGE && !ComboBox_GetDroppedState(app.s.smart))) {
                const int i = ComboBox_GetCurSel(app.s.smart) - 1;
                if (i >= 0 && i < static_cast<int>(app.smartList.size())) applyQueryToUi(app.smartList[i].query);
                EnableWindow(app.s.smartDelete, i >= 0);
            }
            return true;
        case IDC_S_SMART_SAVE:
            saveSmartFolderUi();
            return true;
        case IDC_S_EXPAND:
            expandLinksUi();
            return true;
        case IDC_S_REVIEW:
            scheduleSearch(30);
            return true;
        case IDC_S_YES:
        case IDC_S_NO:
            markSelected(id == IDC_S_YES);
            return true;
        case IDC_S_BULK:
            bulkTagUi();
            return true;
        case IDC_S_SMART_DELETE:
            deleteSmartFolderUi();
            return true;
    }
    return false;
}

void saveSearchSettings() {
    WritePrivateProfileStringW(L"Search", L"tags", editText(app.s.tags).c_str(), iniPath().c_str());
    WritePrivateProfileStringW(L"Search", L"text", editText(app.s.text).c_str(), iniPath().c_str());
    WritePrivateProfileStringW(L"Search", L"clip", app.similarTo ? L"" : editText(app.s.clip).c_str(), iniPath().c_str());
    WritePrivateProfileStringW(L"Search", L"confidence",
                               std::to_wstring(SendMessageW(app.s.conf, TBM_GETPOS, 0, 0)).c_str(), iniPath().c_str());
    WritePrivateProfileStringW(L"Search", L"sort", std::to_wstring(ComboBox_GetCurSel(app.s.sort)).c_str(), iniPath().c_str());
    WritePrivateProfileStringW(L"Search", L"descending", Button_GetCheck(app.s.desc) == BST_CHECKED ? L"1" : L"0",
                               iniPath().c_str());
    std::wstring unchecked;
    for (const auto& n : app.uncheckedSources) unchecked += widen(n) + L"|";
    WritePrivateProfileStringW(L"Search", L"uncheckedSources", unchecked.empty() ? nullptr : unchecked.c_str(),
                               iniPath().c_str());
    iniSet(L"tab", std::to_wstring(app.tab));
}

void loadSearchSettings() {
    wchar_t buf[4096];
    GetPrivateProfileStringW(L"Search", L"tags", L"", buf, 4096, iniPath().c_str());
    SetWindowTextW(app.s.tags, buf);
    GetPrivateProfileStringW(L"Search", L"text", L"", buf, 4096, iniPath().c_str());
    SetWindowTextW(app.s.text, buf);
    GetPrivateProfileStringW(L"Search", L"clip", L"", buf, 4096, iniPath().c_str());
    SetWindowTextW(app.s.clip, buf);
    const int conf = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Search", L"confidence", 50, iniPath().c_str())), 0, 100);
    SendMessageW(app.s.conf, TBM_SETPOS, TRUE, conf);
    wchar_t v[16];
    swprintf(v, 16, L"%.2f", conf / 100.0);
    setText(app.s.confValue, v);
    ComboBox_SetCurSel(app.s.sort, std::max(0, static_cast<int>(GetPrivateProfileIntW(L"Search", L"sort", 0, iniPath().c_str()))));
    Button_SetCheck(app.s.desc, GetPrivateProfileIntW(L"Search", L"descending", 0, iniPath().c_str()) ? BST_CHECKED : BST_UNCHECKED);
    GetPrivateProfileStringW(L"Search", L"uncheckedSources", L"", buf, 4096, iniPath().c_str());
    app.uncheckedSources.clear();
    std::wstring all = buf;
    size_t a = 0;
    while (a < all.size()) {
        size_t b = all.find(L'|', a);
        if (b == std::wstring::npos) b = all.size();
        if (b > a) app.uncheckedSources.insert(narrow(all.substr(a, b - a)));
        a = b + 1;
    }
}

// ---------------------------------------------------------------- 画面

void layoutSearch(int y, int W, int bottom);

void layout() {
    RECT rc;
    GetClientRect(app.hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int m = px(8), btnW = px(150), btnH = px(28), rowH = px(26);
    SendMessageW(app.toolbar, TB_AUTOSIZE, 0, 0);
    RECT tr;
    GetWindowRect(app.toolbar, &tr);
    int y = (tr.bottom - tr.top) + px(2);
    // 画面の切り替えのタブ（タブの見出しだけを使う）
    const int tabH = px(26);
    MoveWindow(app.tabs, m, y, W - 2 * m, tabH, TRUE);
    y += tabH + px(6);
    const int infoH = px(72);  // 4 行（パス・画像の情報・ファイルのタグ・検索用タグ）
    const int bottom = H - m - infoH - px(4);
    MoveWindow(app.info, m, bottom + px(4), W - 2 * m, infoH, TRUE);
    if (app.tab == 1) {
        layoutSearch(y, W, bottom);
        return;
    }
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
    x += px(204);
    MoveWindow(app.variants, x, y + px(2), px(220), rowH, TRUE);
    y += rowH + px(10);

    MoveWindow(app.progress, m, y + px(3), px(220), px(16), TRUE);
    MoveWindow(app.status, m + px(228), y + px(1), W - 2 * m - px(228), px(20), TRUE);
    y += px(26);

    const int listW = std::max(px(260), (W - 3 * m) * 2 / 5);
    MoveWindow(app.groups, m, y, listW, std::max(0, bottom - y), TRUE);
    MoveWindow(app.thumbs, 2 * m + listW, y, std::max(0, W - 3 * m - listW), std::max(0, bottom - y), TRUE);
}

HWND makeControl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle) {
    HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, app.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.inst, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
    return h;
}

void addColumn(HWND lv, int i, const wchar_t* title, int width, int fmt) {
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
    {CMD_ADD_FOLDER, 0xE8F4, L"追", L"フォルダを追加", true},
    {CMD_SEARCH, 0xE721, L"検", L"検索開始", true},
    {CMD_QUERY, 0xE8B9, L"1", L"1 枚で検索", true},
    {CMD_EXPORT, 0xE74E, L"保", L"結果を保存", false},
    {0, 0, nullptr, nullptr, false},
    {CMD_OPEN, 0xE8E5, L"開", L"開く", false},
    {CMD_EXPLORER, 0xE838, L"場", L"エクスプローラーで表示", false},
    {CMD_COPY_PATH, 0xE8C8, L"複", L"パスをコピー", false},
    {0, 0, nullptr, nullptr, false},
    {CMD_KEY_SETTINGS, 0xE765, L"鍵", L"ショートカットキーの設定", true},
    {CMD_CHECK_UPDATE, 0xE895, L"更", L"アップデートを確認", true},
    {CMD_HELP, 0xE897, L"?", L"使い方", true},
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
            if (d.id == CMD_SEARCH) startImage = b.iBitmap;
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

void createSearchControls();

void createControls() {
    createToolbar();
    app.tabs = makeControl(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS | TCS_FOCUSNEVER, IDC_TABS);
    for (const wchar_t* t : {L"重複を探す", L"検索"}) {
        TCITEMW it{};
        it.mask = TCIF_TEXT;
        it.pszText = const_cast<wchar_t*>(t);
        TabCtrl_InsertItem(app.tabs, TabCtrl_GetItemCount(app.tabs), &it);
    }
    app.foldersLabel = makeControl(WC_STATICW, L"検索するフォルダ（ウィンドウにドラッグ＆ドロップしても追加できます）", 0,
                                   IDC_FOLDERS_LABEL);
    app.folders = makeControl(WC_LISTBOXW, L"", WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT,
                              IDC_FOLDERS, WS_EX_CLIENTEDGE);
    app.add = makeControl(WC_BUTTONW, L"フォルダを追加…", WS_TABSTOP, CMD_ADD_FOLDER);
    app.remove = makeControl(WC_BUTTONW, L"選んだフォルダを外す", WS_TABSTOP, CMD_REMOVE_FOLDER);
    app.thresholdCaption = makeControl(WC_STATICW, L"しきい値", 0, IDC_THRESHOLD_CAPTION);
    app.threshold = makeControl(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, IDC_THRESHOLD);
    SendMessageW(app.threshold, TBM_SETRANGE, TRUE, MAKELPARAM(0, kMaxThreshold));
    SendMessageW(app.threshold, TBM_SETTICFREQ, 5, 0);
    app.thresholdLabel = makeControl(WC_STATICW, L"", 0, IDC_THRESHOLD_LABEL);
    app.exact = makeControl(WC_BUTTONW, L"完全一致だけ", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_EXACT);
    app.flat = makeControl(WC_BUTTONW, L"単色に近い画像も比べる", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_FLAT);
    app.variants = makeControl(WC_BUTTONW, L"差分（AI）も出す", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_VARIANTS);
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
    app.views[kDupView].lv = app.thumbs;
    resetView(kDupView, 0);

    app.info = makeControl(WC_STATICW, L"", SS_NOPREFIX, IDC_INFO);  // 4 行まで（省略の指定をすると 1 行になる）
    app.dupPage = {app.foldersLabel, app.folders, app.add,      app.remove,   app.thresholdCaption, app.threshold,
                   app.thresholdLabel, app.exact, app.flat, app.variants,     app.progress, app.status,           app.groups,
                   app.thumbs};
    createSearchControls();
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

bool onSearchNotify(NMHDR* nm);

LRESULT onNotify(NMHDR* nm) {
    if (onSearchNotify(nm)) return 0;
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
                const Group* cg = currentGroup();
                const bool variant = cg && cg->kind == Kind::Variant;  // 差分は見た目の距離を持たない
                if (di->item.mask & LVIF_TEXT)
                    copyOut(di->item, (variant ? std::wstring() : L"距離" + std::to_wstring(dist) + L"  ") + sizeText(e->size) + L"  " +
                                          std::to_wstring(e->f.width) + L"×" + std::to_wstring(e->f.height) + L"  " +
                                          fileName(e->path));
                if (di->item.mask & LVIF_IMAGE) di->item.iImage = thumbImage(kDupView, i, widen(e->path), memberImageId(i));
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
                openPath(memberPath(ia->iItem));
                break;
            }
            case NM_RCLICK: {
                auto* ia = reinterpret_cast<NMITEMACTIVATE*>(nm);
                if (ia->iItem < 0) break;
                ListView_SetItemState(app.thumbs, ia->iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                imageContextMenu(memberPath(ia->iItem));
                break;
            }
        }
    }
    return 0;
}

std::wstring menuText(int cmd) {
    const CmdInfo* ci = findCommand(cmd);
    std::wstring text = ci ? ci->label : L"";
    const std::string key = keyLabel(app.keys, cmd);
    if (!key.empty()) text += L"\t" + widen(key);
    return text;
}

// 本体のメニュー（ショートカットキーの設定画面で機能を選ぶメニューにも使う）
void appendMainMenus(HMENU root) {
    auto popup = [&](const wchar_t* title, std::initializer_list<int> cmds) {
        HMENU m = CreatePopupMenu();
        for (int c : cmds) {  // 0 は区切り線
            if (c) AppendMenuW(m, MF_STRING, static_cast<UINT_PTR>(c), menuText(c).c_str());
            else AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(m), title);
    };
    popup(L"ファイル(&F)", {CMD_ADD_FOLDER, CMD_REMOVE_FOLDER, 0, CMD_EXPORT, 0, CMD_EXIT});
    popup(L"表示(&V)", {CMD_TAB_DUPES, CMD_TAB_SEARCH, 0, CMD_SAVE_SMART, CMD_DELETE_SMART, 0, CMD_EXPAND_LINKS, CMD_LINKS});
    popup(L"検索(&S)", {CMD_SEARCH, CMD_QUERY, 0, CMD_TOGGLE_EXACT, CMD_TOGGLE_FLAT, 0, CMD_THRESHOLD_DOWN, CMD_THRESHOLD_UP});
    popup(L"画像(&I)", {CMD_PREV_GROUP, CMD_NEXT_GROUP, 0, CMD_OPEN, CMD_EXPLORER, CMD_COPY_PATH});
    popup(L"タグ(&T)", {CMD_REVIEW, 0, CMD_TAG_YES, CMD_TAG_NO, 0, CMD_BULK_TAG, 0, CMD_UNDO});
    popup(L"その他(&O)", {CMD_SETTINGS, CMD_KEY_SETTINGS, 0, CMD_INDEXER_TOGGLE, CMD_INDEXER_AUTOSTART, 0, CMD_CHECK_UPDATE, CMD_MODELS, CMD_RELEASES});
    popup(L"ヘルプ(&H)", {CMD_HELP, 0, CMD_ABOUT});
}

void syncMenuChecks() {
    if (!app.menuBar) return;
    CheckMenuItem(app.menuBar, CMD_TOGGLE_EXACT,
                  MF_BYCOMMAND | (Button_GetCheck(app.exact) == BST_CHECKED ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(app.menuBar, CMD_TOGGLE_FLAT,
                  MF_BYCOMMAND | (Button_GetCheck(app.flat) == BST_CHECKED ? MF_CHECKED : MF_UNCHECKED));
}

void rebuildMenu() {
    HMENU old = app.menuBar;
    app.menuBar = CreateMenu();
    appendMainMenus(app.menuBar);
    SetMenu(app.hwnd, app.menuBar);
    if (old) DestroyMenu(old);
    syncMenuChecks();
}

// ---------------------------------------------------------------- ショートカットキー

// 割り当ては imgdupe.ini の [Keys] に「機能 = キー, キー」で持つ。一度も保存していなければ標準の割り当て
void loadKeys() {
    if (!iniInt(L"keysSaved", 0)) {
        app.keys = defaultKeys();
    } else {
        std::vector<wchar_t> buf(32768);
        const DWORD n = GetPrivateProfileSectionW(L"Keys", buf.data(), static_cast<DWORD>(buf.size()), iniPath().c_str());
        std::vector<std::string> lines;
        for (DWORD i = 0; i < n;) {
            const std::wstring line(&buf[i]);
            lines.push_back(narrow(line));
            i += static_cast<DWORD>(line.size()) + 1;
        }
        app.keys = keysFromLines(lines);
    }
    for (size_t i = 0; i < app.keyTmp.size(); ++i) {
        wchar_t name[64];
        GetPrivateProfileStringW(L"KeyTmp", (L"Slot" + std::to_wstring(i + 1)).c_str(), L"", name, 64, iniPath().c_str());
        const CmdInfo* ci = findCommand(narrow(name));
        app.keyTmp[i] = ci ? ci->id : 0;
    }
}

void saveKeys() {
    std::wstring section;
    for (const auto& line : keyLines(app.keys)) section += widen(line) + L'\0';
    section += L'\0';
    WritePrivateProfileSectionW(L"Keys", section.c_str(), iniPath().c_str());
    iniSet(L"keysSaved", L"1");
    for (size_t i = 0; i < app.keyTmp.size(); ++i) {
        const CmdInfo* ci = app.keyTmp[i] ? findCommand(app.keyTmp[i]) : nullptr;
        WritePrivateProfileStringW(L"KeyTmp", (L"Slot" + std::to_wstring(i + 1)).c_str(),
                                   ci ? widen(ci->name).c_str() : nullptr, iniPath().c_str());
    }
}

class KeyHost : public KeyMapHost {
public:
    KeyMap& keys() override { return app.keys; }
    void buildCommandMenu(HMENU root) override { appendMainMenus(root); }
    void onKeysChanged() override {
        saveKeys();
        rebuildMenu();  // メニューのキーの表示を更新
    }
    std::vector<int>& keyTmp() override { return app.keyTmp; }
};
KeyHost keyHost;

// 押されたキーに機能が割り当ててあれば、その機能を実行して true
bool handleShortcut(const MSG& msg) {
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
    if (msg.hwnd != app.hwnd && !IsChild(app.hwnd, msg.hwnd)) return false;  // 別のウィンドウ（設定画面など）
    const UINT vk = static_cast<UINT>(msg.wParam);
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU) return false;
    unsigned mods = 0;
    if (GetKeyState(VK_CONTROL) < 0) mods |= KMOD_CTRL;
    if (GetKeyState(VK_SHIFT) < 0) mods |= KMOD_SHIFT;
    if (GetKeyState(VK_MENU) < 0) mods |= KMOD_ALT;
    // 文字の入力欄では、文字・カーソル移動・コピーなどのキーはそのまま入力欄に渡す
    wchar_t cls[32] = L"";
    GetClassNameW(msg.hwnd, cls, 32);
    // 開いているドロップダウンのキー（Enter で選ぶ、Esc で閉じるなど）は奪わない
    if (lstrcmpiW(cls, L"ComboLBox") == 0 || (lstrcmpiW(cls, L"ComboBox") == 0 && ComboBox_GetDroppedState(msg.hwnd)))
        return false;
    if (lstrcmpiW(cls, L"Edit") == 0 && !(mods & KMOD_ALT) && !(vk >= VK_F1 && vk <= VK_F24)) {
        const bool ctrlLetter = (mods & KMOD_CTRL) && vk >= 'A' && vk <= 'Z' && !wcschr(L"ACVXYZ", static_cast<wchar_t>(vk));
        if (!ctrlLetter) return false;
    }
    auto it = app.keys.find(KeyStroke{mods, vk});
    if (it == app.keys.end()) return false;
    SendMessageW(app.hwnd, WM_COMMAND, static_cast<WPARAM>(it->second), 0);
    return true;
}

void selectGroupRelative(int delta) {
    const int n = static_cast<int>(app.result.groups.size());
    if (!n) return;
    const int cur = ListView_GetNextItem(app.groups, -1, LVNI_SELECTED);
    const int next = std::clamp(cur < 0 ? 0 : cur + delta, 0, n - 1);
    ListView_SetItemState(app.groups, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(app.groups, next, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(app.groups, next, FALSE);
}

void shutdown() {
    app.cancel = true;
    if (app.worker.joinable()) app.worker.join();
    expandCancel = true;
    if (app.expandThread.joinable()) app.expandThread.join();
    cleanupTemporaryLinks();  // 一時リンクは終了時に消す（仕様）
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
            loadKeys();
            rebuildMenu();
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
            if (reinterpret_cast<HWND>(lp) == app.s.conf) {
                wchar_t v[16];
                swprintf(v, 16, L"%.2f", SendMessageW(app.s.conf, TBM_GETPOS, 0, 0) / 100.0);
                setText(app.s.confValue, v);
                scheduleSearch(LOWORD(wp) == TB_THUMBTRACK ? 100 : 50);  // 止まってから少しして探す
                return 0;
            }
            if (reinterpret_cast<HWND>(lp) == app.threshold)
                setText(app.thresholdLabel, thresholdText(static_cast<int>(SendMessageW(app.threshold, TBM_GETPOS, 0, 0))));
            return 0;
        case WM_COMMAND:
            if (onSearchCommand(wp)) return 0;
            switch (LOWORD(wp)) {
                case CMD_ADD_FOLDER:
                    if (!app.running) {
                        pickFolder();
                        saveSettings();
                    }
                    break;
                case CMD_REMOVE_FOLDER: {
                    const int n = ListBox_GetSelCount(app.folders);
                    if (n > 0) {
                        std::vector<int> sel(n);
                        ListBox_GetSelItems(app.folders, n, sel.data());
                        for (int k = n - 1; k >= 0; --k) ListBox_DeleteString(app.folders, sel[k]);
                        saveSettings();
                    }
                    break;
                }
                case CMD_SEARCH:
                    if (app.updating && !app.running) break;  // 更新のダウンロード中
                    if (app.tab != 0) switchTab(0);
                    if (app.running) {
                        app.cancel = true;
                        setText(app.status, L"中止しています…");
                    } else {
                        startSearch(L"");
                    }
                    break;
                case CMD_QUERY: {
                    if (app.running) break;
                    const std::wstring p = pickImage();
                    if (!p.empty()) startSearch(p);
                    break;
                }
                case CMD_EXPORT:
                    if (!app.running) exportResult();
                    break;
                case CMD_OPEN: openPath(selectedPath()); break;
                case CMD_EXPLORER: showPathInExplorer(selectedPath()); break;
                case CMD_COPY_PATH: copyText(selectedPath()); break;
                case CMD_CHECK_UPDATE:
                    if (app.running)
                        MessageBoxW(hwnd, L"検索が終わってから確認してください。", L"imgdupe", MB_ICONINFORMATION);
                    else
                        startCheck(kManual);
                    break;
                case CMD_MODELS: startModelsCheck(ModelsAsk::Offer); break;
                case CMD_PREV_GROUP: selectGroupRelative(-1); break;
                case CMD_NEXT_GROUP: selectGroupRelative(1); break;
                case CMD_TOGGLE_EXACT:
                case CMD_TOGGLE_FLAT: {
                    HWND box = LOWORD(wp) == CMD_TOGGLE_EXACT ? app.exact : app.flat;
                    if (app.running) break;
                    Button_SetCheck(box, Button_GetCheck(box) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED);
                    syncMenuChecks();
                    saveSettings();
                    break;
                }
                case IDC_EXACT:
                case IDC_FLAT:
                    syncMenuChecks();
                    break;
                case IDC_VARIANTS:
                    // 出している結果に、差分のグループを足す・外す
                    if (!app.running && app.haveFiles && !app.resultIsQuery) {
                        Result r = std::move(app.result);
                        showResult(std::move(r), false);
                    }
                    break;
                case CMD_THRESHOLD_DOWN:
                case CMD_THRESHOLD_UP: {
                    if (app.running) break;
                    const int t = std::clamp(static_cast<int>(SendMessageW(app.threshold, TBM_GETPOS, 0, 0)) +
                                                 (LOWORD(wp) == CMD_THRESHOLD_UP ? 1 : -1),
                                             0, kMaxThreshold);
                    SendMessageW(app.threshold, TBM_SETPOS, TRUE, t);
                    setText(app.thresholdLabel, thresholdText(t));
                    break;
                }
                case CMD_KEY_SETTINGS: showKeyMapWindow(hwnd, &keyHost); break;
                case CMD_TAB_DUPES: switchTab(0); break;
                case CMD_TAB_SEARCH: switchTab(1); break;
                case CMD_SAVE_SMART:
                    if (app.tab != 1) switchTab(1);
                    saveSmartFolderUi();
                    break;
                case CMD_DELETE_SMART:
                    if (app.tab != 1) switchTab(1);
                    deleteSmartFolderUi();
                    break;
                case CMD_EXPAND_LINKS:
                    if (app.tab != 1) switchTab(1);
                    expandLinksUi();
                    break;
                case CMD_LINKS: showLinksWindow(); break;
                case CMD_TAG_YES:
                case CMD_TAG_NO:
                    if (app.tab == 1) markSelected(LOWORD(wp) == CMD_TAG_YES);
                    break;
                case CMD_BULK_TAG:
                    if (app.tab == 1) bulkTagUi();
                    break;
                case CMD_UNDO:
                    if (app.tab != 1) switchTab(1);
                    undoTagEdit();
                    break;
                case CMD_REVIEW:
                    if (app.tab != 1) switchTab(1);
                    Button_SetCheck(app.s.review, Button_GetCheck(app.s.review) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED);
                    scheduleSearch(10);
                    break;
                case CMD_INDEXER_TOGGLE:
                    saveSettings();  // 調べるフォルダを DB に書いてから
                    if (indexer::isRunning()) {
                        indexer::requestQuit(app.dataDir);
                        setText(app.status, L"バックグラウンドの解析を止めました");
                    } else if (indexer::start()) {
                        setText(app.status, L"バックグラウンドの解析を始めました（タスクトレイに常駐します）");
                    }
                    break;
                case CMD_INDEXER_AUTOSTART: {
                    const bool on = !indexer::autoStartEnabled();
                    indexer::setAutoStart(on);
                    if (on && !indexer::isRunning()) {
                        saveSettings();
                        indexer::start();
                    }
                    break;
                }
                case CMD_SETTINGS: showSettingsWindow(); break;
                case CMD_HELP: showHelp(); break;
                case CMD_RELEASES: ShellExecuteW(hwnd, L"open", APP_RELEASES_URL, nullptr, nullptr, SW_SHOWNORMAL); break;
                case CMD_ABOUT:
                    MessageBoxW(hwnd,
                                (L"imgdupe " + widen(IMGDUPE_VERSION) + L"\r\n重複画像検索\r\n\r\n"
                                 L"github.com/" APP_REPOSITORY L"\r\n\r\n"
                                 L"設定・キャッシュ: " + app.dataDir)
                                    .c_str(),
                                L"バージョン情報", MB_ICONINFORMATION);
                    break;
                case CMD_EXIT: SendMessageW(hwnd, WM_CLOSE, 0, 0); break;
            }
            return 0;
        case WM_NOTIFY:
            return onNotify(reinterpret_cast<NMHDR*>(lp));
        case WM_INITMENUPOPUP: {
            HMENU m = reinterpret_cast<HMENU>(wp);
            const bool on = indexer::isRunning();
            std::wstring text = on ? L"バックグラウンドの解析を止める(&B)" : L"バックグラウンドの解析を始める(&B)";
            const std::string key = keyLabel(app.keys, CMD_INDEXER_TOGGLE);
            if (!key.empty()) text += L"\t" + widen(key);
            ModifyMenuW(m, CMD_INDEXER_TOGGLE, MF_BYCOMMAND | MF_STRING, CMD_INDEXER_TOGGLE, text.c_str());
            CheckMenuItem(m, CMD_INDEXER_AUTOSTART, MF_BYCOMMAND | (indexer::autoStartEnabled() ? MF_CHECKED : MF_UNCHECKED));
            return 0;
        }
        case WM_TIMER:
            if (wp == kProgressTimer) updateProgress();
            if (wp == kSearchTimer) runSearch();
            if (wp == kExitTimer) DestroyWindow(hwnd);  // 確認に時間がかかりすぎたら、そのまま終わる
            return 0;
        case WM_APP_SEARCHED:
            onSearched(reinterpret_cast<SearchDone*>(lp));
            return 0;
        case WM_APP_EXPANDED:
            onExpanded(reinterpret_cast<ExpandDone*>(lp));
            return 0;
        case WM_APP_DONE:
            if (app.closing) {  // 終了しようとして中止した検索
                delete reinterpret_cast<Outcome*>(lp);
                if (app.worker.joinable()) app.worker.join();
                return 0;
            }
            onDone(reinterpret_cast<Outcome*>(lp));
            return 0;
        case WM_APP_MODELS: {
            std::unique_ptr<models::ManifestResult> r(reinterpret_cast<models::ManifestResult*>(lp));
            onModelsChecked(*r);
            return 0;
        }
        case WM_APP_MODEL_PROGRESS: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
            if (app.modelsDownloading && !app.modelsCancel) {
                setText(app.status, *text);
                SetWindowTextW(hwnd, (L"imgdupe - " + *text).c_str());
            }
            return 0;
        }
        case WM_APP_MODELS_DONE: {
            std::unique_ptr<std::wstring> err(reinterpret_cast<std::wstring*>(lp));
            onModelsDone(*err);
            return 0;
        }
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
            ThumbView& v = app.views[r->view];
            bool current;
            {
                std::lock_guard<std::mutex> lock(app.thumbMutex);
                current = r->gen == v.gen;
            }
            if (current && r->item < static_cast<int>(v.imageOf.size())) {
                v.imageOf[r->item] = r->bmp ? ImageList_Add(v.images, r->bmp, nullptr) : 1;
                if (v.imageOf[r->item] < 0) v.imageOf[r->item] = 1;
                ListView_RedrawItems(v.lv, r->item, r->item);
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
            if (app.modelsDownloading) {
                if (MessageBoxW(hwnd, L"AI 解析の部品をダウンロードしています。中止して終了しますか？\r\n（入れ終わった部品はそのまま使えます）",
                                L"imgdupe", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    app.closeAfterModels = true;
                    app.modelsCancel = true;
                }
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
    opt.dbFile = narrow(dbPath());
    opt.legacyCacheFile = narrow(cachePath());
    std::wstring out = L"imgdupe_result", queryPath;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"-t") opt.threshold = std::clamp(_wtoi(next().c_str()), 0, 64);
        else if (a == L"-o") out = next();
        else if (a == L"--exact-only") opt.exactOnly = true;
        else if (a == L"--include-flat") opt.includeFlat = true;
        else if (a == L"-q") queryPath = next();
        else if (a == L"--db") opt.dbFile = narrow(next());
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

IndexCallbacks windowsIndexCallbacks(std::atomic<uint64_t>& found, const std::atomic<bool>& cancel) {
    IndexCallbacks cb;
    cb.scan = [&found, &cancel](const std::string& root, std::vector<ScannedFile>& files) {
        imgwin::scanFolder(widen(root), files, found, cancel);
    };
    cb.decode = [](const std::string& path) { return imgwin::decodeFeatures(widen(path)); };
    cb.encodeThumb = [](const std::string& path, int side, std::vector<uint8_t>& bytes, int& w, int& h) {
        return imgwin::encodeThumbnail(widen(path), side, bytes, w, h);
    };
    cb.readMeta = [](const std::string& path, FileMeta& m) { return imgwin::readFileMeta(widen(path), m); };
    cb.threadInit = imgwin::threadBegin;
    cb.threadExit = imgwin::threadEnd;
    cb.taggerVersion = [] { return tagger::stageVersion(app.dataDir); };
    cb.makeTagger = [](std::string& error) {
        tagger::LoadResult r = tagger::load(app.dataDir);
        error = narrow(r.error);
        return r.analyzer;
    };
    cb.clipVersion = [] { return clip::stageVersion(app.dataDir); };
    cb.prepareClip = [](Db& db) { clip::prepare(app.dataDir, db); };
    cb.makeClip = [](std::string& error) {
        clip::LoadResult r = clip::loadAnalyzer(app.dataDir);
        error = narrow(r.error);
        return r.analyzer;
    };
    cb.afterClip = [](Db& db, unsigned threads, const std::atomic<bool>& cancel, Progress* p) {
        clip::updateVariants(app.dataDir, db, threads, cancel, p);
    };
    return cb;
}

// アンインストール（「アプリと機能」から imgdupe.exe --uninstall で呼ばれる）
int runUninstall() {
    const wchar_t* title = L"imgdupe のアンインストール";
    if (MessageBoxW(nullptr, L"imgdupe をアンインストールしますか？\r\n（画像は消えません）", title, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 1;
    const int data = MessageBoxW(nullptr,
                                 L"設定と画像の情報（imgdupe.db・サムネイル・imgdupe.ini）も消しますか？\r\n"
                                 L"「いいえ」なら残します（入れ直した時にそのまま使えます）。",
                                 title, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (data == IDCANCEL) return 1;
    // バックグラウンドの解析を止め、自動起動をやめる
    if (indexer::isRunning()) {
        indexer::requestQuit(app.dataDir);
        for (int i = 0; i < 100 && indexer::isRunning(); ++i) Sleep(100);
    }
    indexer::setAutoStart(false);
    // 一時リンク（固定にしたものは残す）
    {
        Db db;
        if (db.open(narrow(dbPath()))) {
            try {
                removeLinks(db, "", true);
            } catch (const std::exception&) {
            }
        }
    }
    install::removeShortcuts();
    install::unregisterUninstall();
    models::removeAll(app.dataDir);
    if (data == IDYES) {
        for (const wchar_t* f : {L"imgdupe.db", L"imgdupe.db-wal", L"imgdupe.db-shm", L"imgdupe.ini", L"imgdupe_cache.bin"})
            DeleteFileW((app.dataDir + L"\\" + f).c_str());
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((app.dataDir + L"\\thumbs_*.pack").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do DeleteFileW((app.dataDir + L"\\" + fd.cFileName).c_str());
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (app.dataDir != exeDir()) RemoveDirectoryW(app.dataDir.c_str());
    }
    MessageBoxW(nullptr, L"imgdupe をアンインストールしました。", title, MB_ICONINFORMATION);
    // 動いている exe は自分では消せないので、終わってから cmd に消してもらう（消えるまで 1 秒ごとに最大 20 回。フォルダは空の時だけ消える）
    const std::wstring exe = upd::exePath(), dir = exeDir();
    std::wstring cmd = L"cmd.exe /c for /l %i in (1,1,20) do (if exist \"" + exe + L"\" (ping -n 2 127.0.0.1 >nul & del /f /q \"" +
                       exe + L"\" \"" + exe + L".old\" 2>nul)) & rmdir \"" + dir + L"\" 2>nul";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0;
}

// imgdupe.exe --index <フォルダ>...: 走査して DB に登録し、ハッシュ・サムネイル・メタデータ・タグを作る
int runIndexCli(int argc, wchar_t** argv) {
    Console con;
    IndexOptions opt;
    opt.dbFile = narrow(dbPath());
    opt.packDir = narrow(app.dataDir);
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--no-thumbs") opt.thumbnails = false;
        else if (a == L"-j" && i + 1 < argc) opt.threads = static_cast<unsigned>(std::max(1, _wtoi(argv[++i])));
        else if (a == L"--db" && i + 1 < argc) opt.dbFile = narrow(argv[++i]);
        else opt.roots.push_back(narrow(stripTrailingSlash(a)));
    }
    if (opt.roots.empty()) {
        con.print(L"使い方: imgdupe.exe --index <フォルダ>... [--no-thumbs] [-j スレッド数] [--db ファイル]");
        return 2;
    }
    std::atomic<bool> cancel{false};
    IndexProgress prog;
    const IndexCallbacks cb = windowsIndexCallbacks(prog.prog.total, cancel);
    try {
        const IndexReport r = indexOnce(opt, cb, prog, cancel);
        con.print(L"画像 " + withCommas(r.files) + L" 枚（追加 " + withCommas(r.added) + L"・削除 " + withCommas(r.removed) +
                  L"）、ハッシュ " + withCommas(r.hashed) + L" 枚、サムネイル " + withCommas(r.thumbs) + L" 枚、メタデータ " +
                  withCommas(r.metas) + L" 枚、json・フォルダ名 " + withCommas(r.names) + L" 枚、タグ付け " +
                  withCommas(r.tagged) + L" 枚、CLIP " + withCommas(r.clipped) + L" 枚、失敗 " + withCommas(r.failed) + L" 枚");
        if (!r.clipError.empty()) con.print(L"CLIP を使えませんでした: " + widen(r.clipError));
        if (!r.taggerError.empty()) con.print(L"WD14 タガーを使えませんでした: " + widen(r.taggerError));
    } catch (const std::exception& e) {
        con.print(L"エラー: " + widen(e.what()));
        return 1;
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    app.inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    app.dataDir = findDataDir();
    upd::cleanupOldExe();

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && lstrcmpiW(argv[1], L"--uninstall") == 0) {
        LocalFree(argv);
        const int code = runUninstall();
        CoUninitialize();
        return code;
    }
    if (argv && argc >= 2 && lstrcmpiW(argv[1], L"--restart-indexer") == 0) {
        // インストーラーから: 動いているバックグラウンドの解析を、この exe で動かし直す
        LocalFree(argv);
        if (indexer::isRunning()) {
            indexer::requestQuit(app.dataDir);
            indexer::start(true);
        }
        CoUninitialize();
        return 0;
    }
    install::syncDisplayVersion(upd::exePath(), widen(IMGDUPE_VERSION));
    if (argv && argc >= 2 && lstrcmpiW(argv[1], L"--indexer") == 0) {
        const bool waitForOld = argc >= 3 && lstrcmpiW(argv[2], L"--restart") == 0;
        LocalFree(argv);
        const int code = indexer::runResident(inst, app.dataDir, waitForOld);
        CoUninitialize();
        return code;
    }
    if (argv && argc >= 2 && (lstrcmpiW(argv[1], L"--cli") == 0 || lstrcmpiW(argv[1], L"--index") == 0)) {
        const int code = lstrcmpiW(argv[1], L"--cli") == 0 ? runCli(argc, argv) : runIndexCli(argc, argv);
        LocalFree(argv);
        CoUninitialize();
        return code;
    }
    // exe にフォルダをドロップして起動した時は、そのフォルダをすぐ検索する
    std::vector<std::wstring> argFolders;
    for (int i = 1; argv && i < argc; ++i) argFolders.emplace_back(argv[i]);
    if (argv) LocalFree(argv);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS |
                                              ICC_DATE_CLASSES | ICC_TAB_CLASSES};
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
    Button_SetCheck(app.variants, iniInt(L"showVariants", 1) ? BST_CHECKED : BST_UNCHECKED);
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
    loadSearchSettings();
    cleanupTemporaryLinks();  // 前回、異常終了などで残った一時リンク
    if (Db* db = uiDb()) {
        try {
            imgwin::setScanExcludes(customVfolderRoots(*db));
        } catch (const std::exception&) {
        }
    }
    app.thumbThread = std::thread(thumbLoop);
    switchTab(std::clamp(iniInt(L"tab", 0), 0, 1));
    UpdateWindow(hwnd);
    if (autoStart) startSearch(L"");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // ショートカットキー（ダイアログの Enter・Tab などより先に見る）
        if (handleShortcut(msg)) continue;
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
