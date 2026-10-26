// インストーラー（imgdupe_setup.exe）。imgdupe.exe を中に持ち（リソース 101）、ユーザーごとの場所に入れて、
// 選んだ AI の部品（ONNX Runtime・モデル）をダウンロードする。管理者権限は要らない。
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "install_win.hpp"
#include "models_win.hpp"
#include "resource.h"
#include "version.hpp"
#include "win_util.hpp"

using winutil::Com;
using winutil::narrow;
using winutil::widen;

namespace {

constexpr int kPayloadId = 101;
constexpr UINT WM_APP_MANIFEST = WM_APP + 1;  // lParam: models::ManifestResult*
constexpr UINT WM_APP_PROGRESS = WM_APP + 2;  // wParam: 0〜1000、lParam: std::wstring*
constexpr UINT WM_APP_FINISHED = WM_APP + 3;  // lParam: Finished*

enum { IDC_DIR = 100, IDC_BROWSE, IDC_COMPONENTS, IDC_STARTMENU, IDC_DESKTOP, IDC_PROGRESS, IDC_STATUS, IDC_LAUNCH };

struct Finished {
    bool appOk = false;
    std::wstring error;        // 本体を入れられなかった理由
    std::wstring modelError;   // モデルを入れられなかった理由（本体は入っている）
};

struct Setup {
    HINSTANCE inst = nullptr;
    HWND wnd = nullptr, dir = nullptr, browse = nullptr, list = nullptr, startMenu = nullptr, desktop = nullptr,
         progress = nullptr, status = nullptr, launch = nullptr, ok = nullptr, cancel = nullptr;
    HFONT font = nullptr, bold = nullptr;
    double scale = 1;
    bool manifestOk = false;
    imgdupe::Manifest manifest;
    std::vector<std::string> choices;  // 一覧の項目 → 部品の ID
    std::thread worker;
    std::atomic<bool> cancelled{false};
    bool running = false, done = false;
    std::wstring installedExe;
} g;

int px(int v) { return static_cast<int>(v * g.scale + 0.5); }

std::wstring version() { return widen(IMGDUPE_VERSION); }

std::wstring windowText(HWND h) {
    std::wstring t(static_cast<size_t>(GetWindowTextLengthW(h)), L'\0');
    if (!t.empty()) GetWindowTextW(h, &t[0], static_cast<int>(t.size()) + 1);
    return t;
}

HWND child(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), g.wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return c;
}

// 部品のうち、ほかの部品から「要る」と言われるだけのもの（ONNX Runtime）は一覧に出さない
bool isDependencyOnly(const std::string& id) {
    for (const auto& c : g.manifest.components)
        for (const auto& r : c.requires_)
            if (r == id) return true;
    return false;
}

uint64_t sizeWithRequires(const std::string& id) {
    const auto plan = imgdupe::planInstall(g.manifest, imgdupe::Installed{}, {id});
    uint64_t n = 0;
    for (const auto* c : plan) n += c->totalSize();
    return n;
}

void fillComponents(const std::wstring& message) {
    ListView_DeleteAllItems(g.list);
    g.choices.clear();
    auto add = [](const std::wstring& text, bool check) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = ListView_GetItemCount(g.list);
        it.pszText = const_cast<wchar_t*>(text.c_str());
        const int i = ListView_InsertItem(g.list, &it);
        ListView_SetCheckState(g.list, i, check);
    };
    if (!g.manifestOk) {
        add(message, false);
        return;
    }
    const imgdupe::Installed installed = models::loadInstalled(windowText(g.dir));
    for (const auto& c : g.manifest.components) {
        if (isDependencyOnly(c.id)) continue;
        std::wstring text = widen(c.name) + L"（約 " + models::sizeText(sizeWithRequires(c.id)) + L"）";
        if (!c.description.empty()) text += L"  " + widen(c.description);
        auto it = installed.find(c.id);
        if (it != installed.end()) text += it->second.version == c.version ? L"［入っています］" : L"［新しい版があります］";
        g.choices.push_back(c.id);
        add(text, true);
    }
    if (g.choices.empty()) add(L"（入れられる部品はありません）", false);
}

