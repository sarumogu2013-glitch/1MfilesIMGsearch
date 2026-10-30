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
#include <deque>
#include <mutex>
#include <thread>

#include "clip.hpp"
#include "db.hpp"
#include "image_win.hpp"
#include "indexer.hpp"
#include "log.hpp"
#include "pathinfer.hpp"
#include "tagstore.hpp"
#include "log_win.hpp"
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
constexpr UINT WM_SHOW_TASKS = WM_APP + 2;  // GUI から「スキャンタスクの一覧」
constexpr UINT_PTR kTipTimer = 1;
constexpr ULONGLONG kChangeQuietMs = 10 * 1000;       // フォルダが変わってから、これだけ静かになったら調べる
constexpr ULONGLONG kRescanMs = 30 * 60 * 1000;       // 変化が無くても、これだけたったら調べ直す
constexpr DWORD kIdleMs = 60 * 1000;                  // これだけ操作が無ければ全力で処理する

enum : UINT { ID_PAUSE = 1, ID_RESCAN, ID_OPEN_GUI, ID_QUIT, ID_TASKS, ID_LOGS, ID_LOAD0 = 100 };  // ID_LOAD0 + 負荷
enum : UINT { ID_TASK_TOP = 1, ID_TASK_UP, ID_TASK_DOWN, ID_TASK_BOTTOM, ID_TASK_REMOVE };

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
    std::atomic<bool> dirty{true};             // 調べ直しが要る（フォルダ全体）
    std::atomic<bool> inferDirty{false};       // フォルダ名からの推し量りだけやり直す（教えた形が変わった）
    std::atomic<bool> compactDb{false};        // DB を詰める（画面の「DB を詰めて小さくする」）
    std::atomic<bool> tasksDirty{true};        // スキャンタスクが足された
    std::atomic<bool> preempt{false};          // 今のタスクより上に別のタスクが来た（indexOnce が切れ目で譲る）
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
    int popupView = 0;                          // 0 = 新規作成中、1 = 総合、2 = タスク
    std::vector<std::pair<RECT, int>> popupTabs;
    struct OverallRow {
        std::wstring name;
        StageCount count;
    };
    std::vector<OverallRow> overall;            // 総合（m で守る）
    int64_t overallImages = -1;                 // DB の画像の数（-1 = まだ数えていない。m で守る）
    ULONGLONG overallAt = 0;                    // 最後に数えた時刻
    std::atomic<bool> counting{false};
    // スキャンタスクの一覧（UI のスレッドだけで使う）
    Db uiDb;
    bool uiDbOpen = false;
    std::vector<ScanTask> tasks;
    std::vector<std::pair<RECT, int64_t>> taskRows;  // 見えている行（ID）
    int64_t taskSel = 0;
    int taskScroll = 0;                              // 上に送った量（px）
    int taskDragFrom = -1, taskDropAt = -1;           // ドラッグで並べ替え中（行の番号・入れる所）
    POINT taskDragPt{};
    bool taskMenuOpen = false;                        // 行のメニューを出している間は閉じない
    // 速さ（CPU の側・GPU の側。窓を描くたびに数を覚えて、最近 1 分の増え方から出す）
    struct Rate {
        int phase = -1;
        std::deque<std::pair<ULONGLONG, uint64_t>> samples;
    } rates[2];
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

// スキャンタスクの並びが変わった時: 今のタスク（かフォルダ全体の確認）より上に別のものが来たら、切れ目で譲ってもらう
void checkPreempt(const std::vector<ScanTask>& tasks) {
    if (!g->running) return;
    const int64_t cur = g->prog.task;
    if (cur > 0 ? (tasks.empty() || tasks.front().id != cur) : cur == 0 && !tasks.empty()) g->preempt = true;
}

void checkPreempt(Db& db) {
    try {
        checkPreempt(scanTasks(db));
    } catch (const std::exception&) {
    }
}

