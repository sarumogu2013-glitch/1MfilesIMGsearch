// CLIP を ONNX Runtime で動かす（Windows 専用）。画像側は indexer の段階 clip、言葉側は画面の「内容」の検索で使う。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "clip.hpp"
#include "db.hpp"

namespace clip {

// 段階 clip の版（入っているモデルの版から。入っていないか、設定で切ってあれば 0）
int stageVersion(const std::wstring& dataDir);
std::wstring vectorPath(const std::wstring& dataDir);  // <dataDir>\vectors_clip.bin

// 段階 clip の前に呼ぶ: ベクトルのファイルの版が今のモデルと違う（無い）なら、段階 clip の記録を消してやり直させる。
// 古いレコードや消えた画像が多ければ、ファイルを作り直す
void prepare(const std::wstring& dataDir, imgdupe::Db& db);

struct LoadResult {
    std::shared_ptr<imgdupe::Analyzer> analyzer;
    std::wstring device;
    std::wstring error;
};
// 段階 clip（画像側のモデルを読む）
LoadResult loadAnalyzer(const std::wstring& dataDir, int cpuThreads = 0, std::function<void()> threadInit = {},
                        std::function<void()> threadExit = {});

// 言葉をベクトルにする（言葉側のモデルを CPU で読む。最初の 1 回は数秒かかる）。入っていなければ nullptr と理由
std::shared_ptr<imgdupe::ClipModel> loadText(const std::wstring& dataDir, std::wstring& error);

}  // namespace clip
