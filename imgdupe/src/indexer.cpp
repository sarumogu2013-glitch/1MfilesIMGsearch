#include "indexer.hpp"

#include <algorithm>
#include <exception>
#include <thread>
#include <cstring>
#include <stdexcept>

#include "clip.hpp"
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
Outcome runOne(Db& db, RunState& s, IndexPhase phase, Analyzer& a, const std::vector<int64_t>* within, unsigned threads,
               std::atomic<int>& phaseOut, Progress& prog, IndexReport& rep, size_t& counter, bool countFailed) {
    phaseOut = static_cast<int>(phase);
    // 1 スレッドだけの時（控えめで操作中）は前処理が遅く、GPU が数枚ごとに動いたり止まったりする。
    // まとめた数の 4 倍たまってから続けて流し、その間は休ませる（細かく波打つと、グラフィックカードから音が出やすい）
    const size_t burst = threads <= 1 && a.batchSize() ? a.batchSize() * 4 : 0;
    const StageReport sr = runStage(db, a, threads, s.cancel, &prog, 256, within, s.opt.preempt, burst);
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
    auto stage = [&](IndexPhase phase, Analyzer& a, size_t& counter) {
        return runOne(db, s, phase, a, within, threads, progress.gpuPhase, progress.gpuProg, rep, counter, false);
    };
    Outcome o;
    // WD14 タガー（重い。モデルの読み込みにも時間がかかるので、済んでいない画像がある時だけ読む）
    const int tv = cb.taggerVersion ? cb.taggerVersion() : 0;
    if (tv && cb.makeTagger && !pendingImages(db, stage::kTagger, tv, 1, within).empty()) {
        progress.gpuPhase = static_cast<int>(IndexPhase::Tagger);
        progress.gpuProg.done = 0;
        progress.gpuProg.total = 0;
        if (!s.taggerTried) {
            s.taggerTried = true;
            s.tagger = cb.makeTagger(rep.taggerError);
        }
        if (s.tagger && (o = stage(IndexPhase::Tagger, *s.tagger, rep.tagged)) != Outcome::Done) return o;
    }
    // CLIP（似た画像・言葉での検索のためのベクトル）
    const int cv = cb.clipVersion ? cb.clipVersion() : 0;
    if (cv && cb.makeClip) {
        if (cb.prepareClip && !s.clipPrepared) {
            s.clipPrepared = true;
            cb.prepareClip(db);
        }
        if (!pendingImages(db, kStageClip, cv, 1, within).empty()) {
            progress.gpuPhase = static_cast<int>(IndexPhase::Clip);
            progress.gpuProg.done = 0;
            progress.gpuProg.total = 0;
            if (!s.clipTried) {
                s.clipTried = true;
                s.clip = cb.makeClip(rep.clipError);
            }
            if (s.clip && (o = stage(IndexPhase::Clip, *s.clip, rep.clipped)) != Outcome::Done) return o;
        }
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

// 全体に掛かる仕上げ（差分グループ・照会・辞書）。どれも変わっていなければすぐ終わる
bool finish(Db& db, RunState& s, bool lookup) {
    const IndexCallbacks& cb = s.cb;
    IndexProgress& progress = s.progress;
    if ((cb.clipVersion ? cb.clipVersion() : 0) && cb.makeClip && cb.afterClip) {
        progress.phase = static_cast<int>(IndexPhase::Variants);
        cb.afterClip(db, s.opt.threads, s.cancel, &progress.prog);
        if (s.cancel) return false;
    }
    if (lookup && cb.lookup) {
        progress.phase = static_cast<int>(IndexPhase::Lookup);
        cb.lookup(db, s.cancel, &progress.prog);
        if (s.cancel) return false;
    }
    if (cb.merge) {
        progress.phase = static_cast<int>(IndexPhase::Merge);
        cb.merge(db);
    }
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
    const auto ids = upsertScanned(db, files);
    const size_t removed = removeUnder.empty() ? 0 : removeMissing(db, removeUnder, ids);
    s.rep.removed += removed;
    s.rep.added += static_cast<size_t>(std::max<int64_t>(0, imageCount(db) - before + static_cast<int64_t>(removed)));
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
        Stmt move(db.handle(),
                  "INSERT OR IGNORE INTO tags(image_id,tag,kind,confidence,source,manual) "
                  "SELECT ?,tag,kind,confidence,source,manual FROM tags WHERE image_id=? AND manual=1");
        Stmt del(db.handle(), "DELETE FROM images WHERE id=?");
        for (const auto& [from, to] : dup) {
            move.bind(1, to).bind(2, from);
            move.step();
            move.reset();
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
    auto stop = [&] {
        rep.cancelled = true;
        progress.task = -1;
        progress.phase = static_cast<int>(IndexPhase::Done);
        return rep;
    };
    for (;;) {
        if (cancel) return stop();
        if (opt.preempt) *opt.preempt = false;
        const std::vector<ScanTask> tasks = scanTasks(db);
        if (!tasks.empty()) {
            // 一番上のスキャンタスク: そのフォルダ・ファイルだけを調べ、全部の段階を済ませる
            const ScanTask& t = tasks.front();
            progress.task = t.id;
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
            if (o == Outcome::Yielded) continue;
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
            // 前の版のスキャンタスクで、書き方の違うパスで二重に登録した画像を消す
            rep.removed += removeMisspelled(db, opt.roots);
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
    return rep;
}

}  // namespace imgdupe
