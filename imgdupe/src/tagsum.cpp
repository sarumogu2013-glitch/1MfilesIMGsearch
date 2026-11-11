#include "tagsum.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include "dict.hpp"
#include "meta.hpp"
#include "refs.hpp"
#include "search.hpp"
#include "tagger.hpp"

namespace imgdupe {

namespace {

std::string lowerKey(const std::string& s) {
    std::string k = s;
    for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k;
}

}  // namespace

bool aiTagSource(const std::string& source) {
    return source == kSourceTagger || source == kSourceDict || source == kSourceClip || source == kSourceReference;
}

std::vector<TagSummary> summarizeTags(Db& db, const std::vector<int64_t>& ids, bool edit, const std::atomic<bool>* cancel) {
    struct Sum {
        std::string tag, kind;
        bool file = false;
        int count = 0;
        double lo = 2, hi = -1;
        int stLo = 3, stHi = -1;
        bool editable = false;
    };
    std::vector<Sum> sums;
    std::unordered_map<std::string, size_t> at;  // 小文字のタグ（埋め込みは印付き）→ sums の番号
    struct Agg {
        std::string tag, kind;
        double keep = 1;  // 1 - 合わせた確度（combineConfidence と同じ）
        int manual = -1;
        bool file = false, dbOnly = false, ai = false;
    };
    Stmt st(db.handle(), "SELECT tag,kind,source,confidence,manual FROM tags WHERE image_id=? ORDER BY kind,confidence DESC,tag");
    std::vector<Agg> agg;
    std::unordered_map<std::string, size_t> aggAt;
    for (int64_t id : ids) {
        if (cancel && *cancel) return {};
        agg.clear();
        aggAt.clear();
        st.bind(1, id);
        while (st.step()) {
            const std::string kind = st.text(1);
            if (kind == "rating") continue;  // general / sensitive などの年齢区分は出さない
            const std::string tag = st.text(0), source = st.text(2);
            const double conf = st.f64(3);
            const bool manual = st.i64(4) != 0;
            auto f = aggAt.emplace(lowerKey(tag), agg.size());
            if (f.second) agg.push_back(Agg{tag, kind});
            Agg& a = agg[f.first->second];
            if (source == kSourceEmbedded) a.file = true;
            else a.dbOnly = true;
            if (manual) a.manual = conf > 0.5 ? 1 : 0;
            else a.keep *= 1 - std::min(1.0, std::max(0.0, conf));
            if (aiTagSource(source)) a.ai = true;
        }
        st.reset();
        auto add = [&](const Agg& a, bool file, double p, int state, bool editable) {
            auto f = at.emplace(lowerKey(a.tag) + (file ? "\x01" : ""), sums.size());
            if (f.second) {
                Sum s;
                s.tag = a.tag;
                s.kind = a.kind;
                s.file = file;
                sums.push_back(std::move(s));
            }
            Sum& u = sums[f.first->second];
            ++u.count;
            u.lo = std::min(u.lo, p);
            u.hi = std::max(u.hi, p);
            u.stLo = std::min(u.stLo, state);
            u.stHi = std::max(u.stHi, state);
            u.editable = u.editable || editable;
        };
        for (const auto& a : agg) {
            const double p = a.manual >= 0 ? a.manual : 1 - a.keep;
            const int state = a.manual == 1 ? 2 : a.manual == 0 ? 0 : 1;
            // 手で否定したタグは、ファイルに書き込まれていても出さない（ファイルは書き換えない）
            if (a.file && a.manual != 0) add(a, true, 1.0, state, false);
            // 手動編集では、否定したタグも出す（クリックで戻せるように）
            if (a.dbOnly && (p >= 0.35 || (edit && a.manual >= 0))) add(a, false, p, state, a.ai || a.manual >= 0);
        }
    }
    std::vector<std::pair<double, TagSummary>> rows;
    rows.reserve(sums.size());
    for (const auto& u : sums) {
        TagSummary t;
        t.tag = u.tag;
        t.kind = u.kind;
        t.confidence = edit ? u.lo : u.hi;
        t.file = u.file;
        t.count = u.count;
        t.total = static_cast<int>(ids.size());
        t.state = u.stLo;
        t.mixed = u.stLo != u.stHi;
        t.editable = u.editable;
        rows.push_back({u.file ? 2.0 : t.confidence, std::move(t)});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    std::vector<TagSummary> out;
    out.reserve(rows.size());
    for (auto& r : rows) out.push_back(std::move(r.second));
    return out;
}

}  // namespace imgdupe
