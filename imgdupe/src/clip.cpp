#include "clip.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

#include "json.hpp"

namespace imgdupe {

namespace {

// UTF-8 を 1 文字ずつ（壊れたバイトは U+FFFD）
std::vector<uint32_t> decodeUtf8(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t n = 1;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 6 && i + 1 < s.size()) cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F), n = 2;
        else if ((c >> 4) == 14 && i + 2 < s.size()) cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F), n = 3;
        else if ((c >> 3) == 30 && i + 3 < s.size())
            cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F), n = 4;
        out.push_back(cp);
        i += n;
    }
    return out;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool isSpace(uint32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x3000 || c == 0xA0 || (c >= 0x2000 && c <= 0x200A); }
bool isControl(uint32_t c) { return (c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7F || (c >= 0x80 && c < 0xA0); }

// BERT が 1 文字ずつに分ける漢字の範囲（かな・ハングルは含まない）
bool isCjk(uint32_t c) {
    return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x20000 && c <= 0x2A6DF) ||
           (c >= 0x2A700 && c <= 0x2B73F) || (c >= 0x2B740 && c <= 0x2B81F) || (c >= 0x2B820 && c <= 0x2CEAF) ||
           (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x2F800 && c <= 0x2FA1F);
}

// 記号（ASCII の記号と、Unicode の主な句読点・括弧）
bool isPunct(uint32_t c) {
    return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126) ||
           (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x205E) || (c >= 0x3001 && c <= 0x3003) ||
           (c >= 0x3008 && c <= 0x3011) || (c >= 0x3014 && c <= 0x301F) || c == 0x30FB || (c >= 0xFF01 && c <= 0xFF0F) ||
           (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65) || c == 0xA1 ||
           c == 0xA7 || c == 0xAB || c == 0xB6 || c == 0xB7 || c == 0xBB || c == 0xBF;
}

}  // namespace

bool WordPiece::load(const std::string& vocabTxt) {
    vocab_.clear();
    int64_t id = 0;
    size_t a = 0;
    while (a < vocabTxt.size()) {
        size_t b = vocabTxt.find('\n', a);
        if (b == std::string::npos) b = vocabTxt.size();
        std::string tok = vocabTxt.substr(a, b - a);
        if (!tok.empty() && tok.back() == '\r') tok.pop_back();
        vocab_.emplace(tok, id++);
        a = b + 1;
    }
    auto find = [&](const char* t, int64_t def) {
        auto it = vocab_.find(t);
        return it == vocab_.end() ? def : it->second;
    };
    cls_ = find("[CLS]", 101);
    sep_ = find("[SEP]", 102);
    unk_ = find("[UNK]", 100);
    return vocab_.size() > 3;
}

std::vector<std::string> WordPiece::tokens(const std::string& text) const {
    // 基本の分割: 制御文字を消し、漢字と記号の前後で区切り、空白で分ける
    std::vector<std::vector<uint32_t>> words;
    std::vector<uint32_t> cur;
    auto flush = [&] {
        if (!cur.empty()) words.push_back(std::move(cur));
        cur.clear();
    };
    for (uint32_t c : decodeUtf8(text)) {
        if (c == 0 || c == 0xFFFD || isControl(c)) continue;
        if (isSpace(c)) {
            flush();
        } else if (isCjk(c) || isPunct(c)) {
            flush();
            words.push_back({c});
        } else {
            cur.push_back(c);
        }
    }
    flush();
    // WordPiece: 前から一番長く辞書にあるものを取る（2 つ目からは ## を付ける）
    std::vector<std::string> out;
    for (const auto& w : words) {
        if (w.size() > 100) {
            out.push_back("[UNK]");
            continue;
        }
        std::vector<std::string> pieces;
        size_t start = 0;
        bool bad = false;
        while (start < w.size()) {
            size_t end = w.size();
            std::string found;
            while (start < end) {
                std::string sub = start > 0 ? "##" : "";
                for (size_t i = start; i < end; ++i) appendUtf8(sub, w[i]);
                if (vocab_.count(sub)) {
                    found = std::move(sub);
                    break;
                }
                --end;
            }
            if (found.empty()) {
                bad = true;
                break;
            }
            pieces.push_back(std::move(found));
            start = end;
        }
        if (bad) out.push_back("[UNK]");
        else out.insert(out.end(), pieces.begin(), pieces.end());
    }
    return out;
}

