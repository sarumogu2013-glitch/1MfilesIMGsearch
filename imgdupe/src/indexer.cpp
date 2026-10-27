#include "indexer.hpp"

#include <algorithm>
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

// 解析の段階（ハッシュ〜品質）。within があればその画像だけ
Outcome runStages(Db& db, RunState& s, const std::vector<int64_t>* within) {
    const IndexOptions& opt = s.opt;
    const IndexCallbacks& cb = s.cb;
    IndexProgress& progress = s.progress;
    IndexReport& rep = s.rep;
    auto stage = [&](IndexPhase phase, Analyzer& a, size_t& counter, bool countFailed) {
        progress.phase = static_cast<int>(phase);
        const StageReport sr = runStage(db, a, opt.threads, s.cancel, &progress.prog, 256, within, opt.preempt);
        counter += countFailed ? sr.processed : sr.processed - sr.failed;
        rep.failed += sr.failed;
        return outcomeOf(sr);
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
    // WD14 タガー（重い。モデルの読み込みにも時間がかかるので、済んでいない画像がある時だけ読む）
    const int tv = cb.taggerVersion ? cb.taggerVersion() : 0;
    if (tv && cb.makeTagger && !pendingImages(db, stage::kTagger, tv, 1, within).empty()) {
        progress.phase = static_cast<int>(IndexPhase::Tagger);
        if (!s.taggerTried) {
            s.taggerTried = true;
            s.tagger = cb.makeTagger(rep.taggerError);
        }
        if (s.tagger && (o = stage(IndexPhase::Tagger, *s.tagger, rep.tagged, false)) != Outcome::Done) return o;
    }
    // CLIP（似た画像・言葉での検索のためのベクトル）
    const int cv = cb.clipVersion ? cb.clipVersion() : 0;
    if (cv && cb.makeClip) {
        if (cb.prepareClip && !s.clipPrepared) {
            s.clipPrepared = true;
            cb.prepareClip(db);
        }
        if (!pendingImages(db, kStageClip, cv, 1, within).empty()) {
            progress.phase = static_cast<int>(IndexPhase::Clip);
            if (!s.clipTried) {
                s.clipTried = true;
                s.clip = cb.makeClip(rep.clipError);
            }
            if (s.clip && (o = stage(IndexPhase::Clip, *s.clip, rep.clipped, false)) != Outcome::Done) return o;
        }
    }
    // 品質（くっきり度・細かさ）
    if (cb.decodeQuality && cb.qualityEnabled && cb.qualityEnabled() &&
        !pendingImages(db, kStageQuality, kQualityVersion, 1, within).empty()) {
        QualityAnalyzer qa(cb.decodeQuality, cb.threadInit, cb.threadExit);
        if ((o = stage(IndexPhase::Quality, qa, rep.quality, false)) != Outcome::Done) return o;
    }
    return Outcome::Done;
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
            std::vector<std::string> folders;  // 画像の拡張子でないものはフォルダ（中で消えた画像は消す）
            for (const auto& p : t.paths)
                if (!isImageExt(p)) folders.push_back(p);
            const std::vector<int64_t> ids = scanAndRegister(db, s, t.paths, folders);
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
