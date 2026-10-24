// サムネイルのパックファイル（仕様「データ層」: 100 万個の小さなファイルを作ると NTFS が遅くなるので、まとめて詰める）。
// thumbs_000.pack, thumbs_001.pack ... に追記していき、どこにあるかは DB の thumbs テーブルに持つ。OS に依存しない。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace imgdupe {

class Db;

enum class ThumbFormat : uint8_t { Jpeg = 1, WebP = 2 };

struct ThumbData {
    ThumbFormat format = ThumbFormat::Jpeg;
    uint16_t width = 0, height = 0;
    std::vector<uint8_t> bytes;
};

class ThumbPack {
public:
    // dir: パックファイルを置くフォルダ。maxPackBytes を超えたら次のファイルに移る
    explicit ThumbPack(std::string dir, uint64_t maxPackBytes = 2000ull * 1024 * 1024);

    // 追記して thumbs テーブルに場所を書く（呼び出し側のトランザクションの中で呼ぶ）
    bool put(Db& db, int64_t imageId, const ThumbData& t);
    // 無い・壊れている時は false
    bool get(Db& db, int64_t imageId, ThumbData& out) const;

    // 使われていない部分（消えた画像・作り直したサムネイル）の合計（バイト）
    uint64_t garbageBytes(Db& db) const;

    std::string packPath(int index) const;

private:
    std::string dir_;
    uint64_t max_;
};

}  // namespace imgdupe
