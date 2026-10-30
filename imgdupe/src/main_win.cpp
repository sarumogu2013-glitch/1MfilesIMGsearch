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
#include <climits>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include "commands.hpp"
#include "core.hpp"
#include "db.hpp"
#include "dict.hpp"
#include "tagnames.hpp"
#include "tagrel.hpp"
#include "image_win.hpp"
#include "indexer.hpp"
#include "indexer_win.hpp"
#include "install_win.hpp"
#include "keymap_win.hpp"
#include "ann.hpp"
#include "clip_win.hpp"
#include "lookup_win.hpp"
#include "stats.hpp"
#include "explorer_win.hpp"
#include "sameimg.hpp"
#include "tagger.hpp"
#include "tagger_win.hpp"
#include "models_win.hpp"
#include "resource.h"
#include "search.hpp"
#include "links.hpp"
#include "sqlite3.h"
#include "tagstore.hpp"
#include "pathinfer.hpp"
#include "log.hpp"
#include "log_win.hpp"
#include "update_win.hpp"
#include "version.hpp"
#include "win_util.hpp"

using namespace imgdupe;
using winutil::Com;
using winutil::narrow;
using winutil::widen;
using winutil::addTooltip;
using winutil::makeTooltip;

namespace {

// 注意・エラーのダイアログは、出した内容を動作履歴にも残す
int logBox(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
    const UINT icon = flags & MB_ICONMASK;
    if (icon == MB_ICONWARNING || icon == MB_ICONERROR)
        logs::warn("ダイアログ「" + narrow(title ? title : L"") + "」: " + narrow(text ? text : L""));
    return MessageBoxW(owner, text, title, flags);
}

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
constexpr UINT WM_APP_QUERY_FILE = WM_APP + 11;  // lParam: QueryFetched*（URL の画像をダウンロードした）
constexpr UINT WM_APP_QUERY_VEC = WM_APP + 12;   // lParam: QueryVector*（似た画像を探す画像のベクトル）
constexpr UINT WM_APP_QUERY_TAGS = WM_APP + 13;  // lParam: QueryTagsDone*（生成したタグ）
constexpr UINT WM_APP_CONVERT = WM_APP + 14;     // lParam: std::wstring*（DB の変換の様子。空なら済んだ）
constexpr UINT WM_APP_SOURCES = WM_APP + 10;        // lParam: std::vector<std::pair<std::string, int64_t>>*（出どころの数）
constexpr UINT_PTR kProgressTimer = 1;
constexpr UINT_PTR kSearchTimer = 3;
constexpr UINT_PTR kExitTimer = 2;
constexpr UINT_PTR kSameTimer = 4;  // エクスプローラーで画像を選んでから、少し待って同じ画像を探す
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
    IDC_S_QUALITY,
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
    IDC_S_DEADLINKS,
    IDC_S_CLIP,
    IDC_S_FREE,
    IDC_S_SCOPE,
    IDC_S_GO,
    IDC_S_ADVANCED,
    IDC_S_DATEFIELD,
    IDC_DUPBAR,
    IDC_RIBBON_LINE,
    IDC_QUERYBAR,
    IDC_QUERY_STRIP,
    IDC_QUERY_TAGS,
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
        HWND tags, conf, confValue, sources, text, clip, rating, format, quality, from, to, noTaken, unclassified, deadLinks, minW, minKb, maxKb, path, download,
            sort, desc, clear, count, results, smart, smartSave, smartDelete, expand, review, yes, no, bulk;
        HWND free, scope, go, advanced, dateField;
        // 見出し（リボンと詳細検索）
        HWND lSort, lClip, lSmart, lRating, lFormat, lQuality, lWidth, lSize, lSizeTo, lPath, lDownload, lDate, lDateTo, lConf, lSources;
        std::vector<HWND> labels;
    } s{};
    std::vector<HWND> searchPage;
    std::vector<HWND> advancedPage;  // 詳細検索（普段は隠す）
    bool advancedShown = false;
    HWND dupBar = nullptr, ribbonLine = nullptr;  // 重複を探すリボンのボタン、リボンの下の線
    std::vector<std::wstring> searchPaths;        // 検索の結果のパス（searchIds と同じ並び。エクスプローラーに出した分）
    std::vector<int64_t> searchIds;
    size_t searchTotal = 0;
    unsigned searchGen = 0;
    bool searchShown = false;  // 一度でも検索の画面を開いたか
    std::unordered_map<int, ImageInfo> infoCache;  // 一覧の項目 → 画像の情報
    std::unique_ptr<Db> uiDb;  // 画面の表示に使う DB の接続（UI のスレッドだけで使う）
    std::vector<std::string> sourceNames;    // 出どころの一覧の並び
    std::set<std::string> uncheckedSources;  // 外した出どころ
    std::unordered_map<std::string, std::string> tagNames;  // タグ（小文字）→ 日本語の表示名
    bool tagJapanese = true;                                // タグを日本語で出す（imgdupe.ini の tagJapanese）
    std::wstring reselectPath;       // 次の検索の結果で選ぶ画像（タグをクリックして探した時に選んでいた画像）
    std::wstring returnDir, returnSel;
  // 検索の結果を出す前に見ていたフォルダと選んでいた項目（条件を空にして探すと戻る）
    std::vector<int64_t> idsFilter;  // 検索の条件の「選んだ画像」（選んだ画像を保存したスマートフォルダを開いた時）
    bool fillingSources = false;
    // 出どころの数（全部のタグを数えるので重い。裏で数えて覚えておく）
    std::vector<std::pair<std::string, int64_t>> sourceCounts;
    ULONGLONG sourceCountsAt = 0;  // 最後に数え終えた時刻（0 = まだ）
    bool countingSources = false;
    bool sourcesStale = false;     // 数えている間にまた変わった
    std::vector<SmartFolder> smartList;  // スマートフォルダの一覧（コンボボックスの 1 番目から）
    bool expanding = false;              // フォルダに展開している最中
    int64_t similarTo = 0;               // 「似た画像を探す」の元の画像（0 = 使っていない）
    std::vector<float> searchScores;     // 内容（CLIP）で探した時の、各画像の似ている度合い
    std::wstring similarLabel;           // その時に「内容」の欄に出している文字
    // 画像で探す（クリップボード・ドロップ・URL の画像。<dataDir>\query に一時に置く）
    HWND queryBar = nullptr, queryStrip = nullptr, queryTags = nullptr;
    std::vector<std::wstring> queryFiles;  // 新しい順
    std::wstring querySel;                 // 選んでいる画像
    std::map<std::wstring, HBITMAP> queryThumbs;
    std::vector<float> similarVec;         // 似た画像を探す（インデックスに無い画像のベクトル。空 = 使っていない）
    std::mutex guiTaggerMutex;
    std::shared_ptr<imgdupe::Analyzer> guiTagger;  // タグの生成に使うタガー（最初に使う時に読む）
    bool queryBusy = false;
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

// エクスプローラーの画面のホスト（中身は「エクスプローラーの画面」のところ）
struct ExplorerApp : ExplorerHost {
    std::unique_ptr<Explorer> view;
    SameImageIndex index;          // 見た目一致を探すための、DB の全部の画像の dHash
    std::atomic<unsigned> gen{0};  // 新しく探し始めたら古い結果は捨てる
    std::wstring pendingSel;       // 選んでから少し待って探す画像
    bool shown = false;

    void ExplorerOpen(const std::wstring& path) override;
    void ExplorerSelected(const std::wstring& path, bool image) override;
    void ExplorerFindSameFile(const std::wstring& path) override;
    void ExplorerFindSameUrl(const std::wstring& url) override;
    void ExplorerAddScanTask(const std::vector<std::wstring>& paths, bool top) override;
    bool ExplorerIsRoot(const std::wstring& dir) override;
    void ExplorerRemoveRoot(const std::wstring& dir) override;
    void ExplorerSettingsChanged(const ExplorerSettings& s) override;
    std::vector<int> ExplorerStates(const std::wstring& dir, const std::vector<std::pair<std::wstring, bool>>& items) override;
    std::vector<ExplorerTag> ExplorerTags(const std::wstring& path) override;
    void ExplorerSearchTags(const std::vector<std::wstring>& tags) override;
    void ExplorerFindSimilarAi(const std::wstring& path) override;
    void ExplorerRelookup(const std::wstring& path) override;
    std::wstring ExplorerTagLabel(const std::wstring& tag) override;
    void ExplorerRenameTag(const std::wstring& tag) override;
    void ExplorerRemoveTag(const std::wstring& path, const std::wstring& tag) override;
    void ExplorerAddTag(const std::vector<std::wstring>& paths, const std::wstring& tag) override;
    void ExplorerDropTag(const std::wstring& tag, POINT screen) override;
    std::vector<std::pair<std::wstring, std::wstring>> ExplorerProperties(const std::wstring& path) override;
    void ExplorerSaveSelection() override;
    void ExplorerTagSelection() override;
    void ExplorerTeachFolder(const std::wstring& dir) override;
} ex;

int px(int v) { return static_cast<int>(v * app.scale + 0.5); }

// ---------------------------------------------------------------- ツールバー

int stopImage = 0, startImage = 0;  // 検索開始ボタンの絵（検索中は中止の絵にする）

void tbEnable(int id, bool on) {
    for (HWND bar : {app.toolbar, app.dupBar}) SendMessageW(bar, TB_ENABLEBUTTON, id, MAKELPARAM(on ? TRUE : FALSE, 0));
}

void tbSetText(int id, const wchar_t* text) {
    TBBUTTONINFOW bi{sizeof(bi)};
    bi.dwMask = TBIF_TEXT;
    bi.pszText = const_cast<wchar_t*>(text);
    for (HWND bar : {app.toolbar, app.dupBar}) SendMessageW(bar, TB_SETBUTTONINFOW, id, reinterpret_cast<LPARAM>(&bi));
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

void saveExplorerDir();

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
    saveExplorerDir();
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

std::vector<std::wstring> pickFolders(const wchar_t* title) {
    std::vector<std::wstring> out;
    Com<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.put()))))
        return out;
    DWORD opt = 0;
    dlg->GetOptions(&opt);
    dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT);
    dlg->SetTitle(title);
    if (FAILED(dlg->Show(app.hwnd))) return out;
    Com<IShellItemArray> items;
    if (FAILED(dlg->GetResults(items.put()))) return out;
    DWORD n = 0;
    items->GetCount(&n);
    for (DWORD i = 0; i < n; ++i) {
        Com<IShellItem> item;
        PWSTR p = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            out.emplace_back(p);
            CoTaskMemFree(p);
        }
    }
    return out;
}

// path が調べるフォルダのどれかの中（か、そのもの）か
bool underAnyRoot(const std::wstring& path) {
    const std::wstring p = stripTrailingSlash(path);
    for (std::wstring r : folderList()) {
        r = stripTrailingSlash(r);
        if (p.size() < r.size() || _wcsnicmp(p.c_str(), r.c_str(), r.size()) != 0) continue;
        if (p.size() == r.size() || p[r.size()] == L'\\' || p[r.size()] == L'/' || r.back() == L'\\') return true;
    }
    return false;
}

// スキャンタスクを足す: まだ調べるフォルダの外のフォルダは調べるフォルダにも足す（これからも変化を見る）。
// いくつ選んでも 1 つのタスク（操作ごと）。top なら一番上（今のタスクを止めてでも先に）
void addScanTask(const std::vector<std::wstring>& paths, bool top) {
    std::vector<std::wstring> list;
    size_t dirs = 0, files = 0;
    bool rootsChanged = false;
    for (const auto& raw : paths) {
        const std::wstring p = stripTrailingSlash(raw);
        const DWORD attr = GetFileAttributesW(p.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            if (!underAnyRoot(p)) {
                addFolder(p);
                rootsChanged = true;
            }
            ++dirs;
        } else {
            if (!isImageExt(narrow(p))) continue;
            ++files;
        }
        if (std::find(list.begin(), list.end(), p) == list.end()) list.push_back(p);
    }
    if (list.empty()) return;
    if (rootsChanged) saveSettings();
    auto nameOf = [](const std::wstring& p) {
        const size_t k = p.find_last_of(L"\\/");
        return k == std::wstring::npos || k + 1 == p.size() ? p : p.substr(k + 1);
    };
    std::wstring label;
    if (list.size() == 1) label = (dirs ? L"フォルダ: " : L"画像: ") + nameOf(list[0]);
    else {
        if (dirs) label = L"フォルダ " + std::to_wstring(dirs) + L" 個";
        if (files) label += (label.empty() ? L"" : L"・") + std::wstring(L"画像 ") + std::to_wstring(files) + L" 枚";
        label += L"（" + nameOf(list[0]) + L" など）";
    }
    indexer::addScanTask(app.dataDir, label, list, top);
    logs::info("スキャンタスクを足した: " + narrow(label) + (top ? "（一番上）" : "") + (rootsChanged ? "・調べるフォルダにも足した" : ""));
    if (!indexer::isRunning()) indexer::start();
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
    // 結果は、エクスプローラーの一覧に出す（まとまりは重なった絵。開くと中の画像）
    std::vector<ExplorerGroup> groups;
    for (size_t i = 0; i < app.result.groups.size(); ++i) {
        const Group& g = app.result.groups[i];
        ExplorerGroup eg;
        eg.title = (query ? std::wstring(L"似た画像") : L"#" + std::to_wstring(i + 1) + L"　" + widen(kindName(g.kind))) + L"　" +
                   withCommas(g.members.size()) + L" 枚" + (query ? L"" : L"・消せば " + sizeText(g.reclaim) + L" 空く");
        for (const auto& m : g.members) eg.paths.push_back(widen(app.result.files[m.index].path));
        groups.push_back(std::move(eg));
    }
    ex.view->SetGroups(query ? L"1 枚で検索" : L"重複", std::move(groups));
}

// ---------------------------------------------------------------- 検索

