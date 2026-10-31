#include "sameimg.hpp"

#include "meta.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
                                                            int hashVersion, const std::vector<std::string>* names) {
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
    if (names) {
        // 名前ごとに引く（パスの索引で 1 件ずつ）
        Stmt st(db.handle(),
                "SELECT coalesce(s.state, 0) FROM images i "
                "LEFT JOIN stages s ON s.image_id=i.id AND s.stage='hash' AND s.version=? WHERE i.path=?");
        for (const auto& name : *names)
            for (const char sep : {'\\', '/'}) {
                std::string p = prefix;
                std::replace(p.begin(), p.end(), sep == '/' ? '\\' : '/', sep);
                st.bind(1, static_cast<int64_t>(hashVersion)).bind(2, p + sep + name);
                const bool found = st.step();
                if (found) out[name] = st.i64(0) != 0 ? IndexState::Indexed : IndexState::Pending;
                st.reset();
                if (found) break;
            }
        return out;
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

std::unordered_map<std::string, ProgressCount> childProgress(Db& db, const std::string& dir, const std::vector<std::string>& roots,
                                                             const std::vector<StageKey>& stages, const std::atomic<bool>* cancel) {
    std::unordered_map<std::string, ProgressCount> out;
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
    // 段階ごとに、済んだ（版も同じ。失敗も済み）か（主キーで 1 つずつ引く）
    std::string sql = "SELECT i.path";
    for (size_t k = 0; k < stages.size(); ++k)
        sql += ", (SELECT s.state<>0 FROM stages s WHERE s.image_id=i.id AND s.stage=? AND s.version=?)";
    sql += " FROM images i WHERE i.path >= ? AND i.path < ?";
    for (const char sep : {'\\', '/'}) {
        std::string p = prefix;
        if (sep == '/') std::replace(p.begin(), p.end(), '\\', '/');
        else std::replace(p.begin(), p.end(), '/', '\\');
        p += sep;
        Stmt st(db.handle(), sql.c_str());
        int n = 1;
        for (const auto& k : stages) {
            st.bind(n++, k.stage);
            st.bind(n++, static_cast<int64_t>(k.version));
        }
        st.bind(n++, p).bind(n, p + "\xff");
        int64_t rows = 0;
        while (st.step()) {
            if (cancel && (++rows & 1023) == 0 && cancel->load()) return {};
            const std::string rest = st.text(0).substr(p.size());
            const size_t cut = rest.find_first_of("\\/");
            ProgressCount& c = out[cut == std::string::npos ? rest : rest.substr(0, cut)];
            ++c.images;
            if (c.doneBy.size() < stages.size()) c.doneBy.resize(stages.size(), 0);
            for (size_t k = 0; k < stages.size(); ++k)
                if (st.i64(static_cast<int>(k) + 1) != 0) {
                    ++c.doneBy[k];
                    ++c.done;
                }
        }
    }
    return out;
}

double weightedProgress(const ProgressCount& c, const std::vector<double>& weights) {
    if (c.images <= 0) return 0;
    double all = 0, done = 0;
    for (size_t k = 0; k < weights.size(); ++k) {
        all += weights[k];
        if (k < c.doneBy.size()) done += weights[k] * static_cast<double>(c.doneBy[k]) / static_cast<double>(c.images);
    }
    return all > 0 ? std::clamp(done / all, 0.0, 1.0) : 0;
}

int64_t findImageId(Db& db, const std::string& path, const std::vector<std::string>& roots) {
    std::vector<std::string> cands = {path};
    const std::string np = normPath(path);
    for (const auto& r : roots) {
        const std::string nr = normPath(r);
        if (!nr.empty() && under(np, nr)) cands.push_back(trimSep(r) + path.substr(trimSep(r).size()));
    }
    Stmt st(db.handle(), "SELECT id FROM images WHERE path=?");
    for (const auto& c : cands)
        for (const char sep : {'\\', '/'}) {
            std::string p = c;
            std::replace(p.begin(), p.end(), sep == '/' ? '\\' : '/', sep);
            st.bind(1, p);
            if (st.step()) return st.i64(0);
            st.reset();
        }
    return 0;
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

std::vector<std::vector<size_t>> clusterResults(Db& db, const std::vector<int64_t>& ids) {
    std::unordered_map<int64_t, size_t> pos;  // 画像 ID → ids の添字
    for (size_t i = 0; i < ids.size(); ++i) pos.emplace(ids[i], i);
    // 結果どうしを、同じまとまりなら 1 つに（素集合）
    std::vector<size_t> parent(ids.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
    std::function<size_t(size_t)> root = [&](size_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    if (ids.size() > 1) {
        Stmt st(db.handle(),
                "SELECT m.group_id, m.image_id FROM group_members m JOIN groups g ON g.id=m.group_id "
                "WHERE g.kind IN ('exact','similar','variant') ORDER BY m.group_id");
        int64_t current = -1;
        size_t first = SIZE_MAX;
        while (st.step()) {
            const int64_t gid = st.i64(0);
            if (gid != current) {
                current = gid;
                first = SIZE_MAX;
            }
            auto it = pos.find(st.i64(1));
            if (it == pos.end()) continue;
            if (first == SIZE_MAX) first = it->second;
            else parent[root(it->second)] = root(first);
        }
    }
    std::vector<std::vector<size_t>> out;
    std::unordered_map<size_t, size_t> clusterOf;  // 根 → out の添字
    for (size_t i = 0; i < ids.size(); ++i) {
        const size_t r = root(i);
        auto it = clusterOf.find(r);
        if (it == clusterOf.end()) {
            clusterOf.emplace(r, out.size());
            out.push_back({i});
        } else {
            out[it->second].push_back(i);
        }
    }
    return out;
}

namespace {

std::string dateTimeText(int64_t t) {
    int y, mo, d, h, mi, s;
    unixToCivil(t, y, mo, d, h, mi, s);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
    return buf;
}

std::string sizeText(int64_t bytes) {
    char buf[32];
    if (bytes >= 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024) std::snprintf(buf, sizeof buf, "%.1f KB", bytes / 1024.0);
    else std::snprintf(buf, sizeof buf, "%lld バイト", static_cast<long long>(bytes));
    return buf;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> imageProperties(Db& db, int64_t id,
                                                                 const std::function<std::string(int64_t)>& timeText) {
    std::vector<std::pair<std::string, std::string>> out;
    auto when = [&](int64_t t) { return timeText ? timeText(t) : dateTimeText(t); };
    auto add = [&](const char* name, const std::string& v) {
        if (!v.empty()) out.emplace_back(name, v);
    };
    {
        Stmt st(db.handle(), "SELECT path, size, width, height, mtime FROM images WHERE id=?");
        st.bind(1, id);
        if (!st.step()) return out;
        add("パス", st.text(0));
        if (st.i64(2) > 0) add("大きさ", std::to_string(st.i64(2)) + " × " + std::to_string(st.i64(3)) + " px");
        add("ファイルサイズ", sizeText(st.i64(1)));
        if (st.i64(4) > 0) add("更新日時", when(fileTimeToUnix(static_cast<uint64_t>(st.i64(4)))));
    }
    {
        Stmt st(db.handle(),
                "SELECT format, bit_depth, animated, taken, camera, lens, focal, iso, has_gps, software, rating, title, subject, "
                "comment, author, created, zone_url, png_text FROM file_meta WHERE image_id=?");
        st.bind(1, id);
        if (st.step()) {
            std::string fmt = st.text(0);
            if (!fmt.empty() && st.i64(1) > 0) fmt += "（" + std::to_string(st.i64(1)) + " ビット）";
            if (st.i64(2)) fmt += "・アニメーション";
            add("形式", fmt);
            if (st.i64(15) > 0) add("作成日時", when(st.i64(15)));
            if (st.i64(3) > 0) add("撮影日時", dateTimeText(st.i64(3)));  // EXIF の時刻はその場所の時刻のまま持っている
            if (!st.isNull(10) && st.i64(10) >= 0) add("星", std::string(static_cast<size_t>(st.i64(10)), '*') + " (" + std::to_string(st.i64(10)) + ")");
            add("タイトル", st.text(11));
            add("件名", st.text(12));
            add("コメント", st.text(13));
            add("作者", st.text(14));
            add("カメラ", st.text(4));
            add("レンズ", st.text(5));
            if (st.f64(6) > 0) {
                char b[32];
                std::snprintf(b, sizeof b, "%.0f mm", st.f64(6));
                add("焦点距離", b);
            }
            if (st.i64(7) > 0) add("ISO", std::to_string(st.i64(7)));
            if (st.i64(8)) add("位置情報", "あり");
            add("ソフトウェア", st.text(9));
            add("ダウンロード元", st.text(16));
            std::string png = st.text(17);
            if (png.size() > 400) png = png.substr(0, 400) + "…";
            add("PNG のテキスト", png);
        }
    }
    {
        Stmt st(db.handle(), "SELECT sharpness, detail, effective, aesthetic FROM quality WHERE image_id=?");
        st.bind(1, id);
        if (st.step()) {
            char b[96];
            if (!st.isNull(1)) {
                std::snprintf(b, sizeof b, "細かさ %.2f（実質 %lldpx）・くっきり %.2f", st.f64(1), static_cast<long long>(st.i64(2)),
                              st.isNull(0) ? 0.0 : st.f64(0));
                add("品質", b);
            }
            if (!st.isNull(3)) {
                std::snprintf(b, sizeof b, "%.1f", st.f64(3));
                add("見栄え", b);
            }
        }
    }
    {
        Stmt st(db.handle(),
                "SELECT g.kind, count(*) - 1 FROM group_members m JOIN groups g ON g.id=m.group_id "
                "JOIN group_members o ON o.group_id=m.group_id WHERE m.image_id=? GROUP BY m.group_id");
        st.bind(1, id);
        while (st.step()) {
            const std::string k = st.text(0);
            const std::string n = std::to_string(st.i64(1)) + " 枚";
            if (k == "exact") add("完全一致", n);
            else if (k == "similar") add("見た目一致", n);
            else if (k == "variant") add("差分", n);
        }
    }
    return out;
}

// ---------------------------------------------------------------- 画像で探す

std::string queryImageName(int64_t addedUnix, int seq, const std::string& ext) {
    return "q_" + std::to_string(addedUnix) + "_" + std::to_string(seq) + ext;
}

int64_t queryImageTime(const std::string& name) {
    if (name.size() < 5 || name.compare(0, 2, "q_") != 0) return 0;
    int64_t t = 0;
    size_t i = 2;
    for (; i < name.size() && name[i] >= '0' && name[i] <= '9'; ++i) t = t * 10 + (name[i] - '0');
    if (i == 2 || i >= name.size() || name[i] != '_') return 0;
    return t;
}

std::vector<std::string> queryImagesToRemove(const std::vector<std::string>& names, int64_t now, size_t maxCount, int64_t maxAge) {
    std::vector<std::pair<int64_t, std::string>> ours;
    for (const auto& n : names)
        if (const int64_t t = queryImageTime(n)) ours.emplace_back(t, n);
    // 新しい順（同じ秒なら名前の大きい方 = 後から足した方）
    std::sort(ours.begin(), ours.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second > b.second; });
    std::vector<std::string> out;
    for (size_t i = 0; i < ours.size(); ++i)
        if (i >= maxCount || now - ours[i].first > maxAge) out.push_back(ours[i].second);
    return out;
}

std::vector<uint8_t> bmpFromDib(const uint8_t* dib, size_t size) {
    auto u32 = [&](size_t at) {
        return static_cast<uint32_t>(dib[at]) | static_cast<uint32_t>(dib[at + 1]) << 8 | static_cast<uint32_t>(dib[at + 2]) << 16 |
               static_cast<uint32_t>(dib[at + 3]) << 24;
    };
    if (!dib || size < 40) return {};
    const uint32_t headerSize = u32(0);
    if (headerSize < 40 || headerSize > size) return {};
    const unsigned bitCount = static_cast<unsigned>(dib[14] | dib[15] << 8);
    const uint32_t compression = u32(16), clrUsed = u32(32);
    size_t extra = 0;
    if (headerSize == 40 && (compression == 3 || compression == 6)) extra = compression == 3 ? 12 : 16;  // BI_BITFIELDS の色のマスク
    const size_t colors = clrUsed ? clrUsed : bitCount <= 8 ? (size_t(1) << bitCount) : 0;
    const size_t offBits = 14 + headerSize + extra + colors * 4;
    if (offBits - 14 > size) return {};
    std::vector<uint8_t> out(14 + size);
    const uint32_t fileSize = static_cast<uint32_t>(out.size());
    out[0] = 'B';
    out[1] = 'M';
    for (int k = 0; k < 4; ++k) {
        out[2 + k] = static_cast<uint8_t>(fileSize >> (8 * k));
        out[10 + k] = static_cast<uint8_t>(offBits >> (8 * k));
    }
    std::copy(dib, dib + size, out.begin() + 14);
    return out;
}

}  // namespace imgdupe
