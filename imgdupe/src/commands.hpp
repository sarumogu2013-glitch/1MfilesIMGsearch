// 機能（メニュー・ツールバー・ショートカットキーで呼べるもの）と、キーの割り当て。
// 機能の番号は WM_COMMAND の ID にもそのまま使う。OS に依存しない（テストできる）。
#pragma once

#include <map>
#include <string>
#include <vector>

namespace imgdupe {

enum Cmd : int {
    CMD_ADD_FOLDER = 300,
    CMD_REMOVE_FOLDER,
    CMD_SEARCH,  // 検索開始 / 中止
    CMD_QUERY,
    CMD_EXPORT,
    CMD_OPEN,
    CMD_EXPLORER,
    CMD_COPY_PATH,
    CMD_PREV_GROUP,
    CMD_NEXT_GROUP,
    CMD_TOGGLE_EXACT,
    CMD_TOGGLE_FLAT,
    CMD_THRESHOLD_DOWN,
    CMD_THRESHOLD_UP,
    CMD_KEY_SETTINGS,
    CMD_SETTINGS,
    CMD_DICTIONARY,
    CMD_TAGDICT,
    CMD_REFS,
    CMD_STATS,
    CMD_TAB_EXPLORER,
    CMD_FIND_SAME,
    CMD_TAB_DUPES,
    CMD_TAB_SEARCH,
    CMD_SAVE_SMART,
    CMD_DELETE_SMART,
    CMD_EXPAND_LINKS,
    CMD_LINKS,
    CMD_TAG_YES,
    CMD_TAG_NO,
    CMD_BULK_TAG,
    CMD_REVIEW,
    CMD_UNDO,
    CMD_TAG_JAPANESE,
    CMD_INDEXER_TOGGLE,
    CMD_INDEXER_AUTOSTART,
    CMD_CHECK_UPDATE,
    CMD_MODELS,
    CMD_RELEASES,
    CMD_HELP,
    CMD_ABOUT,
    CMD_EXIT,
};

struct CmdInfo {
    int id;
    const char* name;       // 設定ファイルでの名前
    const wchar_t* label;   // メニューの表示
    const char* keys;       // 標準のキー（, 区切り）
};
const std::vector<CmdInfo>& allCommands();
const CmdInfo* findCommand(int id);
const CmdInfo* findCommand(const std::string& name);

// キー: 修飾キー + 仮想キーコード
enum : unsigned { KMOD_CTRL = 1, KMOD_SHIFT = 2, KMOD_ALT = 4 };
struct KeyStroke {
    unsigned mods = 0;
    unsigned code = 0;
    bool operator<(const KeyStroke& o) const { return mods != o.mods ? mods < o.mods : code < o.code; }
    bool operator==(const KeyStroke& o) const { return mods == o.mods && code == o.code; }
};
using KeyMap = std::map<KeyStroke, int>;  // キー → 機能

// "Ctrl+O" / "F5" / "Ctrl+Shift+Up" / "Num5" / "VK1F" を読む。読めなければ false
bool parseKey(const std::string& text, KeyStroke& out);
std::string keyName(const KeyStroke& k);

KeyMap defaultKeys();
// 設定ファイルの [Keys] の行（"Search = F5, Ctrl+R"）。一覧の順に、割り当てのある機能だけ
std::vector<std::string> keyLines(const KeyMap& keys);
// [Keys] の行から読む（知らない機能やキーの行は飛ばす）
KeyMap keysFromLines(const std::vector<std::string>& lines);
// メニューに出すキーの表示（最初の 1 つ。無ければ空）
std::string keyLabel(const KeyMap& keys, int cmd);

}  // namespace imgdupe
