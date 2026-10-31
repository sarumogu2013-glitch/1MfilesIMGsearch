#include "indexer.hpp"

#include "archive.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <exception>
#include <functional>
#include <thread>
#include <cstring>
#include <stdexcept>

#include "clip.hpp"
#include "log.hpp"
#include "pathinfer.hpp"
#include "tagstore.hpp"
#include "stats.hpp"
#include "names.hpp"
#include "quality.hpp"

namespace imgdupe {

// ---------------------------------------------------------------- 解析器

AnalyzerOutput HashAnalyzer::analyze(const AnalyzerInput& in) {
    AnalyzerOutput out;
    const Features f = cb_.decode(in.path);
    out.data.resize(sizeof(Features));
    std::memcpy(out.data.data(), &f, sizeof(Features));
    return out;  // 読めない画像も「済み」（readable=0）にする。同じ版で何度も読み直さないため
}

bool HashAnalyzer::store(Db& db, int64_t imageId, AnalyzerOutput& out) {
    if (out.data.size() != sizeof(Features)) return false;
    Features f;
    std::memcpy(&f, out.data.data(), sizeof(Features));
    storeFeatures(db, imageId, f);
    return true;
}

void HashAnalyzer::threadBegin() {
    if (cb_.threadInit) cb_.threadInit();
}
void HashAnalyzer::threadEnd() {
    if (cb_.threadExit) cb_.threadExit();
}

AnalyzerOutput ThumbAnalyzer::analyze(const AnalyzerInput& in) {
    AnalyzerOutput out;
    out.ok = cb_.encodeThumb && cb_.encodeThumb(in.path, kThumbSide, out.data, out.width, out.height);
    return out;
}

bool ThumbAnalyzer::store(Db& db, int64_t imageId, AnalyzerOutput& out) {
    ThumbData t;
    t.format = ThumbFormat::Jpeg;
    t.width = static_cast<uint16_t>(out.width);
    t.height = static_cast<uint16_t>(out.height);
    t.bytes = std::move(out.data);
    return pack_.put(db, imageId, t);
}

void ThumbAnalyzer::threadBegin() {
    if (cb_.threadInit) cb_.threadInit();
}
void ThumbAnalyzer::threadEnd() {
    if (cb_.threadExit) cb_.threadExit();
}

// ---------------------------------------------------------------- 総合の進み具合

int64_t imageCount(Db& db) {
    Stmt st(db.handle(), "SELECT count(*) FROM images");
    return st.step() ? st.i64(0) : 0;
}

StageCount stageCount(Db& db, const std::string& stage, int version) {
    StageCount c;
    Stmt st(db.handle(), "SELECT state, count(*) FROM stages WHERE stage=? AND version=? GROUP BY state");
    st.bind(1, stage).bind(2, static_cast<int64_t>(version));
    while (st.step()) {
        if (st.i64(0) == static_cast<int64_t>(StageState::Done)) c.done = st.i64(1);
        else if (st.i64(0) == static_cast<int64_t>(StageState::Failed)) c.failed = st.i64(1);
    }
    return c;
}

// ---------------------------------------------------------------- 1 回分

namespace {

enum class Outcome { Done, Cancelled, Yielded };

// 1 回の間、重いモデル（タガー・CLIP）は 1 度だけ読む（スキャンタスクごとに読み直さない）
struct RunState {
    const IndexOptions& opt;
    const IndexCallbacks& cb;
    IndexProgress& progress;
    const std::atomic<bool>& cancel;
    IndexReport& rep;
    std::shared_ptr<Analyzer> tagger, clip;
    bool taggerTried = false, clipTried = false, clipPrepared = false;
};

Outcome outcomeOf(const StageReport& sr) {
    return sr.yielded ? Outcome::Yielded : sr.cancelled ? Outcome::Cancelled : Outcome::Done;
}

Outcome combine(Outcome a, Outcome b) {
    if (a == Outcome::Cancelled || b == Outcome::Cancelled) return Outcome::Cancelled;
    if (a == Outcome::Yielded || b == Outcome::Yielded) return Outcome::Yielded;
    return Outcome::Done;
}

// 1 つの段階を流す。済んだら progress.doneMask に印を付ける
const char* phaseLogName(IndexPhase p) {
    switch (p) {
        case IndexPhase::Scan: return "フォルダを調べる";
        case IndexPhase::Hash: return "ハッシュ";
        case IndexPhase::Thumb: return "サムネイル";
        case IndexPhase::Meta: return "メタデータ";
        case IndexPhase::Names: return "json・フォルダ名";
        case IndexPhase::Tagger: return "タグ付け";
        case IndexPhase::Clip: return "CLIP";
        case IndexPhase::Variants: return "差分のまとまり";
        case IndexPhase::Quality: return "品質";
        case IndexPhase::Lookup: return "出典の照会";
        case IndexPhase::Merge: return "仕上げ";
        case IndexPhase::Convert: return "タグの置き場所の変換";
        default: return "?";
    }
}

double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// タグの置き場所を新しい形に変換する（済んでいれば何もしない）。止めたら false
bool convertTagStore(Db& db, IndexProgress& progress, const std::atomic<bool>& cancel) {
    TagConversionState st = tagConversionState(db);
    if (!st.pending) {
        // v2.18 で変換した DB: 索引を作り直す（タグの検索が速くなる）
        if (!tagIndexUpgradePending(db)) return true;
        if (cancel) return false;
        progress.phase = static_cast<int>(IndexPhase::Convert);
        progress.prog.done = progress.prog.total = 0;
        logs::info("タグの索引を作り直す（数分かかることがある）");
        const auto t0 = std::chrono::steady_clock::now();
        try {
            upgradeTagIndex(db);
        } catch (const std::exception& e) {
            logs::error(std::string("タグの索引を作り直せなかった（次の回でやり直す）: ") + e.what());
            return false;
        }
        char t[96];
        std::snprintf(t, sizeof t, "タグの索引を作り直した（%.0f 秒）", secondsSince(t0));
        logs::info(t);
        return true;
    }
    progress.phase = static_cast<int>(IndexPhase::Convert);
    progress.prog.done = static_cast<uint64_t>(st.done);
    progress.prog.total = static_cast<uint64_t>(st.total);
    logs::info("タグの置き場所を新しい形に変換する（画像の ID " + std::to_string(st.done) + " / " + std::to_string(st.total) + " から）");
    const auto t0 = std::chrono::steady_clock::now();
    std::string step;
    for (;;) {
        if (cancel) {
            logs::info("タグの変換を途中で止めた（続きは次の回で）");
            return false;
        }
        bool finished = false;
        try {
            finished = convertTagsStep(db, 5000, &step);
        } catch (const std::exception& e) {
            logs::error(std::string("タグの変換でエラー（次の回でやり直す）: ") + e.what());
            return false;
        }
        if (finished) break;
        st = tagConversionState(db);
        progress.prog.done = static_cast<uint64_t>(st.done);
        progress.prog.total = static_cast<uint64_t>(st.total);
        if (st.done >= st.total) logs::info("タグを移し終えた。索引を作って入れ替える（数分かかることがある）");
    }
    char t[128];
    std::snprintf(t, sizeof t, "タグの置き場所を新しい形にした（%.0f 秒）", secondsSince(t0));
    logs::info(t);
    return true;
}


// 動作履歴: 「段階 タグ付け: 512 / 1400187 枚（失敗 0）・120.3 秒・4.3 枚/秒・譲った」
void logStage(IndexPhase phase, const StageReport& sr, uint64_t total, double sec) {
    if (!sr.processed && !total) return;  // する事が無かった段階は書かない
    char t[256];
    std::snprintf(t, sizeof t, "段階 %s: %zu / %llu 枚（失敗 %zu）・%.1f 秒・%.1f 枚/秒%s", phaseLogName(phase), sr.processed,
                  static_cast<unsigned long long>(total), sr.failed, sec, sec > 0 ? double(sr.processed) / sec : 0.0,
                  sr.yielded ? "・上のスキャンタスクに譲った" : sr.cancelled ? "・止めた" : "");
    (sr.failed && sr.failed * 10 > sr.processed ? logs::warn : logs::info)(t);
}

Outcome runOne(Db& db, RunState& s, IndexPhase phase, Analyzer& a, const std::vector<int64_t>* within, unsigned threads,
               std::atomic<int>& phaseOut, Progress& prog, IndexReport& rep, size_t& counter, bool countFailed) {
    phaseOut = static_cast<int>(phase);
    const auto t0 = std::chrono::steady_clock::now();
    // 1 スレッドだけの時（控えめで操作中）は前処理が遅く、GPU が数枚ごとに動いたり止まったりする。
    // まとめた数の 4 倍たまってから続けて流し、その間は休ませる（細かく波打つと、グラフィックカードから音が出やすい）
    const size_t burst = threads <= 1 && a.batchSize() ? a.batchSize() * 4 : 0;
    const StageReport sr = runStage(db, a, threads, s.cancel, &prog, 256, within, s.opt.preempt, burst);
    logStage(phase, sr, prog.total, secondsSince(t0));
    counter += countFailed ? sr.processed : sr.processed - sr.failed;
    rep.failed += sr.failed;
    const Outcome o = outcomeOf(sr);
    if (o == Outcome::Done) s.progress.doneMask.fetch_or(1u << static_cast<int>(phase));
    return o;
}

// CPU の段階（ハッシュ・サムネイル・メタデータ・json とフォルダ名・品質）
Outcome runCpuStages(Db& db, RunState& s, const std::vector<int64_t>* within, unsigned threads, IndexReport& rep) {
    const IndexOptions& opt = s.opt;
    const IndexCallbacks& cb = s.cb;
    IndexProgress& progress = s.progress;
    auto stage = [&](IndexPhase phase, Analyzer& a, size_t& counter, bool countFailed) {
        return runOne(db, s, phase, a, within, threads, progress.phase, progress.prog, rep, counter, countFailed);
    };
    Outcome o;
    HashAnalyzer hash(cb);
    if ((o = stage(IndexPhase::Hash, hash, rep.hashed, true)) != Outcome::Done) return o;
    if (opt.thumbnails) {
        ThumbPack pack(opt.packDir);
        ThumbAnalyzer thumb(cb, pack);
        if ((o = stage(IndexPhase::Thumb, thumb, rep.thumbs, false)) != Outcome::Done) return o;
    }
    if (cb.readMeta) {
        MetaAnalyzer meta(cb.readMeta, cb.threadInit, cb.threadExit);
        if ((o = stage(IndexPhase::Meta, meta, rep.metas, false)) != Outcome::Done) return o;
    }
    // json・フォルダ名（ファイルを読むだけなので OS に依存しない）
    NamesAnalyzer names;
    if ((o = stage(IndexPhase::Names, names, rep.names, false)) != Outcome::Done) return o;
    // 品質（くっきり度・細かさ）
    if (cb.decodeQuality && cb.qualityEnabled && cb.qualityEnabled() &&
        !pendingImages(db, kStageQuality, kQualityVersion, 1, within).empty()) {
        QualityAnalyzer qa(cb.decodeQuality, cb.threadInit, cb.threadExit);
        if ((o = stage(IndexPhase::Quality, qa, rep.quality, false)) != Outcome::Done) return o;
    }
    return Outcome::Done;
}

// GPU の段階（WD14 タガー・CLIP）。モデルは 1 回の間に 1 度だけ読む
Outcome runGpuStages(Db& db, RunState& s, const std::vector<int64_t>* within, unsigned threads, IndexReport& rep) {
    const IndexCallbacks& cb = s.cb;
    IndexProgress& progress = s.progress;
    auto stage = [&](IndexPhase phase, Analyzer& a, size_t& counter, const std::vector<int64_t>* ids) {
        return runOne(db, s, phase, a, ids, threads, progress.gpuPhase, progress.gpuProg, rep, counter, false);
    };
    Outcome o;
    const int tv = cb.taggerVersion ? cb.taggerVersion() : 0;
    const int cv = cb.clipVersion ? cb.clipVersion() : 0;
    if (cv && cb.makeClip && cb.prepareClip && !s.clipPrepared) {
        s.clipPrepared = true;
        cb.prepareClip(db);
    }
    // モデルは、済んでいない画像がある時だけ読む（重い。読み込みにも時間がかかる）
    auto taggerReady = [&] {
        if (!s.taggerTried) {
            s.taggerTried = true;
            s.tagger = cb.makeTagger(rep.taggerError);
            if (!s.tagger) logs::error("タグ付けのモデルを読めませんでした: " + rep.taggerError);
        }
        return s.tagger != nullptr;
    };
    auto clipReady = [&] {
        if (!s.clipTried) {
            s.clipTried = true;
            s.clip = cb.makeClip(rep.clipError);
            if (!s.clip) logs::error("CLIP のモデルを読めませんでした: " + rep.clipError);
        }
        return s.clip != nullptr;
    };
    bool taggerOn = tv && cb.makeTagger, clipOn = cv && cb.makeClip;
    // WD14 タガーと CLIP を交互に、それぞれ約 kGpuTurnSec 秒分ずつ進める（残りが多い時に、片方が済むまでもう片方が何日も
    // 始まらないと、似た画像を探せる画像がなかなか増えないので）。速さが違う（タガーは重い）ので、枚数は測った速さから決める。
    // スキャンタスク（within）の時は、その画像を全部
    constexpr double kGpuTurnSec = 600;
    size_t taggerChunk = 2048, clipChunk = 2048;
    auto nextChunk = [&](size_t done, double sec) {
        const double rate = sec > 1 ? done / sec : 0;
        return rate > 0 ? std::clamp<size_t>(static_cast<size_t>(rate * kGpuTurnSec), 256, 200000) : size_t(2048);
    };
    std::vector<int64_t> lastTagger, lastClip;  // 前の回の分（進まなくなったらやめる）
    for (;;) {
        if (!within && cb.betweenGpuTurns) cb.betweenGpuTurns(db);
        bool any = false;
        if (taggerOn) {
            std::vector<int64_t> ids = pendingImages(db, stage::kTagger, tv, within ? 0 : taggerChunk, within);
            if (ids.empty() || ids == lastTagger || !taggerReady()) {
                taggerOn = false;
            } else {
                any = true;
                lastTagger = ids;
                progress.gpuPhase = static_cast<int>(IndexPhase::Tagger);
                progress.gpuProg.done = 0;
                progress.gpuProg.total = 0;
                const auto t0 = std::chrono::steady_clock::now();
                const size_t before = rep.tagged;
                if ((o = stage(IndexPhase::Tagger, *s.tagger, rep.tagged, &ids)) != Outcome::Done) return o;
                taggerChunk = nextChunk(rep.tagged - before, secondsSince(t0));
            }
        }
        if (clipOn) {
            std::vector<int64_t> ids = pendingImages(db, kStageClip, cv, within ? 0 : clipChunk, within);
            if (ids.empty() || ids == lastClip || !clipReady()) {
                clipOn = false;
            } else {
                any = true;
                lastClip = ids;
                progress.gpuPhase = static_cast<int>(IndexPhase::Clip);
                progress.gpuProg.done = 0;
                progress.gpuProg.total = 0;
                const auto t0 = std::chrono::steady_clock::now();
                const size_t before = rep.clipped;
                if ((o = stage(IndexPhase::Clip, *s.clip, rep.clipped, &ids)) != Outcome::Done) return o;
                clipChunk = nextChunk(rep.clipped - before, secondsSince(t0));
            }
        }
        if (!any || within) break;
    }
    return Outcome::Done;
}

// 解析の段階。CPU の段階と GPU の段階は別のスレッド（DB の接続も別）で同時に進める
// （GPU が推論している間に CPU でサムネイルなどを作る）。操作中の「控えめ」（1 スレッド）の時だけは順に。
// within があればその画像だけ
Outcome runStages(Db& db, RunState& s, const std::vector<int64_t>* within) {
    const IndexOptions& opt = s.opt;
    IndexProgress& progress = s.progress;
    progress.doneMask = 0;
    progress.gpuPhase = static_cast<int>(IndexPhase::Idle);
    const unsigned all = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
    Outcome cpu, gpu;
    IndexReport gpuRep;
    if (all <= 1) {
        cpu = runCpuStages(db, s, within, 1, s.rep);
        progress.phase = static_cast<int>(IndexPhase::Idle);
        gpu = cpu == Outcome::Done ? runGpuStages(db, s, within, 1, gpuRep) : cpu;
    } else {
        // GPU の側は画像を読んで縮める（前処理）スレッドに、全体の 3 分の 1（2 つ以上）
        const unsigned gpuThreads = std::max(2u, all / 3), cpuThreads = std::max(1u, all - gpuThreads);
        std::exception_ptr error;
        gpu = Outcome::Done;
        std::thread lane([&] {
            try {
                Db gdb;
                if (!gdb.open(opt.dbFile)) throw std::runtime_error("画像の情報（DB）を開けません: " + gdb.error());
                // このスレッドは推論を流すだけ（画像を読むのは前処理のスレッド）。優先度は下げない（下げると GPU への投入が途切れる）
                gpu = runGpuStages(gdb, s, within, gpuThreads, gpuRep);
            } catch (...) {
                error = std::current_exception();
            }
            progress.gpuPhase = static_cast<int>(IndexPhase::Idle);
        });
        try {
            cpu = runCpuStages(db, s, within, cpuThreads, s.rep);
        } catch (...) {
            lane.join();
            throw;
        }
        progress.phase = static_cast<int>(IndexPhase::Idle);  // CPU の側は済んだ（GPU の側を待つ）
        lane.join();
        if (error) std::rethrow_exception(error);
    }
    progress.gpuPhase = static_cast<int>(IndexPhase::Idle);
    s.rep.tagged += gpuRep.tagged;
    s.rep.clipped += gpuRep.clipped;
    s.rep.failed += gpuRep.failed;
    if (!gpuRep.taggerError.empty()) s.rep.taggerError = gpuRep.taggerError;
    if (!gpuRep.clipError.empty()) s.rep.clipError = gpuRep.clipError;
    return combine(cpu, gpu);
}

// DB の手入れ（フォルダ全体の回の仕上げで）: 検索の読み方を選ぶための数（ANALYZE。週に 1 回、少しだけ読んで見積もる）、
// 統計の集計（6 時間に 1 回。統計の画面はこれをすぐ出す）
void maintainDb(Db& db, const std::atomic<bool>& cancel) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto due = [&](const char* key, int64_t every) { return now - std::atoll(db.setting(key, "0").c_str()) >= every; };
    if (!tagConversionState(db).pending && due("maintain:analyze", 7 * 86400)) {
        const auto t0 = std::chrono::steady_clock::now();
        if (db.exec("PRAGMA analysis_limit=1000; ANALYZE;")) {
            db.setSetting("maintain:analyze", std::to_string(now));
            char t[96];
            std::snprintf(t, sizeof t, "検索の読み方のための数を取り直した（ANALYZE、%.1f 秒）", secondsSince(t0));
            logs::info(t);
        }
    }
    if (cancel || !due("maintain:stats", 6 * 3600)) return;
    try {
        const auto t0 = std::chrono::steady_clock::now();
        const StatsSnapshot snap = computeStats(db);
        if (cancel) return;
        saveStatsSnapshot(db, snap);
        db.setSetting("maintain:stats", std::to_string(now));
        char t[96];
        std::snprintf(t, sizeof t, "統計を集計して残した（%.1f 秒）", secondsSince(t0));
        logs::info(t);
    } catch (const std::exception& e) {
        logs::warn(std::string("統計を集計できなかった: ") + e.what());
    }
}

// 全体に掛かる仕上げ（差分グループ・照会・辞書）。どれも変わっていなければすぐ終わる
bool finish(Db& db, RunState& s, bool lookup) {
    const IndexCallbacks& cb = s.cb;
    IndexProgress& progress = s.progress;
    // 時間がかかった時だけ書く（何も変わっていなければすぐ終わる）
    auto timed = [&](IndexPhase phase, const std::function<void()>& f) {
        progress.phase = static_cast<int>(phase);
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const double sec = secondsSince(t0);
        if (sec >= 1.0) {
            char t[128];
            std::snprintf(t, sizeof t, "段階 %s: %.1f 秒%s", phaseLogName(phase), sec, s.cancel ? "・止めた" : "");
            logs::info(t);
        }
    };
    if ((cb.clipVersion ? cb.clipVersion() : 0) && cb.makeClip && cb.afterClip) {
        timed(IndexPhase::Variants, [&] { cb.afterClip(db, s.opt.threads, s.cancel, &progress.prog); });
        if (s.cancel) return false;
    }
    if (lookup && cb.lookup) {
        timed(IndexPhase::Lookup, [&] { cb.lookup(db, s.cancel, &progress.prog); });
        if (s.cancel) return false;
    }
    // フォルダ名から作者・アカウントを推し量る（画像・作者の情報・教えた形が変わっていなければすぐ終わる）
    timed(IndexPhase::Merge, [&] {
        try {
            const InferSummary r = runFolderInference(db, false, &s.cancel);
            if (!r.skipped) {
                char t[200];
                std::snprintf(t, sizeof t, "フォルダ名から推し量った: フォルダ %zu 個のうち変わった %zu 個（画像 %zu 枚を書き直し、推し量れた画像 %zu 枚）%s",
                              r.dirs, r.changedDirs, r.images, r.tagged, r.cancelled ? "・途中で止めた" : "");
                logs::info(t);
            }
        } catch (const std::exception& e) {
            logs::error(std::string("フォルダ名から推し量れなかった: ") + e.what());
        }
    });
    if (cb.merge) timed(IndexPhase::Merge, [&] { cb.merge(db); });
    if (s.cancel) return false;
    if (lookup) maintainDb(db, s.cancel);
    return true;
}

// 走査して登録する。removeUnder の下で見つからなかった画像は消す。見つけた画像の ID（パスの順）を返す
std::vector<int64_t> scanAndRegister(Db& db, RunState& s, const std::vector<std::string>& paths,
                                     const std::vector<std::string>& removeUnder) {
    s.progress.phase = static_cast<int>(IndexPhase::Scan);
    s.progress.prog.done = 0;
    s.progress.prog.total = 0;
    std::vector<ScannedFile> files;
    for (const auto& r : paths) {
        if (s.cancel) return {};
        s.cb.scan(r, files);
    }
    if (s.cancel) return {};
    std::sort(files.begin(), files.end(), [](const ScannedFile& a, const ScannedFile& b) { return a.path < b.path; });
    files.erase(std::unique(files.begin(), files.end(),
                            [](const ScannedFile& a, const ScannedFile& b) { return a.path == b.path; }),
                files.end());
    s.rep.files += files.size();
    const int64_t before = imageCount(db);
    // 読めないフォルダ（外したドライブなど）の下は消さない（つなぎ直した時にタグ・CLIP などを作り直さずに済むように）
    std::vector<std::string> removable;
    for (const auto& r : removeUnder) {
        if (!s.cb.rootAvailable || s.cb.rootAvailable(r)) removable.push_back(r);
        else logs::warn("調べるフォルダが見つからない（ドライブを外した・名前を変えたなど）ので、その下の画像は消さずに残す: " + r);
    }
    // 動かした・名前を変えた画像は、パスだけ書き換えて引き継ぐ。保管にある画像が戻ってきたら、保管から戻す
    size_t carried = 0;
    if (!s.opt.archiveFile.empty()) {
        try {
            const MoveReport mr = matchMovedImages(db, files, removable, s.opt.archiveFile, s.opt.vectorFile);
            carried = mr.moved + mr.restored;
            if (mr.moved) logs::info("動かした・名前を変えた画像 " + std::to_string(mr.moved) + " 枚を見つけて、解析の結果を引き継いだ");
            if (mr.restored) logs::info("保管にあった画像 " + std::to_string(mr.restored) + " 枚が戻ってきたので、保管から戻した");
        } catch (const std::exception& e) {
            logs::error(std::string("動かした画像を見分けられなかった（新しい画像として登録する）: ") + e.what());
        }
    }
    const auto ids = upsertScanned(db, files);
    size_t removed = 0;
    if (!removable.empty()) {
        if (s.opt.archiveFile.empty()) {
            removed = removeMissing(db, removable, ids);
        } else {
            // 消さずに保管へ移す（タガーのタグ・手動の確定・CLIP のベクトルを守る）
            const std::vector<int64_t> gone = missingImages(db, removable, ids);
            removed = archiveImages(db, s.opt.archiveFile, gone, s.opt.vectorFile, "見つからない");
            if (removed) logs::info("見つからなくなった画像 " + std::to_string(removed) + " 枚を保管へ移した（" + s.opt.archiveFile + "）");
        }
    }
    s.rep.removed += removed;
    s.rep.added += static_cast<size_t>(std::max<int64_t>(0, imageCount(db) - before + static_cast<int64_t>(removed)));
    {
        char t[160];
        std::snprintf(t, sizeof t, "フォルダを調べた: 画像 %zu 枚（新しく %zu 枚・消えた %zu 枚）", files.size(),
                      static_cast<size_t>(std::max<int64_t>(0, imageCount(db) - before + static_cast<int64_t>(removed))), removed);
        logs::info(t);
    }
    std::vector<int64_t> out;
    out.reserve(files.size());
    for (const auto& f : files) {
        const auto it = ids.find(f.path);
        if (it != ids.end()) out.push_back(it->second);
    }
    return out;
}

}  // namespace

