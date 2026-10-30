#include "refs.hpp"
#include "tagstore.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace imgdupe {

std::vector<RefSet> collectRefSets(Db& db, const std::string& refsPrefix) {
    std::map<std::string, RefSet> byTag;  // 名前（小文字にしない。タグはそのまま）
    // 手動で「正しい」と確定したタグ
    {
        Stmt st(db.handle(), "SELECT tag, kind, image_id FROM tags WHERE manual=1 AND confidence >= 0.5 ORDER BY tag, image_id");
        while (st.step()) {
            RefSet& r = byTag[st.text(0)];
            r.tag = st.text(0);
            if (r.kind.empty()) r.kind = st.text(1);
            r.ids.push_back(st.i64(2));
        }
        for (auto it = byTag.begin(); it != byTag.end();)
            it = it->second.ids.size() < kMinManualReferences ? byTag.erase(it) : std::next(it);
    }
    // refs\<名前>\ のフォルダの画像
    if (!refsPrefix.empty()) {
        Stmt st(db.handle(), "SELECT id, path FROM images WHERE substr(path, 1, ?) = ? ORDER BY id");
        st.bind(1, static_cast<int64_t>(refsPrefix.size())).bind(2, refsPrefix);
        while (st.step()) {
            const std::string rest = st.text(1).substr(refsPrefix.size());
            const size_t sep = rest.find_first_of("\\/");
            if (sep == std::string::npos || sep == 0) continue;  // refs の直下のファイルは使わない
            const std::string name = rest.substr(0, sep);
            RefSet& r = byTag[name];
            r.tag = name;
            if (r.kind.empty()) {
                // 同じ名前のタグが既にあれば、その種類にそろえる（無ければキャラ）
                Stmt k(db.handle(), "SELECT kind FROM tags WHERE tag=? LIMIT 1");
                k.bind(1, name);
                r.kind = k.step() ? k.text(0) : "character";
            }
            r.ids.push_back(st.i64(0));
        }
    }
    std::vector<RefSet> out;
    for (auto& [tag, r] : byTag) {
        std::sort(r.ids.begin(), r.ids.end());
        r.ids.erase(std::unique(r.ids.begin(), r.ids.end()), r.ids.end());
        out.push_back(std::move(r));
    }
    return out;
}

double referenceProbability(double sim) { return 1.0 / (1.0 + std::exp(-(sim - 0.80) * 25.0)); }

std::string refStateHash(const std::vector<RefSet>& sets, size_t vectorRecords) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            h ^= static_cast<const unsigned char*>(p)[i];
            h *= 1099511628211ull;
        }
    };
    for (const auto& s : sets) {
        mix(s.tag.data(), s.tag.size());
        mix(s.kind.data(), s.kind.size());
        for (int64_t id : s.ids) mix(&id, sizeof id);
    }
    mix(&vectorRecords, sizeof vectorRecords);
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

size_t applyReferences(Db& db, const VectorIndex& vecs, const std::vector<RefSet>& sets, unsigned threads) {
    size_t used = 0;
    db.begin();
    try {
        deleteSourceTags(db, "reference");
        TagWriter ins(db, TagConflict::Replace);
        std::vector<float> v, mean;
        for (const auto& s : sets) {
            mean.assign(vecs.dim(), 0.f);
            size_t n = 0;
            for (int64_t id : s.ids)
                if (vecs.get(id, v)) {
                    for (size_t i = 0; i < v.size(); ++i) mean[i] += v[i];
                    ++n;
                }
            if (!n) continue;
            normalizeVector(mean);
            ++used;
            for (const auto& [id, sim] : vecs.topK(mean.data(), vecs.size(), {}, threads)) {
                const double p = referenceProbability(sim);
                if (p < 0.2) break;  // 内積の大きい順
                ins.put(id, s.tag, s.kind, p, "reference", false);
            }
        }
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    return used;
}

}  // namespace imgdupe