void setStatus(const std::wstring& s) { SetWindowTextW(g.status, s.c_str()); }

bool writePayload(const std::wstring& exe, std::wstring& error) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(kPayloadId), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    const void* p = data ? LockResource(data) : nullptr;
    const DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!p || size < 1024) {
        error = L"インストーラーが壊れています（中の imgdupe.exe がありません）";
        return false;
    }
    // 実行中でも入れ替えられるように、新しいファイルを書いてから名前を付け替える（古い方は次の起動で消える）
    const std::wstring newPath = exe + L".new";
    // 前の .old をまだ使っているもの（古い版のバックグラウンドの解析など）があると消せないので、その時は別の名前にする
    std::wstring oldPath = exe + L".old";
    for (int i = 1; i <= 50 && !DeleteFileW(oldPath.c_str()) && GetFileAttributesW(oldPath.c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
        oldPath = exe + L".old" + std::to_wstring(i);
    HANDLE f = CreateFileW(newPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = L"インストール先に書き込めません: " + exe;
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(f, p, size, &written, nullptr) && written == size;
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(newPath.c_str());
        error = L"インストール先に書き込めません（ディスクの空きを確認してください）";
        return false;
    }
    if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!MoveFileExW(exe.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(newPath.c_str());
            error = L"今の imgdupe.exe を置き換えられませんでした。imgdupe を終了してから、もう一度お試しください";
            return false;
        }
    }
    if (!MoveFileExW(newPath.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(oldPath.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        error = L"imgdupe.exe を置けませんでした";
        return false;
    }
    return true;
}

bool indexerRunning() {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\imgdupe-indexer");
    if (!h) return false;
    CloseHandle(h);
    return true;
}

void runInstall(std::wstring dir, std::vector<const imgdupe::Component*> comps, bool startMenu, bool desktop) {
    auto* fin = new Finished;
    auto progress = [](int permille, const std::wstring& text) {
        PostMessageW(g.wnd, WM_APP_PROGRESS, static_cast<WPARAM>(permille), reinterpret_cast<LPARAM>(new std::wstring(text)));
    };
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    do {
        progress(0, L"imgdupe.exe を入れています…");
        if (SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) != ERROR_SUCCESS && GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
            fin->error = L"フォルダを作れません: " + dir;
            break;
        }
        const std::wstring exe = dir + L"\\imgdupe.exe";
        const bool indexerWasRunning = indexerRunning();
        if (!writePayload(exe, fin->error)) break;
        install::createShortcuts(exe, startMenu, desktop);
        fin->appOk = true;
        g.installedExe = exe;
        // バックグラウンドの解析が動いていたら、新しい版で動かし直す
        if (indexerWasRunning) {
            std::wstring cmd = L"\"" + exe + L"\" --restart-indexer";
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
        // AI の部品
        uint64_t modelBytes = 0;
        if (!comps.empty()) {
            fin->modelError = models::install(dir, comps, [&](const std::wstring& name, uint64_t done, uint64_t total) {
                progress(total ? static_cast<int>(done * 1000 / total) : 0,
                         name + L" をダウンロードしています… " + models::sizeText(done) + L" / " + models::sizeText(total));
                return !g.cancelled.load();
            });
        }
        for (const auto& [id, c] : models::loadInstalled(dir))
            if (const auto* m = g.manifest.find(id); m && m->version == c.version) modelBytes += m->totalSize();
        WIN32_FILE_ATTRIBUTE_DATA a{};
        GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &a);
        install::registerUninstall(dir, version(), static_cast<unsigned long>((a.nFileSizeLow + modelBytes) / 1024));
    } while (false);
    if (SUCCEEDED(co)) CoUninitialize();
    PostMessageW(g.wnd, WM_APP_FINISHED, 0, reinterpret_cast<LPARAM>(fin));
}

void startInstall() {
    std::wstring dir = windowText(g.dir);
    while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.size() < 3 || dir[1] != L':') {
        MessageBoxW(g.wnd, L"インストール先のフォルダを入れてください（例: C:\\Users\\…\\imgdupe）。", L"imgdupe のセットアップ", MB_ICONWARNING);
        return;
    }
    std::vector<std::string> wanted;
    for (size_t i = 0; i < g.choices.size(); ++i)
        if (ListView_GetCheckState(g.list, static_cast<int>(i))) wanted.push_back(g.choices[i]);
    const auto comps = g.manifestOk ? imgdupe::planInstall(g.manifest, models::loadInstalled(dir), wanted)
                                    : std::vector<const imgdupe::Component*>{};
    g.running = true;
    g.cancelled = false;
    for (HWND h : {g.dir, g.browse, g.list, g.startMenu, g.desktop, g.ok}) EnableWindow(h, FALSE);
    SetWindowTextW(g.cancel, L"中止");
    const bool sm = Button_GetCheck(g.startMenu) == BST_CHECKED, dt = Button_GetCheck(g.desktop) == BST_CHECKED;
    g.worker = std::thread(runInstall, dir, comps, sm, dt);
}

