#include "pathinfer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <unordered_map>

#include "names.hpp"

namespace imgdupe {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!std::isdigit(c)) return false;
    return true;
}

// アカウント名らしい（英数字と _ . - だけ。英字を含む）
bool handleLike(const std::string& s) {
    if (s.empty() || s.size() > 40) return false;
    bool alpha = false;
    for (unsigned char c : s) {
        if (std::isalpha(c)) alpha = true;
        else if (!(std::isdigit(c) || c == '_' || c == '.' || c == '-')) return false;
    }
    return alpha;
}

// 全角のかっこ・記号・数字・空白を半角に
std::string normalize(const std::string& in) {
    static const std::pair<const char*, const char*> table[] = {
        {"【", "["}, {"】", "]"}, {"［", "["}, {"］", "]"}, {"（", "("}, {"）", ")"}, {"＠", "@"}, {"＿", "_"},
        {"＃", "#"}, {"　", " "}, {"：", ":"}, {"＝", "="}, {"－", "-"}, {"０", "0"}, {"１", "1"}, {"２", "2"},
        {"３", "3"}, {"４", "4"}, {"５", "5"}, {"６", "6"}, {"７", "7"}, {"８", "8"}, {"９", "9"}};
    std::string s = in;
    for (const auto& [from, to] : table) {
        const std::string f = from;
        for (size_t p = 0; (p = s.find(f, p)) != std::string::npos;) {
            s.replace(p, f.size(), to);
            p += std::string(to).size();
        }
    }
    return s;
}

// 名前の中のサイト名（"pixiv 12345" の pixiv）。x・user は普通の言葉にも出るので、ここでは数えない
const char* labelSite(const std::string& word) {
    static const std::pair<const char*, const char*> table[] = {
        {"pixiv", "pixiv"}, {"pxv", "pixiv"}, {"pid", "pixiv"}, {"twitter", "x"}, {"fanbox", "fanbox"}, {"fantia", "fantia"},
        {"skeb", "skeb"}, {"nijie", "nijie"}, {"misskey", "misskey"}, {"bluesky", "bluesky"}, {"bsky", "bluesky"},
        {"booth", "booth"}, {"patreon", "patreon"}, {"ci-en", "ci-en"}, {"cien", "ci-en"}, {"id", "id"}, {"uid", "id"}};
    const std::string l = lower(word);
    for (const auto& [k, v] : table)
        if (l == k) return v;
    return nullptr;
}

// フォルダの名前そのものがサイト名（gallery-dl の一番上のフォルダなど）
std::string folderSite(const std::string& name) {
    static const std::pair<const char*, const char*> table[] = {
        {"pixiv", "pixiv"}, {"twitter", "x"}, {"x", "x"}, {"x.com", "x"}, {"twitter.com", "x"}, {"fanbox", "fanbox"},
        {"pixivfanbox", "fanbox"}, {"pixiv fanbox", "fanbox"}, {"fantia", "fantia"}, {"skeb", "skeb"}, {"nijie", "nijie"},
        {"misskey", "misskey"}, {"bluesky", "bluesky"}, {"bsky", "bluesky"}, {"booth", "booth"}, {"patreon", "patreon"},
        {"ci-en", "ci-en"}, {"cien", "ci-en"}, {"danbooru", "danbooru"}, {"gelbooru", "danbooru"}, {"yandere", "danbooru"},
        {"yande.re", "danbooru"}, {"konachan", "danbooru"}};
    const std::string l = lower(name);
    for (const auto& [k, v] : table)
        if (l == k) return v;
    return {};
}

struct Token {
    char type;          // 'D' 数字、'S' 文字、'@' @ のついたもの、'K' サイト名、'P' 記号
    std::string text;   // 値（K は小文字のサイト名、P は記号）
};

