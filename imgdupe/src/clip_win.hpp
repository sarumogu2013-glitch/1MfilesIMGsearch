// CLIP を ONNX Runtime で動かす（Windows 専用）。画像側は indexer の段階 clip、言葉側は画面の「内容」の検索で使う。
#pragma once

#include <atomic>
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

// 段階 clip の後に呼ぶ: 近似最近傍の索引（ann_clip.hnsw）に新しい画像を足し、変わっていれば差分グループを作り直す。
// 似ている度合いの下限は imgdupe.ini の [imgdupe] variantSimilarity（既定 0.92）
void updateVariants(const std::wstring& dataDir, imgdupe::Db& db, unsigned threads, const std::atomic<bool>& cancel,
                    imgdupe::Progress* progress);
double variantSimilarity(const std::wstring& dataDir);

// 行動・アイテム辞書（<dataDir>\dictionary.txt。無ければ例を書いて置く）を当てる。
// force でなければ、辞書・タガーのタグ・CLIP のベクトルが前回から変わっていない時は何もしない。
// 当てた項目の数を entries に返す。辞書が読めなければその理由を返す（それ以外は空）
std::wstring applyDictionary(const std::wstring& dataDir, imgdupe::Db& db, bool force, size_t* entries = nullptr);
std::wstring dictionaryPath(const std::wstring& dataDir);

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
