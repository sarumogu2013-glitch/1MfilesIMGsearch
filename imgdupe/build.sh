#!/usr/bin/env bash
# imgdupe のビルド。
#   ./build.sh        Windows 用の out/imgdupe.exe を作る（MinGW-w64。Linux からのクロスビルド / MSYS2 どちらでも可）
#   ./build.sh test   OS 非依存部分のテストを手元の g++ でビルドして実行する
# 版は環境変数 IMGDUPE_VERSION で埋め込む（未指定なら dev）。
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out

# SQLite（third_party/sqlite のアマルガメーション）。大きいので、ソースが変わった時だけコンパイルする
SQLITE_FLAGS="-O2 -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_FOREIGN_KEYS=1 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_MATH_FUNCTIONS -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0"
sqlite_obj() {  # $1: C コンパイラ, $2: 出力（ソースかフラグが変わった時だけ作り直す）
  if [ ! -f "$2" ] || [ third_party/sqlite/sqlite3.c -nt "$2" ] || [ "$(cat "$2.flags" 2>/dev/null)" != "$SQLITE_FLAGS" ]; then
    echo "SQLite をコンパイルしています ($2)"
    # shellcheck disable=SC2086
    "$1" $SQLITE_FLAGS -c third_party/sqlite/sqlite3.c -o "$2"
    printf '%s' "$SQLITE_FLAGS" > "$2.flags"
  fi
}

CORE_SRC="src/core.cpp src/db.cpp src/analyzer.cpp src/pack.cpp src/indexer.cpp src/meta.cpp src/search.cpp src/json.cpp src/update_logic.cpp src/commands.cpp src/links.cpp src/names.cpp"

if [ "${1:-}" = "test" ]; then
  sqlite_obj "${CC:-gcc}" out/sqlite3_host.o
  ${CXX:-g++} -std=c++17 -O2 -Wall -Wextra -pthread -Isrc -Ithird_party/sqlite \
    $CORE_SRC src/platform_posix.cpp tests/test_core.cpp out/sqlite3_host.o -o out/test_core -ldl
  ./out/test_core
  exit 0
fi

VERSION=${IMGDUPE_VERSION:-dev}

pick() { for c in "$@"; do if command -v "$c" >/dev/null 2>&1; then echo "$c"; return; fi; done; }
CXX=${CXX:-$(pick x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++ g++)}
CC=${CC:-$(pick x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-gcc gcc)}
WINDRES=${WINDRES:-$(pick x86_64-w64-mingw32-windres windres)}
if [ -z "$CXX" ] || [ -z "$CC" ] || [ -z "$WINDRES" ]; then
  echo "MinGW-w64 の gcc / g++ と windres が見つかりません" >&2
  exit 1
fi

sqlite_obj "$CC" out/sqlite3_win.o
"$WINDRES" -I src -O coff -i src/app.rc -o out/app.res.o
# -mpopcnt: ハミング距離の計算に POPCNT 命令を使う（2008 年以降の x64 CPU はすべて対応）
# shellcheck disable=SC2086
"$CXX" -std=c++17 -O2 -mpopcnt -s -Wall -Wextra -Wno-missing-field-initializers -municode -mwindows -static \
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -DIMGDUPE_VERSION="\"$VERSION\"" -Isrc -Ithird_party/sqlite \
  $CORE_SRC src/platform_win.cpp src/image_win.cpp src/update_win.cpp src/keymap_win.cpp src/indexer_win.cpp src/main_win.cpp \
  out/sqlite3_win.o out/app.res.o \
  -o out/imgdupe.exe \
  -lwindowscodecs -lwinhttp -lbcrypt -lcomctl32 -lshell32 -lshlwapi -lole32 -loleaut32 -luuid -lgdi32 -luser32
echo "out/imgdupe.exe ($VERSION)"