void classifyWord(const std::string& w, std::vector<Token>& out) {
    if (w.empty()) return;
    if (allDigits(w)) {
        out.push_back({'D', w});
        return;
    }
    // 記号だけ（" - " の - など）
    bool punct = true;
    for (unsigned char c : w)
        if (!(c < 0x80 && std::ispunct(c))) punct = false;
    if (punct) {
        out.push_back({'P', w});
        return;
    }
    if (labelSite(w)) {
        out.push_back({'K', lower(w)});
        return;
    }
    const bool ascii = std::all_of(w.begin(), w.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if (ascii) {
        // pixiv_12345・pixiv12345・pixiv=12345
        size_t k = 0;
        while (k < w.size() && (std::isalpha(static_cast<unsigned char>(w[k])) || w[k] == '-')) ++k;
        if (k > 0 && k < w.size() && labelSite(w.substr(0, k))) {
            size_t d = k;
            while (d < w.size() && (w[d] == '_' || w[d] == ':' || w[d] == '=' || w[d] == '.' || w[d] == '-' || w[d] == '#')) ++d;
            if (d < w.size() && allDigits(w.substr(d))) {
                out.push_back({'K', lower(w.substr(0, k))});
                out.push_back({'D', w.substr(d)});
                return;
            }
        }
    }
    // 12345_name（数字が 3 桁以上の時だけ分ける）
    {
        size_t d = 0;
        while (d < w.size() && std::isdigit(static_cast<unsigned char>(w[d]))) ++d;
        if (d >= 3 && d + 1 < w.size() && (w[d] == '_' || w[d] == '-' || w[d] == '.')) {
            out.push_back({'D', w.substr(0, d)});
            classifyWord(w.substr(d + 1), out);
            return;
        }
    }
    // name_12345（数字が 4 桁以上の時だけ）
    {
        size_t d = w.size();
        while (d > 0 && std::isdigit(static_cast<unsigned char>(w[d - 1]))) --d;
        if (w.size() - d >= 4 && d >= 2 && (w[d - 1] == '_' || w[d - 1] == '-' || w[d - 1] == '.')) {
            classifyWord(w.substr(0, d - 1), out);
            out.push_back({'D', w.substr(d)});
            return;
        }
    }
    out.push_back({'S', w});
}

std::vector<Token> tokenize(const std::string& raw) {
    const std::string s = normalize(raw);
    std::vector<Token> out;
    std::string word;
    bool at = false;  // 直前が @
    auto flush = [&] {
        if (word.empty()) return;
        if (at) {
            out.push_back({'@', word});
        } else {
            classifyWord(word, out);
        }
        word.clear();
        at = false;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == ' ' || c == '\t') {
            flush();
            at = false;
            out.push_back({' ', " "});
        } else if (c == '[' || c == ']' || c == '(' || c == ')' || c == '#') {
            flush();
            at = false;
            out.push_back({'P', std::string(1, c)});
        } else if (c == '@') {
            flush();
            at = true;
        } else {
            word += c;
        }
    }
    flush();
    // 空白でつながった文字は 1 つにまとめる（"山田 太郎"、"夏の 本"）。ほかの空白は捨てる
    std::vector<Token> merged;
    for (size_t i = 0; i < out.size(); ++i) {
        const Token& t = out[i];
        if (t.type == ' ') {
            continue;
        }
        if (t.type == 'S' && !merged.empty() && merged.back().type == 'S' && i > 0 && out[i - 1].type == ' ') {
            merged.back().text += " " + t.text;
            continue;
        }
        merged.push_back(t);
    }
    return merged;
}

double clampConf(double c) { return std::min(1.0, std::max(0.0, c)); }

void addTag(std::vector<InferredTag>& out, const std::string& tag, const std::string& kind, double confidence) {
    if (tag.empty() || tag.size() > 200) return;
    for (auto& o : out)
        if (o.kind == kind && lower(o.tag) == lower(tag)) {
            o.confidence = std::max(o.confidence, confidence);
            return;
        }
    out.push_back({tag, kind, clampConf(confidence)});
}

bool hasKind(const std::vector<InferredTag>& v, const std::string& kind) {
    for (const auto& t : v)
        if (t.kind == kind) return true;
    return false;
}

std::string siteOfLabel(const std::string& label, const std::string& ctx) {
    if (label.empty()) return {};
    const char* s = labelSite(label);
    if (!s) return {};
    if (std::string(s) == "id") return ctx.empty() || ctx == "danbooru" ? (label == "pid" ? "pixiv" : "") : ctx;
    return s;
}

// アカウントにする値の形を確かめる（数字の ID のサイトは数字だけ、名前のサイトは名前らしいもの）
bool accountValueOk(const std::string& site, const std::string& v) {
    if (site == "pixiv" || site == "fantia" || site == "nijie") return allDigits(v) && v.size() <= 12;
    if (site == "x") return handleLike(v);
    if (site == "danbooru" || site.empty()) return false;
    return handleLike(v) || (allDigits(v) && v.size() <= 12);
}

// 名前のディレクトリ部分
std::string parentOf(const std::string& path) {
    const size_t k = path.find_last_of("\\/");
    return k == std::string::npos ? std::string() : path.substr(0, k);
}
std::string leafOf(const std::string& path) {
    const size_t k = path.find_last_of("\\/");
    return k == std::string::npos ? path : path.substr(k + 1);
}

}  // namespace

const std::vector<std::string>& inferRoles() {
    static const std::vector<std::string> roles = {"",      "ignore",  "artist",  "circle",  "title",   "event",   "pixiv",
                                                   "x",     "fanbox",  "fantia",  "skeb",    "nijie",   "misskey", "bluesky",
                                                   "booth", "patreon", "ci-en"};
    return roles;
}

bool isAccountRole(const std::string& role) {
    return !role.empty() && role != "ignore" && role != "artist" && role != "circle" && role != "title" && role != "event";
}

