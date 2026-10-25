// 外部照会（lookup.hpp）を WinHTTP で動かす（Windows 専用）。設定は imgdupe.ini の [imgdupe]:
//   lookupEnabled（ID 照会。既定 0）、sauceEnabled（逆画像検索。既定 0）、sauceKey（SauceNAO の API キー）、sauceDailyLimit（既定 100）
// 環境変数 IMGDUPE_DANBOORU_URL / IMGDUPE_PIXIV_URL / IMGDUPE_SAUCENAO_URL で行き先を変えられる（テスト用）。
#pragma once

#include <atomic>
#include <string>

#include "db.hpp"
#include "lookup.hpp"

namespace lookup {

struct Settings {
    bool idLookup = false, reverse = false;
    std::wstring sauceKey;
    int dailyLimit = imgdupe::kSauceNaoDefaultDailyLimit;
};
Settings load(const std::wstring& dataDir);
void save(const std::wstring& dataDir, const Settings& s);

// 照会する（どちらも切ってあれば何もしない。サムネイルは <dataDir> のパックから読む）
imgdupe::LookupReport run(const std::wstring& dataDir, imgdupe::Db& db, const std::atomic<bool>& cancel,
                          imgdupe::Progress* progress);

}  // namespace lookup
