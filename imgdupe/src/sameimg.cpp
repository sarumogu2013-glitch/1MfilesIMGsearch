#include "sameimg.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>

namespace imgdupe {

namespace {

// 比べる用: 小文字（ASCII）、/ を \ に、末尾の区切りを除く
std::string normPath(const std::string& p) {
    std::string s = p;
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '/') c = '\\';
    }
    while (s.size() > 1 && s.back() == '\\') s.pop_back();
    return s;
}

std::string trimSep(std::string s) {
    while (s.size() > 1 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}

// p が base と同じか、その下か（比べる用の形で）
bool under(const std::string& np, const std::string& nb) {
    return np == nb || (np.size() > nb.size() && np.compare(0, nb.size(), nb) == 0 && np[nb.size()] == '\\');
}

std::string folderOf(const std::string& path) {
    const size_t sep = path.find_last_of("\\/");
    return sep == std::string::npos ? std::string() : path.substr(0, sep);
}

}  // namespace

bool SameImageIndex::refresh(Db& db, int hashVersion) {
    int64_t count = 0, maxId = 0;
    {
        Stmt st(db.handle(), "SELECT count(*), coalesce(max(image_id),0) FROM stages WHERE stage='hash' AND state=1 AND version=?");
        st.bind(1, static_cast<int64_t>(hashVersion));
        if (st.step()) {
            count = st.i64(0);
            maxId = st.i64(1);
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        if (count == count_ && maxId == maxId_) return false;
    }
    std::vector<Row> rows;
    rows.reserve(static_cast<size_t>(count));
    Stmt st(db.handle(),
            "SELECT i.id, i.dh, i.dv, i.color, i.contrast FROM images i JOIN stages s ON s.image_id=i.id AND s.stage='hash' "
            "WHERE s.state=1 AND s.version=? AND i.readable=1");
    st.bind(1, static_cast<int64_t>(hashVersion));
    while (st.step()) {
        const int64_t c = st.i64(3);
        rows.push_back({st.i64(0), static_cast<uint64_t>(st.i64(1)), static_cast<uint64_t>(st.i64(2)), static_cast<uint8_t>(c >> 16),
                        static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c), static_cast<uint8_t>(st.i64(4))});
    }
    std::lock_guard<std::mutex> lock(m_);
    rows_ = std::move(rows);
    count_ = count;
    maxId_ = maxId;
    return true;
}

size_t SameImageIndex::size() const {
    std::lock_guard<std::mutex> lock(m_);
    return rows_.size();
}

std::vector<SameMatch> SameImageIndex::find(Db& db, const SameQuery& q,
                                            const std::function<bool(const std::string&, Sha256&)>& shaOf) const {
    std::map<int64_t, SameMatch> found;
    const std::string self = normPath(q.selfPath);
    // 完全一致: 大きさが同じものの SHA-256 を比べる
    if (q.size > 0 && q.hasSha) {
        Stmt st(db.handle(), "SELECT id, path, sha, width, height FROM images WHERE size=?");
        st.bind(1, static_cast<int64_t>(q.size));
        while (st.step()) {
            SameMatch m;
            m.id = st.i64(0);
            m.path = st.text(1);
            if (!self.empty() && normPath(m.path) == self) continue;
            const auto blob = st.blob(2);
            Sha256 sha{};
            if (blob.size() == 32) std::copy(blob.begin(), blob.end(), sha.begin());
            else if (!shaOf || !shaOf(m.path, sha)) continue;
            if (sha != q.sha) continue;
            m.size = q.size;
            m.width = static_cast<uint32_t>(st.i64(3));
            m.height = static_cast<uint32_t>(st.i64(4));
            m.exact = true;
            found[m.id] = m;
        }
    }
    // 見た目一致
    std::vector<std::pair<int, int64_t>> near;
    if (q.f.ok && q.f.contrast >= q.flatContrast) {
        std::lock_guard<std::mutex> lock(m_);
        for (const Row& r : rows_) {
            const int d = popcount64(r.dh ^ q.f.dh) + popcount64(r.dv ^ q.f.dv);
            if (d > q.threshold || r.contrast < q.flatContrast) continue;
            if (std::abs(r.r - q.f.r) > q.colorTolerance || std::abs(r.g - q.f.g) > q.colorTolerance ||
                std::abs(r.b - q.f.b) > q.colorTolerance)
                continue;
            near.push_back({d, r.id});
        }
    }
    std::sort(near.begin(), near.end());
    if (near.size() > 5000) near.resize(5000);  // 多すぎる時（ほとんど同じ画像ばかりのフォルダなど）は近いものだけ
    Stmt info(db.handle(), "SELECT path, size, width, height FROM images WHERE id=?");
    for (const auto& [d, id] : near) {
        auto it = found.find(id);
        if (it != found.end()) continue;  // 完全一致に入っている
        info.bind(1, id);
        if (info.step()) {
            SameMatch m;
            m.id = id;
            m.path = info.text(0);
            m.size = static_cast<uint64_t>(info.i64(1));
            m.width = static_cast<uint32_t>(info.i64(2));
            m.height = static_cast<uint32_t>(info.i64(3));
            m.distance = d;
            if (self.empty() || normPath(m.path) != self) found[id] = m;
        }
        info.reset();
    }
    std::vector<SameMatch> out;
    for (auto& [id, m] : found) out.push_back(std::move(m));
    std::stable_sort(out.begin(), out.end(), [](const SameMatch& a, const SameMatch& b) {
        if (a.exact != b.exact) return a.exact;
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.path < b.path;
    });
    return out;
}

