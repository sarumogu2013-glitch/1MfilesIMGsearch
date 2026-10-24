#include "ann.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "hnswlib.h"
#include "platform.hpp"

namespace imgdupe {

namespace {

// fp16 のベクトルの内積の距離（1 - 内積）
struct HalfIpSpace : hnswlib::SpaceInterface<float> {
    size_t dim;
    explicit HalfIpSpace(size_t d) : dim(d) {}
    size_t get_data_size() override { return dim * 2; }
    hnswlib::DISTFUNC<float> get_dist_func() override { return &dist; }
    void* get_dist_func_param() override { return &dim; }
    static float dist(const void* a, const void* b, const void* param) {
        const size_t n = *static_cast<const size_t*>(param);
        const auto* x = static_cast<const uint16_t*>(a);
        const auto* y = static_cast<const uint16_t*>(b);
        float s = 0;
        for (size_t i = 0; i < n; ++i) s += halfToFloat(x[i]) * halfToFloat(y[i]);
        return 1.f - s;
    }
};

constexpr size_t kM = 16, kEfConstruction = 100, kEfSearch = 64;

struct UnionFind {
    std::vector<size_t> p;
    explicit UnionFind(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
    size_t find(size_t x) {
        while (p[x] != x) x = p[x] = p[p[x]];
        return x;
    }
    void unite(size_t a, size_t b) {
        a = find(a);
        b = find(b);
        if (a != b) p[std::max(a, b)] = std::min(a, b);
    }
};

}  // namespace

struct AnnIndex::Impl {
    std::unique_ptr<HalfIpSpace> space;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> hnsw;
    std::string path, version;
    uint32_t dim = 0;
};

AnnIndex::AnnIndex() : impl_(new Impl) {}
AnnIndex::~AnnIndex() = default;

bool AnnIndex::open(const std::string& path, uint32_t dim, const std::string& version) {
    Impl& m = *impl_;
    m.path = path;
    m.version = version;
    m.dim = dim;
    m.space = std::make_unique<HalfIpSpace>(dim);
    m.hnsw.reset();
    // 版と次元は横の .meta に書いてある
    std::string meta;
    if (FILE* f = openFile(path + ".meta", "rb")) {
        char buf[512];
        const size_t n = std::fread(buf, 1, sizeof buf, f);
        std::fclose(f);
        meta.assign(buf, n);
    }
    if (meta == std::to_string(dim) + "\n" + version && fileSize(path) > 0) {
        try {
            m.hnsw = std::make_unique<hnswlib::HierarchicalNSW<float>>(m.space.get(), path, false, 0, true);
        } catch (const std::exception&) {
            m.hnsw.reset();  // 壊れていたら作り直す
        }
    }
    if (!m.hnsw) m.hnsw = std::make_unique<hnswlib::HierarchicalNSW<float>>(m.space.get(), 1024, kM, kEfConstruction, 100, true);
    m.hnsw->setEf(kEfSearch);
    return true;
}

bool AnnIndex::save() {
    Impl& m = *impl_;
    if (!m.hnsw) return false;
    const std::string tmp = m.path + ".tmp";
    try {
        m.hnsw->saveIndex(tmp);
    } catch (const std::exception&) {
        return false;
    }
    if (!replaceFile(tmp, m.path)) return false;
    if (FILE* f = openFile(m.path + ".meta", "wb")) {
        const std::string meta = std::to_string(m.dim) + "\n" + m.version;
        std::fwrite(meta.data(), 1, meta.size(), f);
        std::fclose(f);
        return true;
    }
    return false;
}

size_t AnnIndex::size() const {
    auto& h = *impl_->hnsw;
    return h.getCurrentElementCount() - h.getDeletedCount();
}

bool AnnIndex::has(int64_t id) const {
    auto& h = *impl_->hnsw;
    std::unique_lock<std::mutex> lock(h.label_lookup_lock);
    auto it = h.label_lookup_.find(static_cast<hnswlib::labeltype>(id));
    return it != h.label_lookup_.end() && !h.isMarkedDeleted(it->second);
}

bool AnnIndex::sync(const VectorIndex& vecs, const std::function<bool(int64_t)>& alive, unsigned threads,
                    const std::atomic<bool>& cancel, Progress* progress) {
    auto& h = *impl_->hnsw;
    bool changed = false;
    // 消えた画像
    std::vector<int64_t> dead;
    {
        std::unique_lock<std::mutex> lock(h.label_lookup_lock);
        for (const auto& [label, internal] : h.label_lookup_)
            if (!h.isMarkedDeleted(internal) && (!alive(static_cast<int64_t>(label)) || !vecs.has(static_cast<int64_t>(label))))
                dead.push_back(static_cast<int64_t>(label));
    }
    for (int64_t id : dead) {
        h.markDelete(static_cast<hnswlib::labeltype>(id));
        changed = true;
    }
    // 足す画像
    std::vector<int64_t> add;
    for (int64_t id : vecs.ids())
        if (alive(id) && !has(id)) add.push_back(id);
    if (add.empty()) return changed;
    if (h.getCurrentElementCount() + add.size() > h.getMaxElements())
        h.resizeIndex(std::max(h.getMaxElements() * 2, h.getCurrentElementCount() + add.size() + 1024));
    if (progress) {
        progress->total = add.size();
        progress->done = 0;
    }
    if (!threads) threads = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i; !cancel && (i = next.fetch_add(1)) < add.size();) {
            // 消した印の付いた場所を使い回す（同じ画像がまた来た時も）
            h.addPoint(vecs.raw(add[i]), static_cast<hnswlib::labeltype>(add[i]), true);
            if (progress) progress->done.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (auto& th : pool) th.join();
    return true;
}

std::vector<std::pair<int64_t, float>> AnnIndex::knn(const uint16_t* q, size_t k) const {
    std::vector<std::pair<int64_t, float>> out;
    auto& h = *impl_->hnsw;
    if (!q || !k || h.getCurrentElementCount() == 0) return out;
    auto pq = h.searchKnn(q, k);
    while (!pq.empty()) {
        out.emplace_back(static_cast<int64_t>(pq.top().second), 1.f - pq.top().first);
        pq.pop();
    }
    std::reverse(out.begin(), out.end());  // 近い順
    return out;
}

std::vector<std::vector<int64_t>> variantGroups(const AnnIndex& ann, const VectorIndex& vecs, double minSim, size_t k,
                                                unsigned threads, const std::atomic<bool>& cancel, Progress* progress) {
    std::vector<int64_t> ids;
    for (int64_t id : vecs.ids())
        if (ann.has(id)) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    std::unordered_map<int64_t, size_t> index;
    for (size_t i = 0; i < ids.size(); ++i) index[ids[i]] = i;
    // それぞれの画像の近い k 個（似ている度合い minSim 以上だけ）
    std::vector<std::vector<std::pair<size_t, float>>> near(ids.size());
    if (progress) {
        progress->total = ids.size();
        progress->done = 0;
    }
    if (!threads) threads = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i; !cancel && (i = next.fetch_add(1)) < ids.size();) {
            for (const auto& [id, sim] : ann.knn(vecs.raw(ids[i]), k + 1)) {
                if (id == ids[i] || sim < minSim) continue;
                auto it = index.find(id);
                if (it != index.end()) near[i].emplace_back(it->second, sim);
            }
            std::sort(near[i].begin(), near[i].end());
            if (progress) progress->done.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (auto& th : pool) th.join();
    if (cancel) return {};
    // お互いに近い 2 枚だけをつなぐ（つながりが長く伸びて大きなまとまりになるのを防ぐ）
    struct Edge {
        size_t a, b;
        float sim;
    };
    std::vector<Edge> edges;
    for (size_t i = 0; i < ids.size(); ++i)
        for (const auto& [j, sim] : near[i]) {
            if (j <= i) continue;
            auto it = std::lower_bound(near[j].begin(), near[j].end(), std::make_pair(i, -2.f));
            if (it != near[j].end() && it->first == i) edges.push_back({i, j, std::min(sim, it->second)});
        }
    // それでも大きすぎるまとまり（似た絵柄が鎖のようにつながったもの）は、下限を上げて分け直す
    constexpr size_t kMaxGroup = 100;
    std::vector<std::vector<int64_t>> out;
    std::function<void(const std::vector<size_t>&, const std::vector<Edge>&, double)> cluster =
        [&](const std::vector<size_t>& nodes, const std::vector<Edge>& es, double th) {
            std::unordered_map<size_t, size_t> local;
            for (size_t n = 0; n < nodes.size(); ++n) local[nodes[n]] = n;
            UnionFind uf(nodes.size());
            for (const auto& e : es)
                if (e.sim >= th) uf.unite(local[e.a], local[e.b]);
            std::unordered_map<size_t, std::vector<size_t>> byRoot;
            for (size_t n = 0; n < nodes.size(); ++n) byRoot[uf.find(n)].push_back(nodes[n]);
            for (auto& [root, members] : byRoot) {
                if (members.size() < 2) continue;
                if (members.size() > kMaxGroup && th < 0.99) {
                    std::unordered_set<size_t> in(members.begin(), members.end());
                    std::vector<Edge> sub;
                    for (const auto& e : es)
                        if (e.sim >= th && in.count(e.a)) sub.push_back(e);
                    cluster(members, sub, std::min(0.99, th + 0.01));
                    continue;
                }
                std::vector<int64_t> g;
                for (size_t n : members) g.push_back(ids[n]);
                std::sort(g.begin(), g.end());
                out.push_back(std::move(g));
            }
        };
    std::vector<size_t> all(ids.size());
    std::iota(all.begin(), all.end(), 0);
    cluster(all, edges, minSim);
    std::sort(out.begin(), out.end());
    return out;
}

void replaceVariantGroups(Db& db, const std::vector<std::vector<int64_t>>& groups) {
    db.begin();
    try {
        db.exec("DELETE FROM groups WHERE kind='variant'");
        Stmt g(db.handle(), "INSERT INTO groups(kind,created) VALUES('variant',?)");
        Stmt m(db.handle(), "INSERT INTO group_members(group_id,image_id,representative,distance) VALUES(?,?,?,0)");
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        for (const auto& grp : groups) {
            g.bind(1, now);
            g.step();
            g.reset();
            Stmt last(db.handle(), "SELECT last_insert_rowid()");
            last.step();
            const int64_t gid = last.i64(0);
            for (size_t i = 0; i < grp.size(); ++i) {
                m.bind(1, gid).bind(2, grp[i]).bind(3, static_cast<int64_t>(i == 0));
                m.step();
                m.reset();
            }
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}

}  // namespace imgdupe