void onFinished(Finished* p) {
    std::unique_ptr<Finished> fin(p);
    if (g.worker.joinable()) g.worker.join();
    g.running = false;
    if (!fin->appOk) {
        setStatus(L"インストールできませんでした");
        MessageBoxW(g.wnd, fin->error.c_str(), L"imgdupe のセットアップ", MB_ICONWARNING);
        for (HWND h : {g.dir, g.browse, g.list, g.startMenu, g.desktop, g.ok}) EnableWindow(h, TRUE);
        SetWindowTextW(g.cancel, L"閉じる");
        return;
    }
    g.done = true;
    SendMessageW(g.progress, PBM_SETPOS, 1000, 0);
    if (fin->modelError.empty()) {
        setStatus(L"インストールしました。");
    } else {
        setStatus(L"imgdupe は入れました。AI の部品は入れられませんでした（あとで「アップデートを確認」から入れられます）。");
        MessageBoxW(g.wnd, (L"AI の部品を入れられませんでした。\r\n\r\n" + fin->modelError).c_str(), L"imgdupe のセットアップ",
                    MB_ICONWARNING);
    }
    ShowWindow(g.launch, SW_SHOW);
    SetWindowTextW(g.ok, L"完了");
    EnableWindow(g.ok, TRUE);
    ShowWindow(g.cancel, SW_HIDE);
}