std::vector<SameFolder> groupSameByFolder(const std::vector<SameMatch>& matches) {
    std::vector<SameFolder> out;
    std::map<std::string, size_t> index;  // 比べる用の形 → out の添字
    for (size_t i = 0; i < matches.size(); ++i) {
        const std::string folder = folderOf(matches[i].path);
        const std::string key = normPath(folder);
        auto it = index.find(key);
        if (it == index.end()) {
            it = index.emplace(key, out.size()).first;
            out.push_back(SameFolder{folder, 0, 0, {}});
        }
        SameFolder& f = out[it->second];
        (matches[i].exact ? f.exact : f.similar)++;
        f.matches.push_back(i);
    }
    std::stable_sort(out.begin(), out.end(), [](const SameFolder& a, const SameFolder& b) {
        if (a.exact != b.exact) return a.exact > b.exact;
        return a.exact + a.similar > b.exact + b.similar;
    });
    return out;
}

std::unordered_map<std::string, IndexState> fileIndexStates(Db& db, const std::string& dir, const std::vector<std::string>& roots,
                                                            int hashVersion) {
    std::unordered_map<std::string, IndexState> out;
    // DB のパスは roots の書き方で始まるので、roots の中なら頭をそろえる
    std::string prefix = trimSep(dir);
    const std::string nd = normPath(dir);
    size_t best = 0;
    for (const auto& r : roots) {
        const std::string nr = normPath(r);
        if (under(nd, nr) && nr.size() >= best) {
            best = nr.size();
            prefix = trimSep(r) + trimSep(dir).substr(trimSep(r).size());
        }
    }
    for (const char sep : {'\\', '/'}) {
        std::string p = prefix;
        if (sep == '/') std::replace(p.begin(), p.end(), '\\', '/');
        else std::replace(p.begin(), p.end(), '/', '\\');
        p += sep;
        Stmt st(db.handle(),
                "SELECT i.path, coalesce(s.state, 0) FROM images i "
                "LEFT JOIN stages s ON s.image_id=i.id AND s.stage='hash' AND s.version=? "
                "WHERE i.path >= ? AND i.path < ?");
        st.bind(1, static_cast<int64_t>(hashVersion)).bind(2, p).bind(3, p + "\xff");
        while (st.step()) {
            const std::string rest = st.text(0).substr(p.size());
            if (rest.empty() || rest.find_first_of("\\/") != std::string::npos) continue;  // 下の階層
            out[rest] = st.i64(1) != 0 ? IndexState::Indexed : IndexState::Pending;  // 読めなかった（失敗）も処理は済んでいる
        }
    }
    return out;
}

bool folderHasIndexed(Db& db, const std::string& dir) {
    for (const char sep : {'\\', '/'}) {
        std::string p = trimSep(dir);
        std::replace(p.begin(), p.end(), sep == '/' ? '\\' : '/', sep);
        p += sep;
        Stmt st(db.handle(), "SELECT 1 FROM images WHERE path >= ? AND path < ? LIMIT 1");
        st.bind(1, p).bind(2, p + "\xff");
        if (st.step()) return true;
    }
    return false;
}

IndexState folderIndexState(const std::string& dir, const std::vector<std::string>& roots) {
    const std::string nd = normPath(dir);
    bool partial = false;
    for (const auto& r : roots) {
        const std::string nr = normPath(r);
        if (nr.empty()) continue;
        if (under(nd, nr)) return IndexState::Indexed;
        if (under(nr, nd)) partial = true;
    }
    return partial ? IndexState::Partial : IndexState::None;
}

}  // namespace imgdupe