std::string accountTag(const std::string& site, const std::string& value) {
    std::string v = value;
    while (!v.empty() && v[0] == '@') v.erase(0, 1);
    if (v.empty()) return {};
    if (site == "x" || site == "misskey" || site == "skeb") return site + ":@" + v;
    return site + ":" + v;
}

std::string accountUrl(const std::string& tag) {
    const size_t c = tag.find(':');
    if (c == std::string::npos) return {};
    const std::string site = tag.substr(0, c);
    std::string v = tag.substr(c + 1);
    while (!v.empty() && v[0] == '@') v.erase(0, 1);
    if (v.empty()) return {};
    if (site == "pixiv") return "https://www.pixiv.net/users/" + v;
    if (site == "x") return "https://x.com/" + v;
    if (site == "fanbox") return "https://" + v + ".fanbox.cc/";
    if (site == "fantia") return "https://fantia.jp/fanclubs/" + v;
    if (site == "skeb") return "https://skeb.jp/@" + v;
    if (site == "nijie") return "https://nijie.info/members.php?id=" + v;
    if (site == "misskey") return "https://misskey.io/@" + v;
    if (site == "bluesky") return "https://bsky.app/profile/" + v;
    if (site == "booth") return "https://" + v + ".booth.pm/";
    if (site == "patreon") return "https://www.patreon.com/" + v;
    if (site == "ci-en") return "https://ci-en.dlsite.com/creator/" + v;
    return {};
}

NameShape nameShape(const std::string& folderName) {
    NameShape out;
    const std::vector<Token> tokens = tokenize(folderName);
    std::string lastLabel;
    for (const Token& t : tokens) {
        if (!out.key.empty()) out.key += ' ';
        if (t.type == 'D' || t.type == 'S' || t.type == '@') {
            out.key += t.type;
            out.slots.push_back({t.text, t.type, lastLabel});
            lastLabel.clear();
        } else if (t.type == 'K') {
            out.key += t.text;
            lastLabel = t.text;
        } else {
            out.key += t.text;
            // ":"・"_"・"=" などはサイト名と値の間に入ってもよい
            if (t.text != ":" && t.text != "_" && t.text != "=" && t.text != "-" && t.text != "#" && t.text != "(" && t.text != "[")
                lastLabel.clear();
        }
    }
    return out;
}

std::vector<InferredTag> applyRoles(const NameShape& shape, const std::vector<std::string>& roles, double confidence) {
    std::vector<InferredTag> out;
    for (size_t i = 0; i < shape.slots.size() && i < roles.size(); ++i) {
        const std::string& r = roles[i];
        const std::string& v = shape.slots[i].value;
        if (r.empty() || r == "ignore") continue;
        if (r == "artist" || r == "circle" || r == "title" || r == "event") addTag(out, v, r, confidence);
        else addTag(out, accountTag(r, v), "account", confidence);
    }
    return out;
}

std::vector<InferredTag> inferFromName(const std::string& folderName, const std::string& site) {
    std::vector<InferredTag> out;
    if (folderName.empty() || folderName[0] == '.') return out;
    DoujinName d;
    if (parseDoujinName(folderName, d)) return out;  // 同人誌の形は段階 names で
    const NameShape shape = nameShape(folderName);
    std::vector<bool> used(shape.slots.size(), false);
    bool account = false;
    for (size_t i = 0; i < shape.slots.size(); ++i) {
        const NameSlot& s = shape.slots[i];
        std::string st = siteOfLabel(s.label, site);
        if (s.cls == '@') {
            if (st.empty()) st = site == "misskey" || site == "bluesky" || site == "fanbox" || site == "skeb" ? site : "x";
            if (!handleLike(s.value) && !allDigits(s.value)) continue;
            addTag(out, accountTag(st, s.value), "account", s.label.empty() ? 0.85 : kInferLabeledConfidence);
            used[i] = account = true;
        } else if (!st.empty() && accountValueOk(st, s.value)) {
            addTag(out, accountTag(st, s.value), "account", kInferLabeledConfidence);
            used[i] = account = true;
        }
    }
    // サイト名のフォルダの下（gallery-dl の既定の形: pixiv は "ID アカウント"、twitter は "アカウント" など）
    if (!account && !site.empty() && site != "danbooru") {
        const auto& sl = shape.slots;
        if (sl.size() == 1 && accountValueOk(site, sl[0].value)) {
            const bool numericSite = site == "pixiv" || site == "fantia" || site == "nijie";
            if (numericSite && sl[0].cls == 'D') {
                addTag(out, accountTag(site, sl[0].value), "account", 0.85);
                used[0] = account = true;
            } else if (!numericSite && sl[0].cls == 'S') {
                addTag(out, accountTag(site, sl[0].value), "account", 0.85);
                used[0] = account = true;
            }
        } else if (sl.size() == 2 && sl[0].cls == 'D' && sl[1].cls == 'S' && accountValueOk(site, sl[0].value)) {
            addTag(out, accountTag(site, sl[0].value), "account", 0.85);
            used[0] = account = true;
        } else if (sl.size() == 1 && sl[0].cls == 'S' && site == "pixiv") {
            addTag(out, sl[0].value, "artist", 0.7);
            used[0] = true;
        }
    }
    // アカウントが分かった時: 残りの文字が 1 つだけなら、それが作者の名前
    if (account) {
        int rest = -1, count = 0;
        for (size_t i = 0; i < shape.slots.size(); ++i)
            if (!used[i] && shape.slots[i].cls == 'S') {
                rest = static_cast<int>(i);
                ++count;
            }
        if (count == 1) addTag(out, shape.slots[static_cast<size_t>(rest)].value, "artist", kInferGroupConfidence);
    }
    return out;
}

