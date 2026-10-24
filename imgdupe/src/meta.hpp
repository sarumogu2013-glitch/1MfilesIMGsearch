// 段階 4: メタデータ・名前解析（仕様「ファイルのメタデータ」「もとから付いているタグ・星・タイトル」「ファイル名の ID 判定」）。
// ファイルからの読み取り（WIC・NTFS）は呼び出し側が渡し、ここでは解析と DB への書き込みを行う。OS に依存しない。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "analyzer.hpp"

namespace imgdupe {

constexpr int kMetaVersion = 1;
constexpr double kEmbeddedTagConfidence = 0.90;  // 出どころ「ファイル埋め込み」の信頼度の初期値
constexpr const char* kSourceEmbedded = "embedded";

struct FileMeta {
    // 画像の基本
    std::string format;  // "JPEG" / "PNG" など
    int bitDepth = 0;
    bool animated = false;
    // EXIF
    int64_t taken = 0;  // 撮影日時（Unix 秒、0 = 無し）
    std::string camera, lens, software;
    double focal = 0;
    int iso = 0;
    bool hasGps = false;
    // XMP / IPTC / Windows のプロパティ
    std::vector<std::string> keywords;
    int rating = -1;  // 0〜5（-1 = 無し）
    std::string title, subject, comment, author;
    // PNG のテキスト（画像生成 AI のプロンプトなど）
    std::string pngText;
    // NTFS
    int64_t created = 0, accessed = 0;  // Unix 秒
    uint32_t attributes = 0;
    uint32_t hardlinks = 0;
    uint64_t fileId = 0;
    // 代替データストリーム Zone.Identifier
    std::string zoneUrl, zoneReferrer;
};

// ファイル名から分かる投稿の ID
struct SourceId {
    std::string site;    // "pixiv" / "x" / "md5"
    std::string postId;
    int page = -1;       // pixiv の _p0 など
};
std::vector<SourceId> parseFilenameIds(const std::string& fileName);

// PNG のチャンクから tEXt / iTXt（圧縮していないもの）の文字と、アニメーション（acTL）かどうかを読む
struct PngInfo {
    std::vector<std::pair<std::string, std::string>> texts;  // キーワード → 文字
    bool animated = false;
    std::vector<uint8_t> exif;  // eXIf チャンク（TIFF 形式）
};
bool parsePng(const uint8_t* data, size_t len, PngInfo& out);

// JPEG の先頭（APP1 の EXIF と XMP）から読める分を m に入れる（XMP がある項目は XMP を優先）。JPEG でなければ false
bool parseJpegMeta(const uint8_t* data, size_t len, FileMeta& m);
// TIFF 形式の EXIF（"II*\0" / "MM\0*" から）を読む
void parseExif(const uint8_t* tiff, size_t len, FileMeta& m);
// XMP（XML）の dc:subject / dc:title / dc:description / dc:creator / xmp:Rating を読む（値があるものだけ上書き）
void parseXmp(const std::string& xml, FileMeta& m);

// PNG の情報を m に入れる（XMP のテキストは XMP として読み、それ以外は pngText に）
void applyPng(const PngInfo& pi, FileMeta& m);
// a で空の項目を b で埋める
void fillMissing(FileMeta& a, const FileMeta& b);

// Zone.Identifier の中身（[ZoneTransfer] HostUrl= / ReferrerUrl=）
void parseZoneIdentifier(const std::string& text, std::string& hostUrl, std::string& referrerUrl);

// Windows の FILETIME（1601 年からの 100ns）→ Unix 秒
int64_t fileTimeToUnix(uint64_t fileTime);
// EXIF の "2024:05:01 12:34:56" → Unix 秒（ローカル時刻のまま UTC として数える。0 = 読めない）
int64_t parseExifDate(const std::string& s);

// file_meta・埋め込みのタグ・出典・全文検索の索引を書く（呼び出し側のトランザクションの中で）
void storeMeta(Db& db, int64_t imageId, const std::string& path, const FileMeta& m);

// 全文検索（タイトル・件名・コメント・キーワード・PNG のテキスト）。一致した画像 ID
std::vector<int64_t> searchMetaText(Db& db, const std::string& query, size_t limit = 1000);

class MetaAnalyzer : public Analyzer {
public:
    using Reader = std::function<bool(const std::string& path, FileMeta& out)>;
    explicit MetaAnalyzer(Reader reader, std::function<void()> threadInit = {}, std::function<void()> threadExit = {})
        : reader_(std::move(reader)), init_(std::move(threadInit)), exit_(std::move(threadExit)) {}
    std::string stageName() const override { return stage::kMeta; }
    int version() const override { return kMetaVersion; }
    AnalyzerOutput analyze(const AnalyzerInput& in) override;
    bool store(Db& db, int64_t imageId, AnalyzerOutput& out) override;
    void threadBegin() override {
        if (init_) init_();
    }
    void threadEnd() override {
        if (exit_) exit_();
    }

private:
    Reader reader_;
    std::function<void()> init_, exit_;
};

}  // namespace imgdupe