// GUI からの指示
void handleCommand(Db& db, const std::string& c) {
    logs::info("画面からの指示: " + c);
    if (c == "quit") g->quit = true;
    else if (c == "rescan") g->dirty = true;
    else if (c == "infer") g->inferDirty = true;
    else if (c == "compact") g->compactDb = true;
    else if (c == "pause") g->paused = true;
    else if (c == "resume") g->paused = false;
    else if (c == "tasks") {
        g->tasksDirty = true;
        checkPreempt(db);
    } else if (c == "showtasks") PostMessageW(g->hwnd, WM_SHOW_TASKS, 0, 0);
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

void runOnce(const std::vector<std::wstring>& roots, bool full) {
    IndexOptions opt;
    opt.fullPass = full;
    opt.preempt = &g->preempt;
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
        const std::wstring w = widen(root);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &fa) && !(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            // スキャンタスクのファイル 1 つ
            if (!isImageExt(root)) return;
            ScannedFile f;
            f.path = root;
            f.size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
            f.mtime = static_cast<int64_t>((static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) |
                                           fa.ftLastWriteTime.dwLowDateTime);
            files.push_back(std::move(f));
            found.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        imgwin::scanFolder(w, files, found, g->cancelRun);
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
        if (r.analyzer) logs::info("タグ付けのモデルを読んだ（" + narrow(r.device) + "）");
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
        if (r.analyzer) logs::info("CLIP のモデルを読んだ（" + narrow(r.device) + "）");
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
                    for (const auto& c : takeCommands(cmdDb)) handleCommand(cmdDb, c);
                } catch (const std::exception&) {
                }
            }
            if (!g->cancelRun && (g->paused || g->quit || !busyReason().empty())) {
                logs::info(std::string("処理を止める: ") + (g->quit ? "終了の指示" : g->paused ? "一時停止" : narrow(busyReason())));
                g->cancelRun = true;
            }
            Sleep(1000);
        }
    });
    g->running = true;
    const bool background = g->load <= kLoadLight;
    if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);  // CPU とディスクの優先度を下げる
    try {
        const IndexReport r = indexOnce(opt, cb, g->prog, g->cancelRun);
        if (r.cancelled) {
            if (full) g->dirty = true;
            g->tasksDirty = true;
        } else {
            wchar_t t[240];
            SYSTEMTIME now;
            GetLocalTime(&now);
            if (full)
                swprintf(t, 240, L"%02d:%02d に完了（画像 %zu 枚、追加 %zu・削除 %zu、ハッシュ %zu、サムネイル %zu、メタデータ %zu）",
                         now.wHour, now.wMinute, r.files, r.added, r.removed, r.hashed, r.thumbs, r.metas);
            else
                swprintf(t, 240, L"%02d:%02d にスキャンタスク %zu 個が完了（画像 %zu 枚）", now.wHour, now.wMinute, r.tasks, r.files);
            setStatus(&g->lastResult, t);
        }
    } catch (const std::exception& e) {
        logs::error(std::string("解析の途中でエラー: ") + e.what());
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
            for (const auto& c : takeCommands(db)) handleCommand(db, c);
            const std::vector<std::string> r = getRoots(db);
            if (r != rootsUtf8) {
                logs::info("調べるフォルダ: " + std::to_string(r.size()) + " 個");
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
        bool haveTasks = false;
        if (g->tasksDirty) {
            try {
                haveTasks = !scanTasks(db).empty();
            } catch (const std::exception&) {
            }
            if (!haveTasks) g->tasksDirty = false;
        }
        const bool full = g->dirty && !rootsUtf8.empty();
        std::wstring reason = g->paused                         ? L"一時停止中"
                              : rootsUtf8.empty() && !haveTasks ? L"調べるフォルダがありません"
                                                                : busyReason();
        {
            static std::wstring lastReason = L"-";
            if (reason != lastReason) {
                if (!reason.empty()) logs::info("待つ: " + narrow(reason));
                lastReason = reason;
            }
        }
        setStatus(&g->waitReason, reason);
        if ((full || haveTasks) && reason.empty()) {
            if (full) g->dirty = false;
            g->tasksDirty = false;
            g->cancelRun = false;
            g->preempt = false;
            std::vector<std::wstring> roots;
            {
                std::lock_guard<std::mutex> lock(g->m);
                roots = g->roots;
            }
            runOnce(roots, full);
            lastRun = GetTickCount64();
            continue;
        }
        if (g->compactDb && !g->paused) {
            // DB を詰める（終わるまで解析はしない）
            g->compactDb = false;
            setStatus(&g->waitReason, L"DB を詰めています（数分かかることがあります。終わるまで解析は止まります）");
            const DbSpace before = dbSpace(db);
            const auto t0 = GetTickCount64();
            std::string error;
            if (compactDb(db, error)) {
                const DbSpace after = dbSpace(db);
                char t[160];
                std::snprintf(t, sizeof t, "DB を詰めた: %.0f MB → %.0f MB（%llu 秒）", before.fileBytes / 1048576.0,
                              after.fileBytes / 1048576.0, static_cast<unsigned long long>((GetTickCount64() - t0) / 1000));
                logs::info(t);
                setStatus(&g->lastResult, widen(t));
            } else {
                logs::error("DB を詰められなかった: " + error);
                setStatus(&g->lastResult, L"DB を詰められませんでした: " + widen(error));
            }
            setStatus(&g->waitReason, L"");
            continue;
        }
        if (g->inferDirty && !g->paused) {
            // 教えた形が変わった: フォルダ名からの推し量りだけやり直す（走査はしない）
            g->inferDirty = false;
            try {
                const InferSummary r = runFolderInference(db, false, &g->quit);  // 終わる時は止める
                logs::info("フォルダ名から推し量り直した: 変わったフォルダ " + std::to_string(r.changedDirs) + " 個・画像 " +
                           std::to_string(r.images) + " 枚");
            } catch (const std::exception& e) {
                logs::error(std::string("フォルダ名から推し量れなかった: ") + e.what());
            }
            continue;
        }
        std::unique_lock<std::mutex> lock(g->m);
        g->cv.wait_for(lock, std::chrono::seconds(5), [] { return g->quit.load(); });
    }
    imgwin::threadEnd();
    PostMessageW(g->hwnd, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------- タスクトレイ

std::wstring laneText(IndexPhase p, const Progress& pr, const std::wstring& device);

std::wstring statusText() {
    std::wstring wait, last, taggerDevice;
    {
        std::lock_guard<std::mutex> lock(g->m);
        taggerDevice = g->taggerDevice;
        wait = g->waitReason;
        last = g->lastResult;
    }
    if (g->running) {
        // CPU の側と GPU の側（同時に進めている時は両方）
        std::wstring t = laneText(static_cast<IndexPhase>(g->prog.phase.load()), g->prog.prog, taggerDevice);
        const std::wstring gpu = laneText(static_cast<IndexPhase>(g->prog.gpuPhase.load()), g->prog.gpuProg, taggerDevice);
        if (!gpu.empty()) t += (t.empty() ? L"" : L" ／ ") + gpu;
        return t.empty() ? L"処理中" : t;
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
        case IndexPhase::Convert: return L"DB の変換（タグの置き場所を新しい形に。初回だけ）";
        default: return L"";
    }
}

// 1 つの側（CPU / GPU）の今の段階と進み具合。何もしていなければ空
std::wstring laneText(IndexPhase p, const Progress& pr, const std::wstring& device) {
    if (p == IndexPhase::Idle || p == IndexPhase::Done) return L"";
    std::wstring t = phaseName(p);
    if ((p == IndexPhase::Tagger || p == IndexPhase::Clip) && !device.empty()) t += L"（" + device + L"）";
    const uint64_t done = pr.done, total = pr.total;
    if (p == IndexPhase::Scan) t += L"（" + std::to_wstring(total) + L" 枚）";
    else if (p != IndexPhase::Merge && total) t += L" " + std::to_wstring(done) + L" / " + std::to_wstring(total);
    return t;
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

// 「約 120 枚/秒・残り 約 3 時間 20 分」（まだ測れていなければ空）
std::wstring rateText(int lane, int phase, uint64_t done, uint64_t total) {
    Resident::Rate& r = g->rates[lane];
    const ULONGLONG now = GetTickCount64();
    if (r.phase != phase || (!r.samples.empty() && done < r.samples.back().second)) r.samples.clear();
    r.phase = phase;
    if (r.samples.empty() || now - r.samples.back().first >= 900) r.samples.emplace_back(now, done);
    while (r.samples.size() > 2 && now - r.samples.front().first > 60 * 1000) r.samples.pop_front();
    if (r.samples.size() < 3) return L"";
    const double sec = double(r.samples.back().first - r.samples.front().first) / 1000.0;
    const double perSec = sec > 0 ? double(r.samples.back().second - r.samples.front().second) / sec : 0;
    wchar_t t[96];
    if (perSec < 0.05) return L"速さを測っています…";
    const double left = total > done ? double(total - done) / perSec : 0;
    const long long mins = static_cast<long long>(left / 60 + 0.5);
    if (perSec >= 10) swprintf(t, 96, L"約 %.0f 枚/秒", perSec);
    else swprintf(t, 96, L"約 %.1f 枚/秒", perSec);
    std::wstring out = t;
    if (mins >= 60 * 24) swprintf(t, 96, L"・残り 約 %lld 日 %lld 時間", mins / (60 * 24), mins / 60 % 24);
    else if (mins >= 60) swprintf(t, 96, L"・残り 約 %lld 時間 %lld 分", mins / 60, mins % 60);
    else swprintf(t, 96, L"・残り 約 %lld 分", std::max(1LL, mins));
    return out + t;
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

// スキャンタスクの一覧を読み直す（UI のスレッド）
void refreshTasks() {
    try {
        if (!g->uiDbOpen) g->uiDbOpen = g->uiDb.open(dbFileOf(g->dataDir));
        if (g->uiDbOpen) g->tasks = scanTasks(g->uiDb);
    } catch (const std::exception&) {
    }
}

const ScanTask* findTask(int64_t id) {
    for (const auto& t : g->tasks)
        if (t.id == id) return &t;
    return nullptr;
}

// 一覧の行の高さと、一覧の場所（paintPopup が決める）
int taskRowH() { return popupPx(46); }
int g_taskTop = 0, g_taskBottom = 0;

// 並びを変えた・外した後: 今の処理より上に来たものがあれば譲ってもらう
void afterTaskChange() {
    refreshTasks();
    checkPreempt(g->tasks);
    g->tasksDirty = true;
    g->cv.notify_all();
    if (g->popup) InvalidateRect(g->popup, nullptr, FALSE);
}

std::wstring taskPathsText(const ScanTask& t) {
    if (t.paths.empty()) return L"";
    std::wstring s = widen(t.paths.front());
    if (t.paths.size() > 1) s += L" ほか " + std::to_wstring(t.paths.size() - 1) + L" 個";
    return s;
}

void paintTasks(HDC dc, const RECT& rc, int m, int y, HFONT font, HFONT bold) {
    const int rowH = taskRowH();
    g_taskTop = y;
    g_taskBottom = rc.bottom - m - popupPx(22);
    g->taskRows.clear();
    const bool running = g->running;
    const int64_t cur = running ? g->prog.task.load() : -1;
    const size_t n = g->tasks.size();
    const int total = static_cast<int>(n + 1) * rowH;
    g->taskScroll = std::clamp(g->taskScroll, 0, std::max(0, total - (g_taskBottom - g_taskTop)));
    HRGN clip = CreateRectRgn(0, g_taskTop, rc.right, g_taskBottom);
    SelectClipRgn(dc, clip);
    for (size_t i = 0; i <= n; ++i) {
        const int top = g_taskTop + static_cast<int>(i) * rowH - g->taskScroll;
        if (top + rowH < g_taskTop || top > g_taskBottom) continue;
        RECT row = {m, top, rc.right - m, top + rowH - popupPx(4)};
        const bool fixed = i == n;  // 最後の行: いつものフォルダ全体の確認（動かせない）
        const int64_t id = fixed ? 0 : g->tasks[i].id;
        const bool now = cur == id && (fixed ? cur == 0 : true);
        const bool sel = !fixed && g->taskSel == id;
        HBRUSH b = CreateSolidBrush(sel ? RGB(220, 234, 252) : now ? RGB(232, 245, 236) : RGB(255, 255, 255));
        HPEN pen = CreatePen(PS_SOLID, 1, sel ? RGB(0, 120, 215) : RGB(215, 220, 228));
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
        RoundRect(dc, row.left, row.top, row.right, row.bottom, popupPx(6), popupPx(6));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b);
        DeleteObject(pen);
        // 番号
        RECT nr = {row.left + popupPx(6), row.top, row.left + popupPx(30), row.bottom};
        SetTextColor(dc, fixed ? RGB(150, 150, 150) : RGB(0, 100, 220));
        SelectObject(dc, bold);
        DrawTextW(dc, fixed ? L"―" : std::to_wstring(i + 1).c_str(), -1, &nr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT lr = {row.left + popupPx(34), row.top + popupPx(4), row.right - popupPx(6), row.top + popupPx(22)};
        SetTextColor(dc, fixed ? RGB(110, 110, 110) : RGB(30, 30, 30));
        const std::wstring label = fixed ? L"フォルダ全体の確認（いつもの処理）" : widen(g->tasks[i].label);
        DrawTextW(dc, label.c_str(), -1, &lr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, font);
        RECT sr = {lr.left, lr.bottom, lr.right, row.bottom - popupPx(2)};
        std::wstring sub;
        if (now) {
            std::wstring lanes = laneText(static_cast<IndexPhase>(g->prog.phase.load()), g->prog.prog, L"");
            const std::wstring gpu = laneText(static_cast<IndexPhase>(g->prog.gpuPhase.load()), g->prog.gpuProg, L"");
            if (!gpu.empty()) lanes += (lanes.empty() ? L"" : L" ・ ") + gpu;
            sub = L"▶ 処理中: " + lanes;
            SetTextColor(dc, RGB(30, 130, 60));
        } else {
            sub = fixed ? L"タスクが無くなってから、調べるフォルダ全体の変化を見ます" : taskPathsText(g->tasks[i]);
            SetTextColor(dc, RGB(120, 120, 120));
        }
        DrawTextW(dc, sub.c_str(), -1, &sr,
                  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | (fixed || now ? DT_END_ELLIPSIS : DT_PATH_ELLIPSIS));
        if (!fixed) g->taskRows.push_back({row, id});
    }
    // ドラッグで入れる所
    if (g->taskDropAt >= 0) {
        const int ly = g_taskTop + g->taskDropAt * rowH - g->taskScroll - popupPx(3);
        fillRect(dc, {m, ly, rc.right - m, ly + popupPx(3)}, RGB(0, 120, 215));
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    if (n == 0) {
        RECT er = {m, g_taskTop + rowH + popupPx(6), rc.right - m, g_taskBottom};
        SetTextColor(dc, RGB(120, 120, 120));
        DrawTextW(dc,
                  L"スキャンタスクはありません。imgdupe でフォルダやファイルを右クリックして「スキャンタスクに追加」で足せます。",
                  -1, &er, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
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
        const std::wstring names[] = {L"新規作成中", L"総合", L"タスク（" + std::to_wstring(g->tasks.size()) + L"）"};
        for (int k = 0; k < 3; ++k) {
            SIZE sz{};
            GetTextExtentPoint32W(dc, names[k].c_str(), static_cast<int>(names[k].size()), &sz);
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
            DrawTextW(dc, names[k].c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
            const auto gpuPhase = static_cast<IndexPhase>(g->prog.gpuPhase.load());
            const int64_t task = g->prog.task;
            if (task == 0) text(L"対象: フォルダ全体の確認", RGB(90, 90, 90));
            else if (const ScanTask* t = task > 0 ? findTask(task) : nullptr) text(L"対象: スキャンタスク「" + widen(t->label) + L"」", RGB(90, 90, 90));
            // CPU の側と GPU の側（同時に進める）。それぞれ段階の名前と進み具合の棒
            auto lane = [&](const wchar_t* side, IndexPhase p, const Progress& pr, int laneNo) {
                if (p == IndexPhase::Idle || p == IndexPhase::Done) return;
                SelectObject(dc, bold);
                std::wstring now = std::wstring(side) + phaseName(p);
                if ((p == IndexPhase::Tagger || p == IndexPhase::Clip) && !device.empty()) now += L"（" + device + L"）";
                text(now, RGB(30, 30, 30));
                SelectObject(dc, font);
                const uint64_t done = pr.done, total = pr.total;
                if (p != IndexPhase::Scan && total) {
                    drawBar(dc, {m, y, rc.right - m, y + popupPx(10)}, double(done) / double(total), RGB(0, 120, 215));
                    y += popupPx(14);
                    wchar_t t[96];
                    swprintf(t, 96, L"%llu / %llu 枚（%.0f%%）", static_cast<unsigned long long>(done), static_cast<unsigned long long>(total),
                             100.0 * double(done) / double(total));
                    const std::wstring rate = rateText(laneNo, static_cast<int>(p), done, total);
                    text(rate.empty() ? std::wstring(t) : std::wstring(t) + L"　" + rate, RGB(60, 60, 60));
                } else if (p == IndexPhase::Scan) {
                    text(std::to_wstring(total) + L" 枚見つけました", RGB(60, 60, 60));
                }
            };
            const bool both = gpuPhase != IndexPhase::Idle;
            if (both && phase == IndexPhase::Idle) text(L"CPU: 済み（GPU の側を待っています）", RGB(90, 90, 90));
            lane(both ? L"CPU: " : L"", phase, g->prog.prog, 0);
            lane(L"GPU: ", gpuPhase, g->prog.gpuProg, 1);
            // 段階の並び（済んだもの・今・まだ）
            y += popupPx(4);
            const unsigned mask = g->prog.doneMask;
            const IndexPhase order[] = {IndexPhase::Scan, IndexPhase::Hash, IndexPhase::Thumb, IndexPhase::Meta, IndexPhase::Names,
                                        IndexPhase::Tagger, IndexPhase::Clip, IndexPhase::Quality, IndexPhase::Variants,
                                        IndexPhase::Lookup, IndexPhase::Merge};
            const bool finishing = phase == IndexPhase::Variants || phase == IndexPhase::Lookup || phase == IndexPhase::Merge;
            for (IndexPhase p : order) {
                const bool cur = p == phase || p == gpuPhase;
                const bool past = !cur && (p == IndexPhase::Scan ? phase != IndexPhase::Scan
                                           : finishing && static_cast<int>(p) < static_cast<int>(phase) ? true
                                                                                                         : (mask >> static_cast<int>(p)) & 1u);
                RECT r = {m, y, rc.right - m, y + popupPx(18)};
                SetTextColor(dc, cur ? RGB(0, 100, 220) : past ? RGB(40, 140, 60) : RGB(150, 150, 150));
                const std::wstring line = std::wstring(cur ? L"▶ " : past ? L"✓ " : L"・ ") + phaseName(p);
                DrawTextW(dc, line.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
                y += popupPx(18);
            }
        } else {
            text(last.empty() ? L"まだ新しく作るものはありません" : L"前回: " + last, RGB(60, 60, 60));
        }
    } else if (g->popupView == 2) {
        paintTasks(dc, rc, m, y, font, bold);
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
    DrawTextW(dc,
              g->popupView == 2 ? L"ドラッグで並べ替え、右クリックで一番上へ・外す など" : L"右クリックでメニュー、ダブルクリックで imgdupe を開く",
              -1, &hr, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteObject(bold);
}

void showMenu();
void openGui();

// スキャンタスクの行の右クリック
void taskMenu(HWND hwnd, int64_t id) {
    size_t index = 0;
    for (size_t i = 0; i < g->tasks.size(); ++i)
        if (g->tasks[i].id == id) index = i;
    const bool first = index == 0, last = index + 1 >= g->tasks.size();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (first ? MF_GRAYED : 0), ID_TASK_TOP, L"一番上へ（最優先）(&T)");
    AppendMenuW(menu, MF_STRING | (first ? MF_GRAYED : 0), ID_TASK_UP, L"1 つ上へ(&U)");
    AppendMenuW(menu, MF_STRING | (last ? MF_GRAYED : 0), ID_TASK_DOWN, L"1 つ下へ(&D)");
    AppendMenuW(menu, MF_STRING | (last ? MF_GRAYED : 0), ID_TASK_BOTTOM, L"一番下へ(&B)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TASK_REMOVE, L"タスクから外す(&R)");
    POINT pt;
    GetCursorPos(&pt);
    g->taskMenuOpen = true;
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    g->taskMenuOpen = false;
    DestroyMenu(menu);
    if (!cmd || !g->uiDbOpen) return;
    try {
        switch (cmd) {
            case ID_TASK_TOP: moveScanTask(g->uiDb, id, TaskMove::Top); break;
            case ID_TASK_UP: moveScanTask(g->uiDb, id, TaskMove::Up); break;
            case ID_TASK_DOWN: moveScanTask(g->uiDb, id, TaskMove::Down); break;
            case ID_TASK_BOTTOM: moveScanTask(g->uiDb, id, TaskMove::Bottom); break;
            case ID_TASK_REMOVE: removeScanTask(g->uiDb, id); break;
        }
    } catch (const std::exception&) {
    }
    afterTaskChange();
    if (GetForegroundWindow() != hwnd) SetForegroundWindow(hwnd);
}

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
                if (!IsWindowVisible(hwnd)) {
                    KillTimer(hwnd, kPopupTimer);
                    return 0;
                }
                if (g->popupView == 1 && GetTickCount64() - g->overallAt > 5000) countOverall();
                if (g->taskDragFrom < 0) refreshTasks();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT pt = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            for (const auto& [r, view] : g->popupTabs)
                if (PtInRect(&r, pt)) {
                    if (g->popupView != view) {
                        g->popupView = view;
                        WritePrivateProfileStringW(L"imgdupe", L"indexerPopupView", std::to_wstring(view).c_str(),
                                                   (g->dataDir + L"\\imgdupe.ini").c_str());
                        if (view == 1) countOverall();
                        refreshTasks();
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
            if (g->popupView == 2)
                for (size_t i = 0; i < g->taskRows.size(); ++i)
                    if (PtInRect(&g->taskRows[i].first, pt)) {
                        g->taskSel = g->taskRows[i].second;
                        for (size_t k = 0; k < g->tasks.size(); ++k)
                            if (g->tasks[k].id == g->taskSel) g->taskDragFrom = static_cast<int>(k);
                        g->taskDragPt = pt;
                        SetCapture(hwnd);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        break;
                    }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (g->taskDragFrom >= 0 && GetCapture() == hwnd) {
                const int y = static_cast<short>(HIWORD(lp));
                if (g->taskDropAt < 0 && std::abs(y - g->taskDragPt.y) < GetSystemMetrics(SM_CYDRAG)) return 0;
                const int at = (y - g_taskTop + g->taskScroll + taskRowH() / 2) / taskRowH();
                g->taskDropAt = std::clamp(at, 0, static_cast<int>(g->tasks.size()));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (g->taskDragFrom >= 0) {
                const int from = g->taskDragFrom, at = g->taskDropAt;
                g->taskDragFrom = g->taskDropAt = -1;
                if (GetCapture() == hwnd) ReleaseCapture();
                if (at >= 0 && at != from && at != from + 1 && from < static_cast<int>(g->tasks.size())) {
                    std::vector<int64_t> ids;
                    for (const auto& t : g->tasks) ids.push_back(t.id);
                    const int64_t moved = ids[from];
                    ids.erase(ids.begin() + from);
                    ids.insert(ids.begin() + (at > from ? at - 1 : at), moved);
                    try {
                        if (g->uiDbOpen) setScanTaskOrder(g->uiDb, ids);
                    } catch (const std::exception&) {
                    }
                    afterTaskChange();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (g->taskDragFrom >= 0 && reinterpret_cast<HWND>(lp) != hwnd) {
                g->taskDragFrom = g->taskDropAt = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (g->popupView == 2) {
                g->taskScroll -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * taskRowH();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            ShowWindow(hwnd, SW_HIDE);
            openGui();
            return 0;
        case WM_RBUTTONUP: {
            POINT pt = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            if (g->popupView == 2)
                for (const auto& [r, id] : g->taskRows)
                    if (PtInRect(&r, pt)) {
                        g->taskSel = id;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        taskMenu(hwnd, id);
                        return 0;
                    }
            showMenu();
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && !g->taskMenuOpen) ShowWindow(hwnd, SW_HIDE);  // ほかを触ったら閉じる
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

// アイコンの近く（画面の右下など、作業領域の中）に出す。toggle なら、出ている時は隠す
void togglePopup(bool toggle = true, int view = -1) {
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
                                  0, 2);
    }
    if (view >= 0) g->popupView = view;
    refreshTasks();
    if (IsWindowVisible(g->popup)) {
        if (toggle) ShowWindow(g->popup, SW_HIDE);
        else InvalidateRect(g->popup, nullptr, FALSE);
        return;
    }
    const int w = popupPx(400), h = popupPx(500);
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
    // SetWindowPos で出した時は WM_SHOWWINDOW が来ないので、ここで 1 秒ごとの描き直しを始める
    SetTimer(g->popup, kPopupTimer, 1000, nullptr);
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
    AppendMenuW(menu, MF_STRING, ID_TASKS, L"スキャンタスクの一覧(&T)");
    {
        HMENU sub = CreatePopupMenu();
        for (int i = kLoadLight; i <= kLoadHeavy; ++i)
            AppendMenuW(sub, MF_STRING | (loadLevel(g->dataDir) == i ? MF_CHECKED : 0), ID_LOAD0 + i, loadLevelName(i));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), L"処理の負荷(&L)");
    }
    AppendMenuW(menu, MF_STRING, ID_OPEN_GUI, L"imgdupe を開く(&O)");
    AppendMenuW(menu, MF_STRING, ID_LOGS, L"動作履歴（ログ）のフォルダを開く(&G)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_QUIT, L"バックグラウンドの解析を終了(&X)");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g->hwnd);  // メニューの外をクリックした時に閉じるように
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g->hwnd, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case ID_PAUSE:
            g->paused = !g->paused;
            logs::info(g->paused ? "トレイから一時停止" : "トレイから再開");
            break;
        case ID_RESCAN: g->dirty = true; break;
        case ID_OPEN_GUI: openGui(); break;
        case ID_TASKS: togglePopup(false, 2); break;
        case ID_QUIT:
            logs::info("トレイから終了");
            g->quit = true;
            break;
        case ID_LOGS: ShellExecuteW(nullptr, L"open", (g->dataDir + L"\\logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        default:
            if (cmd >= ID_LOAD0 + kLoadLight && cmd <= ID_LOAD0 + kLoadHeavy && static_cast<int>(cmd - ID_LOAD0) != g->load) {
                setLoadLevel(g->dataDir, static_cast<int>(cmd - ID_LOAD0));
                logs::info("処理の負荷を変えた: " + narrow(loadLevelName(static_cast<int>(cmd - ID_LOAD0))));
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
            if (wp == kTipTimer) {
                updateTip();
                // 10 分ごとに進み具合を書く（止まっていないかを後から確かめられるように）
                static ULONGLONG lastLog = GetTickCount64();
                if (g->running && GetTickCount64() - lastLog >= 10 * 60 * 1000) {
                    lastLog = GetTickCount64();
                    logs::info("進み具合: " + narrow(statusText()));
                }
            }
            return 0;
        case WM_SHOW_TASKS:
            togglePopup(false, 2);
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
    logwin::start(dataDir, "indexer", "解析");
    {
        const std::wstring ini = dataDir + L"\\imgdupe.ini";
        auto on = [&](const wchar_t* key, int def) { return GetPrivateProfileIntW(L"imgdupe", key, def, ini.c_str()) ? "あり" : "なし"; };
        logs::info("設定: 負荷 " + narrow(loadLevelName(loadLevel(dataDir))) + "、タグ付け " + on(L"taggerEnabled", 1) + "、CLIP " +
                   on(L"clipEnabled", 1) + "、品質 " + on(L"qualityEnabled", 1) + "、CPU だけ " + on(L"taggerCpu", 0) + "、出典の照会 " +
                   on(L"lookupEnabled", 0));
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
        logs::error("ウィンドウを作れなかった（" + std::to_string(GetLastError()) + "）");
        g = nullptr;
        logwin::stop();
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
    logwin::stop();
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

void requestCompact(const std::wstring& dataDir) {
    Db db;
    if (db.open(dbFileOf(dataDir))) addCommand(db, "compact");
}

void requestInfer(const std::wstring& dataDir) {
    Db db;
    if (db.open(dbFileOf(dataDir))) addCommand(db, "infer");
}

void setRoots(const std::wstring& dataDir, const std::vector<std::wstring>& roots) {
    Db db;
    if (!db.open(dbFileOf(dataDir))) return;
    std::vector<std::string> r;
    for (const auto& x : roots) r.push_back(narrow(x));
    if (getRoots(db) != r) imgdupe::setRoots(db, r);
}

void addScanTask(const std::wstring& dataDir, const std::wstring& label, const std::vector<std::wstring>& paths, bool top) {
    Db db;
    if (!db.open(dbFileOf(dataDir)) || paths.empty()) return;
    std::vector<std::string> p;
    for (const auto& x : paths) p.push_back(narrow(x));
    imgdupe::addScanTask(db, narrow(label), p, top);
    addCommand(db, "tasks");
}

void requestShowTasks(const std::wstring& dataDir) {
    Db db;
    if (db.open(dbFileOf(dataDir))) addCommand(db, "showtasks");
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