std::vector<int64_t> WordPiece::encode(const std::string& text, size_t maxLen) const {
    std::vector<int64_t> ids{cls_};
    for (const auto& t : tokens(text)) {
        if (ids.size() + 1 >= maxLen) break;
        auto it = vocab_.find(t);
        ids.push_back(it == vocab_.end() ? unk_ : it->second);
    }
    ids.push_back(sep_);
    return ids;
}

bool DenseLayer::load(const std::string& st) {
    if (st.size() < 8) return false;
    uint64_t headerLen;
    std::memcpy(&headerLen, st.data(), 8);
    if (headerLen > st.size() - 8) return false;
    Json j;
    if (!parseJson(st.substr(8, headerLen), j) || j.type != Json::Object) return false;
    for (const auto& [name, t] : j.obj) {
        if (name == "__metadata__" || t.type != Json::Object || t.getString("dtype") != "F32") continue;
        const Json* shape = t.get("shape");
        const Json* offs = t.get("data_offsets");
        if (!shape || shape->arr.size() != 2 || !offs || offs->arr.size() != 2) continue;
        out = std::atoi(shape->arr[0].str.c_str());
        in = std::atoi(shape->arr[1].str.c_str());
        const uint64_t a = std::strtoull(offs->arr[0].str.c_str(), nullptr, 10), b = std::strtoull(offs->arr[1].str.c_str(), nullptr, 10);
        if (out <= 0 || in <= 0 || b - a != static_cast<uint64_t>(out) * in * 4 || 8 + headerLen + b > st.size()) return false;
        w.resize(static_cast<size_t>(out) * in);
        std::memcpy(w.data(), st.data() + 8 + headerLen + a, b - a);
        return true;
    }
    return false;
}

std::vector<float> DenseLayer::apply(const float* x) const {
    std::vector<float> y(static_cast<size_t>(out), 0.f);
    for (int o = 0; o < out; ++o) {
        const float* row = w.data() + static_cast<size_t>(o) * in;
        float s = 0;
        for (int i = 0; i < in; ++i) s += row[i] * x[i];
        y[o] = s;
    }
    return y;
}

void prepareClipInput(const uint8_t* bgra, int w, int h, int side, float* out) {
    static const float mean[3] = {0.48145466f, 0.4578275f, 0.40821073f}, stdv[3] = {0.26862954f, 0.26130258f, 0.27577711f};
    // 短い辺を side に合わせた時の 1 画素が、元の何画素か。中央を切り抜く
    const double scale = static_cast<double>(std::min(w, h)) / side;
    const double ox = (w - side * scale) / 2, oy = (h - side * scale) / 2;
    auto px = [&](int x, int y, int c) {  // c: 0 R, 1 G, 2 B（白を背景に合成）
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        const uint8_t* p = bgra + (static_cast<size_t>(y) * w + x) * 4;
        return (static_cast<float>(p[2 - c]) + (255.f - p[3])) / 255.f;
    };
    const size_t plane = static_cast<size_t>(side) * side;
    for (int y = 0; y < side; ++y) {
        const double sy = oy + (y + 0.5) * scale - 0.5;
        const int y0 = static_cast<int>(std::floor(sy));
        const float fy = static_cast<float>(sy - y0);
        for (int x = 0; x < side; ++x) {
            const double sx = ox + (x + 0.5) * scale - 0.5;
            const int x0 = static_cast<int>(std::floor(sx));
            const float fx = static_cast<float>(sx - x0);
            for (int c = 0; c < 3; ++c) {
                const float v = (px(x0, y0, c) * (1 - fx) + px(x0 + 1, y0, c) * fx) * (1 - fy) +
                                (px(x0, y0 + 1, c) * (1 - fx) + px(x0 + 1, y0 + 1, c) * fx) * fy;
                out[c * plane + static_cast<size_t>(y) * side + x] = (v - mean[c]) / stdv[c];
            }
        }
    }
}

