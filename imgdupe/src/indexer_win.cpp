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

#include "clip.hpp"
#include "db.hpp"
#include "image_win.hpp"
#include "indexer.hpp"
#include "meta.hpp"
#include "names.hpp"
#include "quality.hpp"
#include "links.hpp"
#include "clip_win.hpp"
#include "lookup_win.hpp"
#include "tagger_win.hpp"
#include "resource.h"
#include "win_util.hpp"

using namespace imgdupe;
using winutil::narrow;
using winutil::widen;

namespace indexer {

namespace {

constexpr wchar_t kMutexName[] = L"Local\\imgdupe-indexer";
constexpr wchar_t kClass[] = L"ImgdupeIndexer";
constexpr wchar_t kPopupClass[] = L"ImgdupeIndexerStatus";
constexpr UINT_PTR kPopupTimer = 2;
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"imgdupe-indexer";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR kTipTimer = 1;
constexpr ULONGLONG kChangeQuietMs = 10 * 1000;       // フォルダが変わってから、これだけ静かになったら調べる
constexpr ULONGLONG kRescanMs = 30 * 60 * 1000;       // 変化が無くても、これだけたったら調べ直す
constexpr DWORD kIdleMs = 60 * 1000;                  // これだけ操作が無ければ全力で処理する

enum : UINT { ID_PAUSE = 1, ID_RESCAN, ID_OPEN_GUI, ID_QUIT, ID_LOAD0 = 100 };  // ID_LOAD0 + 負荷

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
    std::wstring taggerDevice;                 // タガーを動かしている所（GPU / CPU。m で守る）
    std::atomic<int> load{kLoadLight};         // 負荷（imgdupe.ini の indexerLoad。回のはじめに読む）

