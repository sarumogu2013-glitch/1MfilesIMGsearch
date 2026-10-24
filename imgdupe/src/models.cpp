#include "models.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <set>

#include "json.hpp"

namespace imgdupe {

uint64_t Component::totalSize() const {
    uint64_t n = 0;
    for (const auto& f : files) n += f.size;
    return n;
}

const Component* Manifest::find(const std::string& id) const {
    for (const auto& c : components)
        if (c.id == id) return &c;
    return nullptr;
}

bool safeRelativePath(const std::string& path) {
    if (path.empty() || path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos) return false;
    size_t a = 0;
    while (a <= path.size()) {
        size_t b = path.find_first_of("/\\", a);
        if (b == std::string::npos) b = path.size();
        const std::string part = path.substr(a, b - a);
        if (part.empty() || part == "." || part == "..") return false;
        a = b + 1;
    }
    return true;
}

namespace {

bool isHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool okUrl(const std::string& url) {
    if (url.compare(0, 8, "https://") == 0) return true;
    // テスト用に手元のサーバーだけ http を許す
    return url.compare(0, 17, "http://127.0.0.1:") == 0 || url.compare(0, 17, "http://localhost:") == 0;
}

std::vector<std::string> strings(const Json* a) {
    std::vector<std::string> out;
    if (a && a->type == Json::Array)
        for (const Json& e : a->arr)
            if (e.type == Json::String) out.push_back(e.str);
    return out;
}

}  // namespace

bool parseManifest(const std::string& json, Manifest& out) {
    Json j;
    if (!parseJson(json, j) || j.type != Json::Object) return false;
    const Json* comps = j.get("components");
    if (!comps || comps->type != Json::Array) return false;
    Manifest m;
    for (const Json& c : comps->arr) {
        if (c.type != Json::Object) return false;
        Component comp;
        comp.id = c.getString("id");
        comp.name = c.getString("name");
        comp.description = c.getString("description");
        comp.version = c.getString("version");
        comp.requires_ = strings(c.get("requires"));
        if (comp.id.empty() || comp.version.empty() || m.find(comp.id)) return false;
        if (comp.name.empty()) comp.name = comp.id;
        const Json* files = c.get("files");
        if (!files || files->type != Json::Array || files->arr.empty()) return false;
        for (const Json& f : files->arr) {
            if (f.type != Json::Object) return false;
            ComponentFile cf;
            cf.path = f.getString("path");
            cf.url = f.getString("url");
            cf.sha256 = f.getString("sha256");
            std::transform(cf.sha256.begin(), cf.sha256.end(), cf.sha256.begin(),
                           [](char ch) { return static_cast<char>(ch >= 'A' && ch <= 'F' ? ch - 'A' + 'a' : ch); });
            const Json* size = f.get("size");
            if (size && size->type == Json::Number) cf.size = std::strtoull(size->str.c_str(), nullptr, 10);
            if (!safeRelativePath(cf.path) || !okUrl(cf.url) || !isHex64(cf.sha256)) return false;
            comp.files.push_back(std::move(cf));
        }
        m.components.push_back(std::move(comp));
    }
    out = std::move(m);
    return true;
}

bool parseInstalled(const std::string& json, Installed& out) {
    Json j;
    if (!parseJson(json, j) || j.type != Json::Object) return false;
    Installed in;
    for (const auto& [id, v] : j.obj) {
        if (v.type != Json::Object) continue;
        InstalledComponent c;
        c.version = v.getString("version");
        c.files = strings(v.get("files"));
        in[id] = std::move(c);
    }
    out = std::move(in);
    return true;
}

std::string installedToJson(const Installed& in) {
    std::string out = "{";
    bool first = true;
    for (const auto& [id, c] : in) {
        if (!first) out += ",";
        first = false;
        jsonString(out, id);
        out += ":{\"version\":";
        jsonString(out, c.version);
        out += ",\"files\":[";
        for (size_t i = 0; i < c.files.size(); ++i) {
            if (i) out += ",";
            jsonString(out, c.files[i]);
        }
        out += "]}";
    }
    out += "}";
    return out;
}

namespace {

std::vector<const Component*> plan(const Manifest& m, const Installed& installed, const std::vector<std::string>& wanted) {
    std::vector<const Component*> out;
    std::set<std::string> visiting, done;
    std::function<void(const std::string&)> visit = [&](const std::string& id) {
        if (done.count(id) || visiting.count(id)) return;  // 循環は無視
        const Component* c = m.find(id);
        if (!c) return;
        visiting.insert(id);
        for (const auto& r : c->requires_) visit(r);
        visiting.erase(id);
        done.insert(id);
        auto it = installed.find(id);
        if (it == installed.end() || it->second.version != c->version) out.push_back(c);
    };
    for (const auto& id : wanted) visit(id);
    return out;
}

}  // namespace

std::vector<const Component*> planInstall(const Manifest& m, const Installed& installed, const std::vector<std::string>& wanted) {
    return plan(m, installed, wanted);
}

std::vector<const Component*> planUpdate(const Manifest& m, const Installed& installed) {
    std::vector<std::string> ids;
    for (const auto& [id, c] : installed) ids.push_back(id);
    return plan(m, installed, ids);
}

std::vector<std::string> staleFiles(const InstalledComponent& before, const Component& now) {
    std::vector<std::string> out;
    for (const auto& f : before.files) {
        bool keep = false;
        for (const auto& n : now.files)
            if (n.path == f) keep = true;
        if (!keep && safeRelativePath(f)) out.push_back(f);
    }
    return out;
}

}  // namespace imgdupe
