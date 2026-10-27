#include "tagger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace imgdupe {

namespace {

// CSV の 1 行を列に分ける（"..." の中の , と "" に対応）
std::vector<std::string> csvFields(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                cur += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                cur += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            out.push_back(std::move(cur));
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(std::move(cur));
    return out;
}

const char* kindOf(int category) {
    switch (category) {
        case 1: return "artist";
        case 3: return "copyright";
        case 4: return "character";
        case 9: return "rating";
        case 0: return "general";
        default: return nullptr;  // メタタグなど
    }
}

}  // namespace

bool parseSelectedTags(const std::string& csv, std::vector<TaggerLabel>& out) {
    out.clear();
    size_t a = 0;
    int nameCol = -1, catCol = -1;
    while (a < csv.size()) {
        size_t b = csv.find('\n', a);
        if (b == std::string::npos) b = csv.size();
        const std::string line = csv.substr(a, b - a);
        a = b + 1;
        if (line.empty() || line == "\r") continue;
        const auto f = csvFields(line);
        if (nameCol < 0) {  // 見出し
            for (size_t i = 0; i < f.size(); ++i) {
                if (f[i] == "name") nameCol = static_cast<int>(i);
                if (f[i] == "category") catCol = static_cast<int>(i);
            }
            if (nameCol < 0 || catCol < 0) return false;
            continue;
        }
        if (static_cast<int>(f.size()) <= std::max(nameCol, catCol)) return false;
        out.push_back({f[nameCol], std::atoi(f[catCol].c_str())});
    }
    return !out.empty();
}

void prepareTaggerInput(const uint8_t* bgra, int w, int h, int side, float* out) {
    // 白で正方形に広げる（画像は中央）
    const int m = std::max({w, h, 1});
    const int ox = (m - w) / 2, oy = (m - h) / 2;
    auto sample = [&](int x, int y, int c) -> float {  // 正方形の上の (x, y) の BGR（白を背景に合成）
        x -= ox;
        y -= oy;
        if (x < 0 || y < 0 || x >= w || y >= h) return 255.f;
        const uint8_t* p = bgra + (static_cast<size_t>(y) * w + x) * 4;
        return static_cast<float>(p[c]) + (255.f - p[3]);  // 乗算済みなので、白の分を足すだけ
    };
    if (m == side) {
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
                for (int c = 0; c < 3; ++c) out[(static_cast<size_t>(y) * side + x) * 3 + c] = sample(x, y, c);
        return;
    }
    // m → side に拡大・縮小（双線形。縮小は呼ぶ側がもう済ませているので、ここはほぼ拡大）
    const double scale = static_cast<double>(m) / side;
    for (int y = 0; y < side; ++y) {
        const double sy = std::max(0.0, (y + 0.5) * scale - 0.5);
        const int y0 = std::min(static_cast<int>(sy), m - 1), y1 = std::min(y0 + 1, m - 1);
        const float fy = static_cast<float>(sy - y0);
        for (int x = 0; x < side; ++x) {
            const double sx = std::max(0.0, (x + 0.5) * scale - 0.5);
            const int x0 = std::min(static_cast<int>(sx), m - 1), x1 = std::min(x0 + 1, m - 1);
            const float fx = static_cast<float>(sx - x0);
            for (int c = 0; c < 3; ++c) {
                const float top = sample(x0, y0, c) * (1 - fx) + sample(x1, y0, c) * fx;
                const float bottom = sample(x0, y1, c) * (1 - fx) + sample(x1, y1, c) * fx;
                out[(static_cast<size_t>(y) * side + x) * 3 + c] = top * (1 - fy) + bottom * fy;
            }
        }
    }
}

std::vector<TagRow> taggerTags(const float* probs, size_t n, const std::vector<TaggerLabel>& labels, double minConfidence) {
    std::vector<TagRow> out;
    int bestRating = -1;
    for (size_t i = 0; i < n && i < labels.size(); ++i) {
        const char* kind = kindOf(labels[i].category);
        if (!kind) continue;
        if (labels[i].category == 9) {
            if (bestRating < 0 || probs[i] > probs[bestRating]) bestRating = static_cast<int>(i);
            continue;
        }
        if (probs[i] >= minConfidence) out.push_back({labels[i].name, kind, kSourceTagger, probs[i], false});
    }
    if (bestRating >= 0) out.push_back({labels[bestRating].name, "rating", kSourceTagger, probs[bestRating], false});
    return out;
}

int taggerStageVersion(const std::string& modelVersion) {
    uint32_t h = 2166136261u;  // FNV-1a
    for (unsigned char c : modelVersion + "#" + std::to_string(kTaggerCodeVersion)) {
        h ^= c;
        h *= 16777619u;
    }
    return static_cast<int>(h & 0x7fffffff) | 1;  // 0 にならないように
}

std::shared_ptr<void> TaggerAnalyzer::prepare(const AnalyzerInput& in) {
    const int side = model_->inputSide();
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (!decode_(in.path, side, px, w, h) || w <= 0 || h <= 0 || w > side || h > side) return nullptr;
    auto input = std::make_shared<std::vector<float>>(static_cast<size_t>(side) * side * 3);
    prepareTaggerInput(px.data(), w, h, side, input->data());
    return input;
}

void TaggerAnalyzer::inferBatch(const std::vector<std::shared_ptr<void>>& prepared, std::vector<AnalyzerOutput>& outs) {
    const size_t n = prepared.size();
    if (!n) return;
    const size_t one = static_cast<size_t>(model_->inputSide()) * model_->inputSide() * 3;
    std::vector<float> input(one * n);
    for (size_t k = 0; k < n; ++k) {
        const auto& v = *std::static_pointer_cast<std::vector<float>>(prepared[k]);
        std::copy(v.begin(), v.end(), input.begin() + static_cast<std::ptrdiff_t>(one * k));
    }
    std::vector<float> probs;
    std::string error;
    if (!model_->runBatch(input, n, probs, error) || probs.empty() || probs.size() % n != 0) {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = error;
        for (auto& o : outs) o.ok = false;
        return;
    }
    const size_t per = probs.size() / n;
    for (size_t k = 0; k < n; ++k) outs[k].tags = taggerTags(probs.data() + per * k, per, labels_);
}

AnalyzerOutput TaggerAnalyzer::analyze(const AnalyzerInput& in) {
    AnalyzerOutput out;
    const auto prepared = prepare(in);
    if (!prepared) {
        out.ok = false;
        return out;
    }
    const std::vector<float>& input = *std::static_pointer_cast<std::vector<float>>(prepared);
    std::vector<float> probs;
    std::string error;
    if (!model_->run(input, probs, error)) {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = error;
        out.ok = false;
        return out;
    }
    out.tags = taggerTags(probs.data(), probs.size(), labels_);
    return out;
}

}  // namespace imgdupe