    // タスクトレイのアイコンをクリックした時の状況の窓
    HWND popup = nullptr;
    int popupView = 0;                          // 0 = 新規作成中、1 = 総合
    std::vector<std::pair<RECT, int>> popupTabs;
    struct OverallRow {
        std::wstring name;
        StageCount count;
    };
    std::vector<OverallRow> overall;            // 総合（m で守る）
    int64_t overallImages = -1;                 // DB の画像の数（-1 = まだ数えていない。m で守る）
    ULONGLONG overallAt = 0;                    // 最後に数えた時刻
    std::atomic<bool> counting{false};
};

Resident* g = nullptr;

// 今は処理を控えるべきか（バッテリー駆動・全画面のアプリ・プレゼンテーション中）。理由を返す。
// 「しっかり」はゲームと同じように扱い、控えない
std::wstring busyReason() {
    if (g->load >= kLoadHeavy) return L"";
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

// 処理に使うスレッドの数（0 = 全部のコア）。控えめ: 操作中は 1、そこそこ: 操作中はコアの半分、しっかり: いつも全部
unsigned threadsForLoad() {
    if (g->load >= kLoadHeavy || userIdle()) return 0;
    if (g->load == kLoadMedium) return std::max(1u, std::thread::hardware_concurrency() / 2);
    return 1;
}

// プロセスの優先度。控えめ: CPU・ディスク・メモリの優先度をまとめて下げる（バックグラウンドモード）、
// そこそこ: 「通常以下」、しっかり: 「通常」（ゲームなどと同じ）
void applyProcessPriority(int load) {
    HANDLE self = GetCurrentProcess();
    if (load <= kLoadLight) {
        SetPriorityClass(self, PROCESS_MODE_BACKGROUND_BEGIN);
        return;
    }
    SetPriorityClass(self, PROCESS_MODE_BACKGROUND_END);
    SetPriorityClass(self, load == kLoadMedium ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
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
    // 見本画像のフォルダも読む（CLIP のベクトルを作って、見本に使う）
    if (GetFileAttributesW(clip::refsDir(g->dataDir).c_str()) != INVALID_FILE_ATTRIBUTES)
        opt.roots.push_back(narrow(clip::refsDir(g->dataDir)));
    opt.dbFile = dbFileOf(g->dataDir);
    opt.packDir = narrow(g->dataDir);
    g->load = loadLevel(g->dataDir);
    applyProcessPriority(g->load);
    opt.threads = threadsForLoad();
    try {
        Db db;
        if (db.open(opt.dbFile)) imgwin::setScanExcludes(customVfolderRoots(db));
    } catch (const std::exception&) {
    }
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
        if (g->load <= kLoadLight) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        else if (g->load == kLoadMedium) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    };
    cb.threadExit = [] {
        if (g->load <= kLoadLight) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        imgwin::threadEnd();
    };
    cb.taggerVersion = [] { return tagger::stageVersion(g->dataDir); };
    cb.makeTagger = [&cb](std::string& error) {
        // 操作中は推論も 1 スレッドだけ（操作していなければ全部のコア）
        tagger::LoadResult r = tagger::load(g->dataDir, threadsForLoad(), cb.threadInit, cb.threadExit);
        error = narrow(r.error);
        if (r.analyzer) {
            std::lock_guard<std::mutex> lock(g->m);
            g->taggerDevice = r.device;
        }
        return r.analyzer;
    };
    cb.clipVersion = [] { return clip::stageVersion(g->dataDir); };
    cb.prepareClip = [](Db& db) { clip::prepare(g->dataDir, db); };
    cb.afterClip = [](Db& db, unsigned threads, const std::atomic<bool>& cancel, Progress* p) {
        clip::updateVariants(g->dataDir, db, threads, cancel, p);
    };
    cb.lookup = [](Db& db, const std::atomic<bool>& cancel, Progress* p) { lookup::run(g->dataDir, db, cancel, p); };
    cb.merge = [](Db& db) {
        clip::applyDictionary(g->dataDir, db, false);
        clip::applyReferences(g->dataDir, db, false);
        clip::applyTagParents(db, false);  // 辞書や見本で付いたタグの親も
        clip::applyAesthetic(g->dataDir, db);
    };
    cb.qualityEnabled = [] { return clip::qualityEnabled(g->dataDir); };
    cb.decodeQuality = [](const std::string& path, int maxSide, std::vector<uint8_t>& bgra, int& w, int& h, int& ow, int& oh) {
        return imgwin::decodeBgra(widen(path), maxSide, bgra, w, h, &ow, &oh);
    };
    cb.makeClip = [&cb](std::string& error) {
        clip::LoadResult r = clip::loadAnalyzer(g->dataDir, threadsForLoad(), cb.threadInit, cb.threadExit);
        error = narrow(r.error);
        if (r.analyzer) {
            std::lock_guard<std::mutex> lock(g->m);
            g->taggerDevice = r.device;
        }
        return r.analyzer;
    };

    // 見張り: 一時停止・終了・控えるべき状態になったら、すぐ止める（続きは次の回）
    std::atomic<bool> done{false};
    // 処理中も GUI からの指示（終了・一時停止など）を見る。見ないと、更新の後に古い版が長い処理を終えるまで残り、
    // その exe（imgdupe.exe.old）を使い続けて次の更新ができなかった
    std::thread guard([&] {
        Db cmdDb;
        const bool cmdOk = cmdDb.open(opt.dbFile);
        for (unsigned tick = 0; !done; ++tick) {
            if (cmdOk && tick % 3 == 0) {
                try {
                    for (const auto& c : takeCommands(cmdDb)) {
                        if (c == "quit") g->quit = true;
                        else if (c == "rescan") g->dirty = true;
                        else if (c == "pause") g->paused = true;
                        else if (c == "resume") g->paused = false;
                    }
                } catch (const std::exception&) {
                }
            }
            if (g->paused || g->quit || !busyReason().empty()) g->cancelRun = true;
            Sleep(1000);
        }
    });
    g->running = true;
    const bool background = g->load <= kLoadLight;
    if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);  // CPU とディスクの優先度を下げる
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
    if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
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

        g->load = loadLevel(g->dataDir);
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
    std::wstring wait, last, taggerDevice;
    {
        std::lock_guard<std::mutex> lock(g->m);
        taggerDevice = g->taggerDevice;
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
            case IndexPhase::Clip:
                swprintf(t, 128, L"CLIP（%ls）%llu / %llu", taggerDevice.c_str(), static_cast<unsigned long long>(done),
                         static_cast<unsigned long long>(total));
                break;
            case IndexPhase::Quality:
                swprintf(t, 128, L"品質 %llu / %llu", static_cast<unsigned long long>(done), static_cast<unsigned long long>(total));
                break;
            case IndexPhase::Lookup:
                swprintf(t, 128, L"出典の照会 %llu / %llu", static_cast<unsigned long long>(done), static_cast<unsigned long long>(total));
                break;
            case IndexPhase::Merge: swprintf(t, 128, L"辞書を当てています"); break;
            case IndexPhase::Variants:
                swprintf(t, 128, L"差分グループ %llu / %llu", static_cast<unsigned long long>(done), static_cast<unsigned long long>(total));
                break;
            case IndexPhase::Tagger:
                swprintf(t, 128, L"タグ付け（%ls）%llu / %llu", taggerDevice.c_str(), static_cast<unsigned long long>(done),
                         static_cast<unsigned long long>(total));
                break;
            case IndexPhase::Names:
                swprintf(t, 128, L"json・フォルダ名 %llu / %llu", static_cast<unsigned long long>(done),
                         static_cast<unsigned long long>(total));
                break;
            default: swprintf(t, 128, L"処理中"); break;
        }
        return t;
    }
    if (!wait.empty()) return wait;
    return last.empty() ? L"待機中" : last;
}

