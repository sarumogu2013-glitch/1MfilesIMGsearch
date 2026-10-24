#!/usr/bin/env bash
# imgdupe のビルド。
#   ./build.sh        Windows 用の out/imgdupe.exe を作る（MinGW-w64。Linux からのクロスビルド / MSYS2 どちらでも可）
#   ./build.sh test   OS 非依存部分のテストを手元の g++ でビルドして実行する
# 版は環境変数 IMGDUPE_VERSION で埋め込む（未指定なら dev）。
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out

if [ "${1:-}" = "test" ]; then
  ${CXX:-g++} -std=c++17 -O2 -Wall -Wextra -pthread -Isrc \
    src/core.cpp src/platform_posix.cpp src/json.cpp src/update_logic.cpp tests/test_core.cpp -o out/test_core
  ./out/test_core
  exit 0
fi

VERSION=${IMGDUPE_VERSION:-dev}

pick() { for c in "$@"; do if command -v "$c" >/dev/null 2>&1; then echo "$c"; return; fi; done; }
CXX=${CXX:-$(pick x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++ g++)}
WINDRES=${WINDRES:-$(pick x86_64-w64-mingw32-windres windres)}
if [ -z "$CXX" ] || [ -z "$WINDRES" ]; then
  echo "MinGW-w64 の g++ と windres が見つかりません" >&2
  exit 1
fi

"$WINDRES" -I src -O coff -i src/app.rc -o out/app.res.o
# -mpopcnt: ハミング距離の計算に POPCNT 命令を使う（2008 年以降の x64 CPU はすべて対応）
"$CXX" -std=c++17 -O2 -mpopcnt -s -Wall -Wextra -Wno-missing-field-initializers -municode -mwindows -static \
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -DIMGDUPE_VERSION="\"$VERSION\"" -Isrc \
  src/core.cpp src/platform_win.cpp src/image_win.cpp src/json.cpp src/update_logic.cpp src/update_win.cpp src/main_win.cpp out/app.res.o \
  -o out/imgdupe.exe \
  -lwindowscodecs -lwinhttp -lbcrypt -lcomctl32 -lshell32 -lshlwapi -lole32 -loleaut32 -luuid -lgdi32 -luser32
echo "out/imgdupe.exe ($VERSION)"
