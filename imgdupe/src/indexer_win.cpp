// 常駐する indexer（imgdupe.exe --indexer）
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX

#include "indexer_win.hpp"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "db.hpp"
#include "image_win.hpp"
#include "indexer.hpp"
#include "resource.h"
#include "win_util.hpp"

using namespace imgdupe;
using winutil::narrow;
using winutil::widen;

namespace indexer {

namespace {

constexpr wchar_t kMutexName[] = L"Local\\imgdupe-indexer";
constexpr wchar_t kClass[] = L"ImgdupeIndexer";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"imgdupe-indexer";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR kTipTimer = 1;
constexpr ULONGLONG kChangeQuietMs = 10 * 1000;       // フォルダが変わってから、これだけ静かになったら調べる
constexpr ULONGLONG kRescanMs = 30 * 60 * 1000;       // 変化が無くても、これだけたったら調べ直す
constexpr DWORD kIdleMs = 60 * 1000;                  // これだけ操作が無ければ全力で処理する

enum : UINT { ID_PAUSE = 1, ID_RESCAN, ID_OPEN_GUI, ID_QUIT };

std::wstring exePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::string dbFileOf(const std::wstring& dataDir) { return narrow(dataDir + L"\\imgdupe.db"); }

struct Resident {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    UINT taskbarCreated = 0;
    NOTIFYICONDATAW nid{};
    std::wstring dataDir;

    std::thread worker, watcher;
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> quit{false}, paused{false}, cancelRun{false}, running{false};
    std::atomic<bool> dirty{true};             // 調べ直しが要る
    std::atomic<ULONGLONG> changeTick{0};      // フォルダの変化に気づいた時刻（0 = なし）
    std::vector<std::wstring> roots;           // watcher が見ているフォルダ（m で守る）
    std::atomic<unsigned> rootsGen{0};
    IndexProgress prog;
    std::wstring lastResult;                   // 最後の結果（m で守る）
    std::wstring waitReason;                   // 止めている理由（m で守る）
};

Resident* g = nullptr;

// 今は処理を控えるべきか（バッテリー駆動・全画面のアプリ・プレゼンテーション中）。理由を返す
std::wstring busyReason() {
    SYSTEM_POWER_STATUS ps;
    if (GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0) return L"バッテリー駆動中";
    QUERY_USER_NOTIFICATION_STATE st;
    if (SUCCEEDED(SHQueryUserNotificationState(&st)) &&
        (st == QUNS_BUSY || st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE))
        return L"全画面のアプリの実行中";
    return L"";
}

bool userIdle() {
    LASTINPUTINFO li{sizeof(li)};
    return GetLastInputInfo(&li) && GetTickCount() - li.dwTime > kIdleMs;
}

void setStatus(std::wstring* field, const std::wstring& v) {
    std::lock_guard<std::mutex> lock(g->m);
    *field = v;
}

// ---------------------------------------------------------------- フォルダの変化を見る

void watchLoop() {
    while (!g->quit) {
        std::vector<std::wstring> roots;
        unsigned gen;
        {
            std::lock_guard<std::mutex> lock(g->m);
            roots = g->roots;
            gen = g->rootsGen;
        }
        std::vector<HANDLE> handles;
        for (const auto& r : roots) {
            if (handles.size() >= MAXIMUM_WAIT_OBJECTS) break;
            HANDLE h = FindFirstChangeNotificationW(
                r.c_str(), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                    FILE_NOTIFY_CHANGE_LAST_WRITE);
            if (h != INVALID_HANDLE_VALUE) handles.push_back(h);
        }
        while (!g->quit && gen == g->rootsGen) {
            if (handles.empty()) {
                Sleep(1000);
                continue;
            }
            const DWORD w = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, 1000);
            if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + handles.size()) {
                if (!g->running) g->changeTick = GetTickCount64();  // 自分の書き込み（処理中）は数えない
                FindNextChangeNotification(handles[w - WAIT_OBJECT_0]);
            }
        }
        for (HANDLE h : handles) FindCloseChangeNotification(h);
    }
}

// ---------------------------------------------------------------- 処理