// ---------------------------------------------------------------- 状況の窓

int popupPx(int v) {
    static const int dpi = [] {
        HDC dc = GetDC(nullptr);
        const int d = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        return d;
    }();
    return MulDiv(v, dpi, 96);
}

const wchar_t* phaseName(IndexPhase p) {
    switch (p) {
        case IndexPhase::Scan: return L"フォルダを調べる";
        case IndexPhase::Hash: return L"ハッシュ";
        case IndexPhase::Thumb: return L"サムネイル";
        case IndexPhase::Meta: return L"メタデータ";
        case IndexPhase::Names: return L"json・フォルダ名";
        case IndexPhase::Tagger: return L"タグ付け（WD14）";
        case IndexPhase::Clip: return L"CLIP";
        case IndexPhase::Variants: return L"差分のまとまり";
        case IndexPhase::Quality: return L"品質";
        case IndexPhase::Lookup: return L"出典の照会";
        case IndexPhase::Merge: return L"仕上げ（辞書・親子関係）";
        default: return L"";
    }
}

// 総合の数を別のスレッドで数える（数秒ごと）
void countOverall() {
    if (g->counting.exchange(true)) return;
    std::thread([] {
        std::vector<Resident::OverallRow> rows;
        int64_t images = -1;
        try {
            Db db;
            if (db.open(dbFileOf(g->dataDir))) {
                images = imageCount(db);
                rows.push_back({L"ハッシュ", stageCount(db, stage::kHash, kHashVersion)});
                rows.push_back({L"サムネイル", stageCount(db, stage::kThumb, kThumbVersion)});
                rows.push_back({L"メタデータ", stageCount(db, stage::kMeta, kMetaVersion)});
                rows.push_back({L"json・フォルダ名", stageCount(db, kStageNames, kNamesVersion)});
                if (const int tv = tagger::stageVersion(g->dataDir)) rows.push_back({L"タグ付け（WD14）", stageCount(db, stage::kTagger, tv)});
                if (const int cv = clip::stageVersion(g->dataDir)) rows.push_back({L"CLIP", stageCount(db, kStageClip, cv)});
                if (clip::qualityEnabled(g->dataDir)) rows.push_back({L"品質", stageCount(db, kStageQuality, kQualityVersion)});
            }
        } catch (const std::exception&) {
        }
        {
            std::lock_guard<std::mutex> lock(g->m);
            g->overall = std::move(rows);
            g->overallImages = images;
            g->overallAt = GetTickCount64();
        }
        g->counting = false;
        if (g->popup) InvalidateRect(g->popup, nullptr, FALSE);
    }).detach();
}