// ---------------------------------------------------------------- フォルダの木で推し量る

namespace {

struct Node {
    std::string path, name, parent;
    std::vector<std::string> children;
    std::string site;  // サイト名のフォルダの下ならそのサイト
    NameShape shape;
    bool doujin = false;
    bool ruled = false;
    std::vector<InferredTag> own;
    std::map<std::string, int> sites;  // 下の画像の出どころの数（自分の下全部）
    int images = 0;                    // 下の画像の数（自分の下全部）
};

bool samePath(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

bool under(const std::string& path, const std::string& dir) {
    if (path.size() < dir.size()) return false;
    if (lower(path.substr(0, dir.size())) != lower(dir)) return false;
    return path.size() == dir.size() || path[dir.size()] == '\\' || path[dir.size()] == '/';
}

}  // namespace

std::map<std::string, std::vector<InferredTag>> inferFolders(const InferInput& in) {
    // 木を作る（画像のあるフォルダとその上のフォルダ全部）
    std::unordered_map<std::string, Node> nodes;
    for (const auto& [dir, ev] : in.dirs) {
        std::string p = dir;
        std::string child;
        while (!p.empty()) {
            auto it = nodes.find(p);
            const bool fresh = it == nodes.end();
            if (fresh) {
                Node n;
                n.path = p;
                n.name = leafOf(p);
                n.parent = parentOf(p);
                it = nodes.emplace(p, std::move(n)).first;
            }
            if (!child.empty()) {
                auto& ch = it->second.children;
                if (std::find(ch.begin(), ch.end(), child) == ch.end()) ch.push_back(child);
            }
            if (!fresh) break;
            child = p;
            p = it->second.parent;
        }
    }
    // 下の画像の数と出どころ（下から上へ足す）
    for (const auto& [dir, ev] : in.dirs) {
        std::string p = dir;
        while (!p.empty()) {
            auto it = nodes.find(p);
            if (it == nodes.end()) break;
            it->second.images += ev.images;
            for (const auto& [s, c] : ev.sites) it->second.sites[s] += c;
            p = it->second.parent;
        }
    }
    // アカウント・作者の名前 → それがあるフォルダ
    std::unordered_map<std::string, std::vector<std::string>> accountDirs, artistDirs;
    for (const auto& [dir, ev] : in.dirs) {
        for (const auto& a : ev.accounts) accountDirs[lower(a)].push_back(dir);
        for (const auto& a : ev.artists) artistDirs[a].push_back(dir);
    }
    auto hasBelow = [](const std::unordered_map<std::string, std::vector<std::string>>& index, const std::string& key,
                       const std::string& dir) {
        auto it = index.find(key);
        if (it == index.end()) return false;
        for (const auto& d : it->second)
            if (under(d, dir)) return true;
        return false;
    };

    // サイト名（上から受け継ぐ）・形・名前だけで分かるもの
    std::vector<Node*> order;  // 浅い順
    for (auto& [p, n] : nodes) order.push_back(&n);
    std::sort(order.begin(), order.end(), [](const Node* a, const Node* b) {
        const auto depth = [](const std::string& s) { return std::count_if(s.begin(), s.end(), [](char c) { return c == '\\' || c == '/'; }); };
        const auto da = depth(a->path), db = depth(b->path);
        return da != db ? da < db : a->path < b->path;
    });
    for (Node* n : order) {
        auto pit = nodes.find(n->parent);
        const std::string inherited = pit == nodes.end() ? std::string() : pit->second.site;
        const std::string own = folderSite(n->name);
        n->site = own.empty() ? inherited : own;
        if (!own.empty() || n->name.empty() || (n->name.size() == 2 && n->name[1] == ':')) continue;  // サイト名・ドライブそのもの
        DoujinName d;
        n->doujin = parseDoujinName(n->name, d);
        if (n->doujin) continue;
        n->shape = nameShape(n->name);
        // 教えた形（その親の下のものを優先）
        const InferRule* rule = nullptr;
        for (const auto& r : in.rules)
            if (r.shape == n->shape.key && r.roles.size() == n->shape.slots.size() && !r.scope.empty() && samePath(r.scope, n->parent))
                rule = &r;
        if (!rule)
            for (const auto& r : in.rules)
                if (r.shape == n->shape.key && r.roles.size() == n->shape.slots.size() && r.scope.empty()) rule = &r;
        if (rule) {
            n->own = applyRoles(n->shape, rule->roles, kInferLabeledConfidence);
            n->ruled = true;
            continue;
        }
        n->own = inferFromName(n->name, inherited);
    }

    // 兄弟で同じ形のもの（3 つ以上）: 部分ごとの役目を、中の画像から分かっていることと照らして決める
    for (auto& [p, parent] : nodes) {
        std::map<std::string, std::vector<Node*>> groups;
        for (const auto& c : parent.children) {
            Node& n = nodes[c];
            if (n.doujin || n.ruled || n.shape.slots.empty() || !folderSite(n.name).empty()) continue;
            groups[n.shape.key].push_back(&n);
        }
        for (auto& [key, members] : groups) {
            const size_t count = members.size();
            if (count < 3) continue;
            const size_t slots = members[0]->shape.slots.size();
            const std::string& ctx = parent.site;
            std::vector<std::string> roles(slots);
            std::vector<std::vector<bool>> matched(slots, std::vector<bool>(count, false));
            std::vector<double> conf(slots, kInferGroupConfidence);
            for (size_t j = 0; j < slots; ++j) {
                const NameSlot& s0 = members[0]->shape.slots[j];
                const std::string labeled = siteOfLabel(s0.label, ctx);
                if (s0.cls == '@') {
                    roles[j] = !labeled.empty() ? labeled
                               : ctx == "misskey" || ctx == "bluesky" || ctx == "fanbox" || ctx == "skeb" ? ctx
                                                                                                         : "x";
                    conf[j] = 0.85;
                    continue;
                }
                if (!labeled.empty()) {
                    roles[j] = labeled;
                    conf[j] = kInferLabeledConfidence;
                    continue;
                }
                if (s0.cls == 'D') {
                    // 中の画像の作者の ID と合うか
                    std::map<std::string, int> hits;
                    for (size_t m = 0; m < count; ++m)
                        for (const char* site : {"pixiv", "fantia", "nijie", "fanbox", "skeb"}) {
                            const std::string key2 = lower(accountTag(site, members[m]->shape.slots[j].value));
                            if (hasBelow(accountDirs, key2, members[m]->path)) {
                                ++hits[site];
                                matched[j][m] = true;
                            }
                        }
                    std::string best;
                    int bestHits = 0;
                    for (const auto& [site, h] : hits)
                        if (h > bestHits) best = site, bestHits = h;
                    if (!best.empty()) {
                        roles[j] = best;
                        continue;
                    }
                    if (ctx == "pixiv" || ctx == "fantia" || ctx == "nijie") {
                        roles[j] = ctx;
                        conf[j] = 0.8;
                        continue;
                    }
                    // pixiv の画像が多いフォルダの並び（"12345678_p0.jpg" など）
                    int pixiv = 0, total = 0;
                    bool shortIds = true;
                    for (Node* m : members) {
                        auto it = m->sites.find("pixiv");
                        pixiv += it == m->sites.end() ? 0 : it->second;
                        total += m->images;
                        shortIds = shortIds && m->shape.slots[j].value.size() <= 10;
                    }
                    if (total > 0 && pixiv * 2 >= total && shortIds) {
                        roles[j] = "pixiv";
                        conf[j] = 0.6;
                    }
                    continue;
                }
                // 文字: 知っている作者の名前・サークルの名前と合うか
                int artistHits = 0, circleHits = 0;
                for (size_t m = 0; m < count; ++m) {
                    const std::string v = lower(members[m]->shape.slots[j].value);
                    const bool below = hasBelow(artistDirs, v, members[m]->path);
                    if (below || in.knownArtists.count(v)) {
                        ++artistHits;
                        matched[j][m] = true;
                    } else if (in.knownCircles.count(v)) {
                        ++circleHits;
                    }
                }
                const auto enough = [&](int hits) {
                    return hits >= 2 || (hits >= 1 && count <= 5) || static_cast<size_t>(hits) * 5 >= count;
                };
                if (artistHits && enough(artistHits)) roles[j] = "artist";
                else if (circleHits && enough(circleHits)) roles[j] = "circle";
            }
            // アカウントの部分があれば、残りの文字が 1 つだけの時はそれが作者の名前
            bool accountSlot = false;
            for (const auto& r : roles) accountSlot = accountSlot || isAccountRole(r);
            if (accountSlot) {
                int rest = -1, n = 0;
                for (size_t j = 0; j < slots; ++j)
                    if (roles[j].empty() && members[0]->shape.slots[j].cls == 'S') rest = static_cast<int>(j), ++n;
                if (n == 1) roles[static_cast<size_t>(rest)] = "artist";
            } else if (slots == 1 && roles[0].empty() && members[0]->shape.slots[0].cls == 'S') {
                // サイト名のフォルダの下の名前だけのフォルダ
                bool handles = true;
                for (Node* m : members) handles = handles && handleLike(m->shape.slots[0].value);
                if (handles && (ctx == "x" || ctx == "fanbox" || ctx == "skeb" || ctx == "misskey" || ctx == "bluesky")) roles[0] = ctx;
                else if (ctx == "pixiv") roles[0] = "artist";
            }
            for (size_t m = 0; m < count; ++m) {
                Node* node = members[m];
                for (size_t j = 0; j < slots; ++j) {
                    const std::string& r = roles[j];
                    if (r.empty()) continue;
                    const std::string& v = node->shape.slots[j].value;
                    const double c = matched[j][m] ? kInferEvidenceConfidence : conf[j];
                    if (r == "artist" || r == "circle") {
                        if (!hasKind(node->own, r)) addTag(node->own, v, r, c);
                    } else if (isAccountRole(r) && accountValueOk(r, v)) {
                        addTag(node->own, accountTag(r, v), "account", c);
                    }
                }
            }
        }
    }

    // 1 つだけのフォルダでも、名前が中の画像の作者と同じなら作者のフォルダ
    for (auto& [p, n] : nodes) {
        if (n.doujin || n.ruled || hasKind(n.own, "artist")) continue;
        for (const auto& s : n.shape.slots) {
            if (s.cls != 'S') continue;
            const std::string v = lower(s.value);
            if (hasBelow(artistDirs, v, n.path)) {
                addTag(n.own, s.value, "artist", kInferEvidenceConfidence);
                break;
            }
        }
    }

    // 作者の名前とアカウントの対応で、片方しか分からないフォルダにもう片方を足す
    std::map<std::string, std::map<std::string, int>> accountNames;  // アカウント（小文字）→ 名前 → 数
    std::map<std::string, std::set<std::string>> nameAccounts;      // 名前（小文字）→ アカウント
    std::map<std::string, std::string> accountSpelling;             // 小文字 → 元の書き方
    auto pair = [&](const std::string& account, const std::string& name) {
        if (account.empty() || name.empty()) return;
        accountNames[lower(account)][name]++;
        nameAccounts[lower(name)].insert(lower(account));
        accountSpelling.emplace(lower(account), account);
    };
    for (const auto& [a, n] : in.pairs) pair(a, n);
    for (auto& [p, n] : nodes) {
        std::vector<const InferredTag*> accounts, artists;
        for (const auto& t : n.own) {
            if (t.kind == "account") accounts.push_back(&t);
            else if (t.kind == "artist") artists.push_back(&t);
        }
        if (accounts.size() == 1)
            for (const auto* a : artists) pair(accounts[0]->tag, a->tag);
    }
    for (auto& [p, n] : nodes) {
        if (n.own.empty()) continue;
        const bool hasArtist = hasKind(n.own, "artist"), hasAccount = hasKind(n.own, "account");
        if (hasAccount && !hasArtist) {
            for (const auto& t : std::vector<InferredTag>(n.own)) {
                if (t.kind != "account") continue;
                auto it = accountNames.find(lower(t.tag));
                if (it == accountNames.end()) continue;
                std::string best;
                int most = 0;
                for (const auto& [name, c] : it->second)
                    if (c > most) best = name, most = c;
                addTag(n.own, best, "artist", kInferDictConfidence);
                break;
            }
        } else if (hasArtist && !hasAccount) {
            for (const auto& t : std::vector<InferredTag>(n.own)) {
                if (t.kind != "artist") continue;
                auto it = nameAccounts.find(lower(t.tag));
                if (it == nameAccounts.end() || it->second.size() != 1) continue;
                addTag(n.own, accountSpelling[*it->second.begin()], "account", kInferDictConfidence);
            }
        }
    }

    // 画像のあるフォルダごと: 自分と上のフォルダの分を合わせる（種類ごとに一番近いフォルダのものだけ）
    std::map<std::string, std::vector<InferredTag>> out;
    for (const auto& [dir, ev] : in.dirs) {
        std::vector<InferredTag> tags;
        std::set<std::string> taken;  // もう決まった種類
        std::string p = dir;
        while (!p.empty()) {
            auto it = nodes.find(p);
            if (it == nodes.end()) break;
            std::set<std::string> here;
            for (const auto& t : it->second.own) {
                if (taken.count(t.kind)) continue;
                addTag(tags, t.tag, t.kind, t.confidence);
                here.insert(t.kind);
            }
            taken.insert(here.begin(), here.end());
            p = it->second.parent;
        }
        std::sort(tags.begin(), tags.end());
        if (!tags.empty()) out[dir] = std::move(tags);
    }
    return out;
}

// ---------------------------------------------------------------- 教えた形

std::vector<InferRule> loadInferRules(Db& db) {
    std::vector<InferRule> out;
    Stmt st(db.handle(), "SELECT id, scope, shape, roles, coalesce(example,'') FROM infer_rules ORDER BY id");
    while (st.step()) {
        InferRule r;
        r.id = st.i64(0);
        r.scope = st.text(1);
        r.shape = st.text(2);
        const std::string roles = st.text(3);
        size_t a = 0;
        while (true) {
            const size_t b = roles.find('\t', a);
            r.roles.push_back(roles.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        r.example = st.text(4);
        out.push_back(std::move(r));
    }
    return out;
}

int64_t saveInferRule(Db& db, const InferRule& rule) {
    std::string roles;
    for (size_t i = 0; i < rule.roles.size(); ++i) roles += (i ? "\t" : "") + rule.roles[i];
    Stmt st(db.handle(),
            "INSERT INTO infer_rules(scope,shape,roles,example,created) VALUES(?,?,?,?,?) "
            "ON CONFLICT(scope,shape) DO UPDATE SET roles=excluded.roles, example=excluded.example, created=excluded.created");
    st.bind(1, rule.scope).bind(2, rule.shape).bind(3, roles).bind(4, rule.example).bind(5, nowUnix());
    st.step();
    Stmt id(db.handle(), "SELECT id FROM infer_rules WHERE scope=? AND shape=?");
    id.bind(1, rule.scope).bind(2, rule.shape);
    return id.step() ? id.i64(0) : 0;
}

void deleteInferRule(Db& db, int64_t id) {
    Stmt st(db.handle(), "DELETE FROM infer_rules WHERE id=?");
    st.bind(1, id);
    st.step();
}

// ---------------------------------------------------------------- DB で

namespace {

constexpr int kInferVersion = 1;

std::string serialize(const std::vector<InferredTag>& tags) {
    std::string s;
    char buf[32];
    for (const auto& t : tags) {
        std::snprintf(buf, sizeof buf, "%.3f", t.confidence);
        s += t.kind + "\t" + t.tag + "\t" + buf + "\n";
    }
    return s;
}

std::vector<TagRow> deserialize(const std::string& s) {
    std::vector<TagRow> out;
    size_t a = 0;
    while (a < s.size()) {
        size_t e = s.find('\n', a);
        if (e == std::string::npos) e = s.size();
        const std::string line = s.substr(a, e - a);
        const size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
        if (t2 != std::string::npos)
            out.push_back({line.substr(t1 + 1, t2 - t1 - 1), line.substr(0, t1), kSourceInfer, std::atof(line.c_str() + t2 + 1), false});
        a = e + 1;
    }
    return out;
}

std::string siteName(const std::string& s) { return s == "twitter" ? "x" : s; }

int64_t scalarQuery(Db& db, const char* sql) {
    Stmt st(db.handle(), sql);
    return st.step() ? st.i64(0) : 0;
}

}  // namespace

std::vector<TagRow> storedInferTags(Db& db, const std::string& imagePath) {
    const std::string dir = parentOf(imagePath);
    if (dir.empty()) return {};
    Stmt st(db.handle(), "SELECT tags FROM infer_dirs WHERE dir=?");
    st.bind(1, dir);
    return st.step() ? deserialize(st.text(0)) : std::vector<TagRow>{};
}

InferSummary runFolderInference(Db& db, bool force, const std::atomic<bool>* cancel) {
    InferSummary sum;
    // 変わったかどうか: 画像の数・一番新しい ID・作者の情報の数・教えた形
    std::string sig = std::to_string(kInferVersion) + "/" + std::to_string(scalarQuery(db, "SELECT count(*) FROM images")) + "/" +
                      std::to_string(scalarQuery(db, "SELECT coalesce(max(id),0) FROM images")) + "/" +
                      std::to_string(scalarQuery(db, "SELECT count(*) FROM tags WHERE kind IN ('artist','circle') AND source<>'infer'")) +
                      "/" + std::to_string(scalarQuery(db, "SELECT count(*) FROM sources WHERE author_id IS NOT NULL AND author_id<>''"));
    const std::vector<InferRule> rules = loadInferRules(db);
    for (const auto& r : rules) {
        sig += "/" + r.scope + "|" + r.shape + "|";
        for (const auto& x : r.roles) sig += x + ",";
    }
    if (!force && db.setting("infer.signature") == sig) {
        sum.skipped = true;
        return sum;
    }

    InferInput in;
    in.rules = rules;
    {
        Stmt st(db.handle(), "SELECT path FROM images");
        while (st.step()) {
            const std::string dir = parentOf(st.text(0));
            if (!dir.empty()) in.dirs[dir].images++;
        }
    }
    {
        Stmt st(db.handle(), "SELECT i.path, s.site, coalesce(s.author_id,'') FROM sources s JOIN images i ON i.id=s.image_id");
        while (st.step()) {
            auto it = in.dirs.find(parentOf(st.text(0)));
            if (it == in.dirs.end()) continue;
            const std::string site = siteName(st.text(1));
            it->second.sites[site]++;
            const std::string author = st.text(2);
            // X の author_id は数字の ID（アカウント名ではない）なので使わない
            if (!author.empty() && site != "x" && site != "md5" && site != "download" && site != "saucenao" && site != "danbooru")
                it->second.accounts.insert(accountTag(site, author));
        }
    }
    {
        Stmt st(db.handle(),
                "SELECT i.path, t.tag, t.kind FROM tags t JOIN images i ON i.id=t.image_id WHERE t.kind IN ('artist','circle') AND "
                "(t.source IN ('meta','id_lookup','reverse','manual') OR (t.source='folder' AND t.confidence>=0.9))");
        while (st.step()) {
            const std::string tag = st.text(1), kind = st.text(2), l = lower(tag);
            if (kind == "circle") {
                in.knownCircles.insert(l);
                continue;
            }
            in.knownArtists.insert(l);
            auto it = in.dirs.find(parentOf(st.text(0)));
            if (it == in.dirs.end()) continue;
            it->second.artists.insert(l);
            it->second.artistNames.emplace(l, tag);
        }
    }
    {
        Stmt st(db.handle(),
                "SELECT DISTINCT t.tag, s.site, s.author_id FROM tags t JOIN sources s ON s.image_id=t.image_id "
                "WHERE t.kind='artist' AND t.source IN ('meta','id_lookup','reverse') AND s.author_id IS NOT NULL AND s.author_id<>''");
        while (st.step()) {
            const std::string site = siteName(st.text(1));
            if (site == "x" || site == "danbooru" || site == "md5" || site == "saucenao") continue;
            in.pairs.emplace_back(accountTag(site, st.text(2)), st.text(0));
        }
    }
    const auto result = inferFolders(in);
    sum.dirs = in.dirs.size();

    // 前の結果と比べて、変わったフォルダの画像だけ書き直す
    std::map<std::string, std::string> stored;
    {
        Stmt st(db.handle(), "SELECT dir, tags FROM infer_dirs");
        while (st.step()) stored[st.text(0)] = st.text(1);
    }
    std::vector<std::pair<std::string, std::string>> changes;  // フォルダ → 新しい結果（空 = 消す）
    for (const auto& [dir, tags] : result) {
        const std::string s = serialize(tags);
        auto it = stored.find(dir);
        if (it == stored.end() || it->second != s) changes.emplace_back(dir, s);
    }
    for (const auto& [dir, s] : stored)
        if (!result.count(dir)) changes.emplace_back(dir, std::string());
    sum.changedDirs = changes.size();

    db.begin();
    try {
        size_t sinceCommit = 0;
        Stmt up(db.handle(), "INSERT INTO infer_dirs(dir,tags) VALUES(?,?) ON CONFLICT(dir) DO UPDATE SET tags=excluded.tags");
        Stmt del(db.handle(), "DELETE FROM infer_dirs WHERE dir=?");
        for (const auto& [dir, s] : changes) {
            if (cancel && *cancel) {
                sum.cancelled = true;
                break;
            }
            if (s.empty()) {
                del.bind(1, dir);
                del.step();
                del.reset();
            } else {
                up.bind(1, dir).bind(2, s);
                up.step();
                up.reset();
            }
            const std::vector<TagRow> rows = deserialize(s);
            const std::string prefix = dir + (dir.find('\\') != std::string::npos ? "\\" : "/");
            std::vector<int64_t> ids;
            {
                Stmt st(db.handle(), "SELECT id, path FROM images WHERE path > ? AND path < ?");
                st.bind(1, prefix).bind(2, prefix + "\xff");
                while (st.step()) {
                    const std::string rest = st.text(1).substr(prefix.size());
                    if (rest.find_first_of("\\/") == std::string::npos) ids.push_back(st.i64(0));
                }
            }
            for (int64_t id : ids) replaceTags(db, id, kSourceInfer, rows);
            sum.images += ids.size();
            if (!rows.empty()) sum.tagged += ids.size();
            sinceCommit += ids.size() + 1;
            if (sinceCommit >= 5000) {
                db.commit();
                db.begin();
                sinceCommit = 0;
            }
        }
        if (!sum.cancelled) db.setSetting("infer.signature", sig);
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
    return sum;
}

}  // namespace imgdupe
