// ONNX Runtime のセッション（Windows 専用。C API）。onnxruntime.dll は models フォルダからフルパスで読み込む
// （System32 にある Windows ML の古い onnxruntime.dll を使わないため）。
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace onnx {

struct Input {
    std::string name;
    std::vector<int64_t> shape;
    const float* f32 = nullptr;    // どちらか一方
    const int64_t* i64 = nullptr;
};

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // 失敗したら理由を返す。useGpu なら DirectML を試し、だめなら CPU。cpuThreads 0 = CPU の数だけ
    std::wstring open(const std::wstring& dllDir, const std::wstring& modelPath, bool useGpu, int cpuThreads);
    bool gpu() const { return gpu_; }
    const std::vector<std::string>& inputNames() const { return inputs_; }
    const std::vector<std::string>& outputNames() const { return outputs_; }
    std::vector<int64_t> inputShape(size_t i) const;  // 決まっていない次元は -1

    // 1 つの出力（float）を受け取る。同時に 1 つずつ（中で順番待ち）
    bool run(const std::vector<Input>& inputs, const std::string& output, std::vector<float>& out, std::vector<int64_t>& shape,
             std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<std::string> inputs_, outputs_;
    bool gpu_ = false;
    std::mutex mutex_;
};

}  // namespace onnx