void setRunning(bool on) {
    app.running = on;
    tbSetText(CMD_SEARCH, on ? L"検索を中止" : L"検索開始");
    SendMessageW(app.dupBar, TB_CHANGEBITMAP, CMD_SEARCH, MAKELPARAM(on ? stopImage : startImage, 0));
    for (HWND h : {app.add, app.remove, app.folders, app.threshold, app.exact, app.flat, app.variants}) EnableWindow(h, !on);
    tbEnable(CMD_QUERY, !on);
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
        logBox(app.hwnd,
                    L"調べるフォルダがありません。ホームでフォルダを右クリックして「スキャンタスクに追加」を選ぶか、"
                    L"「ファイル → フォルダをスキャンタスクに追加」で足してください（ウィンドウにドラッグ＆ドロップしても足せます）。",
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
    logs::info(std::string(query ? "1 枚で重複を探し始めた" : "重複を探し始めた") + "（フォルダ " + std::to_string(opt.roots.size()) +
               " 個・しきい値 " + std::to_string(opt.threshold) + (reuse ? "・読み込み済みを使う" : "") + "）");

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
    if (!o->error.empty()) logs::warn("重複探しが失敗した: " + narrow(o->error));
    else if (o->result.cancelled) logs::info("重複探しを中止した");
    else
        logs::info("重複探しが終わった: 画像 " + std::to_string(o->result.files.size()) + " 枚・まとまり " +
                   std::to_string(o->result.groups.size()) + " 個（" + std::to_string((GetTickCount64() - app.startTick) / 1000) + " 秒）");
    if (!o->error.empty()) {
        setText(app.status, o->error);
        logBox(app.hwnd, o->error.c_str(), L"imgdupe", MB_ICONWARNING);
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
        logBox(app.hwnd, L"AI の部品をダウンロードしています。終わるまでお待ちください。", L"imgdupe", MB_ICONINFORMATION);
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
            logBox(app.hwnd, r.error.c_str(), L"AI 解析の部品", MB_ICONWARNING);
        return;  // アップデートの確認からなら、本体の結果を出した後なので何も言わない
    }
    const imgdupe::Installed installed = models::loadInstalled(app.dataDir);
    uint64_t total = 0;
    const auto updates = imgdupe::planUpdate(r.manifest, installed);
    if (!updates.empty()) {
        const std::wstring lines = componentLines(updates, total);
        if (logBox(app.hwnd,
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
        logBox(app.hwnd, (L"AI 解析の部品はすべて最新です。\r\n\r\n" + lines).c_str(), L"AI 解析の部品", MB_ICONINFORMATION);
        return;
    }
    const std::wstring lines = componentLines(missing, total);
    if (logBox(app.hwnd,
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
    if (!err.empty()) logs::warn("AI 解析の部品を入れられなかった: " + narrow(err));
    else logs::info("AI 解析の部品を入れた");
    if (!err.empty()) {
        setText(app.status, L"AI 解析の部品を入れられませんでした");
        logBox(app.hwnd, (L"AI 解析の部品を入れられませんでした。\r\n\r\n" + err).c_str(), L"AI 解析の部品", MB_ICONWARNING);
        return;
    }
    restartIndexerAfterUpdate();  // 新しいモデルで解析し直す
    setText(app.status, L"AI 解析の部品を入れました");
    logBox(app.hwnd, L"AI 解析の部品を入れました。", L"AI 解析の部品", MB_ICONINFORMATION);
}

void startDownload(CheckMode mode, const imgdupe::ReleaseInfo& info) {
    app.updating = true;
    tbEnable(CMD_CHECK_UPDATE, false);
    setText(app.status, L"新しい版 " + widen(info.tag) + L" をダウンロードしています…");
    logs::info("新しい版 " + info.tag + " のダウンロードを始めた");
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
    if (!err.empty()) logs::warn("アップデートを確認できなかった: " + narrow(err));
    else logs::info("アップデートを確認した: 最新 " + r.info.tag + (cmp == VersionCompare::RemoteNewer ? "（新しい版あり）" : ""));
    const std::wstring cur = widen(IMGDUPE_VERSION);
    const std::wstring versions = L"新しい版: " + widen(r.info.tag) + L"\r\n今の版: " + cur;
    if (mode == kOnExit) {
        // 新しい版がある時だけ聞く。確認できなかった時は何も言わずに終わる
        KillTimer(app.hwnd, kExitTimer);
        if (err.empty() && cmp == VersionCompare::RemoteNewer &&
            logBox(nullptr,
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
        logBox(app.hwnd, (L"アップデートを確認できませんでした。\r\n\r\n" + err + L"\r\n\r\n今の版: " + cur).c_str(),
                    L"imgdupe のアップデート", MB_ICONWARNING);
        return;
    }
    if (cmp != VersionCompare::RemoteNewer) {
        logBox(app.hwnd,
                    (L"最新の版です。\r\n\r\n今の版: " + cur + L"\r\n最新のリリース: " + widen(r.info.tag)).c_str(),
                    L"imgdupe のアップデート", MB_ICONINFORMATION);
        startModelsCheck(ModelsAsk::UpdatesOnly);  // 本体が最新なら、AI の部品の版も確かめる
        return;
    }
    if (logBox(app.hwnd,
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
    if (r.ok) logs::info("新しい版をダウンロードして入れ替えた");
    else logs::warn("更新できなかった: " + narrow(r.error));
    if (r.ok) restartIndexerAfterUpdate();
    if (mode == kOnExit) {
        if (!r.ok)
            logBox(nullptr, (L"更新できませんでした。\r\n\r\n" + r.error).c_str(), L"imgdupe のアップデート",
                        MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        DestroyWindow(app.hwnd);
        return;
    }
    if (!r.ok) {
        setText(app.status, L"更新できませんでした");
        logBox(app.hwnd, (L"更新できませんでした。\r\n\r\n" + r.error).c_str(), L"imgdupe のアップデート",
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
        logBox(app.hwnd, L"更新しました。次に起動した時から新しい版になります。", L"imgdupe のアップデート",
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
std::wstring selectedPath() { return ex.view ? ex.view->Selected() : L""; }

// 画像の右クリックのメニュー
void findSimilar(int64_t imageId, const std::wstring& name);
void relookupImages(int64_t clicked);

// imageId があれば「似た画像を探す」も出す（検索の画面）
void imageContextMenu(const std::wstring& path, int64_t imageId = 0) {
    constexpr int kSimilar = 0x7f00, kRelookup = 0x7f01, kSourcePage = 0x7f10;  // 出典のページは kSourcePage + 番号
    std::vector<SourceLink> links;
    if (imageId)
        if (Db* db = uiDb()) {
            try {
                links = sourceLinks(*db, imageId);
            } catch (const std::exception&) {
            }
        }
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CMD_OPEN, L"開く(&O)");
    AppendMenuW(menu, MF_STRING, CMD_EXPLORER, L"エクスプローラーで表示(&E)");
    AppendMenuW(menu, MF_STRING, CMD_COPY_PATH, L"パスをコピー(&C)");
    if (imageId) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kSimilar, L"似た画像を探す(&S)");
        for (size_t i = 0; i < links.size() && i < 8; ++i) {
            std::wstring label = L"出典のページを開く: " + widen(links[i].url) + (links[i].status == 0 ? L"（リンク切れ）" : L"");
            for (size_t k = 0; (k = label.find(L'&', k)) != std::wstring::npos; k += 2) label.insert(k, 1, L'&');
            AppendMenuW(menu, MF_STRING, kSourcePage + static_cast<int>(i), label.c_str());
        }
        AppendMenuW(menu, MF_STRING, kRelookup, L"出典を調べ直す(&L)");
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
    if (cmd == kRelookup) relookupImages(imageId);
    if (cmd >= kSourcePage && cmd < kSourcePage + static_cast<int>(links.size()))
        ShellExecuteW(app.hwnd, L"open", widen(links[static_cast<size_t>(cmd - kSourcePage)].url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void exportResult() {
    if (app.result.groups.empty()) return;
    const std::wstring base = pickSaveBase();
    if (base.empty()) return;
    const bool ok = writeCsv(app.result, narrow(base + L".csv")) && writeHtml(app.result, narrow(base + L".html"));
    if (!ok) {
        logBox(app.hwnd, L"保存できませんでした。", L"imgdupe", MB_ICONWARNING);
        return;
    }
    if (logBox(app.hwnd,
                    (L"保存しました。\r\n" + base + L".csv\r\n" + base +
                     L".html\r\n\r\nHTML（サムネイル付きの一覧）をブラウザで開きますか？")
                        .c_str(),
                    L"imgdupe", MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(app.hwnd, L"open", (base + L".html").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void showHelp() {
    logBox(app.hwnd,
                L"1. ホームでフォルダを右クリックして「スキャンタスクに追加」を選ぶか、「ファイル → フォルダをスキャンタスクに追加」で調べるフォルダを足します（ウィンドウへのドラッグ＆ドロップでも足せます。複数可）。\r\n"
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
std::vector<int64_t> selectedImageIds(size_t* missing = nullptr);

struct SearchDone {
    unsigned gen;
    SearchResult result;
    std::wstring error;
    bool rest = false;  // 2 回目（先頭の後の残り全部）
    bool clip = false;  // 内容（CLIP）で似ている順に上位だけ
    std::vector<float> scores;
    std::vector<std::wstring> paths;  // ids と同じ並び
    std::vector<std::vector<size_t>> clusters;  // 重複・差分のまとまり（ids の添字の束）
    std::vector<std::wstring> folders;          // 名前がフリーワードに合うフォルダ（一覧の先頭に出す）
};

// インデックスのあるフォルダの一覧（フォルダを名前で探す時に使う。画像の数・一番新しい ID が変わったら作り直す）
struct FolderNames {
    std::mutex m;
    std::string signature;
    std::vector<std::string> folders;
} folderNames;

std::vector<std::string> currentFolders(Db& d) {
    std::string sig;
    {
        Stmt st(d.handle(), "SELECT count(*), coalesce(max(id),0) FROM images");
        if (st.step()) sig = std::to_string(st.i64(0)) + "/" + std::to_string(st.i64(1));
    }
    std::lock_guard<std::mutex> lock(folderNames.m);
    if (folderNames.signature != sig) {
        folderNames.folders = indexedFolders(d);
        folderNames.signature = sig;
    }
    return folderNames.folders;
}

constexpr size_t kFirstResults = 2000;  // 内容（CLIP）で似ている順に出す数
constexpr size_t kListResults = 20000;  // エクスプローラーに出す検索の結果の数（多い時は先頭だけ）

Db* uiDb() {
    if (!app.uiDb) {
        app.uiDb = std::make_unique<Db>();
        if (!app.uiDb->open(narrow(dbPath()))) {
            logs::error("データベースを開けなかった: " + narrow(dbPath()));
            app.uiDb.reset();
        }
    }
    return app.uiDb.get();
}

// ---------------------------------------------------------------- タグの表示名

// 同梱の訳（exe に埋め込んだ data/tags_ja.tsv）
std::string builtinTagNames() {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_TAGS_JA), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const char* p = g ? static_cast<const char*>(LockResource(g)) : nullptr;
    return p ? std::string(p, SizeofResource(nullptr, r)) : std::string();
}

// 起動時: 同梱の訳を DB に入れて（変わった時だけ）、表示名を読む
void loadTagLabels() {
    app.tagJapanese = iniInt(L"tagJapanese", 1) != 0;
    if (Db* db = uiDb()) {
        try {
            installTagNames(*db, builtinTagNames());
            app.tagNames = loadTagNames(*db);
        } catch (const std::exception&) {
        }
    }
}

// 画面に出すタグの名前（日本語で出す設定で、表示名があればそれ）
std::wstring tagLabel(const std::string& tag) {
    if (app.tagJapanese) {
        auto it = app.tagNames.find(tagNameKey(tag));
        if (it != app.tagNames.end()) return widen(it->second);
    }
    return widen(tag);
}

void refreshTagLabels();
void syncMenuChecks();

void setTagJapanese(bool on) {
    app.tagJapanese = on;
    WritePrivateProfileStringW(L"imgdupe", L"tagJapanese", on ? L"1" : L"0", iniPath().c_str());
    refreshTagLabels();
}

// 出どころの表示名
std::wstring sourceLabel(const std::string& s) {
    static const std::pair<const char*, const wchar_t*> names[] = {
        {"manual", L"手動で確認"}, {"embedded", L"ファイル埋め込み"}, {"meta", L"メタデータ"}, {"id_lookup", L"ID 照会"},
        {"reverse", L"逆画像検索"}, {"folder", L"フォルダ名"}, {"same_folder", L"同じフォルダからの推定"},
        {"infer", L"フォルダ名からの推定"},
        {"tagger", L"タガー（WD14）"}, {"clip", L"CLIP"}, {"dict", L"辞書"}, {"parent", L"親子関係"}, {"reference", L"見本画像との類似"}, {"ocr", L"OCR"}};
    for (const auto& [k, v] : names)
        if (s == k) return v;
    return widen(s);
}

std::wstring editText(HWND h) {
    std::wstring t(static_cast<size_t>(GetWindowTextLengthW(h)), L'\0');
    if (!t.empty()) GetWindowTextW(h, &t[0], static_cast<int>(t.size()) + 1);
    return t;
}

// 出どころの一覧を、覚えている数で作り直す（外したものは外したまま）
void fillSources() {
    app.fillingSources = true;
    SendMessageW(app.s.sources, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(app.s.sources);
    app.sourceNames.clear();
    for (const auto& [name, count] : app.sourceCounts) {
        const std::wstring text = sourceLabel(name) + L"（" + withCommas(static_cast<uint64_t>(count)) + L"）";
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = static_cast<int>(app.sourceNames.size());
        it.pszText = const_cast<wchar_t*>(text.c_str());
        const int i = ListView_InsertItem(app.s.sources, &it);
        ListView_SetCheckState(app.s.sources, i, app.uncheckedSources.count(name) ? FALSE : TRUE);
        app.sourceNames.push_back(name);
    }
    if (app.sourceNames.empty()) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.pszText = const_cast<wchar_t*>(app.sourceCountsAt ? L"（タグはまだありません）" : L"（数えています…）");
        ListView_InsertItem(app.s.sources, &it);
    }
    SendMessageW(app.s.sources, WM_SETREDRAW, TRUE, 0);
    app.fillingSources = false;
}

// 出どころの一覧を出す。数は裏で数え直す（タグの数だけ時間がかかるので、検索の画面を開くたびには待たない）。
// changed: タグを変えた後（数が変わったので数え直す）。そうでなければ、1 分以内に数えていれば数え直さない
void refreshSources(bool changed = false) {
    fillSources();
    if (!changed && app.sourceCountsAt && GetTickCount64() - app.sourceCountsAt < 60 * 1000) return;
    if (!changed && !app.advancedShown && app.sourceCountsAt) return;  // 見えていなければ、開いた時に
    if (app.countingSources) {
        app.sourcesStale = true;
        return;
    }
    app.countingSources = true;
    app.sourcesStale = false;
    const std::string file = narrow(dbPath());
    HWND hwnd = app.hwnd;
    std::thread([file, hwnd] {
        auto* out = new std::vector<std::pair<std::string, int64_t>>();
        try {
            Db db;
            if (db.open(file)) *out = tagSources(db);
        } catch (const std::exception&) {
        }
        if (!PostMessageW(hwnd, WM_APP_SOURCES, 0, reinterpret_cast<LPARAM>(out))) delete out;
    }).detach();
}

// ---------------------------------------------------------------- 日付の欄（チェック・年・月・日を自分で描く）
// 年・月・日はそれぞれ別の場所に描くので、その上でホイールを回すとその欄だけが変わる（Windows の日付の部品は欄の場所が分からない）

constexpr UINT DBN_CHANGED = 1;  // 親への WM_COMMAND の通知: 日付かチェックが変わった

struct DateBox {
    bool on = false;
    int y = 2000, m = 1, d = 1;
    int focus = 0;   // キーで変える欄（0 年、1 月、2 日）
    int hover = -1;  // マウスの下の欄
    RECT check{}, parts[3]{};
};

int daysInMonth(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[std::clamp(m, 1, 12) - 1];
}

DateBox* dateBoxOf(HWND h) { return reinterpret_cast<DateBox*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

void dateBoxChanged(HWND h) {
    InvalidateRect(h, nullptr, FALSE);
    SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), DBN_CHANGED), reinterpret_cast<LPARAM>(h));
}

// 欄 part を step だけ変える（日は月をまたぐ。月は年をまたぐ）
void dateBoxStep(DateBox& b, int part, int step) {
    if (part == 0) b.y += step;
    else if (part == 1) {
        b.m += step;
        while (b.m < 1) b.m += 12, --b.y;
        while (b.m > 12) b.m -= 12, ++b.y;
    } else {
        b.d += step;
        if (b.d < 1) {
            if (--b.m < 1) b.m = 12, --b.y;
            b.d = daysInMonth(b.y, b.m);
        } else if (b.d > daysInMonth(b.y, b.m)) {
            b.d = 1;
            if (++b.m > 12) b.m = 1, ++b.y;
        }
    }
    b.y = std::clamp(b.y, 1970, 2999);
    b.d = std::min(b.d, daysInMonth(b.y, b.m));
}

int dateBoxPartAt(const DateBox& b, POINT pt) {
    for (int i = 0; i < 3; ++i)
        if (PtInRect(&b.parts[i], pt)) return i;
    // 欄の間（「年」「月」の文字の上など）は、近い方の欄
    int best = -1, dist = INT_MAX;
    for (int i = 0; i < 3; ++i) {
        const int c = (b.parts[i].left + b.parts[i].right) / 2, dd = std::abs(pt.x - c);
        if (pt.x > b.check.right && dd < dist) best = i, dist = dd;
    }
    return best;
}

LRESULT CALLBACK dateBoxProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    DateBox* b = dateBoxOf(h);
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new DateBox));
            break;
        case WM_NCDESTROY:
            delete b;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
            const bool focused = GetFocus() == h;
            HGDIOBJ old = SelectObject(dc, app.font);
            SetBkMode(dc, TRANSPARENT);
            // チェック
            const int cs = std::min<int>(rc.bottom - rc.top - px(6), px(15));
            b->check = {px(4), (rc.bottom - cs) / 2, px(4) + cs, (rc.bottom - cs) / 2 + cs};
            RECT cr = b->check;
            DrawFrameControl(dc, &cr, DFC_BUTTON, DFCS_BUTTONCHECK | (b->on ? DFCS_CHECKED : 0));
            // 年・月・日（使っていない時は灰色）
            int x = b->check.right + px(6);
            const wchar_t* units[] = {L"年", L"月", L"日"};
            const int vals[] = {b->y, b->m, b->d};
            for (int i = 0; i < 3; ++i) {
                wchar_t num[8];
                swprintf(num, 8, i == 0 ? L"%04d" : L"%02d", vals[i]);
                SIZE sz{};
                GetTextExtentPoint32W(dc, i == 0 ? L"0000" : L"00", i == 0 ? 4 : 2, &sz);
                RECT pr = {x - px(2), rc.top + px(2), x + sz.cx + px(2), rc.bottom - px(2)};
                b->parts[i] = pr;
                if (b->hover == i || (focused && b->focus == i)) {
                    HBRUSH hb = CreateSolidBrush(focused && b->focus == i ? RGB(204, 228, 247) : RGB(232, 240, 250));
                    FillRect(dc, &pr, hb);
                    DeleteObject(hb);
                }
                SetTextColor(dc, b->on ? RGB(20, 20, 20) : RGB(150, 150, 150));
                RECT tr = {x, rc.top, x + sz.cx, rc.bottom};
                DrawTextW(dc, num, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                x += sz.cx + px(2);
                SIZE us{};
                GetTextExtentPoint32W(dc, units[i], 1, &us);
                RECT ur = {x, rc.top, x + us.cx, rc.bottom};
                SetTextColor(dc, RGB(110, 110, 110));
                DrawTextW(dc, units[i], -1, &ur, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                x += us.cx + px(4);
            }
            SelectObject(dc, old);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &pt);
            const int part = dateBoxPartAt(*b, pt);
            if (part < 0) return 0;
            dateBoxStep(*b, part, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
            b->on = true;  // ホイールで変えたら使う
            b->focus = part;
            dateBoxChanged(h);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int part = -1;
            for (int i = 0; i < 3; ++i)
                if (PtInRect(&b->parts[i], pt)) part = i;
            if (part != b->hover) {
                b->hover = part;
                InvalidateRect(h, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            b->hover = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(h);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x <= b->check.right + px(3)) {
                b->on = !b->on;
                dateBoxChanged(h);
                return 0;
            }
            const int part = dateBoxPartAt(*b, pt);
            if (part >= 0) b->focus = part;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN:
            if (wp == VK_LEFT || wp == VK_RIGHT) {
                b->focus = std::clamp(b->focus + (wp == VK_RIGHT ? 1 : -1), 0, 2);
                InvalidateRect(h, nullptr, FALSE);
            } else if (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT) {
                dateBoxStep(*b, b->focus, wp == VK_UP ? 1 : wp == VK_DOWN ? -1 : wp == VK_PRIOR ? 10 : -10);
                b->on = true;
                dateBoxChanged(h);
            } else if (wp == VK_SPACE) {
                b->on = !b->on;
                dateBoxChanged(h);
            }
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void registerDateBox() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = dateBoxProc;
    wc.hInstance = app.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ImgdupeDateBox";
    RegisterClassExW(&wc);
}

void dateBoxSet(HWND h, bool on, int y, int m, int d) {
    if (DateBox* b = dateBoxOf(h)) {
        b->on = on;
        b->y = y;
        b->m = m;
        b->d = d;
        InvalidateRect(h, nullptr, FALSE);
    }
}

std::optional<int64_t> pickerDate(HWND h, bool endOfDay) {
    const DateBox* b = dateBoxOf(h);
    if (!b || !b->on) return std::nullopt;
    return civilToUnix(b->y, b->m, b->d, endOfDay ? 23 : 0, endOfDay ? 59 : 0, endOfDay ? 59 : 0);
}

// 並べ替えの選択肢（検索のリボンの並び）
const SortKey kSortChoices[] = {SortKey::Relevance, SortKey::FileName, SortKey::Path,   SortKey::Modified, SortKey::Created,
                                SortKey::Taken,     SortKey::Resolution, SortKey::Size, SortKey::Rating,   SortKey::Title,
                                SortKey::Confidence, SortKey::Aesthetic, SortKey::Effective};

SearchQuery searchQueryFromUi() {
    SearchQuery q;
    // フリーワード: 「タグに限る」はタグの名前どおり（別名を含む）、ほかはタグ・タイトル・ファイル名などから探して関連度で並べる
    const std::string free = narrow(editText(app.s.free));
    const int scope = std::max(0, ComboBox_GetCurSel(app.s.scope));
    if (scope == 1 || scope == 3) parseTagQuery(free, q.tags, q.excludeTags);
    else q.freeText = free;
    if (scope == 4) q.freeScope = FreeScope::Name;
    // ファイルのタグに限る（ファイルに書き込まれているタグだけ）。語が無ければ条件にしない
    if (scope == 3 && (!q.tags.empty() || !q.excludeTags.empty())) q.sources = {kSourceEmbedded};
    if (scope != 4) q.freeScope = scope == 2 ? FreeScope::Title : FreeScope::All;
    q.minConfidence = SendMessageW(app.s.conf, TBM_GETPOS, 0, 0) / 100.0;
    q.excludeSources.assign(app.uncheckedSources.begin(), app.uncheckedSources.end());
    q.dateField = static_cast<DateField>(std::clamp(ComboBox_GetCurSel(app.s.dateField), 0, 2));
    if (app.similarTo) q.similarTo = app.similarTo;
    else if (app.similarVec.empty()) q.clipText = narrow(editText(app.s.clip));
    q.ids = app.idsFilter;
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
    q.unclassified = Button_GetCheck(app.s.unclassified) == BST_CHECKED;
    q.deadLinks = Button_GetCheck(app.s.deadLinks) == BST_CHECKED;
    q.quality = static_cast<QualityFilter>(std::clamp(ComboBox_GetCurSel(app.s.quality), 0, 4));
    q.minWidth = _wtoi(editText(app.s.minW).c_str());
    q.minSize = static_cast<int64_t>(_wtoi(editText(app.s.minKb).c_str())) * 1024;
    q.maxSize = static_cast<int64_t>(_wtoi(editText(app.s.maxKb).c_str())) * 1024;
    q.pathContains = narrow(editText(app.s.path));
    q.downloadContains = narrow(editText(app.s.download));
    const int sort = std::max(0, ComboBox_GetCurSel(app.s.sort));
    q.sort = kSortChoices[std::min<int>(sort, static_cast<int>(std::size(kSortChoices)) - 1)];
    if (q.sort == SortKey::Relevance && (scope == 1 || scope == 3) && !q.tags.empty()) q.sort = SortKey::Confidence;  // タグの関連度は確度
    q.descending = Button_GetCheck(app.s.desc) == BST_CHECKED;
    q.review = Button_GetCheck(app.s.review) == BST_CHECKED;
    return q;
}

// 詳細検索の条件を変えた: すぐには探さない（いくつも変えてから「検索」で探す）
void advancedChanged() {
    KillTimer(app.hwnd, kSearchTimer);
    if (app.searchShown) setText(app.s.count, L"条件を変えました。「検索」（Enter）を押すと探します");
}

// 日付の欄を、使わない状態（チェックなし）で 1 年前〜今日にしておく
void resetDatePickers() {
    SYSTEMTIME now;
    GetLocalTime(&now);
    const int fd = now.wMonth == 2 && now.wDay == 29 ? 28 : now.wDay;
    dateBoxSet(app.s.from, false, now.wYear - 1, now.wMonth, fd);
    dateBoxSet(app.s.to, false, now.wYear, now.wMonth, now.wDay);
}

// 条件が変わったら、少し待ってから探す（スライダーを動かしている間や、文字を打っている間は探さない）
void scheduleSearch(UINT delayMs) {
    if (!app.searchShown) return;  // まだ一度も検索していない（条件を変えただけでは探し始めない）
    SetTimer(app.hwnd, kSearchTimer, delayMs, nullptr);
}

void runSearch() {
    KillTimer(app.hwnd, kSearchTimer);
    SearchQuery q = searchQueryFromUi();
    if (!hasConditions(q) && q.clipText.empty() && !q.similarTo && !q.review && app.similarVec.empty()) {
        ++app.searchGen;  // 探している途中の結果は出さない
        // 条件を空にして探した: 検索の結果を出す前に見ていたフォルダに戻る
        if (ex.view && ex.view->ShowingList() && !app.returnDir.empty()) {
            ex.view->Show(app.returnDir, app.returnSel);
            setText(app.s.count, L"条件が空なので、検索の前に見ていたフォルダに戻りました");
        } else {
            setText(app.s.count, L"フリーワードか条件を入れてください（何も無いと全部の画像になります）");
        }
        return;
    }
    app.searchShown = true;
    const unsigned gen = ++app.searchGen;
    setText(app.s.count, L"検索中…");
    const std::string db = narrow(dbPath());
    HWND hwnd = app.hwnd;
    const std::wstring dataDir = app.dataDir;
    const std::vector<float> similarVec = app.similarVec;
    std::thread([q, gen, db, hwnd, dataDir, similarVec]() mutable {
        // 1 回目: 先頭だけ（DB は上位だけを並べるので速い）と全部の数。多ければ 2 回目に残りを全部
        auto* done = new SearchDone{gen, {}, L""};
        bool more = false;
        Db d;
        try {
            if (!d.open(db)) throw std::runtime_error(d.error());
            if (!q.clipText.empty() || q.similarTo || !similarVec.empty()) {
                // 内容（CLIP）: ほかの条件に合う画像を、似ている順に上位だけ
                done->clip = true;
                VectorIndex idx;
                if (!idx.load(narrow(clip::vectorPath(dataDir))))
                    throw std::runtime_error("CLIP のベクトルがまだありません（AI 解析の部品の CLIP を入れると、バックグラウンドの解析で作ります）");
                std::vector<float> qv;
                if (!similarVec.empty()) {
                    qv = similarVec;  // インデックスに無い画像（画像で探す）
                } else if (q.similarTo) {
                    if (!idx.get(q.similarTo, qv)) {
                        // この画像はまだ解析で CLIP を作っていない: その場でこの 1 枚だけ作る
                        std::string path;
                        {
                            Stmt st(d.handle(), "SELECT path FROM images WHERE id=?");
                            st.bind(1, q.similarTo);
                            if (st.step()) path = st.text(0);
                        }
                        std::wstring err;
                        if (path.empty() || !clip::encodeImageFile(dataDir, widen(path), qv, err))
                            throw std::runtime_error("この画像の CLIP のベクトルを作れませんでした" + (err.empty() ? std::string() : "（" + narrow(err) + "）"));
                        logs::info("似た画像: 解析でまだ CLIP を作っていない画像なので、その場で作った（" + path + "）");
                    }
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
                if (q.similarTo) {  // 元の画像を先頭に（結果を出した時にそれを選ぶ）
                    done->result.ids.insert(done->result.ids.begin(), q.similarTo);
                    done->scores.insert(done->scores.begin(), 1.0f);
                    ++done->result.total;
                }
            } else {
                q.limit = kListResults;
                done->result = search(d, q);
            }
            // エクスプローラーに出すパス
            Stmt st(d.handle(), "SELECT path FROM images WHERE id=?");
            for (int64_t id : done->result.ids) {
                st.bind(1, id);
                done->paths.push_back(st.step() ? widen(st.text(0)) : std::wstring());
                st.reset();
            }
            // 重複・差分はまとめて出す（確認キューは 1 枚ずつ確かめるのでまとめない）
            if (!q.review) done->clusters = clusterResults(d, done->result.ids);
            // フリーワードだけで探した時は、名前が合うフォルダも出す（すべて・ファイル名に限る）
            SearchQuery rest = q;
            rest.freeText.clear();
            rest.excludeSources.clear();
            rest.limit = 0;
            if (!q.freeText.empty() && (q.freeScope == FreeScope::All || q.freeScope == FreeScope::Name) && !hasConditions(rest) &&
                !done->clip && !q.review)
                for (const auto& f : matchFolders(currentFolders(d), q.freeText, 200)) done->folders.push_back(widen(f));
        } catch (const std::exception& e) {
            done->error = widen(e.what());
        }
        (void)more;
        if (!PostMessageW(hwnd, WM_APP_SEARCHED, 0, reinterpret_cast<LPARAM>(done))) delete done;
    }).detach();
}

void showSearchInfo(int item);

// 検索の結果を、エクスプローラーの真ん中に出す
void onSearched(SearchDone* p) {
    std::unique_ptr<SearchDone> done(p);
    if (done->gen != app.searchGen) return;  // 古い条件の結果
    if (!done->error.empty()) {
        logs::warn("検索できなかった: " + narrow(done->error));
        setText(app.s.count, L"検索できませんでした: " + done->error);
        if (ex.view) ex.view->SetSameStatus(L"検索できませんでした: " + done->error);
        return;
    }
    app.searchIds = std::move(done->result.ids);
    app.searchPaths = std::move(done->paths);
    app.searchTotal = done->result.total;
    app.searchScores = std::move(done->scores);
    app.infoCache.clear();
    const bool review = Button_GetCheck(app.s.review) == BST_CHECKED;
    std::wstring count;
    if (!app.searchTotal) count = review ? L"確かめる画像はありません" : L"見つかりません";
    else if (done->clip)
        count = withCommas(app.searchTotal) + L" 件のうち、" + (app.similarTo || !app.similarVec.empty() ? L"似ている" : L"内容が近い") + L"順に " +
                withCommas(app.searchIds.size()) + L" 件";
    else
        count = withCommas(app.searchTotal) + L" 件" +
                (app.searchTotal > app.searchIds.size() ? L"（先頭の " + withCommas(app.searchIds.size()) + L" 件を出しています）"
                 : review                               ? L"（迷っている順）"
                                                        : L"");
    if (!done->folders.empty())
        count = L"フォルダ " + withCommas(done->folders.size()) + L" 個・画像 " + (app.searchTotal ? count : L"0 件");
    const std::wstring from =
        app.idsFilter.empty() ? L"" : L"保存した " + withCommas(app.idsFilter.size()) + L" 枚（「条件をクリア」で外します）のうち ";
    setText(app.s.count, from + count + (app.searchNotice.empty() ? L"" : L"　" + app.searchNotice));
    app.searchNotice.clear();
    std::wstring what = editText(app.s.free);
    if (what.empty() && !app.idsFilter.empty()) what = L"保存した画像";
    if (what.empty()) what = done->clip ? (app.similarTo || !app.similarVec.empty() ? app.similarLabel : editText(app.s.clip)) : L"条件";
    // 選び直す画像: タグをクリックして探した時はその時に選んでいた画像、タグを変えた後は同じ位置の画像
    int reselect = app.reselect;
    if (!app.reselectPath.empty()) {
        reselect = -1;
        for (size_t i = 0; i < app.searchPaths.size(); ++i)
            if (lstrcmpiW(app.searchPaths[i].c_str(), app.reselectPath.c_str()) == 0) reselect = static_cast<int>(i);
    }
    if (app.similarTo && app.reselectPath.empty() && reselect < 0 && !app.searchPaths.empty()) reselect = 0;  // 似た画像: 元の画像
    std::vector<std::vector<size_t>> clusters = std::move(done->clusters);
    if (clusters.empty())
        for (size_t i = 0; i < app.searchPaths.size(); ++i) clusters.push_back({i});
    std::vector<std::vector<std::wstring>> entries;
    int select = -1;
    // 名前が合うフォルダを先頭に
    for (const auto& f : done->folders) entries.push_back({f});
    for (const auto& c : clusters) {
        std::vector<std::wstring> members;
        for (size_t i : c) {
            if (i >= app.searchPaths.size() || app.searchPaths[i].empty()) continue;
            if (static_cast<int>(i) == reselect) select = static_cast<int>(entries.size());
            members.push_back(app.searchPaths[i]);
        }
        if (!members.empty()) entries.push_back(std::move(members));
    }
    if (!ex.view->ShowingList()) {  // フォルダから検索の結果へ: 条件を空にして探した時に戻る所
        app.returnDir = ex.view->Dir();
        app.returnSel = ex.view->Selected();
    }
    ex.view->ShowList(L"検索: " + what + L"（" + count + L"）", std::move(entries), select);
    app.reselect = -1;
    app.reselectPath.clear();
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
    {
        wchar_t qt[96] = L"";
        if (info->detail >= 0)
            swprintf(qt, 96, L"　細かさ %.2f（実質 %dpx）・くっきり %.2f", info->detail, info->effective, std::max(0.0, info->sharpness));
        line += qt;
        if (info->aesthetic >= 0) {
            swprintf(qt, 96, L"　見栄え %.1f", info->aesthetic);
            line += qt;
        }
    }
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
                if (t.source == kSourceEmbedded) fileTags += (fileTags.empty() ? L"ファイルのタグ: " : L"、") + tagLabel(t.tag);
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
                lines.emplace_back(p, tagLabel(tag) + c + L"（" + a.detail + L"）");
            }
            std::stable_sort(lines.begin(), lines.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            for (size_t i = 0; i < lines.size() && i < 8; ++i) tags += (tags.empty() ? L"検索用タグ: " : L"　") + lines[i].second;
            if (lines.size() > 8) tags += L"　ほか " + std::to_wstring(lines.size() - 8) + L" 個";
        } catch (const std::exception&) {
        }
    }
    if (fileTags.empty()) fileTags = L"ファイルのタグ: なし";
    if (tags.empty()) tags = L"検索用タグ: なし";
    std::wstring sources;
    if (Db* db = uiDb()) {
        try {
            for (const auto& l : sourceLinks(*db, info->id))
                sources += (sources.empty() ? L"　出典: " : L"　") + widen(l.url) + (l.status == 0 ? L"（リンク切れ）" : L"");
        } catch (const std::exception&) {
        }
    }
    setText(app.info, widen(info->path) + L"\r\n" + line + sources + L"\r\n" + fileTags + L"\r\n" + tags);
}

// 「似た画像を探す」: 内容の欄をその画像にして探す（ほかの条件はそのまま）
void findSimilar(int64_t imageId, const std::wstring& name) {
    app.similarVec.clear();
    app.similarTo = imageId;
    app.similarLabel = L"似た画像: " + name;
    SetWindowTextW(app.s.clip, app.similarLabel.c_str());  // EN_CHANGE が来ても同じ文字なので似た画像のまま
    scheduleSearch(10);
}

void clearSearchConditions() {
    app.similarTo = 0;
    app.similarVec.clear();
    app.idsFilter.clear();
    for (HWND h : {app.s.free, app.s.tags, app.s.text, app.s.clip, app.s.minW, app.s.minKb, app.s.maxKb, app.s.path, app.s.download})
        SetWindowTextW(h, L"");
    ComboBox_SetCurSel(app.s.scope, 0);
    ComboBox_SetCurSel(app.s.dateField, 0);
    SendMessageW(app.s.conf, TBM_SETPOS, TRUE, 50);
    setText(app.s.confValue, L"0.50");
    ComboBox_SetCurSel(app.s.rating, 0);
    ComboBox_SetCurSel(app.s.format, 0);
    ComboBox_SetCurSel(app.s.quality, 0);
    resetDatePickers();
    Button_SetCheck(app.s.unclassified, BST_UNCHECKED);
    Button_SetCheck(app.s.deadLinks, BST_UNCHECKED);
    Button_SetCheck(app.s.review, BST_UNCHECKED);
    app.uncheckedSources.clear();
    refreshSources();
    ComboBox_SetCurSel(app.s.smart, 0);
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
}

// 保存した条件を画面に戻す（画面に無い項目は使わない）
void applyQueryToUi(const SearchQuery& q) {
    app.idsFilter = q.ids;
    // フリーワードの欄に戻す（タグの条件は「タグに限る」、前の版の「文字」はフリーワード）
    std::wstring tags;
    for (const auto& t : q.tags) tags += (tags.empty() ? L"" : L" ") + widen(t);
    for (const auto& t : q.excludeTags) tags += (tags.empty() ? L"-" : L" -") + widen(t);
    const bool fileTagsOnly = q.sources.size() == 1 && q.sources[0] == kSourceEmbedded;
    if (!tags.empty()) {
        SetWindowTextW(app.s.free, tags.c_str());
        ComboBox_SetCurSel(app.s.scope, fileTagsOnly ? 3 : 1);
    } else {
        SetWindowTextW(app.s.free, widen(!q.freeText.empty() ? q.freeText : q.text).c_str());
        ComboBox_SetCurSel(app.s.scope, q.freeScope == FreeScope::Title ? 2 : q.freeScope == FreeScope::Name ? 4 : 0);
    }
    const int conf = std::clamp(static_cast<int>(q.minConfidence * 100 + 0.5), 0, 100);
    SendMessageW(app.s.conf, TBM_SETPOS, TRUE, conf);
    wchar_t v[16];
    swprintf(v, 16, L"%.2f", conf / 100.0);
    setText(app.s.confValue, v);
    app.uncheckedSources.clear();
    for (const auto& n : q.excludeSources) app.uncheckedSources.insert(n);
    if (!q.sources.empty() && !fileTagsOnly)
        for (const auto& n : app.sourceNames)
            if (std::find(q.sources.begin(), q.sources.end(), n) == q.sources.end()) app.uncheckedSources.insert(n);
    refreshSources();
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
    ComboBox_SetCurSel(app.s.quality, static_cast<int>(q.quality));
    resetDatePickers();
    auto setDate = [](HWND h, int64_t t) {
        if (!t) return;
        int y, mo, d, hh, mi, ss;
        unixToCivil(t, y, mo, d, hh, mi, ss);
        dateBoxSet(h, true, y, mo, d);
    };
    setDate(app.s.from, q.takenFrom);
    setDate(app.s.to, q.takenTo);
    Button_SetCheck(app.s.unclassified, q.unclassified ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(app.s.deadLinks, q.deadLinks ? BST_CHECKED : BST_UNCHECKED);
    auto setNum = [](HWND h, int64_t n) { SetWindowTextW(h, n > 0 ? std::to_wstring(n).c_str() : L""); };
    setNum(app.s.minW, q.minWidth);
    setNum(app.s.minKb, q.minSize / 1024);
    setNum(app.s.maxKb, q.maxSize / 1024);
    SetWindowTextW(app.s.path, widen(q.pathContains).c_str());
    SetWindowTextW(app.s.download, widen(q.downloadContains).c_str());
    for (size_t i = 0; i < std::size(kSortChoices); ++i)
        if (kSortChoices[i] == q.sort) ComboBox_SetCurSel(app.s.sort, static_cast<int>(i));
    ComboBox_SetCurSel(app.s.dateField, static_cast<int>(q.dateField));
    Button_SetCheck(app.s.desc, q.descending ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(app.s.review, q.review ? BST_CHECKED : BST_UNCHECKED);
    runSearch();
}

// 名前を聞いて、q をスマートフォルダに保存する（同じ名前があれば置き換えてよいか聞く）
bool saveSmartFolderAs(const SearchQuery& q, const std::wstring& message, const std::wstring& initial) {
    Db* db = uiDb();
    if (!db) return false;
    auto name = promptText(L"スマートフォルダに保存", message, initial);
    if (!name) return false;
    while (!name->empty() && iswspace(name->back())) name->pop_back();
    while (!name->empty() && iswspace(name->front())) name->erase(name->begin());
    if (name->empty()) return false;
    for (const auto& f : app.smartList) {
        if (_wcsicmp(widen(f.name).c_str(), name->c_str()) == 0 &&
            logBox(app.hwnd, (L"「" + *name + L"」を置き換えますか？").c_str(), L"imgdupe", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return false;
    }
    try {
        saveSmartFolder(*db, narrow(*name), q);
    } catch (const std::exception& e) {
        logBox(app.hwnd, widen(e.what()).c_str(), L"imgdupe", MB_ICONWARNING);
        return false;
    }
    refreshSmartFolders(*name);
    return true;
}

// 選んだ画像をスマートフォルダに保存する（中身は画像の ID の一覧。開くとその画像を検索の結果として出す）
void saveSelectionSmartUi() {
    size_t missing = 0;
    const std::vector<int64_t> ids = selectedImageIds(&missing);
    if (ids.empty()) {
        logBox(app.hwnd,
                    missing ? L"選んだ画像はまだインデックスにありません（インデックスを作った画像だけ保存できます）。"
                            : L"保存する画像を選んでください（Ctrl や Shift を押しながらクリック、空いている所からドラッグで囲むと、いくつも選べます）。",
                    L"imgdupe", MB_ICONINFORMATION);
        return;
    }
    SearchQuery q;
    q.ids = ids;
    q.sort = SortKey::FileName;
    std::wstring message = L"名前（選んだ " + withCommas(ids.size()) + L" 枚を保存します";
    if (missing) message += L"。インデックスに無い " + withCommas(missing) + L" 枚は入りません";
    message += L"）";
    if (saveSmartFolderAs(q, message, L"選んだ画像") && ex.view) {
        const int i = ComboBox_GetCurSel(app.s.smart) - 1;
        const std::wstring name = i >= 0 && i < static_cast<int>(app.smartList.size()) ? widen(app.smartList[i].name) : L"";
        ex.view->SetSameStatus(withCommas(ids.size()) + L" 枚をスマートフォルダ「" + name + L"」に保存しました（検索の「保存した条件」から開けます）");
    }
}

void saveSmartFolderUi() {
    const int i = ComboBox_GetCurSel(app.s.smart) - 1;
    const std::wstring initial = i >= 0 && i < static_cast<int>(app.smartList.size()) ? widen(app.smartList[i].name) : L"";
    saveSmartFolderAs(searchQueryFromUi(), L"名前（今の検索条件を保存します）", initial);
}

void deleteSmartFolderUi() {
    Db* db = uiDb();
    const int i = ComboBox_GetCurSel(app.s.smart) - 1;
    if (!db || i < 0 || i >= static_cast<int>(app.smartList.size())) return;
    const std::wstring name = widen(app.smartList[i].name);
    if (logBox(app.hwnd, (L"スマートフォルダ「" + name + L"」を削除しますか？（画像は消えません）").c_str(), L"imgdupe",
                    MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    try {
        deleteSmartFolder(*db, app.smartList[i].id);
    } catch (const std::exception&) {
    }
    ComboBox_SetCurSel(app.s.smart, 0);
    refreshSmartFolders(L"");
}

std::wstring trimmed(std::wstring s);

// 保存した条件のファイル（JSON）を選ぶ
std::wstring pickQueriesFile(bool save) {
    Com<IFileDialog> dlg;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                save ? __uuidof(IFileSaveDialog) : __uuidof(IFileOpenDialog), reinterpret_cast<void**>(dlg.put()))))
        return L"";
    const COMDLG_FILTERSPEC types[] = {{L"保存した条件（JSON）", L"*.json"}};
    dlg->SetFileTypes(1, types);
    dlg->SetDefaultExtension(L"json");
    if (save) dlg->SetFileName(L"imgdupe_saved_queries");
    dlg->SetTitle(save ? L"保存した条件を書き出す" : L"保存した条件を読み込む");
    if (FAILED(dlg->Show(app.hwnd))) return L"";
    Com<IShellItem> item;
    PWSTR p = nullptr;
    if (FAILED(dlg->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return L"";
    std::wstring out(p);
    CoTaskMemFree(p);
    return out;
}

// ---------------------------------------------------------------- フォルダ名の形を教える

// 役目の選択肢（画面の並び）。"" は使わない
const std::pair<const char*, const wchar_t*> kTeachRoles[] = {
    {"", L"（使わない）"},         {"artist", L"作者名"},           {"circle", L"サークル名"},
    {"title", L"タイトル"},        {"event", L"イベント"},          {"pixiv", L"pixiv のユーザー ID"},
    {"x", L"X（Twitter）のアカウント"}, {"fanbox", L"FANBOX のクリエイター"}, {"fantia", L"Fantia のファンクラブ ID"},
    {"skeb", L"Skeb のアカウント"}, {"nijie", L"ニジエのユーザー ID"}, {"misskey", L"Misskey のアカウント"},
    {"bluesky", L"Bluesky のアカウント"}, {"booth", L"BOOTH のショップ"}, {"patreon", L"Patreon"},
    {"ci-en", L"Ci-en のクリエイター"}};

std::wstring teachRoleLabel(const std::string& role) {
    for (const auto& [k, v] : kTeachRoles)
        if (role == k) return v;
    return widen(role);
}

// 形を人に見せる形に（"S ( pixiv D )" → "［文字］(pixiv［数字］)"）
std::wstring humanShape(const std::string& key) {
    std::wstring out;
    size_t a = 0;
    while (a <= key.size()) {
        size_t b = key.find(' ', a);
        if (b == std::string::npos) b = key.size();
        const std::string t = key.substr(a, b - a);
        if (t == "D") out += L"［数字］";
        else if (t == "S") out += L"［文字］";
        else if (t == "@") out += L"@［名前］";
        else if (t == "pixiv" || t == "twitter" || t == "fanbox" || t == "fantia" || t == "skeb" || t == "nijie" || t == "id")
            out += L" " + widen(t) + L" ";
        else out += widen(t);
        a = b + 1;
    }
    return out;
}

std::wstring inferredText(const std::vector<InferredTag>& tags) {
    std::wstring t;
    for (const auto& x : tags) {
        if (!t.empty()) t += L"、";
        if (x.kind == "artist") t += L"作者: ";
        else if (x.kind == "circle") t += L"サークル: ";
        else if (x.kind == "title") t += L"タイトル: ";
        else if (x.kind == "event") t += L"イベント: ";
        t += widen(x.tag);
    }
    return t.empty() ? L"（何も付けない）" : t;
}

// dir のフォルダ名の部分ごとに役目を教える（dir が空なら、教えた形の一覧だけ）
void teachFolderUi(const std::wstring& dirIn) {
    Db* db = uiDb();
    if (!db) return;
    std::wstring dir = dirIn;
    while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    const size_t sep = dir.find_last_of(L"\\/");
    const std::wstring name = sep == std::wstring::npos ? dir : dir.substr(sep + 1);
    const std::wstring parent = sep == std::wstring::npos ? L"" : dir.substr(0, sep);
    const std::wstring parentName = parent.substr(parent.find_last_of(L"\\/") == std::wstring::npos ? 0 : parent.find_last_of(L"\\/") + 1);
    const NameShape shape = name.empty() || (name.size() == 2 && name[1] == L':') ? NameShape{} : nameShape(narrow(name));
    const size_t kMaxSlots = 8;
    const size_t slots = std::min(shape.slots.size(), kMaxSlots);

    enum { ID_SAVE = 100, ID_SCOPE_HERE, ID_SCOPE_ANY, ID_RULES, ID_DELETE, ID_ROLE0 = 200 };
    struct State {
        bool done = false;
        int command = 0;
    } st;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            if (msg == WM_COMMAND && st) {
                if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK) st->done = true;
                else if (LOWORD(wp) >= ID_ROLE0 && LOWORD(wp) < ID_ROLE0 + 16) {
                    if (HIWORD(wp) == CBN_SELCHANGE) st->command = LOWORD(wp);
                } else if (LOWORD(wp) == ID_RULES) {
                    if (HIWORD(wp) == LBN_SELCHANGE) st->command = ID_RULES;
                } else st->command = LOWORD(wp);
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
        wc.lpszClassName = L"ImgdupeTeachFolder";
        RegisterClassExW(&wc);
        registered = true;
    }
    const int w = px(660), m = px(12), rowH = px(30);
    const int slotsH = slots ? static_cast<int>(slots) * rowH : px(24);
    const int h = m + px(20) + px(40) + px(22) + slotsH + px(8) + px(48) + px(22) + px(130) + px(10) + px(22) + px(100) + px(10) + px(30) + m;
    RECT owner;
    GetWindowRect(app.hwnd, &owner);
    RECT frame{0, 0, w, h};
    AdjustWindowRectEx(&frame, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ImgdupeTeachFolder", L"フォルダ名から作者などを読み取る形", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               (owner.left + owner.right - fw) / 2, std::max(0L, (owner.top + owner.bottom - fh) / 2), fw, fh, app.hwnd,
                               nullptr, app.inst, nullptr);
    if (!dlg) return;
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    HWND tip = makeTooltip(dlg, px(360));
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int cw, int ch, DWORD exs = 0) {
        HWND c = CreateWindowExW(exs, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cw, ch, dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
        return c;
    };
    int y = m;
    child(WC_STATICW, (L"フォルダ: " + (name.empty() ? std::wstring(L"（なし）") : name)).c_str(), SS_NOPREFIX | SS_ENDELLIPSIS, -1, m, y,
          w - 2 * m, px(20));
    y += px(20);
    child(WC_STATICW,
          slots ? L"名前を部分に分けました。部分ごとに何を表すか選んで「教える」を押すと、同じ形の名前のフォルダ（と中のフォルダ）の画像に、"
                  L"作者・アカウントなどのタグを付けます（AI は使いません。ファイルには書きません）。"
                : L"フォルダを右クリックして「このフォルダ名から作者などを読み取る形を教える」を選ぶと、ここで形を教えられます。",
          SS_NOPREFIX, -1, m, y, w - 2 * m, px(40));
    y += px(40);
    child(WC_STATICW, (L"形: " + (slots ? humanShape(shape.key) : std::wstring(L"―"))).c_str(), SS_NOPREFIX, -1, m, y, w - 2 * m, px(20));
    y += px(22);
    std::vector<HWND> combos;
    // 今の役目（教えた形があればそれ、無ければ名前だけで分かるもの）
    std::vector<InferRule> rules;
    try {
        rules = loadInferRules(*db);
    } catch (const std::exception&) {
    }
    std::vector<std::string> initial(slots);
    {
        const InferRule* found = nullptr;
        for (const auto& r : rules)
            if (r.shape == shape.key && r.roles.size() == shape.slots.size() && (r.scope.empty() || _wcsicmp(widen(r.scope).c_str(), parent.c_str()) == 0))
                if (!found || !r.scope.empty()) found = &r;
        if (found) {
            for (size_t i = 0; i < slots; ++i) initial[i] = found->roles[i];
        } else if (slots) {
            const auto guess = inferFromName(narrow(name), "");
            for (size_t i = 0; i < slots; ++i)
                for (const auto& [role, label] : kTeachRoles) {
                    if (!*role) continue;
                    std::vector<std::string> one(shape.slots.size());
                    one[i] = role;
                    const auto t = applyRoles(shape, one, 1.0);
                    for (const auto& g : guess)
                        if (!t.empty() && g.kind == t[0].kind && g.tag == t[0].tag) initial[i] = role;
                }
        }
    }
    for (size_t i = 0; i < slots; ++i) {
        const NameSlot& s = shape.slots[i];
        const std::wstring kind = s.cls == 'D' ? L"数字" : s.cls == '@' ? L"@ 名前" : L"文字";
        child(WC_STATICW, (L"「" + widen(s.value) + L"」（" + kind + L"）").c_str(), SS_NOPREFIX | SS_ENDELLIPSIS | SS_CENTERIMAGE, -1, m,
              y + static_cast<int>(i) * rowH, px(360), px(26));
        HWND cb = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, ID_ROLE0 + static_cast<int>(i), m + px(370),
                        y + static_cast<int>(i) * rowH, w - 2 * m - px(370), px(400));
        int sel = 0, k = 0;
        for (const auto& [role, label] : kTeachRoles) {
            ComboBox_AddString(cb, label);
            if (initial[i] == role) sel = k;
            ++k;
        }
        ComboBox_SetCurSel(cb, sel);
        combos.push_back(cb);
    }
    if (shape.slots.size() > kMaxSlots)
        child(WC_STATICW, L"（部分が多すぎるので、後ろの部分は使いません）", SS_NOPREFIX, -1, m, y + slotsH - px(4), w - 2 * m, px(20));
    y += slotsH + px(8);
    HWND here = child(WC_BUTTONW, (L"このフォルダと同じ場所（" + (parentName.empty() ? parent : parentName) + L"）の下の同じ形だけ").c_str(),
                      WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP, ID_SCOPE_HERE, m, y, w - 2 * m, px(22));
    HWND any = child(WC_BUTTONW, L"どこでも同じ形なら（「［文字］」だけの形には向きません）",
                     WS_TABSTOP | BS_AUTORADIOBUTTON, ID_SCOPE_ANY, m, y + px(24), w - 2 * m, px(22));
    Button_SetCheck(here, BST_CHECKED);
    EnableWindow(here, slots > 0);
    EnableWindow(any, slots > 0);
    y += px(48);
    HWND countText = child(WC_STATICW, L"", SS_NOPREFIX, -1, m, y, w - 2 * m, px(20));
    y += px(22);
    HWND preview = child(WC_LISTBOXW, L"", WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, -1, m, y, w - 2 * m, px(130), WS_EX_CLIENTEDGE);
    y += px(140);
    child(WC_STATICW, L"教えた形（選んで「削除」で消せます）", SS_NOPREFIX, -1, m, y, w - 2 * m, px(20));
    y += px(22);
    const int delW = px(90);
    HWND ruleList = child(WC_LISTBOXW, L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, ID_RULES, m, y,
                          w - 2 * m - delW - px(8), px(100), WS_EX_CLIENTEDGE);
    HWND delBtn = child(WC_BUTTONW, L"削除", WS_TABSTOP, ID_DELETE, w - m - delW, y, delW, px(28));
    addTooltip(tip, delBtn, L"選んだ教えた形を消します（その形で付けたタグは、次の推し量りで外れます）");
    y += px(110);
    HWND status = child(WC_STATICW, L"", SS_NOPREFIX | SS_ENDELLIPSIS, -1, m, y + px(6), w - 2 * m - px(260), px(20));
    HWND saveBtn = child(WC_BUTTONW, L"教える", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_SAVE, w - m - px(250), y, px(120), px(30));
    addTooltip(tip, saveBtn, L"この形を覚えて、同じ形のフォルダの画像にタグを付けます（バックグラウンドの解析が行います）");
    EnableWindow(saveBtn, slots > 0);
    child(WC_BUTTONW, L"閉じる", WS_TABSTOP, IDOK, w - m - px(120), y, px(120), px(30));

    auto fillRules = [&] {
        try {
            rules = loadInferRules(*db);
        } catch (const std::exception&) {
            rules.clear();
        }
        ListBox_ResetContent(ruleList);
        for (const auto& r : rules) {
            std::wstring roles;
            for (const auto& x : r.roles) roles += (roles.empty() ? L"" : L"・") + teachRoleLabel(x);
            const std::wstring scope = r.scope.empty() ? L"どこでも" : widen(r.scope.substr(r.scope.find_last_of("\\/") + 1)) + L" の下";
            ListBox_AddString(ruleList, (humanShape(r.shape) + L" → " + roles + L"（" + scope + L"、例: " + widen(r.example) + L"）　" +
                                         (r.scope.empty() ? L"" : widen(r.scope)))
                                            .c_str());
        }
        SendMessageW(ruleList, LB_SETHORIZONTALEXTENT, px(1600), 0);
    };
    fillRules();

    // 同じ形のフォルダの名前（同じ場所の下・どこでも）。どこでもの分は初めて選んだ時に集める
    std::vector<std::string> hereNames, anyNames;
    bool anyLoaded = false;
    auto collect = [&](const std::string& under, std::vector<std::string>& out) {
        std::set<std::string> names;
        try {
            const std::string prefix = under.empty() ? std::string() : under + "\\";
            Stmt q(db->handle(), under.empty() ? "SELECT path FROM images" : "SELECT path FROM images WHERE path > ? AND path < ?");
            if (!under.empty()) q.bind(1, prefix).bind(2, prefix + "\xff");
            while (q.step()) {
                const std::string p = q.text(0);
                if (under.empty()) {
                    // どこでも: 全部のフォルダの名前
                    size_t a = 0;
                    const size_t last = p.find_last_of("\\/");
                    while (last != std::string::npos && a < last) {
                        size_t b = p.find_first_of("\\/", a);
                        if (b == std::string::npos || b > last) b = last;
                        if (b > a) names.insert(p.substr(a, b - a));
                        a = b + 1;
                    }
                } else {
                    const std::string rest = p.substr(prefix.size());
                    const size_t b = rest.find_first_of("\\/");
                    if (b != std::string::npos) names.insert(rest.substr(0, b));
                }
            }
        } catch (const std::exception&) {
        }
        for (const auto& n : names)
            if (nameShape(n).key == shape.key) out.push_back(n);
    };
    if (slots) collect(narrow(parent), hereNames);
    auto currentRoles = [&] {
        std::vector<std::string> roles(shape.slots.size());
        for (size_t i = 0; i < combos.size(); ++i) {
            const int k = ComboBox_GetCurSel(combos[i]);
            if (k >= 0 && k < static_cast<int>(std::size(kTeachRoles))) roles[i] = kTeachRoles[k].first;
        }
        return roles;
    };
    auto updatePreview = [&] {
        if (!slots) {
            setText(countText, L"");
            return;
        }
        const bool anywhere = Button_GetCheck(any) == BST_CHECKED;
        if (anywhere && !anyLoaded) {
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            collect("", anyNames);
            anyLoaded = true;
        }
        const auto& list = anywhere ? anyNames : hereNames;
        const auto roles = currentRoles();
        SendMessageW(preview, WM_SETREDRAW, FALSE, 0);
        ListBox_ResetContent(preview);
        size_t shown = 0;
        for (const auto& n : list) {
            if (shown++ >= 500) break;
            ListBox_AddString(preview, (widen(n) + L"　→　" + inferredText(applyRoles(nameShape(n), roles, 1.0))).c_str());
        }
        SendMessageW(preview, WM_SETREDRAW, TRUE, 0);
        setText(countText, L"同じ形のフォルダ: " + withCommas(list.size()) + L" 個" + (list.size() > 500 ? L"（初めの 500 個を出します）" : L"") +
                               L"　どう付くか:");
    };
    updatePreview();

    EnableWindow(app.hwnd, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(combos.empty() ? ruleList : combos[0]);
    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            st.done = true;
            continue;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!st.command) continue;
        const int cmd = st.command;
        st.command = 0;
        try {
            if (cmd == ID_SCOPE_HERE || cmd == ID_SCOPE_ANY || (cmd >= ID_ROLE0 && cmd < ID_ROLE0 + 16)) {
                updatePreview();
            } else if (cmd == ID_SAVE) {
                InferRule r;
                r.scope = Button_GetCheck(any) == BST_CHECKED ? std::string() : narrow(parent);
                r.shape = shape.key;
                r.roles = currentRoles();
                r.example = narrow(name);
                saveInferRule(*db, r);
                std::string roles;
                for (const auto& x : r.roles) roles += (roles.empty() ? "" : ",") + (x.empty() ? std::string("-") : x);
                logs::info("フォルダ名の形を教えた: " + r.shape + " → " + roles + "（" + (r.scope.empty() ? std::string("どこでも") : r.scope) + "）");
                fillRules();
                indexer::requestInfer(app.dataDir);
                if (!indexer::isRunning()) indexer::start();
                setText(status, L"教えました。少し待つとタグが付きます");
            } else if (cmd == ID_DELETE) {
                const int i = ListBox_GetCurSel(ruleList);
                if (i >= 0 && i < static_cast<int>(rules.size()) &&
                    logBox(dlg, L"選んだ形を消しますか？（その形で付けたタグは外れます）", L"imgdupe", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                    deleteInferRule(*db, rules[i].id);
                    logs::info("教えたフォルダ名の形を消した: " + rules[i].shape);
                    fillRules();
                    indexer::requestInfer(app.dataDir);
                    if (!indexer::isRunning()) indexer::start();
                    setText(status, L"消しました");
                }
            }
        } catch (const std::exception& e) {
            logBox(dlg, (L"できませんでした: " + widen(e.what())).c_str(), L"imgdupe", MB_ICONWARNING);
        }
    }
    EnableWindow(app.hwnd, TRUE);
    DestroyWindow(dlg);
    SetForegroundWindow(app.hwnd);
}

// 保存した条件の管理（削除・並べ替え・名前の変更・書き出し・読み込み）
void manageSmartFoldersUi() {
    Db* db = uiDb();
    if (!db) return;
    enum { ID_LIST = 100, ID_LOAD, ID_UP, ID_DOWN, ID_RENAME, ID_DELETE, ID_EXPORT, ID_IMPORT };
    struct State {
        HWND list = nullptr;
        bool done = false;
        int command = 0;  // 押されたボタン（下のループで処理する）
    } st;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            if (msg == WM_COMMAND && st) {
                if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK) st->done = true;
                else if (LOWORD(wp) == ID_LIST) {
                    if (HIWORD(wp) == LBN_DBLCLK) st->command = ID_LOAD;
                } else st->command = LOWORD(wp);
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
        wc.lpszClassName = L"ImgdupeManageQueries";
        RegisterClassExW(&wc);
        registered = true;
    }
    const int w = px(480), h = px(360);
    RECT owner;
    GetWindowRect(app.hwnd, &owner);
    RECT frame{0, 0, w, h};
    AdjustWindowRectEx(&frame, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ImgdupeManageQueries", L"保存した条件の管理", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               (owner.left + owner.right - fw) / 2, (owner.top + owner.bottom - fh) / 2, fw, fh, app.hwnd, nullptr,
                               app.inst, nullptr);
    if (!dlg) return;
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    HWND tip = makeTooltip(dlg, px(320));
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int cw, int ch, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cw, ch, dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
        return c;
    };
    const int m = px(12), bw = px(120), bh = px(28), bx = w - m - bw;
    child(WC_STATICW, L"検索の「保存した条件」に出す順に並びます。", SS_NOPREFIX, -1, m, m, w - 2 * m, px(20));
    st.list = child(WC_LISTBOXW, L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, ID_LIST, m, m + px(24),
                    bx - px(8) - m, h - 2 * m - px(24), WS_EX_CLIENTEDGE);
    int by = m + px(24);
    auto btn = [&](const wchar_t* text, int id, const wchar_t* help, int gapAfter = 4) {
        HWND b = child(WC_BUTTONW, text, WS_TABSTOP, id, bx, by, bw, bh);
        addTooltip(tip, b, help);
        by += bh + px(gapAfter);
    };
    btn(L"読み込む", ID_LOAD, L"選んだ条件を検索の画面に読み込んで探します（ダブルクリックでも）", 12);
    btn(L"上へ", ID_UP, L"選んだ条件を 1 つ上に動かします");
    btn(L"下へ", ID_DOWN, L"選んだ条件を 1 つ下に動かします");
    btn(L"名前を変える…", ID_RENAME, L"選んだ条件の名前を変えます");
    btn(L"削除", ID_DELETE, L"選んだ条件を削除します（画像は消えません）", 12);
    btn(L"書き出し…", ID_EXPORT, L"保存した条件を全部 JSON のファイルに書き出します（ほかの PC への引っ越しや控えに）");
    btn(L"読み込み…", ID_IMPORT, L"書き出した JSON のファイルから条件を読み込みます（同じ名前は上書き）");
    child(WC_BUTTONW, L"閉じる", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, bx, h - m - bh, bw, bh);

    std::vector<SmartFolder> list;
    auto fill = [&](int64_t selectId) {
        try {
            list = smartFolders(*db);
        } catch (const std::exception&) {
            list.clear();
        }
        ListBox_ResetContent(st.list);
        int sel = list.empty() ? -1 : 0;
        for (size_t i = 0; i < list.size(); ++i) {
            ListBox_AddString(st.list, widen(list[i].name).c_str());
            if (list[i].id == selectId) sel = static_cast<int>(i);
        }
        ListBox_SetCurSel(st.list, sel);
    };
    fill(0);
    EnableWindow(app.hwnd, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.list);
    MSG msg{};
    bool changed = false;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            st.done = true;
            continue;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!st.command) continue;
        const int cmd = st.command;
        st.command = 0;
        const int i = ListBox_GetCurSel(st.list);
        const bool has = i >= 0 && i < static_cast<int>(list.size());
        try {
            switch (cmd) {
                case ID_LOAD:
                    if (has) {
                        const SearchQuery q = list[i].query;
                        const std::wstring name = widen(list[i].name);
                        st.done = true;
                        EnableWindow(app.hwnd, TRUE);
                        refreshSmartFolders(name);
                        applyQueryToUi(q);
                    }
                    break;
                case ID_UP:
                case ID_DOWN: {
                    const int j = cmd == ID_UP ? i - 1 : i + 1;
                    if (!has || j < 0 || j >= static_cast<int>(list.size())) break;
                    std::vector<int64_t> ids;
                    for (const auto& f : list) ids.push_back(f.id);
                    std::swap(ids[i], ids[j]);
                    setSmartFolderOrder(*db, ids);
                    fill(list[i].id);
                    changed = true;
                    break;
                }
                case ID_RENAME:
                    if (has) {
                        const auto name = promptText(L"名前を変える", L"新しい名前", widen(list[i].name));
                        if (name && !trimmed(*name).empty()) {
                            if (!renameSmartFolder(*db, list[i].id, narrow(trimmed(*name))))
                                logBox(dlg, L"同じ名前の条件がもうあります。", L"imgdupe", MB_ICONINFORMATION);
                            fill(list[i].id);
                            changed = true;
                        }
                        EnableWindow(app.hwnd, FALSE);
                    }
                    break;
                case ID_DELETE:
                    if (has && logBox(dlg, (L"「" + widen(list[i].name) + L"」を削除しますか？（画像は消えません）").c_str(), L"imgdupe",
                                           MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                        deleteSmartFolder(*db, list[i].id);
                        fill(i + 1 < static_cast<int>(list.size()) ? list[i + 1].id : i > 0 ? list[i - 1].id : 0);
                        changed = true;
                    }
                    break;
                case ID_EXPORT: {
                    if (list.empty()) {
                        logBox(dlg, L"保存した条件がありません。", L"imgdupe", MB_ICONINFORMATION);
                        break;
                    }
                    const std::wstring file = pickQueriesFile(true);
                    if (file.empty()) break;
                    const std::string text = exportSmartFolders(list);
                    FILE* f = _wfopen(file.c_str(), L"wb");
                    const bool ok = f && std::fwrite(text.data(), 1, text.size(), f) == text.size();
                    if (f) std::fclose(f);
                    logBox(dlg, ok ? (std::to_wstring(list.size()) + L" 件を書き出しました。").c_str() : L"書き出せませんでした。",
                                L"imgdupe", ok ? MB_ICONINFORMATION : MB_ICONWARNING);
                    break;
                }
                case ID_IMPORT: {
                    const std::wstring file = pickQueriesFile(false);
                    if (file.empty()) break;
                    std::string text;
                    if (FILE* f = _wfopen(file.c_str(), L"rb")) {
                        char buf[65536];
                        size_t n;
                        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
                        std::fclose(f);
                    }
                    std::vector<SmartFolder> got;
                    if (!importSmartFolders(text, got)) {
                        logBox(dlg, L"保存した条件のファイルとして読めませんでした。", L"imgdupe", MB_ICONWARNING);
                        break;
                    }
                    int64_t last = 0;
                    for (const auto& g : got) last = saveSmartFolder(*db, g.name, g.query);
                    fill(last);
                    changed = true;
                    logBox(dlg, (std::to_wstring(got.size()) + L" 件を読み込みました（同じ名前は上書き）。").c_str(), L"imgdupe",
                                MB_ICONINFORMATION);
                    break;
                }
            }
        } catch (const std::exception& e) {
            logBox(dlg, (L"できませんでした: " + widen(e.what())).c_str(), L"imgdupe", MB_ICONWARNING);
        }
    }
    if (msg.message == WM_QUIT) PostQuitMessage(static_cast<int>(msg.wParam));
    EnableWindow(app.hwnd, TRUE);
    DestroyWindow(dlg);
    SetActiveWindow(app.hwnd);
    if (changed) refreshSmartFolders(L"");
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
        logBox(app.hwnd, L"フォルダを作っています。終わるまでお待ちください。", L"imgdupe", MB_ICONINFORMATION);
        return;
    }
    if (!app.searchTotal) {
        logBox(app.hwnd, L"フォルダにする検索結果がありません。", L"imgdupe", MB_ICONINFORMATION);
        return;
    }
    const int i = ComboBox_GetCurSel(app.s.smart) - 1;
    const std::wstring initial = i >= 0 && i < static_cast<int>(app.smartList.size()) ? widen(app.smartList[i].name) : L"検索結果";
    auto input = promptText(L"フォルダを作成", L"フォルダの名前（画像のあるドライブごとに、_vfolders の下に作ります）", initial);
    if (!input || trimmed(*input).empty()) return;
    const std::string name = safeFolderName(narrow(trimmed(*input)));
    std::wstring msg = withCommas(app.searchTotal) + L" 枚を、ハードリンクで「" + widen(name) + L"」フォルダに並べます。\r\n\r\n" +
                       L"・容量は増えません。リンク側の画像を編集すると、元の画像も変わります。\r\n" +
                       L"・このソフトを終了すると消えます（残す時は「展開したフォルダ」で固定にします）。\r\n" +
                       L"・同じ名前で前に展開したフォルダは、今の結果で作り直します。";
    if (app.searchTotal > 10000) msg += L"\r\n\r\n枚数が多いので、少し時間がかかります。";
    if (logBox(app.hwnd, msg.c_str(), L"フォルダに展開", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return;

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
        logBox(app.hwnd, (L"展開できませんでした: " + done->error).c_str(), L"imgdupe", MB_ICONWARNING);
        return;
    }
    const ExpandResult& r = done->result;
    std::wstring msg = withCommas(r.created) + L" 個のリンクを作りました。";
    for (const auto& f : r.folders) msg += L"\r\n" + widen(f);
    if (r.failed)
        msg += L"\r\n\r\n作れなかった画像: " + withCommas(r.failed) +
               L" 枚（元の画像が無い、ネットワークや FAT32 / exFAT のドライブなど）\r\n例: " + widen(r.firstFailure);
    if (!r.created) {
        logBox(app.hwnd, msg.c_str(), L"フォルダに展開", MB_ICONWARNING);
        return;
    }
    msg += L"\r\n\r\nフォルダを開きますか？";
    if (logBox(app.hwnd, msg.c_str(), L"フォルダに展開", MB_YESNO | MB_ICONINFORMATION) == IDYES)
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
        if (logBox(w.wnd, (widen(drive) + L" の画像は、これから\r\n" + root + L"\r\nの下に展開します。").c_str(), L"imgdupe",
                        MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;
        try {
            setVfolderRoot(*db, drive, narrow(root));
            imgwin::setScanExcludes(customVfolderRoots(*db));
        } catch (const std::exception& e) {
            logBox(w.wnd, widen(e.what()).c_str(), L"imgdupe", MB_ICONWARNING);
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
                if (logBox(w.wnd, (L"「" + widen(f.dir) + L"」のリンク " + withCommas(f.count) + L" 個を削除しますか？\r\n（元の画像は消えません）").c_str(),
                                L"imgdupe", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                    return;
                const RemoveResult r = removeLinks(*db, f.dir, false);
                std::wstring msg;
                if (r.failed) msg += withCommas(r.failed) + L" 個は開かれていて消せませんでした（次に起動した時にまた消します）。\r\n";
                if (r.kept) msg += withCommas(r.kept) + L" 個は元の画像が消えていて、データがリンクにしか無いので残しました。\r\n";
                if (!msg.empty()) logBox(w.wnd, msg.c_str(), L"imgdupe", MB_ICONINFORMATION);
                break;
            }
            default: return;
        }
    } catch (const std::exception& e) {
        logBox(w.wnd, widen(e.what()).c_str(), L"imgdupe", MB_ICONWARNING);
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

// エクスプローラーで選んでいる検索の結果（searchIds の番号）
// 今の項目が検索の結果の何番目か（-1 = 結果に無い）
int selectedResult() {
    const std::wstring sel = ex.view ? ex.view->Selected() : L"";
    if (sel.empty()) return -1;
    for (size_t i = 0; i < app.searchPaths.size() && i < app.searchIds.size(); ++i)
        if (lstrcmpiW(app.searchPaths[i].c_str(), sel.c_str()) == 0) return static_cast<int>(i);
    return -1;
}

// エクスプローラーで選んでいる画像の ID（インデックスに無いものは数だけ missing に）
std::vector<int64_t> selectedImageIds(size_t* missing) {
    std::vector<int64_t> out;
    if (missing) *missing = 0;
    if (!ex.view) return out;
    const auto paths = ex.view->SelectedPaths();
    std::unordered_map<std::wstring, int64_t> results;  // 検索の結果なら ID が分かっている
    for (size_t i = 0; i < app.searchPaths.size() && i < app.searchIds.size(); ++i) {
        std::wstring k = app.searchPaths[i];
        CharLowerBuffW(k.data(), static_cast<DWORD>(k.size()));
        results.emplace(std::move(k), app.searchIds[i]);
    }
    Db* db = uiDb();
    std::vector<std::string> roots;
    for (const auto& f : folderList()) roots.push_back(narrow(f));
    std::set<int64_t> seen;
    for (const auto& p : paths) {
        if (!imgdupe::isImageExt(narrow(p))) continue;
        std::wstring k = p;
        CharLowerBuffW(k.data(), static_cast<DWORD>(k.size()));
        int64_t id = 0;
        if (auto it = results.find(k); it != results.end()) id = it->second;
        else if (db) {
            try {
                id = findImageId(*db, narrow(p), roots);
            } catch (const std::exception&) {
            }
        }
        if (!id) {
            if (missing) ++*missing;
            continue;
        }
        if (seen.insert(id).second) out.push_back(id);
    }
    return out;
}

// 選んだ画像のタグを書き換える。検索の結果を出していれば検索し直す（同じ位置の画像を選び直す）
// edit(db, 画像 ID, history) は、タグを変える前に history.touch(画像 ID, タグ) を呼ぶ（取り消せるように）
template <typename F>
void editSelectedTags(const std::vector<int64_t>& ids, const std::string& description, F&& edit) {
    Db* db = uiDb();
    if (!db || ids.empty()) return;
    try {
        db->begin();
        TagHistory history(*db);
        for (int64_t id : ids) edit(*db, id, history);
        history.save(description);
        db->commit();
    } catch (const std::exception& e) {
        try {
            db->rollback();
        } catch (const std::exception&) {
        }
        logBox(app.hwnd, widen(e.what()).c_str(), L"imgdupe", MB_ICONWARNING);
        return;
    }
    refreshSources(true);
    if (ex.view && ex.view->ShowingList()) {
        app.reselect = std::max(0, selectedResult());
        scheduleSearch(10);
    } else if (ex.view) {
        ex.view->RefreshTags();
    }
}

void markSelected(bool correct) {
    std::vector<std::string> tags, exclude;
    parseTagQuery(narrow(editText(app.s.tags)), tags, exclude);
    if (tags.empty()) {
        logBox(app.hwnd, L"「タグ」の欄に、確かめるタグを入れてください（1 つ目のタグを確定します）。", L"imgdupe",
                    MB_ICONINFORMATION);
        return;
    }
    const std::vector<int64_t> items = selectedImageIds();
    if (items.empty()) return;
    const std::string tag = tags[0];
    const std::string desc = std::to_string(items.size()) + " 枚の " + tag + " を「" + (correct ? "正しい" : "違う") + "」に";
    editSelectedTags(items, desc, [&](Db& db, int64_t id, TagHistory& h) {
        h.touch(id, tag);
        setManualTag(db, id, tag, correct);
    });
}

// ---------------------------------------------------------------- 統計の画面

void switchTab(int t);

constexpr UINT WM_STATS_DONE = WM_APP + 53;  // lParam: StatsData*
constexpr UINT WM_STATS_PROGRESS = WM_APP + 55;  // wParam: 済んだ数、lParam: std::wstring*（今数えているもの）

struct StatsData {
    StatsSummary summary;
    std::vector<StatRow> rows[10];  // タグ（kStatKinds の並び）・月・形式
    std::wstring error;
    int64_t at = 0;      // 集計した時刻
    bool saved = false;  // 前に集計して残したもの
};

struct StatsWindow {
    HWND wnd = nullptr, summary = nullptr, view = nullptr, list = nullptr, note = nullptr, progress = nullptr, status = nullptr,
         recount = nullptr;
    std::unique_ptr<StatsData> data;
    bool done = false;
    std::wstring searchTag;  // 閉じた後に検索するタグ
    int64_t most = 1;        // 棒の長さの基準（一番多い数）
} statsWin;

enum { IDC_SS_VIEW = 400, IDC_SS_LIST, IDC_SS_RECOUNT };
const wchar_t* const kStatViews[] = {L"作者別", L"サークル別", L"アカウント別", L"キャラ別", L"作品別", L"イベント別", L"タイトル別", L"行動・アイテム別",
                                     L"月ごとの増え方", L"形式別"};
const std::vector<std::string> kStatKinds[] = {{"artist"}, {"circle"}, {"account"}, {"character"}, {"copyright"}, {"event"}, {"title"}, {"action", "item"}};
constexpr int kStatTagViews = static_cast<int>(std::size(kStatKinds));  // ここまではタグ。後ろに月・形式
// kStatKinds は stats.cpp の statTagKinds と同じ並び（集計して残すのは stats.cpp）
static_assert(std::size(kStatViews) == kStatTagViews + 2 && std::extent_v<decltype(StatsData::rows)> == std::size(kStatViews), "統計の表の数");

std::wstring statsSummaryText(const StatsSummary& s) {
    std::wstring t = L"画像 " + withCommas(s.images) + L" 枚（" + sizeText(s.bytes) + L"）　検索用のタグあり " + withCommas(s.tagged) +
                     L" 枚　出典あり " + withCommas(s.withSource) + L" 枚" +
                     (s.deadLinks ? L"（リンク切れ " + withCommas(s.deadLinks) + L" 枚）" : L"") +
                     (s.blurry || s.soft ? L"　ぼやけ " + withCommas(s.blurry) + L" 枚・細かさ不足 " + withCommas(s.soft) + L" 枚" : L"") + L"\r\n";
    t += L"完全一致 " + withCommas(s.exactGroups) + L" グループ（重なっている分 " + withCommas(s.exactExtra) + L" 枚・" +
         sizeText(s.exactExtraBytes) + L"）　見た目一致 " + withCommas(s.similarGroups) + L" グループ（" + withCommas(s.similarExtra) +
         L" 枚・" + sizeText(s.similarExtraBytes) + L"）\r\n解析: ";
    static const std::pair<const char*, const wchar_t*> names[] = {{"hash", L"ハッシュ"}, {"thumb", L"サムネイル"}, {"meta", L"メタデータ"},
                                                                   {"names", L"json・フォルダ名"}, {"tagger", L"タガー"}, {"clip", L"CLIP"},
                                                                   {"quality", L"品質"}};
    bool first = true;
    for (const auto& [k, label] : names)
        for (const auto& st : s.stages)
            if (st.name == k) {
                t += (first ? L"" : L"、") + std::wstring(label) + L" " + withCommas(st.done);
                if (st.failed) t += L"（失敗 " + withCommas(st.failed) + L"）";
                first = false;
            }
    if (first) t += L"まだありません";
    return t;
}

void fillStatsList() {
    StatsWindow& w = statsWin;
    ListView_DeleteAllItems(w.list);
    if (!w.data) return;
    const int view = std::max(0, ComboBox_GetCurSel(w.view));
    const auto& rows = w.data->rows[view];
    w.most = 1;
    for (const auto& r : rows) w.most = std::max(w.most, r.count);
    SendMessageW(w.list, WM_SETREDRAW, FALSE, 0);
    for (size_t i = 0; i < rows.size(); ++i) {
        std::wstring name = view < kStatTagViews ? tagLabel(rows[i].key) : widen(rows[i].key);
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = static_cast<int>(i);
        it.pszText = name.data();
        ListView_InsertItem(w.list, &it);
        std::wstring n = withCommas(rows[i].count), b = sizeText(rows[i].bytes);
        ListView_SetItemText(w.list, static_cast<int>(i), 1, n.data());
        ListView_SetItemText(w.list, static_cast<int>(i), 2, b.data());
    }
    SendMessageW(w.list, WM_SETREDRAW, TRUE, 0);
    std::wstring note;
    if (view < kStatTagViews)
        note = rows.empty() ? L"まだありません（タガー・json・外部照会などでタグが付くと出ます）"
                            : L"確度 0.5 以上（手動で「違う」と確定したものは除く）の上位 500 件。ダブルクリックでそのタグを検索します。";
    else if (view == kStatTagViews)
        note = L"ファイルの作成日時と更新日時の古い方の月で数えます。";
    else
        note = L"形式が分からない画像は拡張子で数えます。";
    setText(w.note, note);
}

// 統計: 前に集計した結果（バックグラウンドの解析が 6 時間ごとに残す）があればすぐ出す。recount なら今集計し直して残す
void loadStats(bool recount) {
    HWND wnd = statsWin.wnd;
    const std::wstring db = dbPath();
    if (statsWin.recount) EnableWindow(statsWin.recount, FALSE);
    std::thread([wnd, db, recount] {
        const ULONGLONG t0 = GetTickCount64();
        auto* p = new StatsData;
        auto fill = [p](const StatsSnapshot& snap) {
            p->summary = snap.summary;
            for (size_t i = 0; i < snap.views.size() && i < std::size(p->rows); ++i) p->rows[i] = snap.views[i];
            p->at = snap.at;
        };
        try {
            Db d;
            if (!d.open(narrow(db))) throw std::runtime_error(d.error());
            StatsSnapshot snap;
            if (!recount && loadStatsSnapshot(d, snap)) {
                fill(snap);
                p->saved = true;
            } else {
                // 進み具合（済んだ数と、今数えているもの）を窓に送る
                snap = computeStats(d, [wnd](int done, int total, const std::string& what) {
                    auto* t = new std::wstring(widen(what));
                    if (!PostMessageW(wnd, WM_STATS_PROGRESS, MAKEWPARAM(done, total), reinterpret_cast<LPARAM>(t))) delete t;
                });
                saveStatsSnapshot(d, snap);
                fill(snap);
                logs::info("統計を集計した（" + std::to_string((GetTickCount64() - t0) / 1000) + " 秒）");
            }
        } catch (const std::exception& e) {
            p->error = L"集計できませんでした: " + widen(e.what());
            logs::warn("統計を集計できなかった: " + narrow(p->error));
        }
        if (!PostMessageW(wnd, WM_STATS_DONE, 0, reinterpret_cast<LPARAM>(p))) delete p;
    }).detach();
}

void showStatsWindow() {
    StatsWindow& w = statsWin;
    if (w.wnd) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            switch (msg) {
                case WM_COMMAND:
                    if (LOWORD(wp) == IDCANCEL) statsWin.done = true;
                    if (LOWORD(wp) == IDC_SS_VIEW && HIWORD(wp) == CBN_SELCHANGE) fillStatsList();
                    if (LOWORD(wp) == IDC_SS_RECOUNT) {
                        ShowWindow(statsWin.progress, SW_SHOW);
                        SendMessageW(statsWin.progress, PBM_SETPOS, 0, 0);
                        setText(statsWin.status, L"集計しています 0%");
                        loadStats(true);
                    }
                    return 0;
                case WM_CLOSE: statsWin.done = true; return 0;
                case WM_STATS_PROGRESS: {
                    std::unique_ptr<std::wstring> what(reinterpret_cast<std::wstring*>(lp));
                    if (h != statsWin.wnd) return 0;
                    const int done = LOWORD(wp), total = std::max(1, static_cast<int>(HIWORD(wp)));
                    ShowWindow(statsWin.progress, SW_SHOW);
                    const int pct = done * 100 / total;
                    SendMessageW(statsWin.progress, PBM_SETPOS, pct, 0);
                    setText(statsWin.status, L"集計しています " + std::to_wstring(pct) + L"%（" + *what + L"を数えています　" +
                                                 std::to_wstring(done + 1) + L" / " + std::to_wstring(total) + L"）");
                    return 0;
                }
                case WM_STATS_DONE: {
                    std::unique_ptr<StatsData> p(reinterpret_cast<StatsData*>(lp));
                    if (h != statsWin.wnd) return 0;  // 閉じた後に届いた
                    SendMessageW(statsWin.progress, PBM_SETPOS, 100, 0);
                    ShowWindow(statsWin.progress, SW_HIDE);
                    EnableWindow(statsWin.recount, TRUE);
                    if (!p->error.empty()) {
                        setText(statsWin.status, L"");
                    } else {
                        const time_t at = static_cast<time_t>(p->at);
                        tm lt{};
                        localtime_s(&lt, &at);
                        wchar_t when[64];
                        swprintf(when, 64, L"%d/%d %02d:%02d", lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
                        setText(statsWin.status, std::wstring(when) + (p->saved ? L" 時点の集計（解析が 6 時間ごとに更新）"
                                                                                   : L" に集計しました"));
                    }
                    setText(statsWin.summary, p->error.empty() ? statsSummaryText(p->summary) : p->error);
                    statsWin.data = std::move(p);
                    fillStatsList();
                    return 0;
                }
                case WM_NOTIFY: {
                    auto* nm = reinterpret_cast<NMHDR*>(lp);
                    if (nm->idFrom == IDC_SS_LIST && nm->code == NM_CUSTOMDRAW) {
                        // 4 列目に棒を描く（一番多いものを列の幅いっぱいに）
                        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
                        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
                        if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM) && cd->iSubItem == 3 && statsWin.data) {
                            const int view = std::max(0, ComboBox_GetCurSel(statsWin.view));
                            const auto& rows = statsWin.data->rows[view];
                            const size_t i = cd->nmcd.dwItemSpec;
                            RECT r;
                            ListView_GetSubItemRect(statsWin.list, static_cast<int>(i), 3, LVIR_BOUNDS, &r);
                            FillRect(cd->nmcd.hdc, &r, GetSysColorBrush(COLOR_WINDOW));
                            if (i < rows.size() && rows[i].count > 0) {
                                const int width = r.right - r.left - px(8);
                                RECT b{r.left + px(4), r.top + px(3), 0, r.bottom - px(3)};
                                b.right = b.left + std::max(1, static_cast<int>(width * rows[i].count / statsWin.most));
                                FillRect(cd->nmcd.hdc, &b, GetSysColorBrush(COLOR_HIGHLIGHT));
                            }
                            return CDRF_SKIPDEFAULT;
                        }
                        return CDRF_DODEFAULT;
                    }
                    if (nm->idFrom == IDC_SS_LIST && nm->code == NM_DBLCLK && statsWin.data) {
                        const int view = ComboBox_GetCurSel(statsWin.view);
                        const int i = ListView_GetNextItem(statsWin.list, -1, LVNI_SELECTED);
                        if (view >= 0 && view < kStatTagViews && i >= 0 && i < static_cast<int>(statsWin.data->rows[view].size())) {
                            const std::string& tag = statsWin.data->rows[view][static_cast<size_t>(i)].key;
                            if (tag.find(' ') == std::string::npos) {
                                statsWin.searchTag = widen(tag);
                                statsWin.done = true;
                            }
                        }
                    }
                    return 0;
                }
            }
            return DefWindowProcW(h, msg, wp, lp);
        };
        wc.hInstance = app.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ImgdupeStats";
        RegisterClassExW(&wc);
        registered = true;
    }
    const int cw = px(760), ch = px(560), m = px(10), bw = px(100), bh = px(28), rowH = px(24);
    RECT owner;
    GetWindowRect(app.hwnd, &owner);
    RECT frame{0, 0, cw, ch};
    AdjustWindowRectEx(&frame, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
    w = StatsWindow{};
    w.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ImgdupeStats", L"統計", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            (owner.left + owner.right - fw) / 2, (owner.top + owner.bottom - fh) / 2, fw, fh, app.hwnd, nullptr,
                            app.inst, nullptr);
    if (!w.wnd) return;
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int cx, int cy, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy, w.wnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
        return c;
    };
    const int sumH = px(56), noteH = px(22);
    w.summary = child(WC_STATICW, L"集計しています…", SS_NOPREFIX, -1, m, m, cw - 2 * m, sumH);
    int y = m + sumH + px(6);
    child(WC_STATICW, L"表示", SS_NOPREFIX, -1, m, y + px(4), px(40), rowH);
    w.view = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_SS_VIEW, m + px(44), y, px(200), px(240));
    for (const wchar_t* v : kStatViews) ComboBox_AddString(w.view, v);
    ComboBox_SetCurSel(w.view, 0);
    y += rowH + px(8);
    const int listH = ch - y - m - bh - px(8) - noteH;
    w.list = child(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, IDC_SS_LIST, m, y, cw - 2 * m, listH,
                   WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(w.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const struct {
        const wchar_t* title;
        int width;
        int fmt;
    } cols[] = {{L"名前", 240, LVCFMT_LEFT}, {L"枚数", 80, LVCFMT_RIGHT}, {L"大きさ", 80, LVCFMT_RIGHT}, {L"", 300, LVCFMT_LEFT}};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = cols[i].fmt;
        c.cx = px(cols[i].width);
        c.pszText = const_cast<wchar_t*>(cols[i].title);
        ListView_InsertColumn(w.list, i, &c);
    }
    w.note = child(WC_STATICW, L"", SS_NOPREFIX, -1, m, y + listH + px(4), cw - 2 * m, noteH);
    child(WC_BUTTONW, L"閉じる", WS_TABSTOP, IDCANCEL, cw - m - bw, ch - m - bh, bw, bh);
    w.recount = child(WC_BUTTONW, L"集計し直す", WS_TABSTOP, IDC_SS_RECOUNT, cw - m - bw * 2 - px(8), ch - m - bh, bw, bh);
    // 集計の進み具合（済んだら棒は隠す）
    w.progress = child(PROGRESS_CLASSW, L"", PBS_SMOOTH, -1, m, ch - m - bh + px(6), px(200), bh - px(12));
    SendMessageW(w.progress, PBM_SETRANGE32, 0, 100);
    w.status = child(WC_STATICW, L"集計しています 0%", SS_NOPREFIX | SS_ENDELLIPSIS | SS_CENTERIMAGE, -1, m + px(210), ch - m - bh,
                     cw - 2 * m - bw * 2 - px(228), bh);
    loadStats(false);

    EnableWindow(app.hwnd, FALSE);
    ShowWindow(w.wnd, SW_SHOW);
    SetFocus(w.view);
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
    w.data.reset();
    SetActiveWindow(app.hwnd);
    if (!w.searchTag.empty()) {
        // そのタグだけで検索する
        if (app.tab != 1) switchTab(1);
        clearSearchConditions();
        SetWindowTextW(app.s.tags, w.searchTag.c_str());
        scheduleSearch(10);
    }
}

// 「出典を調べ直す」: 外部照会の結果を消し、次のバックグラウンドの解析でもう一度調べる（選んだ画像。右クリックした画像が選ばれていなければその 1 枚）
void relookupImages(int64_t clicked) {
    std::vector<int64_t> ids = selectedImageIds();
    if (std::find(ids.begin(), ids.end(), clicked) == ids.end()) ids.assign(1, clicked);
    const lookup::Settings ls = lookup::load(app.dataDir);
    if (!ls.idLookup && !(ls.reverse && !ls.sauceKey.empty())) {
        logBox(app.hwnd, L"外部照会が切ってあります。「設定」の「外部照会」で、調べ方を選んでください。", L"imgdupe",
                    MB_ICONINFORMATION);
        return;
    }
    Db* db = uiDb();
    if (!db) return;
    try {
        resetLookups(*db, ids);
        addCommand(*db, "rescan");
    } catch (const std::exception& e) {
        logBox(app.hwnd, (L"できませんでした: " + widen(e.what())).c_str(), L"imgdupe", MB_ICONWARNING);
        return;
    }
    if (!indexer::isRunning()) indexer::start();
    logBox(app.hwnd,
                (withCommas(ids.size()) + L" 枚を、バックグラウンドの解析でもう一度調べます（1 秒に 1 件ずつ。SauceNAO は 1 日の上限まで）。")
                    .c_str(),
                L"imgdupe", MB_ICONINFORMATION);
}

void bulkTagUi() {
    size_t missing = 0;
    const std::vector<int64_t> items = selectedImageIds(&missing);
    if (items.empty()) {
        logBox(app.hwnd,
                    missing ? L"選んだ画像はまだインデックスにありません（インデックスを作った画像にだけタグを付けられます）。"
                            : L"タグを付ける画像を選んでください（Ctrl や Shift を押しながらクリック、空いている所からドラッグで囲むと、いくつも選べます）。",
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
        logBox(app.hwnd, widen(e.what()).c_str(), L"imgdupe", MB_ICONWARNING);
        return;
    }
    if (!done) {
        logBox(app.hwnd, L"元に戻せるタグの操作はありません。", L"imgdupe", MB_ICONINFORMATION);
        return;
    }
    app.searchNotice = L"元に戻しました: " + widen(desc);
    app.reselect = std::max(0, selectedResult());
    refreshSources(true);
    if (ex.view && !ex.view->ShowingList()) ex.view->RefreshTags();
    scheduleSearch(10);
}

// ---------------------------------------------------------------- 設定の画面（imgdupe.ini と同じ内容を画面から変える）

struct SettingsWindow {
    HWND wnd = nullptr, autoUpdate = nullptr, autostart = nullptr, quality = nullptr, tagger = nullptr, clip = nullptr, gpu = nullptr, aiStatus = nullptr,
         variantSim = nullptr, tagJapanese = nullptr, load = nullptr, idLookup = nullptr, linkCheck = nullptr, sauce = nullptr, sauceKey = nullptr, sauceLimit = nullptr, sauceStatus = nullptr;
    bool done = false;
    int after = 0;  // 閉じた後に実行する機能（CMD_*。0 = なし）
} settingsWin;

enum { IDC_ST_AUTOUPDATE = 300, IDC_ST_AUTOSTART, IDC_ST_QUALITY, IDC_ST_TAGGER, IDC_ST_CLIP, IDC_ST_VARIANT, IDC_ST_GPU, IDC_ST_MODELS, IDC_ST_LINKS, IDC_ST_KEYS, IDC_ST_INI, IDC_ST_LOOKUP, IDC_ST_LINKCHECK, IDC_ST_SAUCE, IDC_ST_SAUCEKEY, IDC_ST_SAUCELIMIT, IDC_ST_TAGJA, IDC_ST_LOAD };

// 画面の内容を imgdupe.ini などに書く
void applySettings() {
    SettingsWindow& w = settingsWin;
    iniSet(L"autoUpdateCheck", Button_GetCheck(w.autoUpdate) == BST_CHECKED ? L"1" : L"0");
    if ((Button_GetCheck(w.tagJapanese) == BST_CHECKED) != app.tagJapanese) setTagJapanese(!app.tagJapanese);
    const bool autostart = Button_GetCheck(w.autostart) == BST_CHECKED;
    if (autostart != indexer::autoStartEnabled()) {
        indexer::setAutoStart(autostart);
        if (autostart && !indexer::isRunning()) {
            saveSettings();
            indexer::start();
        }
    }
    iniSet(L"qualityEnabled", Button_GetCheck(w.quality) == BST_CHECKED ? L"1" : L"0");
    const int load = std::max(0, ComboBox_GetCurSel(w.load));
    if (load != indexer::loadLevel(app.dataDir)) {
        indexer::setLoadLevel(app.dataDir, load);
        restartIndexerAfterUpdate();  // 動いていれば、新しい負荷でやり直させる（スレッドの数は回のはじめに決まる）
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
    // 外部照会（バックグラウンドの解析が毎回 ini を読むので、作り直さなくてよい）
    lookup::Settings ls;
    ls.idLookup = Button_GetCheck(w.idLookup) == BST_CHECKED;
    ls.reverse = Button_GetCheck(w.sauce) == BST_CHECKED;
    ls.linkCheck = Button_GetCheck(w.linkCheck) == BST_CHECKED;
    ls.sauceKey = editText(w.sauceKey);
    while (!ls.sauceKey.empty() && iswspace(ls.sauceKey.back())) ls.sauceKey.pop_back();
    while (!ls.sauceKey.empty() && iswspace(ls.sauceKey.front())) ls.sauceKey.erase(0, 1);
    ls.dailyLimit = std::clamp(_wtoi(editText(w.sauceLimit).c_str()), 0, 100000);
    const lookup::Settings old = lookup::load(app.dataDir);
    lookup::save(app.dataDir, ls);
    const bool lookupOn = (ls.idLookup && !old.idLookup) || (ls.linkCheck && !old.linkCheck) || (ls.reverse && (!old.reverse || ls.sauceKey != old.sauceKey));
    if (changed) restartIndexerAfterUpdate();  // 動いているバックグラウンドの解析に新しい設定を使わせる
    else if (lookupOn)
        if (Db* db = uiDb()) addCommand(*db, "rescan");  // すぐ照会を始める
}

void settingsCommand(int id, int code) {
    SettingsWindow& w = settingsWin;
    switch (id) {
        case IDOK:
            applySettings();
            w.done = true;
            break;
        case IDCANCEL: w.done = true; break;
        case IDC_ST_SAUCE:
            if (code == BN_CLICKED) {
                EnableWindow(w.sauceKey, Button_GetCheck(w.sauce) == BST_CHECKED);
                EnableWindow(w.sauceLimit, Button_GetCheck(w.sauce) == BST_CHECKED);
            }
            break;
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
    const int cw = px(560), ch = px(744), m = px(12);
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
    gy = group(L"バックグラウンドの解析", px(126));
    w.autostart = child(WC_BUTTONW, L"Windows と一緒に起動する", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_ST_AUTOSTART, in, gy, iw, rowH);
    w.quality = child(WC_BUTTONW, L"画像の品質（ぼやけ・拡大された画像）を調べる（1 枚ずつ読み直すので時間がかかります）",
                      WS_TABSTOP | BS_AUTOCHECKBOX, IDC_ST_QUALITY, in, gy + rowH + px(2), iw, rowH);
    {
        const int ly = gy + 2 * (rowH + px(2)) + px(2);
        child(WC_STATICW, L"処理の負荷", SS_NOPREFIX, -1, in, ly + px(3), px(80), rowH);
        w.load = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_ST_LOAD, in + px(84), ly, iw - px(84), px(200));
        for (int i = indexer::kLoadLight; i <= indexer::kLoadHeavy; ++i) ComboBox_AddString(w.load, indexer::loadLevelName(i));
        child(WC_STATICW,
              L"上げるほど速く進みますが、そのぶんほかのアプリが重くなります。",
              SS_NOPREFIX, -1, in, ly + rowH + px(6), iw, rowH);
    }
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
    gy = group(L"外部照会（インターネットで出典を調べる）", px(160));
    w.idLookup = child(WC_BUTTONW, L"ファイル名の pixiv・Danbooru（MD5）の ID で、投稿のタグと作者を調べる",
                       WS_TABSTOP | BS_AUTOCHECKBOX, IDC_ST_LOOKUP, in, gy, iw, rowH);
    w.linkCheck = child(WC_BUTTONW, L"出典の投稿が消えていないか確かめる（pixiv・Danbooru・X。30 日ごと）", WS_TABSTOP | BS_AUTOCHECKBOX,
                        IDC_ST_LINKCHECK, in, gy + rowH + px(2), iw, rowH);
    w.sauce = child(WC_BUTTONW, L"出典の分からない画像を SauceNAO で探す（縮小したサムネイルを送ります）", WS_TABSTOP | BS_AUTOCHECKBOX,
                    IDC_ST_SAUCE, in, gy + 2 * (rowH + px(2)), iw, rowH);
    {
        const int ry = gy + 3 * (rowH + px(2)) + px(2);
        child(WC_STATICW, L"API キー", SS_NOPREFIX, -1, in + px(18), ry + px(3), px(64), rowH);
        w.sauceKey = child(WC_EDITW, L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD, IDC_ST_SAUCEKEY, in + px(84), ry,
                           iw - px(84) - px(140), rowH);
        child(WC_STATICW, L"1 日に", SS_NOPREFIX, -1, in + iw - px(128), ry + px(3), px(48), rowH);
        w.sauceLimit = child(WC_EDITW, L"", WS_TABSTOP | WS_BORDER | ES_NUMBER, IDC_ST_SAUCELIMIT, in + iw - px(76), ry, px(48), rowH);
        child(WC_STATICW, L"回", SS_NOPREFIX, -1, in + iw - px(24), ry + px(3), px(24), rowH);
        w.sauceStatus = child(WC_STATICW, L"", SS_NOPREFIX, -1, in + px(18), ry + rowH + px(6), iw - px(18), px(34));
    }
    gy = group(L"表示", px(52));
    w.tagJapanese = child(WC_BUTTONW, L"タグを日本語で表示する（タグそのものは英語のまま。日本語でも英語でも検索できます）",
                          WS_TABSTOP | BS_AUTOCHECKBOX, IDC_ST_TAGJA, in, gy, iw, rowH);
    gy = group(L"そのほか", px(60));
    const int b3 = (iw - 2 * px(8)) / 3;
    child(WC_BUTTONW, L"ショートカットキー…", WS_TABSTOP, IDC_ST_KEYS, in, gy, b3, bh);
    child(WC_BUTTONW, L"展開したフォルダ…", WS_TABSTOP, IDC_ST_LINKS, in + b3 + px(8), gy, b3, bh);
    child(WC_BUTTONW, L"設定ファイルを開く", WS_TABSTOP, IDC_ST_INI, in + 2 * (b3 + px(8)), gy, b3, bh);
    child(WC_BUTTONW, L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, cw - m - px(208), ch - m - bh, px(100), bh);
    child(WC_BUTTONW, L"キャンセル", WS_TABSTOP, IDCANCEL, cw - m - px(100), ch - m - bh, px(100), bh);

    // 今の設定を出す
    Button_SetCheck(w.autoUpdate, iniInt(L"autoUpdateCheck", 1) ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(w.tagJapanese, app.tagJapanese ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(w.autostart, indexer::autoStartEnabled() ? BST_CHECKED : BST_UNCHECKED);
    ComboBox_SetCurSel(w.load, indexer::loadLevel(app.dataDir));
    Button_SetCheck(w.quality, clip::qualityEnabled(app.dataDir) ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(w.tagger, iniInt(L"taggerEnabled", 1) ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(w.clip, iniInt(L"clipEnabled", 1) ? BST_CHECKED : BST_UNCHECKED);
    setText(w.variantSim, [&] {
        wchar_t v[16];
        swprintf(v, 16, L"%.2f", clip::variantSimilarity(app.dataDir));
        return std::wstring(v);
    }());
    Button_SetCheck(w.gpu, iniInt(L"taggerCpu", 0) ? BST_UNCHECKED : BST_CHECKED);
    {
        const lookup::Settings ls = lookup::load(app.dataDir);
        Button_SetCheck(w.idLookup, ls.idLookup ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(w.sauce, ls.reverse ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(w.linkCheck, ls.linkCheck ? BST_CHECKED : BST_UNCHECKED);
        setText(w.sauceKey, ls.sauceKey);
        setText(w.sauceLimit, std::to_wstring(ls.dailyLimit));
        EnableWindow(w.sauceKey, ls.reverse);
        EnableWindow(w.sauceLimit, ls.reverse);
        int today = 0;
        if (Db* db = uiDb()) today = imgdupe::sauceSentToday(*db, static_cast<int64_t>(std::time(nullptr)));
        setText(w.sauceStatus, L"API キーは saucenao.com で無料で登録するともらえます（無料は 1 日 100 回まで）。\r\n今日は " +
                                   std::to_wstring(today) + L" 回送りました");
    }
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

// ---------------------------------------------------------------- 行動・アイテムの辞書の画面

struct DictionaryWindow {
    HWND wnd = nullptr, edit = nullptr, status = nullptr, save = nullptr;
    bool done = false, applying = false;
} dictWin;

constexpr UINT WM_DICT_APPLIED = WM_APP + 50;  // lParam: std::wstring*（結果の文）

std::wstring readTextFile(const std::wstring& path) {
    std::string text;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        fclose(f);
    }
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    // 編集欄は CRLF
    std::wstring w = widen(text), out;
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r')) out += L'\r';
        out += w[i];
    }
    return out;
}

void dictionarySave() {
    DictionaryWindow& w = dictWin;
    if (w.applying) return;
    std::wstring text = editText(w.edit);
    std::string utf8 = narrow(text);
    utf8.erase(std::remove(utf8.begin(), utf8.end(), '\r'), utf8.end());
    std::vector<DictEntry> entries;
    std::string error;
    if (!parseDictionary(utf8, entries, error)) {
        setText(w.status, L"保存できません: " + widen(error));
        return;
    }
    const std::wstring path = clip::dictionaryPath(app.dataDir);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f || fwrite(utf8.data(), 1, utf8.size(), f) != utf8.size()) {
        if (f) fclose(f);
        setText(w.status, L"保存できませんでした: " + path);
        return;
    }
    fclose(f);
    w.applying = true;
    EnableWindow(w.save, FALSE);
    setText(w.status, L"保存しました。すべての画像に当てています…");
    HWND wnd = w.wnd;
    const std::wstring dataDir = app.dataDir, db = dbPath();
    std::thread([wnd, dataDir, db] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::wstring msg;
        try {
            Db d;
            if (!d.open(narrow(db))) throw std::runtime_error(d.error());
            size_t n = 0;
            const std::wstring err = clip::applyDictionary(dataDir, d, true, &n);
            msg = err.empty() ? L"当てました（" + std::to_wstring(n) + L" 項目）。検索の「タグ」で項目の名前を入れると探せます" : err;
        } catch (const std::exception& e) {
            msg = L"当てられませんでした: " + widen(e.what());
        }
        CoUninitialize();
        auto* p = new std::wstring(msg);
        if (!PostMessageW(wnd, WM_DICT_APPLIED, 0, reinterpret_cast<LPARAM>(p))) delete p;
    }).detach();
}

void showDictionaryWindow() {
    DictionaryWindow& w = dictWin;
    if (w.wnd) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            switch (msg) {
                case WM_COMMAND:
                    if (LOWORD(wp) == IDOK) dictionarySave();
                    else if (LOWORD(wp) == IDCANCEL && !dictWin.applying) dictWin.done = true;
                    return 0;
                case WM_CLOSE:
                    if (!dictWin.applying) dictWin.done = true;
                    return 0;
                case WM_DICT_APPLIED: {
                    std::unique_ptr<std::wstring> m(reinterpret_cast<std::wstring*>(lp));
                    dictWin.applying = false;
                    EnableWindow(dictWin.save, TRUE);
                    setText(dictWin.status, *m);
                    refreshSources(true);
                    scheduleSearch(10);
                    return 0;
                }
            }
            return DefWindowProcW(h, msg, wp, lp);
        };
        wc.hInstance = app.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ImgdupeDictionary";
        RegisterClassExW(&wc);
        registered = true;
    }
    const int cw = px(640), ch = px(480), m = px(10), bh = px(28);
    RECT owner;
    GetWindowRect(app.hwnd, &owner);
    RECT frame{0, 0, cw, ch};
    AdjustWindowRectEx(&frame, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
    w = DictionaryWindow{};
    w.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ImgdupeDictionary", L"行動・アイテムの辞書", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            (owner.left + owner.right - fw) / 2, (owner.top + owner.bottom - fh) / 2, fw, fh, app.hwnd, nullptr,
                            app.inst, nullptr);
    if (!w.wnd) return;
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int cx, int cy, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy, w.wnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
        return c;
    };
    child(WC_STATICW,
          L"WD14 のタグ（wd14:）と CLIP の文章（clip:）を、自分の分類にまとめます。書き方は先頭のコメントを見てください。"
          L"保存すると、すべての画像に当て直します（数秒〜数十秒）。",
          SS_NOPREFIX, -1, m, m, cw - 2 * m, px(36));
    w.edit = child(WC_EDITW, L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                   -1, m, m + px(40), cw - 2 * m, ch - 3 * m - px(40) - bh - px(24), WS_EX_CLIENTEDGE);
    SendMessageW(w.edit, EM_SETLIMITTEXT, 0, 0);
    w.status = child(WC_STATICW, L"", SS_NOPREFIX, -1, m, ch - m - bh - px(24), cw - 2 * m, px(20));
    w.save = child(WC_BUTTONW, L"保存して当てる", WS_TABSTOP, IDOK, cw - m - px(260), ch - m - bh, px(150), bh);
    child(WC_BUTTONW, L"閉じる", WS_TABSTOP, IDCANCEL, cw - m - px(100), ch - m - bh, px(100), bh);
    const std::wstring path = clip::dictionaryPath(app.dataDir);
    std::wstring text = readTextFile(path);
    if (text.empty()) {
        std::string def = defaultDictionary();
        std::wstring wdef = widen(def), out;
        for (wchar_t c : wdef) {
            if (c == L'\n') out += L'\r';
            out += c;
        }
        text = out;
    }
    SetWindowTextW(w.edit, text.c_str());
    setText(w.status, L"場所: " + path);

    EnableWindow(app.hwnd, FALSE);
    ShowWindow(w.wnd, SW_SHOW);
    SetFocus(w.edit);
    MSG msg;
    while (!w.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && !w.applying) {
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

// ---------------------------------------------------------------- タグ辞書の画面（別名・親子関係、Danbooru からの取り込み）

struct TagDictWindow {
    HWND wnd = nullptr, edit = nullptr, status = nullptr, counts = nullptr, save = nullptr, import = nullptr;
    bool done = false, busy = false;
    std::atomic<bool> cancel{false};
} tagDictWin;

constexpr UINT WM_TAGDICT_STATUS = WM_APP + 51;  // lParam: std::wstring*（途中の様子）
constexpr UINT WM_TAGDICT_DONE = WM_APP + 52;    // lParam: std::wstring*（結果）

void tagDictRefreshCounts() {
    Db* db = uiDb();
    if (!db) return;
    try {
        const RelationCounts c = relationCounts(*db);
        setText(tagDictWin.counts, L"自分で書いた分: 別名 " + withCommas(c.userAlias) + L"・親子 " + withCommas(c.userParent) +
                                       L"　Danbooru から取り込んだ分: 別名 " + withCommas(c.danbooruAlias) + L"・親子 " +
                                       withCommas(c.danbooruParent));
    } catch (const std::exception&) {
    }
}

void tagDictBusy(bool on) {
    TagDictWindow& w = tagDictWin;
    w.busy = on;
    EnableWindow(w.save, !on);
    SetWindowTextW(w.import, on ? L"中止" : L"Danbooru から取り込む");
}

// 別のスレッドで DB を開いて fn を実行し、結果の文を画面に返す
void tagDictRun(std::function<std::wstring(Db&)> fn) {
    tagDictBusy(true);
    tagDictWin.cancel = false;
    HWND wnd = tagDictWin.wnd;
    const std::wstring db = dbPath();
    std::thread([wnd, db, fn] {
        std::wstring msg;
        try {
            Db d;
            if (!d.open(narrow(db))) throw std::runtime_error(d.error());
            msg = fn(d);
        } catch (const std::exception& e) {
            msg = L"できませんでした: " + widen(e.what());
        }
        auto* p = new std::wstring(msg);
        if (!PostMessageW(wnd, WM_TAGDICT_DONE, 0, reinterpret_cast<LPARAM>(p))) delete p;
    }).detach();
}

void tagDictSave() {
    TagDictWindow& w = tagDictWin;
    if (w.busy) return;
    std::string text = narrow(editText(w.edit));
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    std::vector<TagRelation> rels;
    std::string error;
    if (!parseTagRelations(text, rels, error)) {
        setText(w.status, L"保存できません: " + widen(error));
        return;
    }
    setText(w.status, L"保存して、親子関係を当てています…");
    tagDictRun([rels](Db& d) {
        replaceRelations(d, "user", rels);
        const size_t n = clip::applyTagParents(d, true);
        return L"保存しました（" + withCommas(rels.size()) + L" 件）。親子関係で付いたタグ: " + withCommas(n);
    });
}

void tagDictImport() {
    TagDictWindow& w = tagDictWin;
    if (w.busy) {
        w.cancel = true;
        setText(w.status, L"中止しています…");
        return;
    }
    if (logBox(w.wnd,
                    L"Danbooru（danbooru.donmai.us）から、使われている別名と親子関係の一覧を取り込みます。\r\n"
                    L"相手の負担にならないよう 1 秒に 1 回ずつ取るので、数分かかります。取り込んだ分は前の分と入れ替えます。",
                    L"タグ辞書", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
        return;
    HWND wnd = w.wnd;
    setText(w.status, L"Danbooru に問い合わせています…");
    tagDictRun([wnd](Db& d) -> std::wstring {
        std::vector<TagRelation> rels;
        for (const char* kind : {"alias", "parent"}) {
            const std::wstring endpoint = std::string(kind) == "alias" ? L"tag_aliases" : L"tag_implications";
            for (int page = 1; page <= 200; ++page) {
                if (tagDictWin.cancel) return L"中止しました（取り込んだ分は使っていません）";
                wchar_t base[256] = L"https://danbooru.donmai.us";  // 環境変数 IMGDUPE_DANBOORU_URL で変えられる（テスト用）
                GetEnvironmentVariableW(L"IMGDUPE_DANBOORU_URL", base, 256);
                const std::wstring url = std::wstring(base) + L"/" + endpoint +
                                         L".json?search%5Bstatus%5D=active&limit=1000&page=" + std::to_wstring(page) +
                                         L"&only=antecedent_name,consequent_name";
                upd::FetchResult r = upd::fetchText(url, 16 * 1024 * 1024);
                if (!r.ok) return L"Danbooru から取れませんでした: " + r.error;
                std::vector<std::pair<std::string, std::string>> pairs;
                if (!parseDanbooruPairs(r.body, pairs)) return L"Danbooru の応答を読めませんでした";
                if (pairs.empty()) break;  // 最後のページ
                for (auto& [a, c] : pairs) rels.push_back({a, c, kind});
                auto* p = new std::wstring(std::wstring(std::string(kind) == "alias" ? L"別名" : L"親子関係") + L"を取り込んでいます… " +
                                           withCommas(rels.size()) + L" 件");
                if (!PostMessageW(wnd, WM_TAGDICT_STATUS, 0, reinterpret_cast<LPARAM>(p))) delete p;
                Sleep(1000);  // 1 秒に 1 回
            }
        }
        replaceRelations(d, "danbooru", rels);
        const size_t n = clip::applyTagParents(d, true);
        return L"取り込みました（" + withCommas(rels.size()) + L" 件）。親子関係で付いたタグ: " + withCommas(n);
    });
}

void showTagDictWindow() {
    TagDictWindow& w = tagDictWin;
    if (w.wnd) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            switch (msg) {
                case WM_COMMAND:
                    if (LOWORD(wp) == IDOK) tagDictSave();
                    else if (LOWORD(wp) == IDRETRY) tagDictImport();
                    else if (LOWORD(wp) == IDCANCEL && !tagDictWin.busy) tagDictWin.done = true;
                    return 0;
                case WM_CLOSE:
                    if (!tagDictWin.busy) tagDictWin.done = true;
                    return 0;
                case WM_TAGDICT_STATUS: {
                    std::unique_ptr<std::wstring> m(reinterpret_cast<std::wstring*>(lp));
                    if (!tagDictWin.cancel) setText(tagDictWin.status, *m);
                    return 0;
                }
                case WM_TAGDICT_DONE: {
                    std::unique_ptr<std::wstring> m(reinterpret_cast<std::wstring*>(lp));
                    tagDictBusy(false);
                    setText(tagDictWin.status, *m);
                    tagDictRefreshCounts();
                    refreshSources(true);
                    scheduleSearch(10);
                    return 0;
                }
            }
            return DefWindowProcW(h, msg, wp, lp);
        };
        wc.hInstance = app.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ImgdupeTagDict";
        RegisterClassExW(&wc);
        registered = true;
    }
    const int cw = px(640), ch = px(480), m = px(10), bh = px(28);
    RECT owner;
    GetWindowRect(app.hwnd, &owner);
    RECT frame{0, 0, cw, ch};
    AdjustWindowRectEx(&frame, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
    w.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ImgdupeTagDict", L"タグ辞書（別名・親子関係）", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            (owner.left + owner.right - fw) / 2, (owner.top + owner.bottom - fh) / 2, fw, fh, app.hwnd, nullptr,
                            app.inst, nullptr);
    w.done = w.busy = false;
    if (!w.wnd) return;
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int cx, int cy, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy, w.wnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
        return c;
    };
    child(WC_STATICW,
          L"別名は検索でまとめて扱い、親子関係は子のタグが付いた画像に親のタグも付けます（出どころ「親子関係」）。"
          L"ここに書くのは自分の分です。Danbooru の一覧は下のボタンで取り込めます。",
          SS_NOPREFIX, -1, m, m, cw - 2 * m, px(36));
    w.edit = child(WC_EDITW, L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                   -1, m, m + px(40), cw - 2 * m, ch - 3 * m - px(40) - bh - px(48), WS_EX_CLIENTEDGE);
    SendMessageW(w.edit, EM_SETLIMITTEXT, 0, 0);
    w.counts = child(WC_STATICW, L"", SS_NOPREFIX, -1, m, ch - m - bh - px(46), cw - 2 * m, px(20));
    w.status = child(WC_STATICW, L"", SS_NOPREFIX, -1, m, ch - m - bh - px(24), cw - 2 * m, px(20));
    w.import = child(WC_BUTTONW, L"Danbooru から取り込む", WS_TABSTOP, IDRETRY, m, ch - m - bh, px(190), bh);
    w.save = child(WC_BUTTONW, L"保存して当てる", WS_TABSTOP, IDOK, cw - m - px(260), ch - m - bh, px(150), bh);
    child(WC_BUTTONW, L"閉じる", WS_TABSTOP, IDCANCEL, cw - m - px(100), ch - m - bh, px(100), bh);
    std::vector<TagRelation> rels;
    if (Db* db = uiDb()) {
        try {
            rels = loadRelations(*db, "user");
        } catch (const std::exception&) {
        }
    }
    std::wstring text;
    for (wchar_t c : widen(formatTagRelations(rels))) {
        if (c == L'\n') text += L'\r';
        text += c;
    }
    SetWindowTextW(w.edit, text.c_str());
    tagDictRefreshCounts();

    EnableWindow(app.hwnd, FALSE);
    ShowWindow(w.wnd, SW_SHOW);
    SetFocus(w.edit);
    MSG msg;
    while (!w.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && !w.busy) {
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

void layout();


// ---------------------------------------------------------------- エクスプローラーの画面（mgview と同じ形。explorer_win.*）

constexpr UINT WM_SAME_DONE = WM_APP + 54;  // lParam: SameDone*

struct SameDone {
    unsigned gen = 0;
    std::wstring title, error;
    std::wstring original;  // 探した元の画像（一覧の先頭に出す。一時ファイルなら空）
    std::vector<ExplorerMatch> matches;
};


std::wstring fileNameOf(const std::wstring& path) {
    const size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? path : path.substr(sep + 1);
}

// path の画像と同じ画像を DB から探す（別のスレッドで）。self: path そのものを結果から除く（DB の中の画像の時）
void findSameAsync(const std::wstring& path, const std::wstring& title, bool self, bool deleteAfter) {
    const unsigned gen = ++ex.gen;
    KillTimer(app.hwnd, kSameTimer);
    ex.view->SetSameStatus(title + L"　同じ画像を探しています…");
    const std::wstring db = dbPath();
    const int threshold = iniInt(L"threshold", 10);
    std::thread([=] {
        imgwin::threadBegin();
        auto* r = new SameDone;
        r->gen = gen;
        r->title = title;
        if (!deleteAfter) r->original = path;
        try {
            SameQuery q;
            q.f = imgwin::decodeFeatures(path);
            q.threshold = threshold;
            if (self) q.selfPath = narrow(path);
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
                q.size = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
            Db d;
            if (!d.open(narrow(db))) throw std::runtime_error(d.error());
            // 完全一致は、同じ大きさの画像がある時だけ SHA-256 を求める
            if (q.size) {
                Stmt st(d.handle(), "SELECT 1 FROM images WHERE size=? LIMIT 1");
                st.bind(1, static_cast<int64_t>(q.size));
                if (st.step()) q.hasSha = sha256File(narrow(path), q.sha);
            }
            if (q.f.ok && q.f.contrast < q.flatContrast) r->title += L"（単色に近いので、完全一致だけを探します）";
            if (!q.f.ok && !q.hasSha) {
                r->error = L"画像を読めませんでした";
            } else {
                ex.index.refresh(d, kHashVersion);
                for (const auto& m : ex.index.find(d, q, [](const std::string& p, Sha256& out) { return sha256File(p, out); })) {
                    ExplorerMatch e;
                    e.path = widen(m.path);
                    e.exact = m.exact;
                    e.distance = m.distance;
                    e.size = m.size;
                    e.width = static_cast<int>(m.width);
                    e.height = static_cast<int>(m.height);
                    r->matches.push_back(std::move(e));
                }
            }
        } catch (const std::exception& e) {
            r->error = L"探せませんでした: " + widen(e.what());
        }
        imgwin::threadEnd();
        if (deleteAfter) DeleteFileW(path.c_str());
        if (!PostMessageW(app.hwnd, WM_SAME_DONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void onSameDone(SameDone* p) {
    std::unique_ptr<SameDone> r(p);
    if (r->gen != ex.gen || !ex.view) return;  // もう別の画像を探している
    if (!r->error.empty()) ex.view->SetSameStatus(r->title + L"　" + r->error);
    else ex.view->SetSame(r->title, std::move(r->matches), r->original);
}

void ExplorerApp::ExplorerOpen(const std::wstring& path) { openPath(path); }

// 選んだだけでは同じ画像を探さない（右クリック・ドロップ・URL の時だけ）。ツールバーの「開く」などを使えるようにする
void ExplorerApp::ExplorerSelected(const std::wstring& path, bool image) {
    for (int id : {CMD_OPEN, CMD_EXPLORER, CMD_COPY_PATH}) tbEnable(id, !path.empty());
    tbEnable(CMD_FIND_SAME, image);
}

// 画像のタグ（ファイルのタグと、確度を合成した検索用のタグ。確度の高い順）
std::vector<ExplorerTag> ExplorerApp::ExplorerTags(const std::wstring& path) {
    std::vector<ExplorerTag> out;
    Db* db = uiDb();
    if (!db) return out;
    try {
        std::vector<std::string> roots;
        for (const auto& f : folderList()) roots.push_back(narrow(f));
        const int64_t id = findImageId(*db, narrow(path), roots);
        if (!id) return out;
        struct Agg {
            std::string kind;
            std::vector<double> ps;
            int manual = -1;
            bool file = false, dbOnly = false;
        };
        std::vector<std::pair<std::string, Agg>> agg;
        for (const auto& t : tagsOf(*db, id)) {
            if (t.kind == "rating") continue;  // general / sensitive などの年齢区分は出さない
            auto it = std::find_if(agg.begin(), agg.end(), [&](const auto& a) { return lstrcmpiA(a.first.c_str(), t.tag.c_str()) == 0; });
            if (it == agg.end()) it = agg.insert(agg.end(), {t.tag, Agg{t.kind}});
            if (t.source == kSourceEmbedded) it->second.file = true;
            else it->second.dbOnly = true;
            if (t.manual) it->second.manual = t.confidence > 0.5 ? 1 : 0;
            else it->second.ps.push_back(t.confidence);
        }
        std::vector<std::pair<double, ExplorerTag>> rows;
        for (const auto& [tag, a] : agg) {
            const double p = a.manual >= 0 ? a.manual : combineConfidence(a.ps);
            // 手動で外した（「違う」と確定した）タグは、ファイルに書き込まれていても出さない（ファイルは書き換えない）
            if (a.file && a.manual != 0) rows.push_back({2.0, ExplorerTag{widen(tag), widen(a.kind), tagLabel(tag), 1.0, true}});
            if (a.dbOnly && p >= 0.35) rows.push_back({p, ExplorerTag{widen(tag), widen(a.kind), tagLabel(tag), p, false}});
        }
        std::stable_sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        for (auto& r : rows) out.push_back(std::move(r.second));
    } catch (const std::exception&) {
    }
    return out;
}

// タグで検索する（タグをクリック・取り置いたタグを組み合わせて）: 検索のリボンに出して、すぐ探す
void ExplorerApp::ExplorerSearchTags(const std::vector<std::wstring>& tags) {
    // 検索の欄には、画面に出している名前で入れる（その名前でそのタグだけに決まる時。ほかはタグそのもの）
    Db* db = uiDb();
    std::wstring text;
    for (const auto& t : tags) {
        std::wstring word = t;
        const std::wstring label = tagLabel(narrow(t));
        if (db && label != t && label.find_first_of(L" \t\u3000") == std::wstring::npos) {
            try {
                const auto named = tagsNamed(*db, narrow(label));
                if (named.size() == 1 && tagNameKey(named[0]) == tagNameKey(narrow(t))) word = label;
            } catch (const std::exception&) {
            }
        }
        text += (text.empty() ? L"" : L" ") + word;
    }
    app.reselectPath = ex.view ? ex.view->Selected() : L"";  // 結果ではこの画像を選ぶ（あれば）
    if (app.tab != 1) switchTab(1);
    SetWindowTextW(app.s.free, text.c_str());
    ComboBox_SetCurSel(app.s.scope, 1);  // タグに限る（名前どおり）
    runSearch();
}

void ExplorerApp::ExplorerFindSimilarAi(const std::wstring& path) {
    Db* db = uiDb();
    if (!db) return;
    std::vector<std::string> roots;
    for (const auto& f : folderList()) roots.push_back(narrow(f));
    const int64_t id = findImageId(*db, narrow(path), roots);
    if (!id) {
        logBox(app.hwnd, L"この画像はまだインデックスにありません。", L"imgdupe", MB_ICONINFORMATION);
        return;
    }
    if (app.tab != 1) switchTab(1);
    findSimilar(id, fileNameOf(path));
    runSearch();
}

std::wstring ExplorerApp::ExplorerTagLabel(const std::wstring& tag) { return tagLabel(narrow(tag)); }

// 取り置いたタグを画像に落とした: 検索用タグとして手動で「正しい」と確定する（確度 1.0。Ctrl+Z で戻せる。ファイルは書き換えない）
void ExplorerApp::ExplorerAddTag(const std::vector<std::wstring>& paths, const std::wstring& tag) {
    Db* db = uiDb();
    if (!db) return;
    std::vector<std::string> roots;
    for (const auto& f : folderList()) roots.push_back(narrow(f));
    std::vector<int64_t> ids;
    for (const auto& p : paths)
        if (const int64_t id = findImageId(*db, narrow(p), roots)) ids.push_back(id);
    if (ids.empty()) {
        logBox(app.hwnd, L"インデックスに無い画像にはタグを付けられません。", L"imgdupe", MB_ICONINFORMATION);
        return;
    }
    const std::string t = narrow(tag);
    const std::string what = ids.size() == 1 ? "タグを付ける: " + t : std::to_string(ids.size()) + " 枚に「" + t + "」";
    editSelectedTags(ids, what, [&](Db& d, int64_t imageId, TagHistory& history) {
        history.touch(imageId, t);
        setManualTag(d, imageId, t, true);
    });
}

// 取り置いたタグをエクスプローラーの外に落とした: 検索のフリーワードの欄なら、語として足す（探すのは Enter か「検索」で）
void ExplorerApp::ExplorerDropTag(const std::wstring& tag, POINT screen) {
    HWND under = WindowFromPoint(screen);
    if (under != app.s.free) return;
    std::wstring text = editText(app.s.free);
    while (!text.empty() && text.back() == L' ') text.pop_back();
    text += (text.empty() ? L"" : L" ") + tag;
    SetWindowTextW(app.s.free, text.c_str());
    // 「タイトルに限る」「ファイル名・フォルダ名に限る」ではタグに合わないので、「すべて」にする
    if (ComboBox_GetCurSel(app.s.scope) == 2 || ComboBox_GetCurSel(app.s.scope) == 4) ComboBox_SetCurSel(app.s.scope, 0);
    SetFocus(app.s.free);
    SendMessageW(app.s.free, EM_SETSEL, text.size(), text.size());
}

// この画像からタグを外す: 手動で「違う」と確定する（Ctrl+Z で戻せる。ファイルは書き換えない）
void ExplorerApp::ExplorerRemoveTag(const std::wstring& path, const std::wstring& tag) {
    Db* db = uiDb();
    if (!db) return;
    std::vector<std::string> roots;
    for (const auto& f : folderList()) roots.push_back(narrow(f));
    const int64_t id = findImageId(*db, narrow(path), roots);
    if (!id) return;
    const std::string t = narrow(tag);
    editSelectedTags({id}, "タグを外す: " + t, [&](Db& d, int64_t imageId, TagHistory& history) {
        history.touch(imageId, t);
        setManualTag(d, imageId, t, false);
    });
}

// タグの表示名を自分で付ける（空にすると同梱の訳に戻す）
void ExplorerApp::ExplorerRenameTag(const std::wstring& tag) {
    Db* db = uiDb();
    if (!db) return;
    const std::string key = tagNameKey(narrow(tag));
    auto it = app.tagNames.find(key);
    auto input = promptText(L"タグの表示名", L"「" + tag + L"」の表示名（空にすると元の訳に戻ります）",
                            it != app.tagNames.end() ? widen(it->second) : L"");
    if (!input) return;
    std::wstring name = *input;
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
    try {
        setUserTagName(*db, narrow(tag), narrow(name));
        app.tagNames = loadTagNames(*db);
    } catch (const std::exception& e) {
        logBox(app.hwnd, widen(e.what()).c_str(), L"imgdupe", MB_ICONWARNING);
        return;
    }
    refreshTagLabels();
}

// 表示名・表示の言語が変わった: 出しているタグを描き直す
void refreshTagLabels() {
    if (ex.view) ex.view->RefreshTags();
    if (statsWin.wnd) fillStatsList();
    syncMenuChecks();
}

void ExplorerApp::ExplorerSaveSelection() { saveSelectionSmartUi(); }

std::vector<std::pair<std::wstring, std::wstring>> ExplorerApp::ExplorerProperties(const std::wstring& path) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    Db* db = uiDb();
    if (!db) return out;
    try {
        std::vector<std::string> roots;
        for (const auto& f : folderList()) roots.push_back(narrow(f));
        const int64_t id = findImageId(*db, narrow(path), roots);
        if (!id) return out;
        for (auto& [k, v] : imageProperties(*db, id, [](int64_t t) { return narrow(localTimeText(t)); }))
            out.emplace_back(widen(k), widen(v));
    } catch (const std::exception&) {
    }
    return out;
}

void ExplorerApp::ExplorerTagSelection() { bulkTagUi(); }
void ExplorerApp::ExplorerTeachFolder(const std::wstring& dir) { teachFolderUi(dir); }

void ExplorerApp::ExplorerRelookup(const std::wstring& path) {
    Db* db = uiDb();
    if (!db) return;
    std::vector<std::string> roots;
    for (const auto& f : folderList()) roots.push_back(narrow(f));
    const int64_t id = findImageId(*db, narrow(path), roots);
    if (id) relookupImages(id);
}

void ExplorerApp::ExplorerFindSameFile(const std::wstring& path) {
    findSameAsync(path, L"「" + fileNameOf(path) + L"」", true, false);
}

void ExplorerApp::ExplorerFindSameUrl(const std::wstring& url) {
    const unsigned gen = ++ex.gen;
    KillTimer(app.hwnd, kSameTimer);
    const std::wstring title = L"URL の画像（" + url + L"）";
    view->SetSameStatus(title + L"　ダウンロードしています…");
    HWND hwnd = app.hwnd;
    std::thread([=] {
        std::vector<std::wstring> headers = {L"Accept: image/*,*/*;q=0.8"};
        if (url.find(L"pximg.net") != std::wstring::npos) headers.push_back(L"Referer: https://www.pixiv.net/");  // pixiv の画像は Referer が要る
        const upd::FetchResult f = upd::request(url, headers, nullptr, 128u * 1024 * 1024);
        std::wstring error;
        if (!f.ok) error = f.error;
        else if (f.status != 200) error = L"ダウンロードできませんでした (HTTP " + std::to_wstring(f.status) + L")";
        else if (f.body.empty()) error = L"中身が空でした";
        std::wstring tmp;
        if (error.empty()) {
            wchar_t dir[MAX_PATH] = L"";
            GetTempPathW(MAX_PATH, dir);
            tmp = std::wstring(dir) + L"imgdupe_url_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(gen) + L".img";
            HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            DWORD written = 0;
            const bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, f.body.data(), static_cast<DWORD>(f.body.size()), &written, nullptr) &&
                            written == f.body.size();
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            if (!ok) error = L"一時ファイルに書けませんでした";
        }
        if (!error.empty()) {
            auto* r = new SameDone;
            r->gen = gen;
            r->title = title;
            r->error = error;
            if (!PostMessageW(hwnd, WM_SAME_DONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
            return;
        }
        // 続きは画面のスレッドから（gen を新しくしないよう、同じ番号で探す）
        auto* path = new std::wstring(std::to_wstring(gen) + L"\n" + tmp + L"\n" + title);
        if (!PostMessageW(hwnd, WM_SAME_DONE, 1, reinterpret_cast<LPARAM>(path))) delete path;
    }).detach();
}

void ExplorerApp::ExplorerAddScanTask(const std::vector<std::wstring>& paths, bool top) { addScanTask(paths, top); }

bool ExplorerApp::ExplorerIsRoot(const std::wstring& dir) {
    const std::wstring d = stripTrailingSlash(dir);
    for (const auto& f : folderList())
        if (lstrcmpiW(stripTrailingSlash(f).c_str(), d.c_str()) == 0) return true;
    return false;
}

void removeRoot(const std::wstring& dir) {
    const std::wstring d = stripTrailingSlash(dir);
    if (logBox(app.hwnd,
                    (L"「" + d + L"」を調べるフォルダから外しますか？\n\nこれからは中の変化を見なくなります。"
                               L"作ったインデックス（タグなど）は残り、ファイルも消しません。")
                        .c_str(),
                    L"imgdupe", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    const int n = ListBox_GetCount(app.folders);
    for (int i = n - 1; i >= 0; --i) {
        std::wstring s(ListBox_GetTextLen(app.folders, i), L'\0');
        ListBox_GetText(app.folders, i, &s[0]);
        if (lstrcmpiW(stripTrailingSlash(s.c_str()).c_str(), d.c_str()) == 0) ListBox_DeleteString(app.folders, i);
    }
    saveSettings();
}

void ExplorerApp::ExplorerRemoveRoot(const std::wstring& dir) { removeRoot(dir); }

// 「調べるフォルダを外す」: 今の調べるフォルダをメニューに並べて、選んだものを外す
void removeRootMenu() {
    const auto f = folderList();
    if (f.empty()) {
        logBox(app.hwnd, L"調べるフォルダはありません。", L"imgdupe", MB_ICONINFORMATION);
        return;
    }
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < f.size(); ++i) AppendMenuW(menu, MF_STRING, i + 1, f[i].c_str());
    POINT pt;
    GetCursorPos(&pt);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, app.hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd >= 1 && cmd <= f.size()) {
        removeRoot(f[cmd - 1]);
        if (ex.view) ex.view->RefreshStates();
    }
}

void ExplorerApp::ExplorerSettingsChanged(const ExplorerSettings& s) {
    const std::wstring ini = iniPath();
    auto put = [&](const wchar_t* key, int v) { WritePrivateProfileStringW(L"Explorer", key, std::to_wstring(v).c_str(), ini.c_str()); };
    put(L"sizeMode", s.sizeMode);
    put(L"customSize", s.customSize);
    put(L"folderStyle", s.folderStyle);
    put(L"showTree", s.showTree ? 1 : 0);
    put(L"showPreview", s.showPreview ? 1 : 0);
    put(L"treeWidth", s.treeWidth);
    put(L"previewWidth", s.previewWidth);
    put(L"resultsHeight", s.resultsHeight);
    put(L"resultsView", s.resultsView);
    put(L"viewMode", s.viewMode);
    put(L"sortKey", s.sortKey);
    put(L"sortDesc", s.sortDesc ? 1 : 0);
    put(L"previewMode", s.previewMode);
    // 取り置いたタグ（タブで区切る）。ini はシステムの文字コードなので、どの文字でも残せる DB に持つ
    std::string tray;
    for (const auto& t : s.tray) tray += (tray.empty() ? "" : "\t") + narrow(t);
    if (Db* db = uiDb()) {
        try {
            db->setSetting("explorer:tray", tray);
        } catch (const std::exception&) {
        }
    }
}

ExplorerSettings loadExplorerSettings() {
    ExplorerSettings s;
    const std::wstring ini = iniPath();
    auto get = [&](const wchar_t* key, int def) { return static_cast<int>(GetPrivateProfileIntW(L"Explorer", key, def, ini.c_str())); };
    s.sizeMode = get(L"sizeMode", s.sizeMode);
    s.customSize = get(L"customSize", s.customSize);
    s.folderStyle = get(L"folderStyle", s.folderStyle);
    s.showTree = get(L"showTree", 1) != 0;
    s.showPreview = get(L"showPreview", 1) != 0;
    s.treeWidth = get(L"treeWidth", s.treeWidth);
    s.previewWidth = get(L"previewWidth", s.previewWidth);
    s.resultsHeight = get(L"resultsHeight", s.resultsHeight);
    s.resultsView = get(L"resultsView", s.resultsView);
    s.viewMode = get(L"viewMode", s.viewMode);
    s.sortKey = get(L"sortKey", s.sortKey);
    s.sortDesc = get(L"sortDesc", 0) != 0;
    s.previewMode = std::clamp(get(L"previewMode", s.previewMode), 0, 2);
    std::string tray, frequent;
    if (Db* db = uiDb()) {
        try {
            tray = db->setting("explorer:tray");
            frequent = db->setting("explorer:frequent");
        } catch (const std::exception&) {
        }
    }
    for (size_t a = 0; a < frequent.size();) {
        size_t b = frequent.find('\n', a);
        if (b == std::string::npos) b = frequent.size();
        const std::string line = frequent.substr(a, b - a);
        const size_t t = line.find('\t');
        if (t != std::string::npos) s.frequent.emplace_back(widen(line.substr(t + 1)), std::atoi(line.c_str()));
        a = b + 1;
    }
    for (size_t a = 0; a < tray.size();) {
        size_t b = tray.find('\t', a);
        if (b == std::string::npos) b = tray.size();
        if (b > a) s.tray.push_back(widen(tray.substr(a, b - a)));
        a = b + 1;
    }
    return s;
}

std::vector<int> ExplorerApp::ExplorerStates(const std::wstring& dir, const std::vector<std::pair<std::wstring, bool>>& items) {
    std::vector<int> out(items.size(), kStateNoBadge);
    std::vector<std::string> roots;
    for (const auto& f : folderList()) roots.push_back(narrow(f));
    const std::string d = narrow(dir);
    const IndexState here = folderIndexState(d, roots);
    std::unordered_map<std::string, IndexState> files;
    if (Db* db = uiDb()) {
        try {
            // 画像が少なければ名前で 1 つずつ引く（ドライブの直下などで、下の階層の全部の画像を読まない）
            std::vector<std::string> names;
            for (const auto& [name, isDir] : items)
                if (!isDir && imgdupe::isImageExt(narrow(name))) names.push_back(narrow(name));
            files = fileIndexStates(*db, d, roots, kHashVersion, names.size() <= 5000 ? &names : nullptr);
        } catch (const std::exception&) {
        }
    }
    const std::string base = d.empty() || d.back() == '\\' || d.back() == '/' ? d : d + "\\";
    for (size_t i = 0; i < items.size(); ++i) {
        const std::string name = narrow(items[i].first);
        if (items[i].second) {
            IndexState st = folderIndexState(base + name, roots);
            if (st == IndexState::None)
                if (Db* db = uiDb()) {
                    try {
                        if (folderHasIndexed(*db, base + name)) st = IndexState::Partial;  // 調べる対象の外でも、画像が DB にある
                    } catch (const std::exception&) {
                    }
                }
            out[i] = static_cast<int>(st);
        } else {
            auto it = files.find(name);
            // DB に無くても、調べる対象のフォルダの中なら、そのうち処理される
            out[i] = static_cast<int>(it != files.end() ? it->second : here == IndexState::Indexed ? IndexState::Pending : IndexState::None);
        }
    }
    return out;
}

// 更新: 画面の控えを捨てて今の場所を読み直す（DB の接続・画像の情報・タグの表示名も読み直す）。バックグラウンドの解析はしない
void refreshView() {
    app.infoCache.clear();
    app.uiDb.reset();  // 次に使う時に開き直す
    loadTagLabels();
    if (ex.view) ex.view->Refresh();
    if (ex.view) ex.view->SetSameStatus(L"更新しました（バックグラウンドの解析はしていません。新しいファイルのインデックスは、タスクトレイの解析が作ります）");
    logs::info("更新した");
}

// DB を詰めて小さくする: 大きさと空き容量を見せて確かめてから、バックグラウンドの解析に頼む
void compactDbUi() {
    Db* db = uiDb();
    if (!db) return;
    DbSpace sp;
    bool pending = false;
    try {
        sp = dbSpace(*db);
        pending = tagConversionState(*db).pending;
    } catch (const std::exception& e) {
        logBox(app.hwnd, widen(e.what()).c_str(), L"imgdupe", MB_ICONWARNING);
        return;
    }
    if (pending) {
        logBox(app.hwnd, L"DB を新しい形に変換している途中です。変換が済んでから詰めてください（進み具合は下のステータスバーかタスクトレイの窓に出ます）。",
               L"DB を詰める", MB_ICONINFORMATION);
        return;
    }
    auto gb = [](int64_t b) { return sizeText(static_cast<uint64_t>(std::max<int64_t>(0, b))); };
    ULARGE_INTEGER freeBytes{};
    GetDiskFreeSpaceExW(app.dataDir.c_str(), &freeBytes, nullptr, nullptr);
    const int64_t after = sp.fileBytes - sp.freeBytes;
    const int64_t need = after * 2 + after / 10;  // 詰めた後の大きさの一時ファイルと、書き込みの記録（-wal）の分と少し
    std::wstring msg = L"DB（imgdupe.db）: " + gb(sp.fileBytes) + L"\r\n使っていない所: " + gb(sp.freeBytes) + L"\r\n詰めた後: 約 " + gb(after) +
                       L"\r\n\r\n詰めている間は、バックグラウンドの解析が止まり、検索のタグの変更などが少し待たされます（数分〜数十分）。"
                       L"\r\n空き容量が約 " + gb(need) + L" 要ります（今の空き: " + gb(static_cast<int64_t>(freeBytes.QuadPart)) + L"）。";
    if (static_cast<int64_t>(freeBytes.QuadPart) < need) {
        logBox(app.hwnd, (msg + L"\r\n\r\n空き容量が足りないので、詰められません。").c_str(), L"DB を詰める", MB_ICONWARNING);
        return;
    }
    if (sp.freeBytes < sp.fileBytes / 20) msg += L"\r\n\r\n使っていない所が少ないので、あまり小さくなりません。";
    if (logBox(app.hwnd, (msg + L"\r\n\r\n詰めますか？").c_str(), L"DB を詰める", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    indexer::requestCompact(app.dataDir);
    if (!indexer::isRunning()) indexer::start();
    logs::info("DB を詰めるよう頼んだ（" + narrow(gb(sp.fileBytes)) + "、使っていない所 " + narrow(gb(sp.freeBytes)) + "）");
    if (ex.view) ex.view->SetSameStatus(L"DB を詰めるよう、バックグラウンドの解析に頼みました（進み具合と結果はタスクトレイの窓に出ます）");
}

// DB を変換した後（前の表の分が空いたまま）、一度だけ「詰める」をすすめる
void suggestCompact() {
    Db* db = uiDb();
    if (!db) return;
    try {
        if (db->setting("maintain:suggestCompact") != "1") return;
        db->setSetting("maintain:suggestCompact", "0");
    } catch (const std::exception&) {
        return;
    }
    compactDbUi();
}

void saveExplorerDir() {
    if (ex.view && !ex.view->Dir().empty()) WritePrivateProfileStringW(L"Explorer", L"dir", ex.view->Dir().c_str(), iniPath().c_str());
    // よく行く場所（"回数\tパス" の行）。ini はシステムの文字コードなので DB に持つ
    if (ex.view)
        if (Db* db = uiDb()) {
            std::string text;
            for (const auto& [d, n] : ex.view->Frequent()) text += std::to_string(n) + "\t" + narrow(d) + "\n";
            try {
                db->setSetting("explorer:frequent", text);
            } catch (const std::exception&) {
            }
        }
}

// 最初に開くフォルダ: 前回のフォルダ、無ければ調べるフォルダの 1 つ目、無ければピクチャ
std::wstring explorerStartDir() {
    wchar_t buf[4096] = L"";
    GetPrivateProfileStringW(L"Explorer", L"dir", L"", buf, 4096, iniPath().c_str());
    std::wstring d = buf;
    auto isDir = [](const std::wstring& p) {
        const DWORD a = p.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    };
    if (isDir(d)) return d;
    for (const auto& f : folderList())
        if (isDir(f)) return f;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &p)) && p) d = p;
    CoTaskMemFree(p);
    return isDir(d) ? d : L"C:\\";
}

// タブの並び（見た目の位置）と画面の番号（app.tab: 0 = 重複を探す、1 = 検索、2 = ホーム、3 = 画像で探す）
constexpr int kTabOrder[] = {2, 0, 1, 3};
int tabPos(int id) {
    for (int i = 0; i < 4; ++i)
        if (kTabOrder[i] == id) return i;
    return 0;
}

// リボンの切り替え（app.tab: 2 = ホーム、0 = 重複を探す、1 = 検索）。画面はいつもエクスプローラー
void switchTab(int t) {
    app.tab = t;
    TabCtrl_SetCurSel(app.tabs, tabPos(t));
    // 並べ替え・出し入れの間は描かない（部品ごとに描き直すと、エクスプローラーのサムネイルまで何度も描いて遅かった）
    SendMessageW(app.hwnd, WM_SETREDRAW, FALSE, 0);
    layout();  // 先に並べてから出す（前の場所に絵が残らないように）
    ShowWindow(app.toolbar, t == 2 ? SW_SHOW : SW_HIDE);
    if (HWND vb = ex.view->ViewBar()) ShowWindow(vb, t == 2 ? SW_SHOW : SW_HIDE);
    for (HWND h : {app.queryBar, app.queryStrip, app.queryTags}) ShowWindow(h, t == 3 ? SW_SHOW : SW_HIDE);
    for (HWND h : app.dupPage) ShowWindow(h, t == 0 ? SW_SHOW : SW_HIDE);
    for (HWND h : app.searchPage) ShowWindow(h, t == 1 ? SW_SHOW : SW_HIDE);
    for (HWND h : app.advancedPage) ShowWindow(h, t == 1 && app.advancedShown ? SW_SHOW : SW_HIDE);
    SendMessageW(app.hwnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(app.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    if (!ex.shown) {
        ex.shown = true;
        ex.view->Show(explorerStartDir(), L"");
    }
    if (t == 1) {
        refreshSources();
        refreshSmartFolders(L"");
        SetFocus(app.s.free);
    }
}

void toggleAdvanced() {
    app.advancedShown = !app.advancedShown;
    SetWindowTextW(app.s.advanced, app.advancedShown ? L"詳細検索 ▲" : L"詳細検索 ▼");
    iniSet(L"advancedSearch", app.advancedShown ? L"1" : L"0");
    switchTab(app.tab);
}

// 検索のリボン（3 段）
void layoutSearch(int y, int W) {
    const int m = px(8), rowH = px(24), gap = px(6), labelY = px(4);
    int x = m, cy = y;
    auto put = [&](HWND h, int w, int height = 0) {
        MoveWindow(h, x, cy, w, height ? height : rowH, TRUE);
        x += w + gap;
    };
    auto lbl = [&](HWND h, int w) {
        MoveWindow(h, x, cy + labelY, w, rowH - labelY, TRUE);
        x += w + px(2);
    };
    const int freeW = std::clamp(W / 3, px(240), px(560));
    put(app.s.free, freeW);
    put(app.s.scope, px(170), px(200));
    put(app.s.go, px(64));
    x += px(10);
    lbl(app.s.lSort, px(58));
    put(app.s.sort, px(150), px(320));
    put(app.s.desc, px(56));
    x += px(10);
    put(app.s.advanced, px(100));
    put(app.s.clear, px(100));
    cy += rowH + px(5);
    x = m;
    lbl(app.s.lClip, px(70));
    put(app.s.clip, px(200));
    x += px(10);
    lbl(app.s.lSmart, px(84));
    put(app.s.smart, px(170), px(300));
    put(app.s.smartSave, px(56));
    put(app.s.smartDelete, px(56));
    x += px(10);
    put(app.s.review, px(96));
    x += px(10);
    put(app.s.expand, px(100));
    cy += rowH + px(5);
    x = m;
    MoveWindow(app.s.count, m, cy + px(2), W - 2 * m, rowH - px(4), TRUE);
}

// 詳細検索（3 段。右に出どころの一覧）
void layoutAdvanced(int y, int W, int h) {
    const int m = px(8), rowH = px(24), gap = px(6), labelY = px(4);
    const int srcW = px(260);
    int x = m, cy = y;
    auto put = [&](HWND hw, int w, int height = 0) {
        MoveWindow(hw, x, cy, w, height ? height : rowH, TRUE);
        x += w + gap;
    };
    auto lbl = [&](HWND hw, int w) {
        MoveWindow(hw, x, cy + labelY, w, rowH - labelY, TRUE);
        x += w + px(2);
    };
    lbl(app.s.lRating, px(24));
    put(app.s.rating, px(96), px(240));
    lbl(app.s.lFormat, px(32));
    put(app.s.format, px(110), px(260));
    lbl(app.s.lQuality, px(32));
    put(app.s.quality, px(170), px(200));
    lbl(app.s.lWidth, px(80));
    put(app.s.minW, px(60));
    lbl(app.s.lSize, px(118));
    put(app.s.minKb, px(64));
    lbl(app.s.lSizeTo, px(14));
    put(app.s.maxKb, px(64));
    cy += rowH + px(5);
    x = m;
    lbl(app.s.lDate, px(30));
    put(app.s.dateField, px(90), px(200));
    put(app.s.from, px(170));
    lbl(app.s.lDateTo, px(14));
    put(app.s.to, px(170));
    put(app.s.unclassified, px(130));
    put(app.s.deadLinks, px(90));
    cy += rowH + px(5);
    x = m;
    lbl(app.s.lPath, px(80));
    put(app.s.path, px(200));
    lbl(app.s.lDownload, px(130));
    put(app.s.download, px(200));
    lbl(app.s.lConf, px(30));
    put(app.s.conf, px(140), rowH + px(4));
    put(app.s.confValue, px(40));
    // 右に出どころ
    const int sx = std::max(x, W - m - srcW);
    MoveWindow(app.s.lSources, sx, y, srcW, px(18), TRUE);
    MoveWindow(app.s.sources, sx, y + px(18), srcW, std::max(px(40), h - px(24)), TRUE);
}

// 検索のリボン（フリーワード・範囲・並べ替えなど）と、詳細検索（普段は隠す）の部品
void createSearchControls() {
    auto label = [&](const wchar_t* text, std::vector<HWND>& page) {
        HWND h = makeControl(WC_STATICW, text, SS_NOPREFIX, IDC_S_LABEL);
        page.push_back(h);
        return h;
    };
    auto edit = [&](int id, std::vector<HWND>& page, DWORD style = 0) {
        HWND h = makeControl(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL | style, id, WS_EX_CLIENTEDGE);
        page.push_back(h);
        return h;
    };
    auto combo = [&](int id, std::vector<HWND>& page, std::initializer_list<const wchar_t*> items) {
        HWND h = makeControl(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, id);
        for (const wchar_t* t : items) ComboBox_AddString(h, t);
        ComboBox_SetCurSel(h, 0);
        page.push_back(h);
        return h;
    };
    auto button = [&](const wchar_t* text, int id, std::vector<HWND>& page, DWORD style = 0) {
        HWND h = makeControl(WC_BUTTONW, text, WS_TABSTOP | style, id);
        page.push_back(h);
        return h;
    };
    auto& sp = app.searchPage;
    auto& ap = app.advancedPage;
    // 1 段目: フリーワード・範囲・検索・並べ替え・詳細検索・クリア
    app.s.free = edit(IDC_S_FREE, sp);
    SendMessageW(app.s.free, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"フリーワード（空白で区切る。-語 で除く）"));
    app.s.scope = combo(IDC_S_SCOPE, sp, {L"すべて", L"タグに限る", L"タイトルに限る", L"ファイルのタグに限る", L"ファイル名・フォルダ名に限る"});
    app.s.go = button(L"検索", IDC_S_GO, sp, BS_DEFPUSHBUTTON);
    app.s.lSort = label(L"並べ替え", sp);
    app.s.sort = combo(IDC_S_SORT, sp, {L"関連度", L"ファイル名", L"パス", L"更新日時", L"作成日時", L"撮影日時", L"解像度", L"ファイルサイズ",
                                        L"星", L"タイトル", L"確度（1 つ目のタグ）", L"見栄え（AI）", L"実質の解像度"});
    app.s.desc = button(L"降順", IDC_S_DESC, sp, BS_AUTOCHECKBOX);
    app.s.advanced = button(L"詳細検索 ▼", IDC_S_ADVANCED, sp);
    app.s.clear = button(L"条件をクリア", IDC_S_CLEAR, sp);
    // 2 段目: 内容（AI）・スマートフォルダ・確認キュー・フォルダに展開
    app.s.lClip = label(L"内容で探す", sp);
    app.s.clip = edit(IDC_S_CLIP, sp);
    SendMessageW(app.s.clip, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"夕焼けの海 など"));
    app.s.lSmart = label(L"保存した条件", sp);
    app.s.smart = combo(IDC_S_SMART, sp, {});
    app.s.smartSave = button(L"保存…", IDC_S_SMART_SAVE, sp);
    app.s.smartDelete = button(L"管理…", IDC_S_SMART_DELETE, sp);  // 削除・並べ替え・書き出し
    app.s.review = button(L"確認キュー", IDC_S_REVIEW, sp, BS_AUTOCHECKBOX);
    // 「正しい」「違う」「タグ…」はリボンに出さない（メニューの「タグ」とキーで使える）
    std::vector<HWND> unused;
    app.s.yes = button(L"正しい", IDC_S_YES, unused);
    app.s.no = button(L"違う", IDC_S_NO, unused);
    app.s.bulk = button(L"タグ…", IDC_S_BULK, unused);
    for (HWND h : unused) ShowWindow(h, SW_HIDE);
    app.s.expand = button(L"フォルダを作成", IDC_S_EXPAND, sp);
    // 3 段目: 件数
    app.s.count = makeControl(WC_STATICW, L"フリーワードを入れて Enter（検索ボタン）で探します。結果はエクスプローラーに出します。",
                              SS_NOPREFIX | SS_ENDELLIPSIS, IDC_S_COUNT);
    sp.push_back(app.s.count);

    // 詳細検索
    app.s.lRating = label(L"星", ap);
    app.s.rating = combo(IDC_S_RATING, ap, {L"指定なし", L"★1 以上", L"★2 以上", L"★3 以上", L"★4 以上", L"★5", L"星なし"});
    app.s.lFormat = label(L"形式", ap);
    app.s.format = combo(IDC_S_FORMAT, ap, {L"すべて", L"JPEG", L"PNG", L"GIF", L"WebP", L"HEIF", L"BMP", L"TIFF", L"JPEG XR", L"アニメーション"});
    app.s.lQuality = label(L"品質", ap);
    app.s.quality = combo(IDC_S_QUALITY, ap, {L"すべて", L"くっきり", L"ぼやけている", L"拡大・細かさ不足", L"低解像度（実質 800px 未満）"});
    app.s.lWidth = label(L"幅（px）以上", ap);
    app.s.minW = edit(IDC_S_MINW, ap, ES_NUMBER);
    app.s.lSize = label(L"ファイルサイズ（KB）", ap);
    app.s.minKb = edit(IDC_S_MINKB, ap, ES_NUMBER);
    app.s.lSizeTo = label(L"〜", ap);
    app.s.maxKb = edit(IDC_S_MAXKB, ap, ES_NUMBER);
    app.s.lPath = label(L"パス（一部）", ap);
    app.s.path = edit(IDC_S_PATH, ap);
    app.s.lDownload = label(L"ダウンロード元（URL）", ap);
    app.s.download = edit(IDC_S_DOWNLOAD, ap);
    app.s.lDate = label(L"日付", ap);
    app.s.dateField = combo(IDC_S_DATEFIELD, ap, {L"撮影日", L"更新日時", L"作成日時"});
    // 年・月・日の上でホイールを回すと、その欄が 1 ずつ変わる（自分で描く日付の欄）
    registerDateBox();
    app.s.from = makeControl(L"ImgdupeDateBox", L"", WS_TABSTOP, IDC_S_FROM, WS_EX_CLIENTEDGE);
    app.s.lDateTo = label(L"〜", ap);
    app.s.to = makeControl(L"ImgdupeDateBox", L"", WS_TABSTOP, IDC_S_TO, WS_EX_CLIENTEDGE);
    resetDatePickers();
    ap.insert(ap.end(), {app.s.from, app.s.to});
    {
        std::vector<HWND> unused;  // 「撮影日なし」は出さない（保存した条件の読み書きのために部品だけ残す）
        app.s.noTaken = button(L"撮影日なし", IDC_S_NOTAKEN, unused, BS_AUTOCHECKBOX);
        ShowWindow(app.s.noTaken, SW_HIDE);
    }
    app.s.unclassified = button(L"未分類（タグなし）", IDC_S_UNCLASSIFIED, ap, BS_AUTOCHECKBOX);
    app.s.deadLinks = button(L"リンク切れ", IDC_S_DEADLINKS, ap, BS_AUTOCHECKBOX);
    app.s.lConf = label(L"確度", ap);
    app.s.conf = makeControl(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, IDC_S_CONF);
    SendMessageW(app.s.conf, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(app.s.conf, TBM_SETPOS, TRUE, 50);
    app.s.confValue = makeControl(WC_STATICW, L"0.50", SS_LEFT, IDC_S_CONF_VALUE);
    ap.insert(ap.end(), {app.s.conf, app.s.confValue});
    app.s.lSources = label(L"出どころ（チェックしたものだけで確度を計算）", ap);
    app.s.sources = makeControl(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL, IDC_S_SOURCES,
                                WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(app.s.sources, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    addColumn(app.s.sources, 0, L"", 240);
    ap.push_back(app.s.sources);

    // 前の画面の部品（使わないが、条件の読み書きの都合で残す。いつも隠す）
    app.s.tags = makeControl(WC_EDITW, L"", 0, IDC_S_TAGS);
    app.s.text = makeControl(WC_EDITW, L"", 0, IDC_S_TEXT);
    app.s.results = makeControl(WC_LISTVIEWW, L"", LVS_ICON | LVS_OWNERDATA, IDC_S_RESULTS);
    app.views[kSearchView].lv = app.s.results;
    resetView(kSearchView, 0);
    for (HWND h : {app.s.tags, app.s.text, app.s.results}) ShowWindow(h, SW_HIDE);
    for (HWND h : sp) ShowWindow(h, SW_HIDE);
    for (HWND h : ap) ShowWindow(h, SW_HIDE);

    // マウスを乗せた時の説明
    HWND tip = makeTooltip(app.hwnd, px(380));
    const std::pair<HWND, const wchar_t*> tips[] = {
        {app.s.free, L"タグ・タイトル・ファイル名などから探す語。空白で区切るとすべてを含むもの、-語 でその語を含むものを除きます"},
        {app.s.scope,
         L"フリーワードで探す所（すべて / タグだけ / タイトルだけ / ファイルのタグだけ / ファイル名とフォルダ名だけ）。"
         L"「ファイルのタグに限る」は、画像のファイルに書き込まれているタグ（Windows のエクスプローラーのタグなど）だけで探します。"
         L"「すべて」と「ファイル名・フォルダ名に限る」では、名前が合うフォルダも結果の先頭に出します（開くとそのフォルダへ）"},
        {app.s.go, L"今の条件で探して、結果をエクスプローラーに出します（Enter）"},
        {app.s.sort, L"結果の並べ方"},
        {app.s.desc, L"チェックすると逆の順（大きい・新しいものから）に並べます"},
        {app.s.advanced, L"星・形式・品質・大きさ・日付・パス・出どころなどの細かい条件を出す・隠す"},
        {app.s.clear, L"条件を全部空に戻します"},
        {app.s.clip,
         L"画像に写っているものを言葉で探します（AI の CLIP が画像と言葉の近さを比べます。タグが付いていなくても探せます）。"
         L"例: 夕焼けの海、白い猫、赤い服の女の子。英語のほうが正確です。CLIP を入れてある時だけ"},
        {app.s.lClip, L"画像に写っているものを言葉で探します（AI）"},
        {app.s.smart, L"保存した条件を選ぶと、その条件を読み込んで探します"},
        {app.s.smartSave, L"今の条件に名前を付けて保存します（スマートフォルダ）"},
        {app.s.smartDelete, L"保存した条件を削除・並べ替え・名前の変更・書き出し（JSON）・読み込みします"},
        {app.s.review, L"チェックすると、AI のタグが当たっているか迷っている画像から順に並べます（確かめる時に）"},
        {app.s.expand,
         L"検索の結果の画像を、ハードリンクで 1 つのフォルダ（ドライブごとの _vfolders の下）に並べます。"
         L"ほかのアプリ（ビューアなど）でその画像だけを見られます。容量は増えず、元のファイルも動きません。"
         L"このソフトを終了すると消えます（残す時は「表示 → 展開したフォルダ」で固定にします）"},
        {app.s.rating, L"星（レーティング）で絞ります"},
        {app.s.format, L"ファイルの形式で絞ります"},
        {app.s.quality, L"くっきり・ぼやけ・拡大された画像などで絞ります（品質を調べてある時だけ）"},
        {app.s.minW, L"幅がこれ以上（px）の画像だけ"},
        {app.s.minKb, L"ファイルサイズの下限（KB）"},
        {app.s.maxKb, L"ファイルサイズの上限（KB）"},
        {app.s.dateField, L"日付の条件にどの日時を使うか"},
        {app.s.from,
         L"この日から（左のチェックを入れると使います）。年・月・日それぞれの上でホイールを回すと、その欄が 1 ずつ変わります"
         L"（クリックして ↑↓ キーでも。PageUp / PageDown で 10 ずつ）"},
        {app.s.to,
         L"この日まで（左のチェックを入れると使います）。年・月・日それぞれの上でホイールを回すと、その欄が 1 ずつ変わります"
         L"（クリックして ↑↓ キーでも。PageUp / PageDown で 10 ずつ）"},
        {app.s.unclassified, L"タグが 1 つも無い画像だけ"},
        {app.s.deadLinks, L"出典の投稿が消えている画像だけ（出典を確かめる設定にしてある時）"},
        {app.s.path, L"パスにこの文字を含む画像だけ"},
        {app.s.download, L"ダウンロード元の URL にこの文字を含む画像だけ"},
        {app.s.conf, L"タグの確度がこれ以上のものだけをタグとして使います"},
        {app.s.sources, L"チェックした出どころのタグだけで確度を計算します（数はタグの数）"},
        {app.exact, L"中身がまったく同じファイルだけを探します（見た目では比べない）"},
        {app.flat, L"真っ白・真っ黒など単色に近い画像も見た目で比べます（別の画像がまとまりやすくなります）"},
        {app.variants, L"AI（CLIP）で見つけた差分（表情違い・色違いなど）のまとまりも出します"},
        {app.threshold, L"小さいほど厳しく（ほぼ同じものだけ）、大きいほどゆるく（似ているものまで）まとめます"},
    };
    for (const auto& [h, text] : tips) addTooltip(tip, h, text);
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
            advancedChanged();
        }
        return true;
    }
    if (nm->hwndFrom == app.tabs && nm->code == TCN_SELCHANGE) {
        switchTab(kTabOrder[std::clamp(TabCtrl_GetCurSel(app.tabs), 0, 3)]);
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
                if ((app.similarTo || !app.similarVec.empty()) && editText(app.s.clip) != app.similarLabel) {
                    app.similarTo = 0;  // 書き換えたら言葉で探す
                    app.similarVec.clear();
                }
                scheduleSearch(500);  // 言葉のベクトル化は少し重いので、長めに待つ
            }
            return true;
        case IDC_S_MINW: case IDC_S_MINKB: case IDC_S_MAXKB: case IDC_S_PATH:
        case IDC_S_DOWNLOAD:
            if (code == EN_CHANGE) advancedChanged();
            return true;
        case IDC_S_GO:
            runSearch();
            return true;
        case IDC_S_FROM: case IDC_S_TO:
            if (code == DBN_CHANGED) advancedChanged();
            return true;
        case IDC_S_ADVANCED:
            toggleAdvanced();
            return true;
        case IDC_S_SORT: case IDC_S_SCOPE:
            if (code == CBN_SELCHANGE) scheduleSearch(30);
            return true;
        case IDC_S_RATING: case IDC_S_FORMAT: case IDC_S_QUALITY: case IDC_S_DATEFIELD:
            if (code == CBN_SELCHANGE) advancedChanged();
            return true;
        case IDC_S_DESC:
            scheduleSearch(30);
            return true;
        case IDC_S_NOTAKEN: case IDC_S_UNCLASSIFIED: case IDC_S_DEADLINKS:
            advancedChanged();
            return true;
        case IDC_S_CLEAR:
            clearSearchConditions();
            return true;
        case IDC_S_SMART:
            // 開いた一覧の中で動かしている間は読み込まない（選び終えた時か、閉じたままキーで変えた時）
            if (code == CBN_SELENDOK || (code == CBN_SELCHANGE && !ComboBox_GetDroppedState(app.s.smart))) {
                const int i = ComboBox_GetCurSel(app.s.smart) - 1;
                if (i >= 0 && i < static_cast<int>(app.smartList.size())) applyQueryToUi(app.smartList[i].query);
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
            manageSmartFoldersUi();
            return true;
    }
    return false;
}

void saveSearchSettings() {
    WritePrivateProfileStringW(L"Search", L"free", editText(app.s.free).c_str(), iniPath().c_str());
    WritePrivateProfileStringW(L"Search", L"scope", std::to_wstring(ComboBox_GetCurSel(app.s.scope)).c_str(), iniPath().c_str());
    WritePrivateProfileStringW(L"Search", L"clip", app.similarTo ? L"" : editText(app.s.clip).c_str(), iniPath().c_str());
    WritePrivateProfileStringW(L"Search", L"confidence",
                               std::to_wstring(SendMessageW(app.s.conf, TBM_GETPOS, 0, 0)).c_str(), iniPath().c_str());
    WritePrivateProfileStringW(L"Search", L"sortChoice", std::to_wstring(ComboBox_GetCurSel(app.s.sort)).c_str(), iniPath().c_str());
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
    GetPrivateProfileStringW(L"Search", L"free", L"", buf, 4096, iniPath().c_str());
    SetWindowTextW(app.s.free, buf);
    ComboBox_SetCurSel(app.s.scope, std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Search", L"scope", 0, iniPath().c_str())), 0, 4));
    GetPrivateProfileStringW(L"Search", L"clip", L"", buf, 4096, iniPath().c_str());
    SetWindowTextW(app.s.clip, buf);
    const int conf = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Search", L"confidence", 50, iniPath().c_str())), 0, 100);
    SendMessageW(app.s.conf, TBM_SETPOS, TRUE, conf);
    wchar_t v[16];
    swprintf(v, 16, L"%.2f", conf / 100.0);
    setText(app.s.confValue, v);
    // 並べ替えは選択肢の並びが変わったので、新しいキーに持つ（既定は関連度）
    ComboBox_SetCurSel(app.s.sort, std::max(0, static_cast<int>(GetPrivateProfileIntW(L"Search", L"sortChoice", 0, iniPath().c_str()))));
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

// 上にリボン（見出しと、その下に選んだリボンの中身）、検索のリボンなら詳細検索、残りはエクスプローラー
void layout() {
    // WM_CREATE の中の SetMenu でも WM_SIZE が来る（まだ部品を作っていない）
    if (!ex.view) return;
    RECT rc;
    GetClientRect(app.hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int m = px(6), rowH = px(24);
    int y = px(2);
    const int tabH = px(24), panelH = px(84), advH = px(92);
    MoveWindow(app.tabs, m, y, W - 2 * m, tabH, TRUE);
    y += tabH + px(2);
    if (app.tab == 2) {
        // ホーム: 左にボタン、右に表示の設定（表示・並び順・大きさ・表紙・フォルダ一覧・プレビュー）
        SIZE bar{};
        SendMessageW(app.toolbar, TB_GETMAXSIZE, 0, reinterpret_cast<LPARAM>(&bar));
        const int barW = std::max<int>(bar.cx, px(200));
        MoveWindow(app.toolbar, m, y, barW, panelH - px(4), TRUE);
        if (HWND vb = ex.view->ViewBar()) {
            const SIZE vs = ex.view->ViewBarSize();
            MoveWindow(vb, m + barW + px(16), y + px(2), vs.cx, std::min<int>(vs.cy, panelH - px(4)), TRUE);
        }
    } else if (app.tab == 3) {
        // 画像で探す: 左にボタン、真ん中に足した画像、右に生成したタグ・様子
        SIZE bar{};
        SendMessageW(app.queryBar, TB_GETMAXSIZE, 0, reinterpret_cast<LPARAM>(&bar));
        const int barW = std::max<int>(bar.cx, px(200));
        MoveWindow(app.queryBar, m, y, barW, panelH - px(4), TRUE);
        const int tagsW = std::clamp(W / 3, px(260), px(520));
        const int sx = m + barW + px(8), stripW = std::max(px(120), W - m - tagsW - px(8) - sx);
        MoveWindow(app.queryStrip, sx, y, stripW, panelH - px(4), TRUE);
        MoveWindow(app.queryTags, sx + stripW + px(8), y, std::max(px(120), W - m - (sx + stripW + px(8))), panelH - px(4), TRUE);
    } else if (app.tab == 0) {
        // 重複を探す: 左にボタン、右にしきい値などと進み具合（調べるフォルダはホームで足す・外す）
        SIZE bar{};
        SendMessageW(app.dupBar, TB_GETMAXSIZE, 0, reinterpret_cast<LPARAM>(&bar));
        const int barW = std::max<int>(bar.cx, px(300));
        MoveWindow(app.dupBar, m, y, barW, panelH - px(4), TRUE);
        int x = m + barW + px(8);
        int cy = y + px(2);
        MoveWindow(app.thresholdCaption, x, cy + px(4), px(58), rowH - px(4), TRUE);
        MoveWindow(app.threshold, x + px(60), cy, px(150), rowH + px(4), TRUE);
        MoveWindow(app.thresholdLabel, x + px(214), cy + px(4), px(110), rowH - px(4), TRUE);
        cy += rowH + px(4);
        MoveWindow(app.exact, x, cy, px(110), rowH, TRUE);
        MoveWindow(app.flat, x + px(114), cy, px(180), rowH, TRUE);
        MoveWindow(app.variants, x + px(298), cy, px(150), rowH, TRUE);
        cy += rowH + px(4);
        MoveWindow(app.progress, x, cy + px(4), px(150), px(14), TRUE);
        MoveWindow(app.status, x + px(158), cy + px(1), std::max(0, W - m - x - px(158)), px(20), TRUE);
    } else {
        layoutSearch(y, W);
    }
    y += panelH;
    if (app.tab == 1 && app.advancedShown) {
        layoutAdvanced(y, W, advH);
        y += advH;
    }
    MoveWindow(app.ribbonLine, 0, y, W, 2, TRUE);
    y += px(3);
    ex.view->Move(RECT{0, y, W, H});
    RedrawWindow(app.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
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

// ---------------------------------------------------------------- 画像で探す（クリップボード・ドロップ・URL）

// 足した画像は <dataDir>\query に写して置く（10 枚まで、か 1 日分の少ない方。古いものは足す時と起動の時に消す）
std::wstring queryDir() { return app.dataDir + L"\\query"; }

void drawQueryStripLater() {
    if (app.queryStrip) InvalidateRect(app.queryStrip, nullptr, TRUE);
}

// 一時に置いた画像を読み直す（消す分を消して、新しい順に並べる）
void refreshQueryFiles() {
    std::vector<std::string> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((queryDir() + L"\\q_*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(narrow(fd.cFileName));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    const auto gone = queryImagesToRemove(names, static_cast<int64_t>(std::time(nullptr)));
    for (const auto& n : gone) {
        const std::wstring p = queryDir() + L"\\" + widen(n);
        DeleteFileW(p.c_str());
        auto it = app.queryThumbs.find(p);
        if (it != app.queryThumbs.end()) {
            DeleteObject(it->second);
            app.queryThumbs.erase(it);
        }
    }
    std::vector<std::pair<std::string, std::string>> keep;  // (並べる鍵, 名前)
    for (const auto& n : names) {
        if (std::find(gone.begin(), gone.end(), n) != gone.end() || !queryImageTime(n)) continue;
        char key[40];
        snprintf(key, sizeof key, "%020lld", static_cast<long long>(queryImageTime(n)));
        keep.emplace_back(std::string(key) + n, n);
    }
    std::sort(keep.begin(), keep.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    app.queryFiles.clear();
    for (const auto& k : keep) app.queryFiles.push_back(queryDir() + L"\\" + widen(k.second));
    if (std::find(app.queryFiles.begin(), app.queryFiles.end(), app.querySel) == app.queryFiles.end())
        app.querySel = app.queryFiles.empty() ? L"" : app.queryFiles.front();
    drawQueryStripLater();
}

std::wstring extensionOf(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.'), sep = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep) || path.size() - dot > 6) return L".img";
    return path.substr(dot);
}

// 新しい名前で置く（中身を書く関数を渡す）。選んだ状態にする
std::wstring storeQuery(const std::wstring& ext, const std::function<bool(const std::wstring&)>& write) {
    CreateDirectoryW(queryDir().c_str(), nullptr);
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    std::wstring path;
    for (int seq = 0; seq < 1000; ++seq) {
        path = queryDir() + L"\\" + widen(queryImageName(now, seq, narrow(ext)));
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    if (!write(path)) {
        DeleteFileW(path.c_str());
        return L"";
    }
    app.querySel = path;
    refreshQueryFiles();
    if (ex.view) ex.view->PreviewFile(path);  // 足した画像をプレビューに出す
    return path;
}

bool writeBytes(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
    CloseHandle(h);
    return ok;
}

void setQueryStatus(const std::wstring& text) {
    if (app.queryTags) SetWindowTextW(app.queryTags, text.c_str());
    if (text.find(L"できませんでした") != std::wstring::npos || text.find(L"読めません") != std::wstring::npos)
        logs::warn("画像で探す: " + narrow(text));
}

bool isUrlText(const std::wstring& s) { return _wcsnicmp(s.c_str(), L"http://", 7) == 0 || _wcsnicmp(s.c_str(), L"https://", 8) == 0; }

struct QueryFetched {
    std::wstring url, ext, error;
    std::string body;
};

// URL の画像をダウンロードして置く（終わったら WM_APP_QUERY_FILE）
void addQueryUrl(const std::wstring& url) {
    setQueryStatus(L"ダウンロードしています… " + url);
    HWND hwnd = app.hwnd;
    std::thread([url, hwnd] {
        auto* r = new QueryFetched;
        r->url = url;
        std::vector<std::wstring> headers = {L"Accept: image/*,*/*;q=0.8"};
        if (url.find(L"pximg.net") != std::wstring::npos) headers.push_back(L"Referer: https://www.pixiv.net/");  // pixiv は Referer が要る
        const upd::FetchResult f = upd::request(url, headers, nullptr, 128u * 1024 * 1024);
        if (!f.ok) r->error = f.error;
        else if (f.status != 200) r->error = L"ダウンロードできませんでした (HTTP " + std::to_wstring(f.status) + L")";
        else if (f.body.empty()) r->error = L"中身が空でした";
        else r->body.assign(f.body.begin(), f.body.end());
        std::wstring path = url.substr(0, url.find_first_of(L"?#"));
        r->ext = extensionOf(path);
        if (!PostMessageW(hwnd, WM_APP_QUERY_FILE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void onQueryFetched(QueryFetched* p) {
    std::unique_ptr<QueryFetched> r(p);
    if (!r->error.empty()) {
        setQueryStatus(L"URL の画像を取れませんでした: " + r->error);
        return;
    }
    const std::string body = std::move(r->body);
    if (storeQuery(r->ext, [&](const std::wstring& path) { return writeBytes(path, body.data(), body.size()); }).empty())
        setQueryStatus(L"画像を置けませんでした");
    else
        setQueryStatus(L"URL の画像を足しました。上のボタンで、同じ画像・似た画像を探したり、タグを生成したりできます");
}

// ファイル・URL（ドロップ・貼り付け・ファイルの選択）を足す
void addQueryInputs(const std::vector<std::wstring>& files, const std::wstring& url) {
    size_t added = 0;
    for (const auto& f : files) {
        const DWORD attr = GetFileAttributesW(f.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!storeQuery(extensionOf(f), [&](const std::wstring& path) { return CopyFileW(f.c_str(), path.c_str(), FALSE) != 0; }).empty())
            ++added;
    }
    if (!url.empty()) addQueryUrl(url);
    else if (added) setQueryStatus(std::to_wstring(added) + L" 枚を足しました。上のボタンで、同じ画像・似た画像を探したり、タグを生成したりできます");
    else if (!files.empty()) setQueryStatus(L"画像のファイルか、画像の URL をドロップしてください");
}

// クリップボード: 画像（DIB）、コピーしたファイル、URL・パスの文字
void queryPaste() {
    if (!OpenClipboard(app.hwnd)) return;
    std::vector<std::wstring> files;
    std::wstring text;
    std::vector<uint8_t> bmp;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        HDROP d = static_cast<HDROP>(h);
        const UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t buf[32768];
            if (DragQueryFileW(d, i, buf, static_cast<UINT>(std::size(buf)))) files.push_back(buf);
        }
    } else if (HANDLE dib = GetClipboardData(CF_DIB)) {
        if (const auto* p = static_cast<const uint8_t*>(GlobalLock(dib))) {
            bmp = bmpFromDib(p, GlobalSize(dib));
            GlobalUnlock(dib);
        }
    } else if (HANDLE t = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(t))) {
            text = p;
            GlobalUnlock(t);
        }
    }
    CloseClipboard();
    if (!bmp.empty()) {
        if (storeQuery(L".bmp", [&](const std::wstring& path) { return writeBytes(path, bmp.data(), bmp.size()); }).empty())
            setQueryStatus(L"クリップボードの画像を置けませんでした");
        else
            setQueryStatus(L"クリップボードの画像を足しました。上のボタンで、同じ画像・似た画像を探したり、タグを生成したりできます");
        return;
    }
    const size_t nl = text.find_first_of(L"\r\n");
    if (nl != std::wstring::npos) text.resize(nl);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'"')) text.pop_back();
    while (!text.empty() && (text.front() == L' ' || text.front() == L'"')) text.erase(text.begin());
    if (isUrlText(text)) addQueryInputs({}, text);
    else if (!text.empty()) addQueryInputs({text}, L"");
    else if (!files.empty()) addQueryInputs(files, L"");
    else setQueryStatus(L"クリップボードに画像がありません（画像・画像のファイル・画像の URL をコピーしてから押してください）");
}

std::wstring querySelected() {
    if (app.querySel.empty() || GetFileAttributesW(app.querySel.c_str()) == INVALID_FILE_ATTRIBUTES) {
        setQueryStatus(L"先に画像を足してください（貼り付け・ドロップ・ファイル・URL）");
        return L"";
    }
    return app.querySel;
}

void querySame() {
    const std::wstring p = querySelected();
    if (!p.empty()) ex.ExplorerFindSameFile(p);  // 結果はエクスプローラーの一覧に出す
}

struct QueryVector {
    std::wstring path, error;
    std::vector<float> v;
};

void querySimilar() {
    const std::wstring p = querySelected();
    if (p.empty() || app.queryBusy) return;
    app.queryBusy = true;
    setQueryStatus(L"似た画像を探すために、画像を AI（CLIP）で読んでいます…（最初の 1 回は数秒かかります）");
    HWND hwnd = app.hwnd;
    const std::wstring dataDir = app.dataDir;
    std::thread([p, hwnd, dataDir] {
        imgwin::threadBegin();
        auto* r = new QueryVector;
        r->path = p;
        if (!clip::encodeImageFile(dataDir, p, r->v, r->error) && r->error.empty()) r->error = L"できませんでした";
        imgwin::threadEnd();
        if (!PostMessageW(hwnd, WM_APP_QUERY_VEC, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void onQueryVector(QueryVector* p) {
    std::unique_ptr<QueryVector> r(p);
    app.queryBusy = false;
    if (!r->error.empty()) {
        setQueryStatus(L"似た画像を探せませんでした: " + r->error);
        return;
    }
    // 検索の「内容で探す」をこの画像にして探す（ほかの条件はそのまま。結果はエクスプローラーに出す）
    app.similarTo = 0;
    app.similarVec = std::move(r->v);
    app.similarLabel = L"似た画像: " + fileNameOf(r->path);
    SetWindowTextW(app.s.clip, app.similarLabel.c_str());
    setQueryStatus(L"似た画像を探しました（結果はエクスプローラーに出します。検索のリボンの条件も使います）");
    runSearch();
}

struct QueryTagsDone {
    std::wstring error;
    std::vector<TagRow> tags;
};

void queryTags() {
    const std::wstring p = querySelected();
    if (p.empty() || app.queryBusy) return;
    app.queryBusy = true;
    setQueryStatus(L"タグを生成しています…（AI の WD14 タガー。最初の 1 回はモデルの読み込みに数秒かかります）");
    HWND hwnd = app.hwnd;
    const std::wstring dataDir = app.dataDir;
    std::thread([p, hwnd, dataDir] {
        imgwin::threadBegin();
        auto* r = new QueryTagsDone;
        std::shared_ptr<Analyzer> tagger;
        {
            std::lock_guard<std::mutex> lock(app.guiTaggerMutex);
            if (!app.guiTagger) {
                tagger::LoadResult lr = tagger::load(dataDir, 2);
                app.guiTagger = lr.analyzer;
                if (!lr.analyzer)
                    r->error = !lr.error.empty() ? lr.error : L"WD14 タガーが入っていません（「その他 → AI 解析の部品…」で入れられます）";
            }
            tagger = app.guiTagger;
        }
        if (tagger) {
            AnalyzerInput in;
            in.path = narrow(p);
            AnalyzerOutput out = tagger->analyze(in);
            if (!out.ok) r->error = L"画像を読めないか、タグを付けられませんでした";
            r->tags = std::move(out.tags);
        }
        imgwin::threadEnd();
        if (!PostMessageW(hwnd, WM_APP_QUERY_TAGS, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void onQueryTags(QueryTagsDone* p) {
    std::unique_ptr<QueryTagsDone> r(p);
    app.queryBusy = false;
    if (!r->error.empty()) {
        setQueryStatus(L"タグを生成できませんでした: " + r->error);
        return;
    }
    // 種類ごとに、確度の高い順（表示名で）。年齢区分は 1 つ
    const std::pair<const char*, const wchar_t*> kinds[] = {{"character", L"キャラ"}, {"copyright", L"作品"}, {"artist", L"作者"},
                                                            {"general", L"一般"}, {"rating", L"区分"}};
    std::wstring text;
    for (const auto& [kind, name] : kinds) {
        std::vector<const TagRow*> rows;
        for (const auto& t : r->tags)
            if (t.kind == kind) rows.push_back(&t);
        if (rows.empty()) continue;
        std::sort(rows.begin(), rows.end(), [](const TagRow* a, const TagRow* b) { return a->confidence > b->confidence; });
        std::wstring line = std::wstring(name) + L": ";
        for (size_t i = 0; i < rows.size(); ++i) {
            wchar_t c[16];
            swprintf(c, 16, L"（%.2f）", rows[i]->confidence);
            line += (i ? L"、" : L"") + tagLabel(rows[i]->tag) + c;
        }
        text += (text.empty() ? L"" : L"\r\n") + line;
    }
    setQueryStatus(text.empty() ? L"タグは見つかりませんでした" : text);
}

void queryRemove(const std::wstring& path) {
    DeleteFileW(path.c_str());
    auto it = app.queryThumbs.find(path);
    if (it != app.queryThumbs.end()) {
        DeleteObject(it->second);
        app.queryThumbs.erase(it);
    }
    if (app.querySel == path) app.querySel.clear();
    refreshQueryFiles();
}

// 一時に置いた画像の小さな絵（PBGRA の DIB）
HBITMAP queryThumb(const std::wstring& path, int side) {
    auto it = app.queryThumbs.find(path);
    if (it != app.queryThumbs.end()) return it->second;
    HBITMAP bmp = nullptr;
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (imgwin::decodeBgra(path, side, px, w, h) && w > 0 && h > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && bits) memcpy(bits, px.data(), px.size());
    }
    app.queryThumbs[path] = bmp;
    return bmp;
}

constexpr int kQueryCell = 76;  // 1 枚の幅（px(...) で広げる）

int queryHit(int x) {
    const int i = (x - px(4)) / px(kQueryCell);
    return x >= px(4) && i >= 0 && i < static_cast<int>(app.queryFiles.size()) ? i : -1;
}

LRESULT CALLBACK queryStripProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ oldFont = SelectObject(dc, app.font);
            if (app.queryFiles.empty()) {
                RECT tr = rc;
                InflateRect(&tr, -px(8), -px(4));
                SetTextColor(dc, RGB(110, 110, 110));
                DrawTextW(dc,
                          L"ここに画像をドロップ（エクスプローラー・ブラウザーから）、または「貼り付け」（Ctrl+Shift+V）。"
                          L"足した画像は 10 枚まで（1 日たつと消えます）",
                          -1, &tr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            }
            const int cell = px(kQueryCell), side = cell - px(12);
            for (size_t i = 0; i < app.queryFiles.size(); ++i) {
                const std::wstring& p = app.queryFiles[i];
                const int x = px(4) + static_cast<int>(i) * cell;
                RECT box = {x, px(2), x + cell - px(4), rc.bottom - px(2)};
                if (p == app.querySel) {
                    HBRUSH b = CreateSolidBrush(RGB(204, 228, 247));
                    FillRect(dc, &box, b);
                    DeleteObject(b);
                }
                if (HBITMAP bmp = queryThumb(p, side)) {
                    BITMAP bm{};
                    GetObject(bmp, sizeof bm, &bm);
                    HDC mem = CreateCompatibleDC(dc);
                    HGDIOBJ old = SelectObject(mem, bmp);
                    const int bx = x + (cell - px(4) - bm.bmWidth) / 2, by = px(4) + (side - bm.bmHeight) / 2;
                    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                    AlphaBlend(dc, bx, by, bm.bmWidth, bm.bmHeight, mem, 0, 0, bm.bmWidth, bm.bmHeight, bf);
                    SelectObject(mem, old);
                    DeleteDC(mem);
                }
                // 足した時刻
                const int64_t t = queryImageTime(narrow(p.substr(p.find_last_of(L'\\') + 1)));
                wchar_t label[16] = L"";
                if (t) {
                    const std::time_t tt = static_cast<std::time_t>(t);
                    std::tm lt{};
                    localtime_s(&lt, &tt);
                    swprintf(label, 16, L"%02d:%02d", lt.tm_hour, lt.tm_min);
                }
                RECT lr = {x, rc.bottom - px(16), x + cell - px(4), rc.bottom - px(2)};
                SetTextColor(dc, RGB(90, 90, 90));
                DrawTextW(dc, label, -1, &lr, DT_CENTER | DT_SINGLELINE);
            }
            SelectObject(dc, oldFont);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(h);
            const int i = queryHit(GET_X_LPARAM(lp));
            if (i >= 0) {
                app.querySel = app.queryFiles[i];
                drawQueryStripLater();
                if (ex.view) ex.view->PreviewFile(app.querySel);  // 右のプレビューに出す
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            const int i = queryHit(GET_X_LPARAM(lp));
            if (i >= 0) ShellExecuteW(app.hwnd, L"open", app.queryFiles[i].c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case WM_RBUTTONUP: {
            const int i = queryHit(GET_X_LPARAM(lp));
            if (i < 0) return 0;
            const std::wstring path = app.queryFiles[i];
            app.querySel = path;
            drawQueryStripLater();
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, CMD_QUERY_SAME, L"同じ画像を探す(&S)");
            AppendMenuW(menu, MF_STRING, CMD_QUERY_SIMILAR, L"似た画像を探す（AI）(&A)");
            AppendMenuW(menu, MF_STRING, CMD_QUERY_TAGS, L"タグを生成（AI）(&T)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 1, L"開く(&O)");
            AppendMenuW(menu, MF_STRING, 2, L"エクスプローラーで表示(&E)");
            AppendMenuW(menu, MF_STRING, 3, L"この画像を消す(&D)");
            POINT pt;
            GetCursorPos(&pt);
            const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) ShellExecuteW(app.hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else if (cmd == 2)
                ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            else if (cmd == 3) queryRemove(path);
            else if (cmd) SendMessageW(app.hwnd, WM_COMMAND, cmd, 0);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) queryPaste();
            else if (wp == VK_DELETE && !app.querySel.empty()) queryRemove(app.querySel);
            else if ((wp == VK_LEFT || wp == VK_RIGHT) && !app.queryFiles.empty()) {
                auto it = std::find(app.queryFiles.begin(), app.queryFiles.end(), app.querySel);
                int i = it == app.queryFiles.end() ? 0 : static_cast<int>(it - app.queryFiles.begin());
                i = std::clamp(i + (wp == VK_RIGHT ? 1 : -1), 0, static_cast<int>(app.queryFiles.size()) - 1);
                app.querySel = app.queryFiles[i];
                drawQueryStripLater();
                if (ex.view) ex.view->PreviewFile(app.querySel);
            }
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void createQueryControls() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = queryStripProc;
    wc.hInstance = app.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    wc.lpszClassName = L"ImgdupeQueryStrip";
    RegisterClassExW(&wc);
    app.queryStrip = CreateWindowExW(WS_EX_CLIENTEDGE, L"ImgdupeQueryStrip", nullptr, WS_CHILD | WS_TABSTOP, 0, 0, 10, 10, app.hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QUERY_STRIP)), app.inst, nullptr);
    app.queryTags = makeControl(WC_EDITW, L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, IDC_QUERY_TAGS, WS_EX_CLIENTEDGE);
    ShowWindow(app.queryTags, SW_HIDE);
    // ブラウザー・エクスプローラーからのドロップ（ファイルか URL）
    for (HWND target : {app.queryStrip, app.queryTags, app.queryBar}) {
        IDropTarget* drop = MakeFileDropTarget([](const std::vector<std::wstring>& files, const std::wstring& url) { addQueryInputs(files, url); });
        RegisterDragDrop(target, drop);
        drop->Release();
    }
    HWND tip = makeTooltip(app.hwnd, px(380));
    addTooltip(tip, app.queryStrip,
               L"足した画像（新しい順）。クリックで選び、右クリックで探す・消す、ダブルクリックで開きます。"
               L"画像をここにドロップしても足せます（エクスプローラー・ブラウザーから）");
    addTooltip(tip, app.queryTags, L"生成したタグや、今の様子");
    refreshQueryFiles();
    setQueryStatus(L"画像を足して（貼り付け・ドロップ・ファイル・URL）、左のボタンで、同じ画像・似た画像を探したり、タグを生成したりします");
}

struct ToolButton {
    int id;
    wchar_t glyph;         // Segoe Fluent Icons / Segoe MDL2 Assets の文字
    const wchar_t* text;   // フォントが無い時の代わり
    const wchar_t* tip;    // ツールチップ
    bool enabled;
};
// リボン（ホーム）のボタン。重複を探すリボンのボタンは kDupButtons
const ToolButton kToolButtons[] = {
    {CMD_OPEN, 0xE8E5, L"開", L"開く", false},
    {CMD_EXPLORER, 0xE838, L"場", L"場所を表示", false},
    {CMD_COPY_PATH, 0xE8C8, L"複", L"パスをコピー", false},
    {CMD_REFRESH, 0xE72C, L"新", L"更新", true},
    {0, 0, nullptr, nullptr, false},
    {CMD_FIND_SAME, 0xE71E, L"同", L"同じ画像を探す", true},
    {0, 0, nullptr, nullptr, false},
    {CMD_STATS, 0xE9D2, L"統", L"統計", true},
    {CMD_SETTINGS, 0xE713, L"設", L"設定", true},
    {CMD_KEY_SETTINGS, 0xE765, L"鍵", L"キー", true},
    {CMD_CHECK_UPDATE, 0xE895, L"更", L"アップデート", true},
    {CMD_HELP, 0xE897, L"?", L"使い方", true},
};
// 「画像で探す」のリボン
const ToolButton kQueryButtons[] = {
    {CMD_QUERY_PASTE, 0xE77F, L"貼", L"貼り付け", true},
    {CMD_QUERY_OPEN, 0xE8E5, L"開", L"ファイル", true},
    {CMD_QUERY_URL, 0xE71B, L"U", L"URL", true},
    {0, 0, nullptr, nullptr, false},
    {CMD_QUERY_SAME, 0xE71E, L"同", L"同じ画像", true},
    {CMD_QUERY_SIMILAR, 0xE8B9, L"似", L"似た画像（AI）", true},
    {CMD_QUERY_TAGS, 0xE8EC, L"タ", L"タグを生成（AI）", true},
};
const ToolButton kDupButtons[] = {
    {CMD_SEARCH, 0xE721, L"検", L"検索開始", true},
    {CMD_QUERY, 0xE8B9, L"1", L"1 枚で検索", true},
    {CMD_EXPORT, 0xE74E, L"保", L"結果を保存", false},
};

// リボンのボタンの並び（大きな絵の下に名前。Massiguraedit と同じ記号フォントの絵）
template <size_t N>
HWND createRibbonBar(const ToolButton (&defs)[N], int id) {
    HWND bar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                               WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER,
                               0, 0, 0, 0, app.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.inst, nullptr);
    SendMessageW(bar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    const int size = px(28);
    const wchar_t* face = fontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons"
                          : fontExists(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets"
                                                              : nullptr;
    HIMAGELIST images = ImageList_Create(size, size, ILC_COLOR32, 0, 16);
    auto add = [&](wchar_t glyph, const std::wstring& text) {
        HBITMAP bmp = (face && glyph) ? makeGlyphIcon(size, face, std::wstring(1, glyph), 0.8, RGB(40, 40, 40))
                                      : makeGlyphIcon(size, L"MS UI Gothic", text, text.size() > 1 ? 0.5 : 0.75, RGB(40, 40, 40));
        const int n = ImageList_Add(images, bmp, nullptr);
        DeleteObject(bmp);
        return n;
    };
    std::vector<TBBUTTON> buttons;
    for (const ToolButton& d : defs) {
        TBBUTTON b{};
        if (!d.id) {
            b.fsStyle = BTNS_SEP;
        } else {
            b.iBitmap = add(d.glyph, d.text);
            if (d.id == CMD_SEARCH) startImage = b.iBitmap;
            b.idCommand = d.id;
            b.fsState = d.enabled ? TBSTATE_ENABLED : 0;
            b.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
            b.iString = reinterpret_cast<INT_PTR>(d.tip);  // 絵の下に出す
        }
        buttons.push_back(b);
    }
    if (id == IDC_DUPBAR) stopImage = add(0xE71A, L"止");
    SendMessageW(bar, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images));
    SendMessageW(bar, TB_SETBUTTONSIZE, 0, MAKELPARAM(px(64), px(58)));
    SendMessageW(bar, TB_ADDBUTTONSW, buttons.size(), reinterpret_cast<LPARAM>(buttons.data()));
    SendMessageW(bar, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
    return bar;
}

void createToolbar() {
    app.toolbar = createRibbonBar(kToolButtons, IDC_TOOLBAR);
    app.dupBar = createRibbonBar(kDupButtons, IDC_DUPBAR);
    app.queryBar = createRibbonBar(kQueryButtons, IDC_QUERYBAR);
}

void createSearchControls();
void createQueryControls();

void createControls() {
    app.tabs = makeControl(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS | TCS_FOCUSNEVER, IDC_TABS);
    createToolbar();
    for (const wchar_t* t : {L"ホーム", L"重複を探す", L"検索", L"画像で探す"}) {
        TCITEMW it{};
        it.mask = TCIF_TEXT;
        it.pszText = const_cast<wchar_t*>(t);
        TabCtrl_InsertItem(app.tabs, TabCtrl_GetItemCount(app.tabs), &it);
    }
    ex.view = std::make_unique<Explorer>();
    ex.view->Create(app.hwnd, &ex, loadExplorerSettings());
    app.ribbonLine = makeControl(WC_STATICW, L"", SS_ETCHEDHORZ, IDC_RIBBON_LINE);
    app.foldersLabel = makeControl(WC_STATICW, L"調べるフォルダ（インデックスも作ります）", SS_NOPREFIX, IDC_FOLDERS_LABEL);
    app.folders = makeControl(WC_LISTBOXW, L"", WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT,
                              IDC_FOLDERS, WS_EX_CLIENTEDGE);
    app.add = makeControl(WC_BUTTONW, L"フォルダを追加…", WS_TABSTOP, CMD_ADD_FOLDER);
    app.remove = makeControl(WC_BUTTONW, L"選んだフォルダを外す", WS_TABSTOP, CMD_REMOVE_FOLDER);
    ShowWindow(app.add, SW_HIDE);  // リボンのボタンを使う
    ShowWindow(app.remove, SW_HIDE);
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
    app.status = makeControl(WC_STATICW, L"「検索開始」で、調べるフォルダの中の重複を探します。", SS_ENDELLIPSIS | SS_NOPREFIX,
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

    app.info = makeControl(WC_STATICW, L"", SS_NOPREFIX, IDC_INFO);
    // 結果はエクスプローラーの一覧に出すので、前の画面の一覧と情報欄は使わない
    for (HWND h : {app.groups, app.thumbs, app.info}) ShowWindow(h, SW_HIDE);
    // 調べるフォルダの一覧は見せない（中身の入れ物としてだけ使う。足す・外すはホームとファイルのメニュー）
    for (HWND h : {app.foldersLabel, app.folders}) ShowWindow(h, SW_HIDE);
    app.dupPage = {app.dupBar, app.thresholdCaption, app.threshold, app.thresholdLabel,
                   app.exact, app.flat, app.variants, app.progress, app.status};
    createSearchControls();
    createQueryControls();
    app.advancedShown = iniInt(L"advancedSearch", 0) != 0;
    SetWindowTextW(app.s.advanced, app.advancedShown ? L"詳細検索 ▲" : L"詳細検索 ▼");
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

// リボンのボタンにマウスを乗せた時の説明
const wchar_t* commandHelp(int id) {
    switch (id) {
        case CMD_OPEN: return L"選んだ画像・ファイルを、関連付けられたアプリで開きます（Enter）";
        case CMD_EXPLORER: return L"選んだものを Windows のエクスプローラーで表示します（Ctrl+E）";
        case CMD_COPY_PATH: return L"選んだもののパスをクリップボードにコピーします（Ctrl+C）";
        case CMD_COMPACT_DB: return L"DB（imgdupe.db）の使っていない所を詰めて、ファイルを小さくします（バックグラウンドの解析が行います）";
        case CMD_REFRESH: return L"画面の控え（フォルダの中身・サムネイル・画像の情報）を捨てて、今の場所を読み直します。移動したファイルが残っている時など（Ctrl+F5）。"
                                 L"バックグラウンドの解析はしません（要る時はタスクトレイの解析から）";
        case CMD_FIND_SAME: return L"選んだ画像と同じ画像（完全一致・見た目一致）を、今いるフォルダに関係なく調べた全部のフォルダから探して一覧に出します（Ctrl+M）";
        case CMD_STATS: return L"タグ・作者・形式などの数を表とグラフで見ます";
        case CMD_INFER_RULES: return L"フォルダ名から作者・アカウントを読み取るために教えた形の一覧（教えるのはフォルダの右クリックから）";
        case CMD_SETTINGS: return L"AI 解析・処理の負荷・タグの表示などの設定を変えます";
        case CMD_KEY_SETTINGS: return L"ショートカットキーの割り当てを変えます（Ctrl+K）";
        case CMD_CHECK_UPDATE: return L"新しい版が出ていないか確かめ、あれば更新します";
        case CMD_HELP: return L"使い方の説明を出します（F1）";
        case CMD_OPEN_LOGS: return L"動作履歴（ログ）のフォルダを開きます。おかしな動きをした時は、ここの .log を見せてください";
        case CMD_SEARCH: return L"調べるフォルダの中から、同じ画像のまとまりを探します。探している間に押すと中止します（F5）";
        case CMD_QUERY: return L"画像を 1 枚選んで、それと同じ・似た画像を探します（Ctrl+F）";
        case CMD_EXPORT: return L"重複の結果を CSV と、サムネイル付きの HTML に書き出します（Ctrl+S）";
        case CMD_QUERY_PASTE:
            return L"クリップボードの画像（スクリーンショット・ブラウザーでコピーした画像）、コピーした画像のファイル、画像の URL を足します（Ctrl+Shift+V）";
        case CMD_QUERY_OPEN: return L"画像のファイルを選んで足します";
        case CMD_QUERY_URL: return L"画像の URL を入れて、ダウンロードして足します";
        case CMD_QUERY_SAME: return L"選んだ画像と同じ画像（完全一致・見た目一致）を、調べた全部のフォルダから探してエクスプローラーの一覧に出します";
        case CMD_QUERY_SIMILAR:
            return L"選んだ画像に似た画像を AI（CLIP）で探して、エクスプローラーに似ている順に出します（検索のリボンの条件も使います）";
        case CMD_QUERY_TAGS: return L"選んだ画像に AI（WD14 タガー）でタグを付けて、右に出します（ファイルにも DB にも書きません）";
    }
    return nullptr;
}

LRESULT onNotify(NMHDR* nm) {
    if (onSearchNotify(nm)) return 0;
    if ((nm->hwndFrom == app.toolbar || nm->hwndFrom == app.dupBar || nm->hwndFrom == app.queryBar) && nm->code == TBN_GETINFOTIPW) {
        auto* it = reinterpret_cast<NMTBGETINFOTIPW*>(nm);
        if (const wchar_t* help = commandHelp(it->iItem); help && it->pszText && it->cchTextMax > 0)
            lstrcpynW(it->pszText, help, it->cchTextMax);
        return 0;
    }
    if (nm->code == TTN_GETDISPINFOW &&
        (nm->hwndFrom == reinterpret_cast<HWND>(SendMessageW(app.toolbar, TB_GETTOOLTIPS, 0, 0)) ||
         nm->hwndFrom == reinterpret_cast<HWND>(SendMessageW(app.dupBar, TB_GETTOOLTIPS, 0, 0)) ||
         nm->hwndFrom == reinterpret_cast<HWND>(SendMessageW(app.queryBar, TB_GETTOOLTIPS, 0, 0)))) {
        auto* di = reinterpret_cast<NMTTDISPINFOW*>(nm);
        SendMessageW(nm->hwndFrom, TTM_SETMAXTIPWIDTH, 0, px(360));
        if (const wchar_t* help = commandHelp(static_cast<int>(nm->idFrom))) di->lpszText = const_cast<wchar_t*>(help);
        return 0;
    }
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
    popup(L"ファイル(&F)", {CMD_SCAN_FOLDER, CMD_SCAN_FOLDER_TOP, CMD_SCAN_TASKS, 0, CMD_ADD_FOLDER, CMD_REMOVE_FOLDER, 0, CMD_EXPORT, 0, CMD_EXIT});
    popup(L"表示(&V)", {CMD_REFRESH, 0, CMD_TAB_EXPLORER, CMD_TAB_DUPES, CMD_TAB_SEARCH, CMD_TAB_QUERY, 0, CMD_SAVE_SMART, CMD_MANAGE_QUERIES, CMD_DELETE_SMART, 0, CMD_EXPAND_LINKS, CMD_LINKS});
    popup(L"検索(&S)", {CMD_SEARCH, CMD_QUERY, 0, CMD_TOGGLE_EXACT, CMD_TOGGLE_FLAT, 0, CMD_THRESHOLD_DOWN, CMD_THRESHOLD_UP});
    popup(L"画像(&I)", {CMD_OPEN, CMD_EXPLORER, CMD_COPY_PATH, 0, CMD_FIND_SAME, 0, CMD_SAVE_SELECTION, 0, CMD_QUERY_PASTE, CMD_QUERY_OPEN,
                        CMD_QUERY_URL, CMD_QUERY_SAME, CMD_QUERY_SIMILAR, CMD_QUERY_TAGS});
    popup(L"タグ(&T)", {CMD_REVIEW, 0, CMD_TAG_YES, CMD_TAG_NO, 0, CMD_BULK_TAG, 0, CMD_UNDO, 0, CMD_TAG_JAPANESE});
    popup(L"その他(&O)", {CMD_SETTINGS, CMD_KEY_SETTINGS, CMD_DICTIONARY, CMD_TAGDICT, CMD_REFS, CMD_INFER_RULES, 0, CMD_STATS, CMD_COMPACT_DB, 0, CMD_INDEXER_TOGGLE, CMD_INDEXER_AUTOSTART, 0, CMD_CHECK_UPDATE, CMD_MODELS, CMD_RELEASES});
    popup(L"ヘルプ(&H)", {CMD_HELP, CMD_OPEN_LOGS, 0, CMD_ABOUT});
}

void syncMenuChecks() {
    if (!app.menuBar) return;
    CheckMenuItem(app.menuBar, CMD_TOGGLE_EXACT,
                  MF_BYCOMMAND | (Button_GetCheck(app.exact) == BST_CHECKED ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(app.menuBar, CMD_TOGGLE_FLAT,
                  MF_BYCOMMAND | (Button_GetCheck(app.flat) == BST_CHECKED ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(app.menuBar, CMD_TAG_JAPANESE, MF_BYCOMMAND | (app.tagJapanese ? MF_CHECKED : MF_UNCHECKED));
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
    if (msg.message == WM_KEYDOWN && vk == VK_RETURN && mods == 0 && (msg.hwnd == app.s.free || msg.hwnd == app.s.clip)) {
        runSearch();  // フリーワード・内容の欄で Enter
        return true;
    }
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
    // エクスプローラーの中の移動・選ぶキー（矢印・Enter・BackSpace など）はエクスプローラーに渡す
    if (ex.view && (msg.hwnd == ex.view->Window() || IsChild(ex.view->Window(), msg.hwnd))) {
        static const UINT nav[] = {VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_PRIOR, VK_NEXT, VK_HOME, VK_END, VK_RETURN, VK_BACK, VK_TAB, VK_F5, VK_ESCAPE};
        if (mods == 0 && std::find(std::begin(nav), std::end(nav), vk) != std::end(nav)) return false;
        if (mods == KMOD_SHIFT && vk == VK_TAB) return false;
        if (mods == KMOD_CTRL && (vk == 'V' || vk == 'L' || vk == VK_ADD || vk == VK_SUBTRACT || vk == VK_OEM_PLUS || vk == VK_OEM_MINUS)) return false;
        if (mods == KMOD_ALT && (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == 'D')) return false;
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
            if (app.tab == 3) {  // 画像で探す: 落とした画像を足す
                std::vector<std::wstring> files;
                const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i) {
                    wchar_t buf[MAX_PATH * 4];
                    if (DragQueryFileW(drop, i, buf, static_cast<UINT>(std::size(buf)))) files.emplace_back(buf);
                }
                addQueryInputs(files, L"");
            } else {
                // ウィンドウに落としたフォルダ・画像はスキャンタスクに（1 回の操作で 1 つ）
                std::vector<std::wstring> paths;
                const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i) {
                    wchar_t buf[MAX_PATH * 4];
                    if (DragQueryFileW(drop, i, buf, static_cast<UINT>(std::size(buf)))) paths.emplace_back(buf);
                }
                addScanTask(paths, false);
                if (ex.view) ex.view->RefreshStates();
            }
            DragFinish(drop);
            return 0;
        }
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lp) == app.s.conf) {
                wchar_t v[16];
                swprintf(v, 16, L"%.2f", SendMessageW(app.s.conf, TBM_GETPOS, 0, 0) / 100.0);
                setText(app.s.confValue, v);
                advancedChanged();
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
                        for (const auto& f : pickFolders(L"調べるフォルダを選ぶ")) addFolder(f);
                        saveSettings();
                        if (ex.view) ex.view->RefreshStates();
                    }
                    break;
                case CMD_REMOVE_FOLDER:
                    if (!app.running) removeRootMenu();
                    break;
                case CMD_SCAN_FOLDER:
                case CMD_SCAN_FOLDER_TOP: {
                    const bool top = LOWORD(wp) == CMD_SCAN_FOLDER_TOP;
                    const auto dirs = pickFolders(top ? L"最優先でスキャンするフォルダを選ぶ" : L"スキャンタスクに追加するフォルダを選ぶ");
                    if (!dirs.empty()) addScanTask(dirs, top);
                    if (ex.view) ex.view->RefreshStates();
                    break;
                }
                case CMD_SCAN_TASKS:
                    if (!indexer::isRunning()) indexer::start();
                    indexer::requestShowTasks(app.dataDir);
                    break;
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
                        logBox(hwnd, L"検索が終わってから確認してください。", L"imgdupe", MB_ICONINFORMATION);
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
                case CMD_TAB_EXPLORER: switchTab(2); break;
                case CMD_FIND_SAME: {
                    const std::wstring p = selectedPath();
                    if (!p.empty() && imgdupe::isImageExt(narrow(p))) ex.ExplorerFindSameFile(p);
                    break;
                }
                case CMD_TAB_SEARCH: switchTab(1); break;
                case CMD_TAB_QUERY: switchTab(3); break;
                case CMD_QUERY_PASTE:
                    if (app.tab != 3) switchTab(3);
                    queryPaste();
                    break;
                case CMD_QUERY_OPEN: {
                    if (app.tab != 3) switchTab(3);
                    const std::wstring f = pickImage();
                    if (!f.empty()) addQueryInputs({f}, L"");
                    break;
                }
                case CMD_QUERY_URL: {
                    if (app.tab != 3) switchTab(3);
                    const auto url = promptText(L"画像の URL", L"画像の URL（https://…）", L"");
                    if (url && isUrlText(trimmed(*url))) addQueryInputs({}, trimmed(*url));
                    else if (url && !trimmed(*url).empty()) setQueryStatus(L"http:// か https:// で始まる URL を入れてください");
                    break;
                }
                case CMD_QUERY_SAME: querySame(); break;
                case CMD_QUERY_SIMILAR: querySimilar(); break;
                case CMD_QUERY_TAGS: queryTags(); break;
                case CMD_TAG_JAPANESE: setTagJapanese(!app.tagJapanese); break;
                case CMD_SAVE_SELECTION: saveSelectionSmartUi(); break;
                case CMD_SAVE_SMART:
                    if (app.tab != 1) switchTab(1);
                    saveSmartFolderUi();
                    break;
                case CMD_DELETE_SMART:
                    if (app.tab != 1) switchTab(1);
                    deleteSmartFolderUi();
                    break;
                case CMD_MANAGE_QUERIES:
                    if (app.tab != 1) switchTab(1);
                    manageSmartFoldersUi();
                    break;
                case CMD_EXPAND_LINKS:
                    if (app.tab != 1) switchTab(1);
                    expandLinksUi();
                    break;
                case CMD_LINKS: showLinksWindow(); break;
                case CMD_STATS: showStatsWindow(); break;
                case CMD_REFRESH: refreshView(); break;
                case CMD_COMPACT_DB: compactDbUi(); break;
                case CMD_INFER_RULES: teachFolderUi(L""); break;
                case CMD_TAG_YES:
                case CMD_TAG_NO:
                    markSelected(LOWORD(wp) == CMD_TAG_YES);
                    break;
                case CMD_BULK_TAG:
                    bulkTagUi();
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
                case CMD_DICTIONARY: showDictionaryWindow(); break;
                case CMD_TAGDICT: showTagDictWindow(); break;
                case CMD_REFS: {
                    // 無ければ作り、使い方を書いたファイルを置いて開く
                    const std::wstring dir = clip::refsDir(app.dataDir);
                    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
                        CreateDirectoryW(dir.c_str(), nullptr);
                        if (FILE* f = _wfopen((dir + L"\\使い方.txt").c_str(), L"wb")) {
                            const std::string t =
                                "見本画像のフォルダ\r\n\r\n"
                                "ここに「キャラ名」や「作者名」のフォルダを作り、その人の画像を 10〜30 枚入れてください。\r\n"
                                "バックグラウンドの解析が見本の CLIP のベクトルを作り、それに近い画像に、フォルダの名前のタグを付けます\r\n"
                                "（出どころ「見本画像との類似」）。新しいキャラも、見本を足すだけで探せるようになります。\r\n"
                                "確認キューで「正しい」と確定した画像（同じタグに 3 枚以上）も、自動で見本になります。\r\n";
                            fwrite(t.data(), 1, t.size(), f);
                            fclose(f);
                        }
                        saveSettings();  // バックグラウンドの解析に新しいフォルダを読ませる
                        if (indexer::isRunning()) indexer::requestRescan(app.dataDir);
                    }
                    ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                }
                case CMD_HELP: showHelp(); break;
                case CMD_OPEN_LOGS: {
                    const std::wstring dir = app.dataDir + L"\\logs";
                    CreateDirectoryW(dir.c_str(), nullptr);
                    ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                }
                case CMD_RELEASES: ShellExecuteW(hwnd, L"open", APP_RELEASES_URL, nullptr, nullptr, SW_SHOWNORMAL); break;
                case CMD_ABOUT:
                    logBox(hwnd,
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
            if (wp == kSameTimer) {
                KillTimer(hwnd, kSameTimer);
                if (!ex.pendingSel.empty()) ex.ExplorerFindSameFile(ex.pendingSel);
            }
            if (wp == kExitTimer) DestroyWindow(hwnd);  // 確認に時間がかかりすぎたら、そのまま終わる
            return 0;
        case WM_APP_QUERY_FILE:
            onQueryFetched(reinterpret_cast<QueryFetched*>(lp));
            return 0;
        case WM_APP_QUERY_VEC:
            onQueryVector(reinterpret_cast<QueryVector*>(lp));
            return 0;
        case WM_APP_QUERY_TAGS:
            onQueryTags(reinterpret_cast<QueryTagsDone*>(lp));
            return 0;
        case WM_APP_CONVERT: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
            if (wp == 1) {
                suggestCompact();
                return 0;
            }
            if (!ex.view) return 0;
            if (text->empty()) {
                app.sourcesStale = true;
                PostMessageW(hwnd, WM_APP_CONVERT, 1, reinterpret_cast<LPARAM>(new std::wstring(L"?")));  // 詰めるのをすすめる
                ex.view->SetSameStatus(L"DB を新しい形に変換しました（検索・統計が速くなります。「その他 → DB を詰めて小さくする」でファイルも小さくできます）");
            } else {
                ex.view->SetSameStatus(*text);
            }
            return 0;
        }
        case WM_APP_SOURCES: {
            std::unique_ptr<std::vector<std::pair<std::string, int64_t>>> counts(
                reinterpret_cast<std::vector<std::pair<std::string, int64_t>>*>(lp));
            app.sourceCounts = std::move(*counts);
            app.sourceCountsAt = GetTickCount64();
            app.countingSources = false;
            fillSources();
            if (app.sourcesStale) refreshSources(true);
            return 0;
        }
        case WM_APP_SEARCHED:
            onSearched(reinterpret_cast<SearchDone*>(lp));
            return 0;
        case WM_SAME_DONE:
            if (wp == 1) {
                // URL の画像を落とし終えた: "番号\n一時ファイル\n見出し"
                std::unique_ptr<std::wstring> p(reinterpret_cast<std::wstring*>(lp));
                const size_t a = p->find(L'\n'), b = p->find(L'\n', a + 1);
                const std::wstring tmp = p->substr(a + 1, b - a - 1);
                if (static_cast<unsigned>(_wtoi(p->c_str())) != ex.gen) DeleteFileW(tmp.c_str());  // もう別の画像を探している
                else findSameAsync(tmp, p->substr(b + 1), false, true);
            } else {
                onSameDone(reinterpret_cast<SameDone*>(lp));
            }
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
                logBox(hwnd, L"アップデートの処理中です。少し待ってから閉じてください。", L"imgdupe", MB_ICONINFORMATION);
                return 0;
            }
            if (app.modelsDownloading) {
                if (logBox(hwnd, L"AI 解析の部品をダウンロードしています。中止して終了しますか？\r\n（入れ終わった部品はそのまま使えます）",
                                L"imgdupe", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    app.closeAfterModels = true;
                    app.modelsCancel = true;
                }
                return 0;
            }
            if (app.running &&
                logBox(hwnd, L"検索中です。中止して終了しますか？\r\n（読み込んだ分はキャッシュに残します）", L"imgdupe",
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
    cb.lookup = [](Db& db, const std::atomic<bool>& cancel, Progress* p) { lookup::run(app.dataDir, db, cancel, p); };
    cb.merge = [](Db& db) {
        clip::applyDictionary(app.dataDir, db, false);
        clip::applyReferences(app.dataDir, db, false);
        clip::applyTagParents(db, false);  // 辞書や見本で付いたタグの親も
        clip::applyAesthetic(app.dataDir, db);
    };
    cb.qualityEnabled = [] { return clip::qualityEnabled(app.dataDir); };
    cb.decodeQuality = [](const std::string& path, int maxSide, std::vector<uint8_t>& bgra, int& w, int& h, int& ow, int& oh) {
        return imgwin::decodeBgra(widen(path), maxSide, bgra, w, h, &ow, &oh);
    };
    return cb;
}

// アンインストール（「アプリと機能」から imgdupe.exe --uninstall で呼ばれる）
int runUninstall() {
    const wchar_t* title = L"imgdupe のアンインストール";
    if (logBox(nullptr, L"imgdupe をアンインストールしますか？\r\n（画像は消えません）", title, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 1;
    const int data = logBox(nullptr,
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
    logBox(nullptr, L"imgdupe をアンインストールしました。", title, MB_ICONINFORMATION);
    // 動いている exe は自分では消せないので、終わってから cmd に消してもらう（消えるまで 1 秒ごとに最大 20 回。フォルダは空の時だけ消える）
    const std::wstring exe = upd::exePath(), dir = exeDir();
    std::wstring cmd = L"cmd.exe /c for /l %i in (1,1,20) do (if exist \"" + exe + L"\" (ping -n 2 127.0.0.1 >nul & del /f /q \"" +
                       exe + L"\" \"" + exe + L".old*\" 2>nul)) & rmdir \"" + dir + L"\" 2>nul";
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
    if (!opt.roots.empty() && GetFileAttributesW(clip::refsDir(app.dataDir).c_str()) != INVALID_FILE_ATTRIBUTES &&
        std::find(opt.roots.begin(), opt.roots.end(), narrow(clip::refsDir(app.dataDir))) == opt.roots.end())
        opt.roots.push_back(narrow(clip::refsDir(app.dataDir)));  // 見本画像のフォルダも読む
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

// 前の版の DB のタグの変換（バックグラウンドの解析が動いていない時だけ、画面の側で少しずつ。解析が動き出したら任せる）
struct TagConversionRunner {
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<sqlite3*> handle{nullptr};  // 終わる時に止める（sqlite3_interrupt）
} tagConv;

void startTagConversion() {
    {
        Db* db = uiDb();
        if (!db) return;
        try {
            if (!tagConversionState(*db).pending && !tagIndexUpgradePending(*db)) return;
        } catch (const std::exception&) {
            return;
        }
    }
    HWND hwnd = app.hwnd;
    const std::string path = narrow(dbPath());
    tagConv.thread = std::thread([hwnd, path] {
        auto post = [hwnd](const std::wstring& text) {
            auto* t = new std::wstring(text);
            if (!PostMessageW(hwnd, WM_APP_CONVERT, 0, reinterpret_cast<LPARAM>(t))) delete t;
        };
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        Db db;
        if (!db.open(path)) return;
        tagConv.handle = db.handle();
        logs::info("タグの置き場所の変換を、画面の側で始める（バックグラウンドの解析が動いていないので）");
        std::string step;
        bool finished = false;
        while (!tagConv.stop) {
            if (indexer::isRunning()) {  // 解析が動いている: そちらに任せる
                post(L"DB の変換は、バックグラウンドの解析が続けます");
                break;
            }
            try {
                if (!tagConversionState(db).pending) {
                    post(L"タグの索引を作り直しています（初回だけ。数分かかります。使いながら待てます。終わると検索が速くなります）");
                    upgradeTagIndex(db);
                    finished = true;
                } else {
                    finished = convertTagsStep(db, 5000, &step);
                }
            } catch (const std::exception& e) {
                if (!tagConv.stop) logs::warn(std::string("タグの変換を中断した（次に起動した時に続きから）: ") + e.what());
                break;
            }
            if (finished) break;
            const TagConversionState st = tagConversionState(db);
            const int pct = st.total ? static_cast<int>(st.done * 100 / st.total) : 100;
            post(L"DB を新しい形に変換しています " + std::to_wstring(std::min(pct, 99)) + L"%（初回だけ。" + widen(step) +
                 L"。使いながら待てます。終わると検索・統計が速くなります）");
        }
        tagConv.handle = nullptr;
        if (finished) {
            logs::info("タグの置き場所を新しい形にした（画面の側で）");
            post(L"");
        }
    });
}

void stopTagConversion() {
    tagConv.stop = true;
    if (sqlite3* h = tagConv.handle.load()) sqlite3_interrupt(h);  // 索引を作っている途中でもすぐ止める（続きは次に）
    if (tagConv.thread.joinable()) tagConv.thread.join();
}

// タブの見出しの上でホイールを回したら、タブを移る（上へ回すと左、下へ回すと右。端で止まる）。
// タブの下のリボンの部分では移らない。移ったら true
bool wheelOnTabs(POINT screen, int delta) {
    if (!app.tabs || !delta || !IsWindowVisible(app.tabs)) return false;
    const HWND under = WindowFromPoint(screen);
    if (under != app.tabs && under != app.hwnd) return false;  // 見出しの右の空いた所は親の窓のこともある
    POINT pt = screen;
    ScreenToClient(app.tabs, &pt);
    RECT client;
    GetClientRect(app.tabs, &client);
    // 見出しの帯（どの見出しの四角とも同じ高さ。見出しの右の空いた所も含める）
    RECT item{};
    if (!TabCtrl_GetItemRect(app.tabs, 0, &item)) return false;
    if (pt.x < client.left || pt.y < 0 || pt.y >= item.bottom) return false;  // 右は窓の端まで（見出しの右の空いた所も）
    static int rest = 0;  // 細かく回すマウス（高精度のホイール）の分を貯める
    rest += delta;
    if (std::abs(rest) < WHEEL_DELTA) return true;
    const int steps = rest / WHEEL_DELTA;
    rest -= steps * WHEEL_DELTA;
    const int count = static_cast<int>(std::size(kTabOrder));
    const int pos = std::clamp(tabPos(app.tab) - steps, 0, count - 1);
    if (kTabOrder[pos] != app.tab) switchTab(kTabOrder[pos]);
    return true;
}

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
    logwin::start(app.dataDir, "imgdupe", "画面");
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
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                px(1180), px(780), nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        logs::error("ウィンドウを作れなかった（" + std::to_string(GetLastError()) + "）");
        logwin::stop();
        return 1;
    }

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
    loadTagLabels();
    syncMenuChecks();
    switchTab(std::clamp(iniInt(L"tab", 2), 0, 3));
    UpdateWindow(hwnd);
    if (autoStart) startSearch(L"");
    // 前の版の DB なら、タグの置き場所を変換する（解析が動いていればそちらで）。変換が済んでいれば、一度だけ詰めるのをすすめる
    suggestCompact();
    if (!indexer::isRunning()) startTagConversion();
    else if (Db* db = uiDb()) {
        try {
            if (tagConversionState(*db).pending)
                ex.view->SetSameStatus(L"DB を新しい形に変換しています（初回だけ。バックグラウンドの解析が行います。進み具合はタスクトレイの窓に出ます）");
        } catch (const std::exception&) {
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // ショートカットキー（ダイアログの Enter・Tab などより先に見る）
        if (handleShortcut(msg)) continue;
        // ホイールは、カーソルの下の日付の欄に送る（Windows の設定によってはフォーカスのある部品に行くので）
        if (msg.message == WM_MOUSEWHEEL) {
            POINT pt{GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam)};
            if (wheelOnTabs(pt, GET_WHEEL_DELTA_WPARAM(msg.wParam))) continue;
            HWND under = WindowFromPoint(pt);
            if (under && (under == app.s.from || under == app.s.to) && under != msg.hwnd) {
                SendMessageW(under, WM_MOUSEWHEEL, msg.wParam, msg.lParam);
                continue;
            }
        }
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    stopTagConversion();
    logwin::stop();
    CoUninitialize();
    return 0;
}
