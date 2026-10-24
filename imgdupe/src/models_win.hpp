// AI の部品（ONNX Runtime・モデル）のダウンロードと入れ替え（Windows 専用。インストーラーと本体の両方で使う）。
// どの関数も UI を持たないので、別スレッドから呼んでよい。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "models.hpp"

namespace models {

// models.json の場所（環境変数 IMGDUPE_MODELS_URL があればそちら。テスト用）
std::wstring manifestUrl();

struct ManifestResult {
    bool ok = false;
    imgdupe::Manifest manifest;
    std::wstring error;
};
ManifestResult fetchManifest();

std::wstring modelsDir(const std::wstring& dataDir);  // <dataDir>\models
imgdupe::Installed loadInstalled(const std::wstring& dataDir);
bool saveInstalled(const std::wstring& dataDir, const imgdupe::Installed& in);

// progress(今の部品の名前, 受け取ったバイト数, 全部のバイト数)。false を返したら中止
using Progress = std::function<bool(const std::wstring& name, uint64_t done, uint64_t total)>;

// 部品を順にダウンロードして入れる（1 つ入れるたびに installed.json を書く。前の版にしか無いファイルは消す）。
// 失敗・中止したら、その理由を返す（入れ終わった部品はそのまま）。成功なら空
std::wstring install(const std::wstring& dataDir, const std::vector<const imgdupe::Component*>& comps, const Progress& progress);

// 入れた部品をすべて消す（アンインストール用）
void removeAll(const std::wstring& dataDir);

std::wstring sizeText(uint64_t bytes);  // "1.2 GB" など

}  // namespace models