void finish() {
    if (Button_GetCheck(g.launch) == BST_CHECKED && !g.installedExe.empty())
        ShellExecuteW(nullptr, L"open", g.installedExe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    DestroyWindow(g.wnd);
}

void browseFolder() {
    Com<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.put())))) return;
    DWORD opt = 0;
    dlg->GetOptions(&opt);
    dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"インストール先を選ぶ（その中に imgdupe フォルダを作ります）");
    Com<IShellItem> item;
    PWSTR p = nullptr;
    if (FAILED(dlg->Show(g.wnd)) || FAILED(dlg->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return;
    std::wstring dir = p;
    CoTaskMemFree(p);
    while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
    const size_t sep = dir.find_last_of(L'\\');
    const std::wstring last = sep == std::wstring::npos ? dir : dir.substr(sep + 1);
    if (lstrcmpiW(last.c_str(), L"imgdupe") != 0) dir += (dir.back() == L'\\' ? L"" : L"\\") + std::wstring(L"imgdupe");
    SetWindowTextW(g.dir, dir.c_str());
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK:
                    if (g.done) finish();
                    else if (!g.running) startInstall();
                    return 0;
                case IDCANCEL:
                    if (g.running) {
                        g.cancelled = true;
                        setStatus(L"中止しています…");
                    } else {
                        DestroyWindow(hwnd);
                    }
                    return 0;
                case IDC_BROWSE: browseFolder(); return 0;
                case IDC_DIR:
                    if (HIWORD(wp) == EN_KILLFOCUS && g.manifestOk) fillComponents(L"");
                    return 0;
            }
            break;
        case WM_APP_MANIFEST: {
            std::unique_ptr<models::ManifestResult> r(reinterpret_cast<models::ManifestResult*>(lp));
            g.manifestOk = r->ok;
            g.manifest = std::move(r->manifest);
            fillComponents(r->ok ? L"" : L"（" + r->error + L"。AI の部品は後から入れられます）");
            EnableWindow(g.ok, TRUE);
            return 0;
        }
        case WM_APP_PROGRESS: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
            SendMessageW(g.progress, PBM_SETPOS, wp, 0);
            if (!g.cancelled) setStatus(*text);
            return 0;
        }
        case WM_APP_FINISHED: onFinished(reinterpret_cast<Finished*>(lp)); return 0;
        case WM_CLOSE:
            if (g.running) {
                g.cancelled = true;
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g.inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    HDC screen = GetDC(nullptr);
    g.scale = GetDeviceCaps(screen, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, screen);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_BOLD;
    lf.lfHeight = lf.lfHeight * 13 / 10;
    g.bold = CreateFontIndirectW(&lf);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ImgdupeSetup";
    RegisterClassExW(&wc);
    const int cw = 560, ch = 430;
    RECT r{0, 0, px(cw), px(ch)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g.wnd = CreateWindowExW(0, wc.lpszClassName, (L"imgdupe のセットアップ " + version()).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!g.wnd) return 1;

    const int m = 16, w = cw - 2 * m;
    HWND title = child(WC_STATICW, (L"imgdupe " + version() + L" をインストールします").c_str(), SS_NOPREFIX, -1, m, 14, w, 26);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(g.bold), TRUE);
    child(WC_STATICW, L"インストール先（管理者の権限は要りません）", SS_NOPREFIX, -1, m, 52, w, 18);
    std::wstring dir = install::installedDir();
    if (dir.empty()) dir = install::defaultDir();
    g.dir = child(WC_EDITW, dir.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, IDC_DIR, m, 72, w - 96, 24, WS_EX_CLIENTEDGE);
    g.browse = child(WC_BUTTONW, L"参照…", WS_TABSTOP, IDC_BROWSE, cw - m - 88, 71, 88, 26);
    child(WC_STATICW, L"AI 解析の部品（チェックしたものをダウンロードします。後からでも入れられます）", SS_NOPREFIX, -1, m, 110, w, 18);
    g.list = child(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL, IDC_COMPONENTS, m, 130, w, 90,
                   WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    LVCOLUMNW col{};
    col.mask = LVCF_WIDTH;
    col.cx = px(w - 24);
    ListView_InsertColumn(g.list, 0, &col);
    fillComponents(L"（モデルの一覧を取得しています…）");
    g.startMenu = child(WC_BUTTONW, L"スタートメニューに登録する", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_STARTMENU, m, 232, w, 22);
    Button_SetCheck(g.startMenu, BST_CHECKED);
    g.desktop = child(WC_BUTTONW, L"デスクトップにショートカットを作る", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_DESKTOP, m, 256, w, 22);
    g.progress = child(PROGRESS_CLASSW, L"", 0, IDC_PROGRESS, m, 294, w, 18);
    SendMessageW(g.progress, PBM_SETRANGE32, 0, 1000);
    g.status = child(WC_STATICW, L"", SS_NOPREFIX, IDC_STATUS, m, 318, w, 40);
    g.launch = child(WC_BUTTONW, L"imgdupe を起動する", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_LAUNCH, m, ch - 44, 240, 26);
    Button_SetCheck(g.launch, BST_CHECKED);
    ShowWindow(g.launch, SW_HIDE);
    g.ok = child(WC_BUTTONW, L"インストール", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, cw - m - 220, ch - 44, 110, 30);
    g.cancel = child(WC_BUTTONW, L"閉じる", WS_TABSTOP, IDCANCEL, cw - m - 104, ch - 44, 104, 30);
    EnableWindow(g.ok, FALSE);  // 一覧が届くまで（失敗しても入れられるように、届いたら有効にする）

    ShowWindow(g.wnd, show);
    UpdateWindow(g.wnd);
    std::thread([] {
        auto* r = new models::ManifestResult(models::fetchManifest());
        if (!PostMessageW(g.wnd, WM_APP_MANIFEST, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(g.wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g.worker.joinable()) g.worker.join();
    CoUninitialize();
    return 0;
}