void runOnce(const std::vector<std::wstring>& roots) {
    IndexOptions opt;
    for (const auto& r : roots) opt.roots.push_back(narrow(r));
    opt.dbFile = dbFileOf(g->dataDir);
    opt.packDir = narrow(g->dataDir);
    opt.threads = userIdle() ? 0 : 1;  // 操作中は 1 スレッドだけ
    std::atomic<uint64_t>& found = g->prog.prog.total;
    IndexCallbacks cb;
    cb.scan = [&found](const std::string& root, std::vector<ScannedFile>& files) {
        imgwin::scanFolder(widen(root), files, found, g->cancelRun);
    };
    cb.decode = [](const std::string& path) { return imgwin::decodeFeatures(widen(path)); };
    cb.encodeThumb = [](const std::string& path, int side, std::vector<uint8_t>& bytes, int& w, int& h) {
        return imgwin::encodeThumbnail(widen(path), side, bytes, w, h);
    };
    cb.readMeta = [](const std::string& path, FileMeta& m) { return imgwin::readFileMeta(widen(path), m); };
    cb.threadInit = [] {
        imgwin::threadBegin();
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    };
    cb.threadExit = [] {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        imgwin::threadEnd();
    };

    // 見張り: 一時停止・終了・控えるべき状態になったら、すぐ止める（続きは次の回）
    std::atomic<bool> done{false};
    std::thread guard([&] {
        while (!done) {
            if (g->paused || g->quit || !busyReason().empty()) g->cancelRun = true;
            Sleep(1000);
        }
    });
    g->running = true;
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);  // CPU とディスクの優先度を下げる
    try {
        const IndexReport r = indexOnce(opt, cb, g->prog, g->cancelRun);
        if (r.cancelled) {
            g->dirty = true;
        } else {
            wchar_t t[200];
            SYSTEMTIME now;
            GetLocalTime(&now);
            swprintf(t, 200, L"%02d:%02d に完了（画像 %zu 枚、追加 %zu・削除 %zu、ハッシュ %zu、サムネイル %zu、メタデータ %zu）",
                     now.wHour, now.wMinute, r.files, r.added, r.removed, r.hashed, r.thumbs, r.metas);
            setStatus(&g->lastResult, t);
        }
    } catch (const std::exception& e) {
        setStatus(&g->lastResult, L"エラー: " + widen(e.what()));
    }
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    g->running = false;
    g->changeTick = 0;  // 処理中のパック・DB の書き込みによる変化は無視する
    done = true;
    guard.join();
}