std::string spellLikeRoot(const std::string& path, const std::vector<std::string>& roots) {
    auto norm = [](char c) { return c == '/' ? '\\' : (c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c); };
    for (const auto& r : roots) {
        if (r.empty() || path.size() < r.size()) continue;
        bool same = true;
        for (size_t i = 0; i < r.size() && same; ++i) same = norm(path[i]) == norm(r[i]);
        if (!same) continue;
        // 区切りの所で切れているか（D:\a と D:\ab を取り違えない）
        const char last = r.back();
        if (path.size() > r.size() && last != '/' && last != '\\' && path[r.size()] != '/' && path[r.size()] != '\\') continue;
        std::string out = r + path.substr(r.size());
        // 後ろの区切りも、調べるフォルダの書き方（\ か /）にそろえる
        const char sep = r.find('/') != std::string::npos && r.find('\\') == std::string::npos ? '/' : '\\';
        for (size_t i = r.size(); i < out.size(); ++i)
            if (out[i] == '/' || out[i] == '\\') out[i] = sep;
        return out;
    }
    return path;
}

size_t removeMisspelled(Db& db, const std::vector<std::string>& roots) {
    std::vector<std::pair<int64_t, int64_t>> dup;  // (書き方の違う画像, 正しい書き方の画像)
    {
        // 書き方の違うものだけ覚え（ふつうは無い）、正しい書き方は索引で引く（全部のパスを覚えるとメモリを食う）
        std::vector<std::pair<int64_t, std::string>> odd;
        Stmt st(db.handle(), "SELECT id, path FROM images");
        while (st.step()) {
            const std::string p = st.text(1);
            std::string right = spellLikeRoot(p, roots);
            if (right != p) odd.emplace_back(st.i64(0), std::move(right));
        }
        Stmt find(db.handle(), "SELECT id FROM images WHERE path=?");
        for (const auto& [id, right] : odd) {
            find.bind(1, right);
            if (find.step() && find.i64(0) != id) dup.emplace_back(id, find.i64(0));
            find.reset();
        }
    }
    if (dup.empty()) return 0;
    db.begin();
    try {
        // 手動で確定したタグは、正しい書き方の方へ移す（向こうに同じタグの確定があればそちらを残す）
        Stmt del(db.handle(), "DELETE FROM images WHERE id=?");
        for (const auto& [from, to] : dup) {
            copyManualTags(db, from, to);
            del.bind(1, from);
            del.step();
            del.reset();
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    return dup.size();
}

IndexReport indexOnce(const IndexOptions& opt, const IndexCallbacks& cb, IndexProgress& progress,
                      const std::atomic<bool>& cancel) {
    IndexReport rep;
    Db db;
    if (!db.open(opt.dbFile)) throw std::runtime_error("画像の情報（DB）を開けません: " + db.error());
    RunState s{opt, cb, progress, cancel, rep};
    bool scanned = false, tasksDone = false;
    const auto runStart = std::chrono::steady_clock::now();
    {
        char t[160];
        const std::string threads = opt.threads ? std::to_string(opt.threads) : std::string("自動");
        std::snprintf(t, sizeof t, "回を始める（%s・スレッド %s・調べるフォルダ %zu 個・DB の画像 %lld 枚）",
                      opt.fullPass ? "フォルダ全体" : "スキャンタスクだけ", threads.c_str(), opt.roots.size(),
                      static_cast<long long>(imageCount(db)));
        logs::info(t);
    }
    auto stop = [&] {
        char t[96];
        std::snprintf(t, sizeof t, "回を途中で止めた（%.0f 秒）。続きは次の回で", secondsSince(runStart));
        logs::info(t);
        rep.cancelled = true;
        progress.task = -1;
        progress.phase = static_cast<int>(IndexPhase::Done);
        return rep;
    };
    // 前の版の DB: タグの置き場所を新しい形に変換する（ほかの何よりも先に。止めても続きから）
    if (!convertTagStore(db, progress, cancel)) return stop();
    for (;;) {
        if (cancel) return stop();
        if (opt.preempt) *opt.preempt = false;
        const std::vector<ScanTask> tasks = scanTasks(db);
        if (!tasks.empty()) {
            // 一番上のスキャンタスク: そのフォルダ・ファイルだけを調べ、全部の段階を済ませる
            const ScanTask& t = tasks.front();
            progress.task = t.id;
            logs::info("スキャンタスクを始める: 「" + t.label + "」（" + std::to_string(t.paths.size()) + " 個のパス、残りのタスク " +
                       std::to_string(tasks.size()) + " 個）");
            // 調べるフォルダの中のパスは、調べるフォルダと同じ書き方にする（DB のパスは大文字・小文字や / と \ を区別するので、
            // 書き方が違うと同じ画像を別の画像としてもう一度登録してしまう）
            std::vector<std::string> paths;
            for (const auto& p : t.paths) paths.push_back(spellLikeRoot(p, opt.roots));
            std::vector<std::string> folders;  // 画像の拡張子でないものはフォルダ（中で消えた画像は消す）
            for (const auto& p : paths)
                if (!isImageExt(p)) folders.push_back(p);
            const std::vector<int64_t> ids = scanAndRegister(db, s, paths, folders);
            if (cancel) return stop();
            const Outcome o = runStages(db, s, &ids);
            if (o == Outcome::Cancelled) return stop();
            if (o == Outcome::Yielded) {
                logs::info("スキャンタスク「" + t.label + "」を中断して、上に来たタスクを先にする");
                continue;
            }
            logs::info("スキャンタスクを終えた: 「" + t.label + "」（画像 " + std::to_string(ids.size()) + " 枚）");
            removeScanTask(db, t.id);
            ++rep.tasks;
            tasksDone = true;
            continue;
        }
        if (!opt.fullPass) {
            // スキャンタスクだけの回: 済ませた分の差分グループ・辞書だけ当てる
            progress.task = -1;
            if (tasksDone && !finish(db, s, false)) return stop();
            break;
        }
        // フォルダ全体の確認
        progress.task = 0;
        if (!scanned) {
            scanAndRegister(db, s, opt.roots, opt.roots);
            if (cancel) return stop();
            // 外してある画像の印: 今つながっている調べるフォルダの下だけを「ある」に（一覧から外したフォルダ・外したドライブは外してある）
            {
                std::vector<std::string> online;
                for (const auto& r : opt.roots)
                    if (!cb.rootAvailable || cb.rootAvailable(r)) online.push_back(r);
                if (const size_t n = updateOffline(db, online))
                    logs::info("外してある画像の印を付け直した: " + std::to_string(n) + " 枚");
            }
            // 前の版のスキャンタスクで、書き方の違うパスで二重に登録した画像を消す
            if (const size_t n = removeMisspelled(db, opt.roots)) {
                rep.removed += n;
                logs::warn("書き方の違うパスで二重に登録されていた画像 " + std::to_string(n) + " 枚を消した");
            }
            scanned = true;
        }
        const Outcome o = runStages(db, s, nullptr);
        if (o == Outcome::Cancelled) return stop();
        if (o == Outcome::Yielded) continue;
        if (!finish(db, s, true)) return stop();
        break;
    }
    progress.task = -1;
    progress.phase = static_cast<int>(IndexPhase::Done);
    {
        char t[200];
        std::snprintf(t, sizeof t, "回を終えた（%.0f 秒。スキャンタスク %zu 個、ハッシュ %zu・タグ付け %zu・CLIP %zu・失敗 %zu）",
                      secondsSince(runStart), rep.tasks, rep.hashed, rep.tagged, rep.clipped, rep.failed);
        logs::info(t);
    }
    return rep;
}

}  // namespace imgdupe
