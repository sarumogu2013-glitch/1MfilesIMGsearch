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

IndexReport indexOnce(const IndexOptions& opt, const IndexCallbacks& cb, IndexProgress& progress,
                      const std::atomic<bool>& cancel) {
    IndexReport rep;
    Db db;
    if (!db.open(opt.dbFile)) throw std::runtime_error("画像の情報（DB）を開けません: " + db.error());

    progress.phase = static_cast<int>(IndexPhase::Scan);
    progress.prog.done = 0;
    progress.prog.total = 0;
    std::vector<ScannedFile> files;
    for (const auto& r : opt.roots) {
        if (cancel) break;
        cb.scan(r, files);
    }
    if (cancel) {
        rep.cancelled = true;
        return rep;
    }
    std::sort(files.begin(), files.end(), [](const ScannedFile& a, const ScannedFile& b) { return a.path < b.path; });
    files.erase(std::unique(files.begin(), files.end(),
                            [](const ScannedFile& a, const ScannedFile& b) { return a.path == b.path; }),
                files.end());
    rep.files = files.size();
    {
        Stmt st(db.handle(), "SELECT COUNT(*) FROM images");
        const int64_t before = st.step() ? st.i64(0) : 0;
        const auto ids = upsertScanned(db, files);
        rep.removed = removeMissing(db, opt.roots, ids);
        Stmt after(db.handle(), "SELECT COUNT(*) FROM images");
        rep.added = static_cast<size_t>(std::max<int64_t>(0, (after.step() ? after.i64(0) : 0) - before +
                                                              static_cast<int64_t>(rep.removed)));
    }
    files = {};

    progress.phase = static_cast<int>(IndexPhase::Hash);
    HashAnalyzer hash(cb);
    StageReport sr = runStage(db, hash, opt.threads, cancel, &progress.prog);
    rep.hashed = sr.processed;
    rep.failed += sr.failed;
    if (sr.cancelled) {
        rep.cancelled = true;
        return rep;
    }
    if (opt.thumbnails) {
        progress.phase = static_cast<int>(IndexPhase::Thumb);
        ThumbPack pack(opt.packDir);
        ThumbAnalyzer thumb(cb, pack);
        sr = runStage(db, thumb, opt.threads, cancel, &progress.prog);
        rep.thumbs = sr.processed - sr.failed;
        rep.failed += sr.failed;
        if (sr.cancelled) {
            rep.cancelled = true;
            return rep;
        }
    }
    if (cb.readMeta) {
        progress.phase = static_cast<int>(IndexPhase::Meta);
        MetaAnalyzer meta(cb.readMeta, cb.threadInit, cb.threadExit);
        sr = runStage(db, meta, opt.threads, cancel, &progress.prog);
        rep.metas = sr.processed - sr.failed;
        rep.failed += sr.failed;
        if (sr.cancelled) {
            rep.cancelled = true;
            return rep;
        }
    }
    // json・フォルダ名（ファイルを読むだけなので OS に依存しない）
    progress.phase = static_cast<int>(IndexPhase::Names);
    NamesAnalyzer names;
    sr = runStage(db, names, opt.threads, cancel, &progress.prog);
    rep.names = sr.processed - sr.failed;
    rep.failed += sr.failed;
    if (sr.cancelled) {
        rep.cancelled = true;
        return rep;
    }
    // WD14 タガー（重い。モデルの読み込みにも時間がかかるので、済んでいない画像がある時だけ読む）
    const int tv = cb.taggerVersion ? cb.taggerVersion() : 0;
    if (tv && cb.makeTagger && !pendingImages(db, stage::kTagger, tv, 1).empty()) {
        progress.phase = static_cast<int>(IndexPhase::Tagger);
        if (std::shared_ptr<Analyzer> tagger = cb.makeTagger(rep.taggerError)) {
            sr = runStage(db, *tagger, opt.threads, cancel, &progress.prog);
            rep.tagged = sr.processed - sr.failed;
            rep.failed += sr.failed;
            rep.cancelled = sr.cancelled;
        }
    }
    if (rep.cancelled) {
        progress.phase = static_cast<int>(IndexPhase::Done);
        return rep;
    }
    // CLIP（似た画像・言葉での検索のためのベクトル）
    const int cv = cb.clipVersion ? cb.clipVersion() : 0;
    if (cv && cb.makeClip) {
        if (cb.prepareClip) cb.prepareClip(db);
        if (!pendingImages(db, kStageClip, cv, 1).empty()) {
            progress.phase = static_cast<int>(IndexPhase::Clip);
            if (std::shared_ptr<Analyzer> c = cb.makeClip(rep.clipError)) {
                sr = runStage(db, *c, opt.threads, cancel, &progress.prog);
                rep.clipped = sr.processed - sr.failed;
                rep.failed += sr.failed;
                rep.cancelled = sr.cancelled;
            }
        }
        // 差分グループ（索引に新しい画像を足し、変わっていれば作り直す）
        if (!rep.cancelled && cb.afterClip) {
            progress.phase = static_cast<int>(IndexPhase::Variants);
            cb.afterClip(db, opt.threads, cancel, &progress.prog);
            rep.cancelled = cancel.load();
        }
    }
    // 品質（くっきり度・細かさ）
    if (!rep.cancelled && cb.decodeQuality && cb.qualityEnabled && cb.qualityEnabled() &&
        !pendingImages(db, kStageQuality, kQualityVersion, 1).empty()) {
        progress.phase = static_cast<int>(IndexPhase::Quality);
        QualityAnalyzer qa(cb.decodeQuality, cb.threadInit, cb.threadExit);
        sr = runStage(db, qa, opt.threads, cancel, &progress.prog);
        rep.quality = sr.processed - sr.failed;
        rep.failed += sr.failed;
        rep.cancelled = sr.cancelled;
    }
    if (!rep.cancelled && cb.lookup) {
        progress.phase = static_cast<int>(IndexPhase::Lookup);
        cb.lookup(db, cancel, &progress.prog);
        rep.cancelled = cancel.load();
    }
    if (!rep.cancelled && cb.merge) {
        progress.phase = static_cast<int>(IndexPhase::Merge);
        cb.merge(db);
    }
    progress.phase = static_cast<int>(IndexPhase::Done);
    return rep;
}

}  // namespace imgdupe