void workLoop() {
    imgwin::threadBegin();
    Db db;
    if (!db.open(dbFileOf(g->dataDir))) {
        setStatus(&g->lastResult, L"DB を開けません: " + widen(db.error()));
        imgwin::threadEnd();
        return;
    }
    ULONGLONG lastRun = 0;
    std::vector<std::string> rootsUtf8;
    while (!g->quit) {
        try {
            for (const auto& c : takeCommands(db)) {
                if (c == "quit") g->quit = true;
                else if (c == "rescan") g->dirty = true;
                else if (c == "pause") g->paused = true;
                else if (c == "resume") g->paused = false;
            }
            const std::vector<std::string> r = getRoots(db);
            if (r != rootsUtf8) {
                rootsUtf8 = r;
                std::lock_guard<std::mutex> lock(g->m);
                g->roots.clear();
                for (const auto& x : r) g->roots.push_back(widen(x));
                ++g->rootsGen;
                g->dirty = true;
            }
        } catch (const std::exception& e) {
            setStatus(&g->lastResult, L"エラー: " + widen(e.what()));
        }
        if (g->quit) break;
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG change = g->changeTick;
        if (change && now - change > kChangeQuietMs) {
            g->changeTick = 0;
            g->dirty = true;
        }
        if (lastRun && now - lastRun > kRescanMs) g->dirty = true;

        std::wstring reason = g->paused ? L"一時停止中" : rootsUtf8.empty() ? L"調べるフォルダがありません" : busyReason();
        setStatus(&g->waitReason, reason);
        if (g->dirty && reason.empty()) {
            g->dirty = false;
            g->cancelRun = false;
            std::vector<std::wstring> roots;
            {
                std::lock_guard<std::mutex> lock(g->m);
                roots = g->roots;
            }
            runOnce(roots);
            lastRun = GetTickCount64();
            continue;
        }
        std::unique_lock<std::mutex> lock(g->m);
        g->cv.wait_for(lock, std::chrono::seconds(5), [] { return g->quit.load(); });
    }
    imgwin::threadEnd();
    PostMessageW(g->hwnd, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------- タスクトレイ

std::wstring statusText() {
    std::wstring wait, last;
    {
        std::lock_guard<std::mutex> lock(g->m);
        wait = g->waitReason;
        last = g->lastResult;
    }
    if (g->running) {
        const auto phase = static_cast<IndexPhase>(g->prog.phase.load());
        const uint64_t done = g->prog.prog.done, total = g->prog.prog.total;
        wchar_t t[128];
        switch (phase) {
            case IndexPhase::Scan: swprintf(t, 128, L"フォルダを調べています（%llu 枚）", static_cast<unsigned long long>(total)); break;
            case IndexPhase::Hash:
                swprintf(t, 128, L"ハッシュ %llu / %llu", static_cast<unsigned long long>(done), static_cast<unsigned long long>(total));
                break;
            case IndexPhase::Thumb:
                swprintf(t, 128, L"サムネイル %llu / %llu", static_cast<unsigned long long>(done),
                         static_cast<unsigned long long>(total));
                break;
            case IndexPhase::Meta:
                swprintf(t, 128, L"メタデータ %llu / %llu", static_cast<unsigned long long>(done),
                         static_cast<unsigned long long>(total));
                break;
            default: swprintf(t, 128, L"処理中"); break;
        }
        return t;
    }
    if (!wait.empty()) return wait;
    return last.empty() ? L"待機中" : last;
}

void updateTip() {
    const std::wstring tip = L"imgdupe（バックグラウンドの解析）\n" + statusText();
    lstrcpynW(g->nid.szTip, tip.c_str(), ARRAYSIZE(g->nid.szTip));
    g->nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g->nid);
}

void addTrayIcon() {
    g->nid = NOTIFYICONDATAW{};
    g->nid.cbSize = sizeof(g->nid);
    g->nid.hWnd = g->hwnd;
    g->nid.uID = 1;
    g->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g->nid.uCallbackMessage = WM_TRAY;
    g->nid.hIcon = static_cast<HICON>(LoadImageW(g->inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    lstrcpynW(g->nid.szTip, L"imgdupe（バックグラウンドの解析）", ARRAYSIZE(g->nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g->nid);
}

void openGui() {
    std::wstring cmd = L"\"" + exePath() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void showMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, statusText().c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g->paused ? MF_CHECKED : 0), ID_PAUSE, L"一時停止(&P)");
    AppendMenuW(menu, MF_STRING, ID_RESCAN, L"今すぐ調べ直す(&R)");
    AppendMenuW(menu, MF_STRING, ID_OPEN_GUI, L"imgdupe を開く(&O)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_QUIT, L"バックグラウンドの解析を終了(&X)");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g->hwnd);  // メニューの外をクリックした時に閉じるように
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g->hwnd, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case ID_PAUSE: g->paused = !g->paused; break;
        case ID_RESCAN: g->dirty = true; break;
        case ID_OPEN_GUI: openGui(); break;
        case ID_QUIT: g->quit = true; break;
    }
    g->cv.notify_all();
    updateTip();
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g && msg == g->taskbarCreated && msg) {  // Explorer が再起動した時はアイコンを入れ直す
        addTrayIcon();
        return 0;
    }
    switch (msg) {
        case WM_TRAY:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) showMenu();
            else if (LOWORD(lp) == WM_LBUTTONDBLCLK) openGui();
            return 0;
        case WM_TIMER:
            if (wp == kTipTimer) updateTip();
            return 0;
        case WM_CLOSE:
            g->quit = true;
            g->cancelRun = true;
            g->cv.notify_all();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g->nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int runResident(HINSTANCE inst, const std::wstring& dataDir, bool waitForOld) {
    HANDLE mutex = nullptr;
    // 起動し直しの時は、古い版が終わるまで最大 60 秒待つ
    for (int i = 0; i < (waitForOld ? 60 : 1); ++i) {
        mutex = CreateMutexW(nullptr, TRUE, kMutexName);
        if (mutex && GetLastError() != ERROR_ALREADY_EXISTS) break;
        if (mutex) CloseHandle(mutex);
        mutex = nullptr;
        if (waitForOld) Sleep(1000);
    }
    if (!mutex) return 0;  // もう動いている
    Resident r;
    g = &r;
    r.inst = inst;
    r.dataDir = dataDir;
    r.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    // 見えないウィンドウ（メッセージ専用のウィンドウは TaskbarCreated を受け取れないので普通のもの）
    r.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"imgdupe indexer", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst,
                             nullptr);
    if (!r.hwnd) {
        CloseHandle(mutex);
        return 1;
    }
    addTrayIcon();
    SetTimer(r.hwnd, kTipTimer, 1000, nullptr);
    r.worker = std::thread(workLoop);
    r.watcher = std::thread(watchLoop);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    r.quit = true;
    r.cancelRun = true;
    r.cv.notify_all();
    if (r.worker.joinable()) r.worker.join();
    if (r.watcher.joinable()) r.watcher.join();
    g = nullptr;
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}

bool isRunning() {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, kMutexName);
    if (!h) return false;
    CloseHandle(h);
    return true;
}

bool start(bool restart) {
    if (!restart && isRunning()) return true;
    std::wstring cmd = L"\"" + exePath() + L"\" --indexer" + (restart ? L" --restart" : L"");
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void requestQuit(const std::wstring& dataDir) {
    Db db;
    if (db.open(dbFileOf(dataDir))) addCommand(db, "quit");
}

void requestRescan(const std::wstring& dataDir) {
    Db db;
    if (db.open(dbFileOf(dataDir))) addCommand(db, "rescan");
}

void setRoots(const std::wstring& dataDir, const std::vector<std::wstring>& roots) {
    Db db;
    if (!db.open(dbFileOf(dataDir))) return;
    std::vector<std::string> r;
    for (const auto& x : roots) r.push_back(narrow(x));
    if (getRoots(db) != r) imgdupe::setRoots(db, r);
}

bool autoStartEnabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    const bool found = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return found;
}

void setAutoStart(bool on) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        const std::wstring cmd = L"\"" + exePath() + L"\" --indexer";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

}  // namespace indexer
