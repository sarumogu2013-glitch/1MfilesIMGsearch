// WD14 タガーを ONNX Runtime で動かす（Windows 専用）。onnxruntime.dll は models フォルダから実行時に読み込む
// （System32 にある Windows ML の古い onnxruntime.dll を使わないよう、フルパスで読む）。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "analyzer.hpp"

namespace tagger {

struct LoadResult {
    std::shared_ptr<imgdupe::Analyzer> analyzer;  // 入っていない・読めない時は空
    std::wstring device;                          // "GPU（DirectML）" / "CPU"
    std::wstring error;                           // 入っているのに読めなかった理由（入っていないだけなら空）
};

// <dataDir>\models の WD14 と ONNX Runtime から段階 tagger を作る（モデルの読み込みに数秒かかる）。
// 環境変数 IMGDUPE_TAGGER_CPU=1 か imgdupe.ini の [imgdupe] taggerCpu=1 なら GPU を使わない
// cpuThreads: CPU で動かす時のスレッド数（0 = CPU の数だけ）
LoadResult load(const std::wstring& dataDir, int cpuThreads = 0, std::function<void()> threadInit = {},
                std::function<void()> threadExit = {});

// 段階 tagger の版（入っているモデルの版から。入っていないか、設定で切ってあれば 0）。モデルを読まずにすぐ返る
int stageVersion(const std::wstring& dataDir);

}  // namespace tagger
