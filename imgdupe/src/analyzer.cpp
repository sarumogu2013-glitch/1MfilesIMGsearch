#include "analyzer.hpp"

#include <algorithm>
#include <thread>

namespace imgdupe {

StageReport runStage(Db& db, Analyzer& analyzer, unsigned threads, const std::atomic<bool>& cancel, Progress* progress,
                     size_t batch, const std::vector<int64_t>* within, const std::atomic<bool>* yield) {
    StageReport rep;
    const std::string name = analyzer.stageName();
    const int version = analyzer.version();
    if (!threads) threads = std::max(1u, std::thread::hardware_concurrency());
    // 対象の一覧は最初に 1 回だけ作る（100 万枚だと数秒かかるので、ひとまとまりごとには問い合わせない）
    const std::vector<int64_t> all = pendingImages(db, name, version, 0, within);
    if (progress) {
        progress->done = 0;
        progress->total = all.size();
    }
    for (size_t from = 0; from < all.size(); from += batch) {
        if (cancel) {
            rep.cancelled = true;
            break;
        }
        if (yield && *yield) {
            rep.cancelled = rep.yielded = true;
            break;
        }
        const std::vector<int64_t> ids(all.begin() + from, all.begin() + std::min(all.size(), from + batch));
        std::vector<AnalyzerInput> inputs(ids.size());
        {
            Stmt st(db.handle(), "SELECT path FROM images WHERE id=?");
            for (size_t i = 0; i < ids.size(); ++i) {
                st.bind(1, ids[i]);
                inputs[i].imageId = ids[i];
                if (st.step()) inputs[i].path = st.text(0);
                st.reset();
            }
        }
        std::vector<AnalyzerOutput> outputs(ids.size());
        std::vector<uint8_t> finished(ids.size(), 0);
        std::atomic<size_t> next{0};
        auto worker = [&] {
            analyzer.threadBegin();
            for (;;) {
                if (cancel) break;
                const size_t i = next.fetch_add(1);
                if (i >= inputs.size()) break;
                outputs[i] = analyzer.analyze(inputs[i]);
                finished[i] = 1;
                if (progress) progress->done.fetch_add(1, std::memory_order_relaxed);
            }
            analyzer.threadEnd();
        };
        const unsigned n = static_cast<unsigned>(std::min<size_t>(threads, inputs.size()));
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < n; ++t) pool.emplace_back(worker);
        worker();
        for (auto& t : pool) t.join();

        db.begin();
        try {
            for (size_t i = 0; i < ids.size(); ++i) {
                if (!finished[i]) continue;
                AnalyzerOutput& out = outputs[i];
                if (out.ok) out.ok = analyzer.store(db, ids[i], out);
                replaceTags(db, ids[i], name, out.tags);
                setStage(db, ids[i], name, out.ok ? StageState::Done : StageState::Failed, version);
                ++rep.processed;
                if (!out.ok) ++rep.failed;
            }
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }
    }
    return rep;
}

}  // namespace imgdupe