std::vector<float> meanPool(const float* tokens, size_t len, size_t dim, const std::vector<int64_t>& mask) {
    std::vector<float> out(dim, 0.f);
    double n = 0;
    for (size_t t = 0; t < len; ++t) {
        if (t < mask.size() && !mask[t]) continue;
        for (size_t d = 0; d < dim; ++d) out[d] += tokens[t * dim + d];
        n += 1;
    }
    if (n > 0)
        for (float& v : out) v = static_cast<float>(v / n);
    return out;
}

int clipStageVersion(const std::string& modelVersion) {
    uint32_t h = 2166136261u;  // FNV-1a
    for (unsigned char c : modelVersion + "#clip" + std::to_string(kClipCodeVersion)) {
        h ^= c;
        h *= 16777619u;
    }
    return static_cast<int>(h & 0x7fffffff) | 1;
}

RankResult rankByVector(Db& db, const SearchQuery& q, const VectorIndex& index, const float* qv, size_t k, int64_t exclude) {
    RankResult r;
    std::unordered_set<int64_t> allowed;
    const bool filter = hasConditions(q);
    if (filter) {
        SearchQuery cond = q;
        cond.limit = 0;
        cond.review = false;
        for (int64_t id : search(db, cond).ids) allowed.insert(id);
    }
    auto accept = [&](int64_t id) { return id != exclude && (!filter || allowed.count(id)); };
    for (int64_t id : index.ids())
        if (accept(id)) ++r.total;
    for (const auto& [id, score] : index.topK(qv, k, accept)) {
        r.ids.push_back(id);
        r.scores.push_back(score);
    }
    return r;
}

namespace {
struct ClipPayload {
    std::vector<float> v;
};
}  // namespace

std::shared_ptr<void> ClipAnalyzer::prepare(const AnalyzerInput& in) {
    const int side = model_->imageSide();
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    // 短い辺が side 以上になるよう、少し大きめに読む
    if (!decode_(in.path, side * 2, px, w, h) || w <= 0 || h <= 0) return nullptr;
    auto input = std::make_shared<std::vector<float>>(3 * static_cast<size_t>(side) * side);
    prepareClipInput(px.data(), w, h, side, input->data());
    return input;
}

void ClipAnalyzer::inferBatch(const std::vector<std::shared_ptr<void>>& prepared, std::vector<AnalyzerOutput>& outs) {
    const size_t n = prepared.size();
    if (!n) return;
    const size_t one = 3 * static_cast<size_t>(model_->imageSide()) * model_->imageSide(), dim = model_->dim();
    std::vector<float> input(one * n);
    for (size_t k = 0; k < n; ++k) {
        const auto& v = *std::static_pointer_cast<std::vector<float>>(prepared[k]);
        std::copy(v.begin(), v.end(), input.begin() + static_cast<std::ptrdiff_t>(one * k));
    }
    std::vector<float> all;
    std::string error;
    if (!model_->encodeImages(input, n, all, error) || all.size() != dim * n) {
        for (auto& o : outs) o.ok = false;
        return;
    }
    for (size_t k = 0; k < n; ++k) {
        auto p = std::make_shared<ClipPayload>();
        p->v.assign(all.begin() + static_cast<std::ptrdiff_t>(dim * k), all.begin() + static_cast<std::ptrdiff_t>(dim * (k + 1)));
        normalizeVector(p->v);
        outs[k].payload = p;
    }
}

AnalyzerOutput ClipAnalyzer::analyze(const AnalyzerInput& in) {
    AnalyzerOutput out;
    const auto prepared = prepare(in);
    if (!prepared) {
        out.ok = false;
        return out;
    }
    const std::vector<float>& input = *std::static_pointer_cast<std::vector<float>>(prepared);
    auto p = std::make_shared<ClipPayload>();
    std::string error;
    if (!model_->encodeImage(input, p->v, error) || p->v.size() != model_->dim()) {
        out.ok = false;
        return out;
    }
    normalizeVector(p->v);
    out.payload = p;
    return out;
}

bool ClipAnalyzer::store(Db& /*db*/, int64_t imageId, AnalyzerOutput& out) {
    auto p = std::static_pointer_cast<ClipPayload>(out.payload);
    if (!p || !writer_->append(imageId, p->v.data())) return false;
    writer_->flush();
    return true;
}

}  // namespace imgdupe
