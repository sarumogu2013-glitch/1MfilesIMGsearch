// AI 解析で使う部品（ONNX Runtime・モデル）の一覧と、入っている版の管理。OS に依存しない。
// 一覧（models.json）はリリースに添付する。作り方は tools/make_models_json.py と models.src.json。
//   {"version":1,"components":[{"id":"wd14","name":"...","description":"...","version":"...","requires":["onnxruntime"],
//     "files":[{"path":"wd14/model.onnx","url":"https://...","sha256":"...","size":123}]}]}
// 入れた部品は <データのフォルダ>/models/installed.json に {"wd14":{"version":"...","files":["wd14/model.onnx"],
//   "sha256":["..."],"analysisKey":"..."}} で残す。
// analysisKey は解析の結果（タグ・CLIP のベクトル）の版に使う名前。版（Hugging Face のコミット）だけが変わってファイルの中身が同じ時は、
// 前の analysisKey のままにする（作り直さない）。無ければ version
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace imgdupe {

constexpr const char* kModelsDir = "models";
constexpr const char* kInstalledFile = "installed.json";

struct ComponentFile {
    std::string path;    // models フォルダからの相対パス（/ 区切り）
    std::string url;     // https
    std::string sha256;  // 16 進（小文字）
    uint64_t size = 0;
};

struct Component {
    std::string id, name, description, version;
    std::vector<std::string> requires_;  // 先に入れる部品
    std::vector<ComponentFile> files;
    uint64_t totalSize() const;
};

struct Manifest {
    std::vector<Component> components;
    const Component* find(const std::string& id) const;
};

// models.json を読む。形が違えば false（パスに .. や絶対パスがあるもの、https 以外の URL も不可）
bool parseManifest(const std::string& json, Manifest& out);

struct InstalledComponent {
    std::string version;
    std::vector<std::string> files;
    std::vector<std::string> sha256;  // files と同じ並び（前の版の記録には無い）
    std::string analysisKey;
    const std::string& key() const { return analysisKey.empty() ? version : analysisKey; }
};
// 入れた部品のファイルの中身が、一覧の部品と同じか（パスと sha256 が全部同じ。記録に sha256 が無ければ false）
bool sameFiles(const InstalledComponent& installed, const Component& c);
using Installed = std::map<std::string, InstalledComponent>;

bool parseInstalled(const std::string& json, Installed& out);
std::string installedToJson(const Installed& in);

// 入れる（または入れ直す）部品の順番。wanted の部品と、それが要る部品のうち、入っていないか版が違うもの。
// 要る部品を先に並べる。wanted に manifest に無い ID があれば無視する
std::vector<const Component*> planInstall(const Manifest& m, const Installed& installed, const std::vector<std::string>& wanted);
// 入っている部品のうち、版が変わったもの（アップデートの確認で使う。要る部品が足りなければそれも）
std::vector<const Component*> planUpdate(const Manifest& m, const Installed& installed);
// 部品を入れ直した後、前の版にあって今の版に無いファイル（消してよいもの）
std::vector<std::string> staleFiles(const InstalledComponent& before, const Component& now);

// パスとして安全か（相対パスで、.. を含まない）
bool safeRelativePath(const std::string& path);

}  // namespace imgdupe