void fillRect(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

// 進み具合の棒
void drawBar(HDC dc, RECT r, double ratio, COLORREF color) {
    fillRect(dc, r, RGB(226, 230, 236));
    RECT f = r;
    f.right = r.left + static_cast<LONG>((r.right - r.left) * std::clamp(ratio, 0.0, 1.0));
    if (f.right > f.left) fillRect(dc, f, color);
}

void paintPopup(HDC dc, const RECT& rc) {
    fillRect(dc, rc, RGB(250, 251, 253));
    SetBkMode(dc, TRANSPARENT);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bl = ncm.lfMessageFont;
    bl.lfWeight = FW_BOLD;
    HFONT bold = CreateFontIndirectW(&bl);
    HGDIOBJ oldFont = SelectObject(dc, bold);
    const int m = popupPx(12);
    int y = m;
    RECT tr = {m, y, rc.right - m, y + popupPx(20)};
    SetTextColor(dc, RGB(30, 30, 30));
    DrawTextW(dc, L"インデックスの作成状況", -1, &tr, DT_LEFT | DT_SINGLELINE);
    y += popupPx(26);
    // 切り替え
    SelectObject(dc, font);
    g->popupTabs.clear();
    {
        int x = m;
        const wchar_t* names[] = {L"新規作成中", L"総合"};
        for (int k = 0; k < 2; ++k) {
            SIZE sz{};
            GetTextExtentPoint32W(dc, names[k], static_cast<int>(wcslen(names[k])), &sz);
            RECT r = {x, y, x + sz.cx + popupPx(20), y + popupPx(24)};
            const bool on = g->popupView == k;
            HBRUSH b = CreateSolidBrush(on ? RGB(0, 100, 220) : RGB(255, 255, 255));
            HPEN pen = CreatePen(PS_SOLID, 1, on ? RGB(0, 100, 220) : RGB(180, 180, 180));
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
            RoundRect(dc, r.left, r.top, r.right, r.bottom, popupPx(12), popupPx(12));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(b);
            DeleteObject(pen);
            SetTextColor(dc, on ? RGB(255, 255, 255) : RGB(60, 60, 60));
            DrawTextW(dc, names[k], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            g->popupTabs.push_back({r, k});
            x = r.right + popupPx(6);
        }
    }
    y += popupPx(34);
    std::wstring wait, last, device;
    {
        std::lock_guard<std::mutex> lock(g->m);
        wait = g->waitReason;
        last = g->lastResult;
        device = g->taggerDevice;
    }
    auto text = [&](const std::wstring& s, COLORREF c, UINT flags = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX) {
        RECT r = {m, y, rc.right - m, rc.bottom - m};
        RECT calc = r;
        DrawTextW(dc, s.c_str(), -1, &calc, flags | DT_CALCRECT);
        SetTextColor(dc, c);
        DrawTextW(dc, s.c_str(), -1, &r, flags);
        y += static_cast<int>(calc.bottom - calc.top) + popupPx(4);
    };
    if (g->popupView == 0) {
        const bool running = g->running;
        const std::wstring state = g->paused ? L"一時停止中" : running ? L"処理中" : !wait.empty() ? L"待機中（" + wait + L"）" : L"待機中（フォルダが変われば調べます）";
        text(L"状態: " + state, RGB(30, 30, 30));
        text(L"負荷: " + std::wstring(loadLevelName(loadLevel(g->dataDir))), RGB(90, 90, 90));
        y += popupPx(4);
        if (running) {
            const auto phase = static_cast<IndexPhase>(g->prog.phase.load());
            const uint64_t done = g->prog.prog.done, total = g->prog.prog.total;
            SelectObject(dc, bold);
            std::wstring now = phaseName(phase);
            if ((phase == IndexPhase::Tagger || phase == IndexPhase::Clip) && !device.empty()) now += L"（" + device + L"）";
            text(now, RGB(30, 30, 30));
            SelectObject(dc, font);
            if (phase != IndexPhase::Scan && total) {
                drawBar(dc, {m, y, rc.right - m, y + popupPx(10)}, double(done) / double(total), RGB(0, 120, 215));
                y += popupPx(14);
                wchar_t t[96];
                swprintf(t, 96, L"%llu / %llu 枚（%.0f%%）", static_cast<unsigned long long>(done), static_cast<unsigned long long>(total),
                         100.0 * double(done) / double(total));
                text(t, RGB(60, 60, 60));
            } else if (phase == IndexPhase::Scan) {
                text(std::to_wstring(total) + L" 枚見つけました", RGB(60, 60, 60));
            }
            // 段階の並び（済んだもの・今・まだ）
            y += popupPx(4);
            const IndexPhase order[] = {IndexPhase::Scan, IndexPhase::Hash, IndexPhase::Thumb, IndexPhase::Meta, IndexPhase::Names,
                                        IndexPhase::Tagger, IndexPhase::Clip, IndexPhase::Variants, IndexPhase::Quality,
                                        IndexPhase::Lookup, IndexPhase::Merge};
            for (IndexPhase p : order) {
                const bool cur = p == phase, past = static_cast<int>(p) < static_cast<int>(phase);
                RECT r = {m, y, rc.right - m, y + popupPx(18)};
                SetTextColor(dc, cur ? RGB(0, 100, 220) : past ? RGB(40, 140, 60) : RGB(150, 150, 150));
                const std::wstring line = std::wstring(cur ? L"▶ " : past ? L"✓ " : L"・ ") + phaseName(p);
                DrawTextW(dc, line.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
                y += popupPx(18);
            }
        } else {
            text(last.empty() ? L"まだ新しく作るものはありません" : L"前回: " + last, RGB(60, 60, 60));
        }
    } else {
        int64_t images;
        std::vector<Resident::OverallRow> rows;
        {
            std::lock_guard<std::mutex> lock(g->m);
            images = g->overallImages;
            rows = g->overall;
        }
        if (images < 0) {
            text(L"数えています…", RGB(90, 90, 90));
        } else {
            SelectObject(dc, bold);
            text(L"画像 " + std::to_wstring(images) + L" 枚", RGB(30, 30, 30));
            SelectObject(dc, font);
            const int nameW = popupPx(120);
            for (const auto& row : rows) {
                const int64_t finished = row.count.done + row.count.failed;
                const double ratio = images > 0 ? double(finished) / double(images) : 0.0;
                RECT nr = {m, y, m + nameW, y + popupPx(18)};
                SetTextColor(dc, RGB(50, 50, 50));
                DrawTextW(dc, row.name.c_str(), -1, &nr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
                drawBar(dc, {m + nameW, y + popupPx(5), rc.right - m - popupPx(56), y + popupPx(13)}, ratio,
                        finished >= images ? RGB(40, 150, 70) : RGB(0, 120, 215));
                wchar_t pct[16];
                swprintf(pct, 16, L"%.0f%%", 100.0 * ratio);
                RECT pr = {rc.right - m - popupPx(50), y, rc.right - m, y + popupPx(18)};
                DrawTextW(dc, pct, -1, &pr, DT_RIGHT | DT_SINGLELINE);
                y += popupPx(18);
                wchar_t detail[96];
                if (row.count.failed)
                    swprintf(detail, 96, L"%lld / %lld 枚（読めなかったもの %lld）", static_cast<long long>(row.count.done),
                             static_cast<long long>(images), static_cast<long long>(row.count.failed));
                else
                    swprintf(detail, 96, L"%lld / %lld 枚", static_cast<long long>(row.count.done), static_cast<long long>(images));
                RECT dr = {m + nameW, y, rc.right - m, y + popupPx(16)};
                SetTextColor(dc, RGB(120, 120, 120));
                DrawTextW(dc, detail, -1, &dr, DT_LEFT | DT_SINGLELINE);
                y += popupPx(20);
            }
        }
    }
    // 下に操作の説明
    RECT hr = {m, rc.bottom - m - popupPx(16), rc.right - m, rc.bottom - m};
    SetTextColor(dc, RGB(140, 140, 140));
    DrawTextW(dc, L"右クリックでメニュー、ダブルクリックで imgdupe を開く", -1, &hr, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteObject(bold);
}

void showMenu();
void openGui();

LRESULT CALLBACK popupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC buf = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
            HGDIOBJ old = SelectObject(buf, bmp);
            paintPopup(buf, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, buf, 0, 0, SRCCOPY);
            SelectObject(buf, old);
            DeleteObject(bmp);
            DeleteDC(buf);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == kPopupTimer) {
                if (g->popupView == 1 && GetTickCount64() - g->overallAt > 5000) countOverall();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT pt = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            for (const auto& [r, view] : g->popupTabs)
                if (PtInRect(&r, pt) && g->popupView != view) {
                    g->popupView = view;
                    WritePrivateProfileStringW(L"imgdupe", L"indexerPopupView", std::to_wstring(view).c_str(),
                                               (g->dataDir + L"\\imgdupe.ini").c_str());
                    if (view == 1) countOverall();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            ShowWindow(hwnd, SW_HIDE);
            openGui();
            return 0;
        case WM_RBUTTONUP:
            showMenu();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) ShowWindow(hwnd, SW_HIDE);  // ほかを触ったら閉じる
            return 0;
        case WM_SHOWWINDOW:
            if (wp) SetTimer(hwnd, kPopupTimer, 1000, nullptr);
            else KillTimer(hwnd, kPopupTimer);
            break;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) ShowWindow(hwnd, SW_HIDE);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// アイコンの近く（画面の右下など、作業領域の中）に出す
void togglePopup() {
    if (!g->popup) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = popupProc;
        wc.hInstance = g->inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_DBLCLKS | CS_DROPSHADOW;
        wc.lpszClassName = kPopupClass;
        RegisterClassExW(&wc);
        g->popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPopupClass, L"imgdupe", WS_POPUP | WS_BORDER, 0, 0, 10, 10,
                                   nullptr, nullptr, g->inst, nullptr);
        if (!g->popup) return;
        g->popupView = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"imgdupe", L"indexerPopupView", 0,
                                                                         (g->dataDir + L"\\imgdupe.ini").c_str())),
                                  0, 1);
    }
    if (IsWindowVisible(g->popup)) {
        ShowWindow(g->popup, SW_HIDE);
        return;
    }
    const int w = popupPx(380), h = popupPx(400);
    POINT pt;
    GetCursorPos(&pt);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
    const int x = std::clamp(static_cast<int>(pt.x - w / 2), static_cast<int>(work.left), static_cast<int>(work.right - w));
    const int y = pt.y > (work.top + work.bottom) / 2 ? static_cast<int>(work.bottom - h - popupPx(8)) : static_cast<int>(work.top + popupPx(8));
    SetWindowPos(g->popup, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(g->popup);
    if (g->popupView == 1) countOverall();
    InvalidateRect(g->popup, nullptr, FALSE);
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
    {
        HMENU sub = CreatePopupMenu();
        for (int i = kLoadLight; i <= kLoadHeavy; ++i)
            AppendMenuW(sub, MF_STRING | (loadLevel(g->dataDir) == i ? MF_CHECKED : 0), ID_LOAD0 + i, loadLevelName(i));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), L"処理の負荷(&L)");
    }
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
        default:
            if (cmd >= ID_LOAD0 + kLoadLight && cmd <= ID_LOAD0 + kLoadHeavy && static_cast<int>(cmd - ID_LOAD0) != g->load) {
                setLoadLevel(g->dataDir, static_cast<int>(cmd - ID_LOAD0));
                // 今の回を止めて、新しい負荷でやり直す
                g->cancelRun = true;
                g->dirty = true;
            }
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
            else if (LOWORD(lp) == WM_LBUTTONUP) togglePopup();  // クリックで状況の窓
            else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
                if (g->popup) ShowWindow(g->popup, SW_HIDE);
                openGui();
            }
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
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 起動し直しの時は、古い版が終わるのを待つ（古い版が長い処理の途中だと時間がかかるので最大 30 分。
        // 待ちきれずにやめると、古い版が終わった後に何も動いていない状態になっていた）
        const DWORD w = waitForOld ? WaitForSingleObject(mutex, 30 * 60 * 1000) : WAIT_TIMEOUT;
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
            CloseHandle(mutex);
            return 0;  // もう動いている
        }
    }
    // プロセスごと優先度を決める（ONNX Runtime が作るスレッドも含めて。控えめなら CPU とディスクの優先度を下げる）
    applyProcessPriority(loadLevel(dataDir));
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

int loadLevel(const std::wstring& dataDir) {
    const int v = static_cast<int>(GetPrivateProfileIntW(L"imgdupe", L"indexerLoad", kLoadLight, (dataDir + L"\\imgdupe.ini").c_str()));
    return std::clamp(v, static_cast<int>(kLoadLight), static_cast<int>(kLoadHeavy));
}

void setLoadLevel(const std::wstring& dataDir, int level) {
    level = std::clamp(level, static_cast<int>(kLoadLight), static_cast<int>(kLoadHeavy));
    WritePrivateProfileStringW(L"imgdupe", L"indexerLoad", std::to_wstring(level).c_str(), (dataDir + L"\\imgdupe.ini").c_str());
}

const wchar_t* loadLevelName(int level) {
    switch (level) {
        case kLoadMedium: return L"そこそこ（操作中もコアの半分を使う）";
        case kLoadHeavy: return L"しっかり（ゲーム並み。いつも全部のコア。全画面・バッテリーでも止めない）";
        default: return L"控えめ（操作中は 1 スレッドだけ）";
    }
}

}  // namespace indexer
