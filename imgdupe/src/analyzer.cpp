#include "analyzer.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace imgdupe {

namespace {

// 前処理と推論を分けた解析器: 前処理のスレッドが読んだ順に待ち行列に入れ、推論のスレッドが batchSize 枚ずつ取り出して続けて流す。
// burst があれば、待ち行列に burst 枚たまってから流し始め、たまっている間は続けて流す（前処理が遅い時に、GPU が
// 数枚ごとに動いたり止まったりを繰り返さず、まとめて動いてしばらく休むようにする）。
// 待ち行列は batchSize の 3 倍（burst があればその 2 倍）まで（前処理だけが先に進んでメモリを使い切らないように）
void runBatched(Analyzer& analyzer, const std::vector<AnalyzerInput>& inputs, std::vector<AnalyzerOutput>& outputs,
                std::vector<uint8_t>& finished, unsigned threads, const std::atomic<bool>& cancel, Progress* progress, size_t burst) {
    const size_t batch = std::max<size_t>(1, analyzer.batchSize()), start = std::max(batch, burst);
    const size_t limit = std::max(batch * 3, burst * 2);
    bool flowing = false;  // 流している途中（batch 枚あれば続ける）
    std::mutex m;
    std::condition_variable readyCv, roomCv;
    std::deque<std::pair<size_t, std::shared_ptr<void>>> ready;
    size_t producing = 0;  // 動いている前処理のスレッド
    std::atomic<size_t> next{0};
    auto producer = [&] {
        analyzer.threadBegin();
        for (;;) {
            if (cancel) break;
            const size_t i = next.fetch_add(1);
            if (i >= inputs.size()) break;
            std::shared_ptr<void> p = analyzer.prepare(inputs[i]);
            if (!p) {  // 読めない画像は推論に回さない
                outputs[i].ok = false;
                finished[i] = 1;
                if (progress) progress->done.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            std::unique_lock<std::mutex> lock(m);
            while (!roomCv.wait_for(lock, std::chrono::milliseconds(200), [&] { return ready.size() < limit || cancel; })) {
            }
            ready.emplace_back(i, std::move(p));
            readyCv.notify_one();
        }
        analyzer.threadEnd();
        std::lock_guard<std::mutex> lock(m);
        --producing;
        readyCv.notify_one();
    };
    const unsigned n = static_cast<unsigned>(std::max<size_t>(1, std::min<size_t>(threads, inputs.size())));
    producing = n;
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < n; ++t) pool.emplace_back(producer);
    // 推論（このスレッド）: batchSize 枚そろうか、前処理が全部終わったら流す
    std::vector<size_t> idx;
    std::vector<std::shared_ptr<void>> items;
    std::vector<AnalyzerOutput> outs;
    for (;;) {
        idx.clear();
        items.clear();
        {
            std::unique_lock<std::mutex> lock(m);
            // 止める指示（cancel）は知らせが来ないので、少しずつ待って見直す
            while (!readyCv.wait_for(lock, std::chrono::milliseconds(200),
                                     [&] { return ready.size() >= (flowing ? batch : start) || producing == 0 || cancel; })) {
            }
            if (cancel) break;
            while (!ready.empty() && idx.size() < batch) {
                idx.push_back(ready.front().first);
                items.push_back(std::move(ready.front().second));
                ready.pop_front();
            }
            roomCv.notify_all();
            flowing = ready.size() >= batch;  // 続けて流せる分が残っていれば、待たずに続ける
            if (idx.empty() && producing == 0) break;
        }
        if (idx.empty()) continue;
        outs.assign(idx.size(), AnalyzerOutput{});
        analyzer.inferBatch(items, outs);
        for (size_t k = 0; k < idx.size(); ++k) {
            outputs[idx[k]] = std::move(outs[k]);
            finished[idx[k]] = 1;
        }
        if (progress) progress->done.fetch_add(idx.size(), std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lock(m);
        roomCv.notify_all();  // 止めた時に、待っている前処理を起こす
    }
    for (auto& t : pool) t.join();
}

// 1 枚ずつ（前処理と推論を分けない解析器）
void runEach(Analyzer& analyzer, const std::vector<AnalyzerInput>& inputs, std::vector<AnalyzerOutput>& outputs,
             std::vector<uint8_t>& finished, unsigned threads, const std::atomic<bool>& cancel, Progress* progress) {
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
}

}  // namespace

StageReport runStage(Db& db, Analyzer& analyzer, unsigned threads, const std::atomic<bool>& cancel, Progress* progress,
                     size_t batch, const std::vector<int64_t>* within, const std::atomic<bool>* yield, size_t burst) {
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
    // まとめて推論するものは、ひとまとまりを大きめに（切れ目ごとに推論が少し止まるので）
    if (analyzer.batchSize()) batch = std::max<size_t>(batch, 512);
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
        if (analyzer.batchSize()) runBatched(analyzer, inputs, outputs, finished, threads, cancel, progress, burst);
        else runEach(analyzer, inputs, outputs, finished, threads, cancel, progress);

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
