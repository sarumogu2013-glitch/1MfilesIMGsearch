// 版はビルド時に -DIMGDUPE_VERSION="\"v1.0.0\"" のように埋め込む。手元のビルドは "dev"。
// 版の上 2 つ（メジャー.マイナー）は imgdupe/VERSION で決め、パッチはリリースのたびに自動で増える。
#pragma once

#ifndef IMGDUPE_VERSION
#define IMGDUPE_VERSION "dev"
#endif

#define APP_RELEASES_URL L"https://github.com/sarumogu2013-glitch/1MfilesIMGsearch/releases/latest"
