#include "commands.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace imgdupe {

namespace {

// 仮想キーコード（Windows の値。テストのため windows.h を使わずに持つ）
enum : unsigned {
    K_BACK = 0x08, K_TAB = 0x09, K_RETURN = 0x0D, K_PAUSE = 0x13, K_ESCAPE = 0x1B, K_SPACE = 0x20,
    K_PRIOR = 0x21, K_NEXT = 0x22, K_END = 0x23, K_HOME = 0x24, K_LEFT = 0x25, K_UP = 0x26, K_RIGHT = 0x27,
    K_DOWN = 0x28, K_INSERT = 0x2D, K_DELETE = 0x2E, K_APPS = 0x5D, K_NUMPAD0 = 0x60, K_MULTIPLY = 0x6A,
    K_ADD = 0x6B, K_SUBTRACT = 0x6D, K_DECIMAL = 0x6E, K_DIVIDE = 0x6F, K_F1 = 0x70, K_F24 = 0x87,
    K_BROWSER_BACK = 0xA6, K_BROWSER_FORWARD = 0xA7, K_OEM_COMMA = 0xBC,
};

struct KeyNameDef {
    unsigned vk;
    const char* name;
};
const KeyNameDef kKeyNames[] = {
    {K_BACK, "Backspace"}, {K_TAB, "Tab"},       {K_RETURN, "Enter"},   {K_PAUSE, "Pause"},
    {K_ESCAPE, "Esc"},     {K_SPACE, "Space"},   {K_PRIOR, "PageUp"},   {K_NEXT, "PageDown"},
    {K_END, "End"},        {K_HOME, "Home"},     {K_LEFT, "Left"},      {K_UP, "Up"},
    {K_RIGHT, "Right"},    {K_DOWN, "Down"},     {K_INSERT, "Insert"},  {K_DELETE, "Delete"},
    {K_APPS, "Apps"},      {K_MULTIPLY, "Num*"}, {K_ADD, "Num+"},       {K_SUBTRACT, "Num-"},
    {K_DECIMAL, "Num."},   {K_DIVIDE, "Num/"},   {K_BROWSER_BACK, "BrowserBack"},
    {K_BROWSER_FORWARD, "BrowserForward"}, {K_OEM_COMMA, "Comma"},
};

const std::vector<CmdInfo> kCommands = {
    {CMD_SCAN_FOLDER, "ScanFolder", L"フォルダを解析の予約に追加(&S)...", "Ctrl+O"},
    {CMD_SCAN_FOLDER_TOP, "ScanFolderTop", L"フォルダを最優先で解析(&T)...", "Ctrl+Shift+O"},
    {CMD_SCAN_TASKS, "ScanTasks", L"解析の予約の一覧(&K)", ""},
    {CMD_ADD_FOLDER, "AddFolder", L"監視フォルダを追加(&A)...", ""},
    {CMD_REMOVE_FOLDER, "RemoveFolder", L"監視をやめる(&R)", ""},
    {CMD_MANAGE_ROOTS, "ManageFolders", L"監視フォルダの管理(&W)...", ""},
    {CMD_REMAP_ROOT, "RemapFolder", L"監視フォルダの移動先を指定(&M)...", ""},
    {CMD_EXPORT, "Export", L"結果を書き出す(&W)...", "Ctrl+S"},
    {CMD_EXIT, "Exit", L"終了(&X)", ""},
    {CMD_SEARCH, "Search", L"チェック開始 / 中止(&S)", "F5"},
    {CMD_QUERY, "Query", L"1 枚の重複チェック(&Q)...", "Ctrl+F"},
    {CMD_TOGGLE_EXACT, "ExactOnly", L"完全一致だけ(&E)", ""},
    {CMD_TOGGLE_FLAT, "IncludeFlat", L"単色に近い画像も比べる(&F)", ""},
    {CMD_THRESHOLD_DOWN, "ThresholdDown", L"見た目一致を厳しく(&D)", ""},
    {CMD_THRESHOLD_UP, "ThresholdUp", L"見た目一致をゆるく(&U)", ""},
    {CMD_PREV_GROUP, "PrevGroup", L"前のまとまり(&P)", "Ctrl+Up"},
    {CMD_NEXT_GROUP, "NextGroup", L"次のまとまり(&N)", "Ctrl+Down"},
    {CMD_OPEN, "Open", L"開く(&O)", "Enter"},
    {CMD_EXPLORER, "ShowInExplorer", L"エクスプローラーで表示(&E)", "Ctrl+E"},
    {CMD_COPY_PATH, "CopyPath", L"パスをコピー(&C)", "Ctrl+C"},
    {CMD_TAB_EXPLORER, "TabExplorer", L"ホーム(&H)", "Ctrl+3"},
    {CMD_FIND_SAME, "FindSame", L"選んだ画像の同一画像を探す(&M)", "Ctrl+M"},
    {CMD_TAB_DUPES, "TabDuplicates", L"重複チェック(&D)", "Ctrl+1"},
    {CMD_TAB_SEARCH, "TabSearch", L"検索(&S)", "Ctrl+2"},
    {CMD_TAB_QUERY, "TabQuery", L"画像から検索(&Q)", "Ctrl+4"},
    {CMD_QUERY_PASTE, "QueryPaste", L"クリップボードの画像から検索(&V)", "Ctrl+Shift+V"},
    {CMD_QUERY_OPEN, "QueryOpen", L"画像のファイルで探す(&F)...", ""},
    {CMD_QUERY_URL, "QueryUrl", L"画像の URL で探す(&U)...", ""},
    {CMD_QUERY_SAME, "QuerySame", L"この画像の同一画像を探す(&S)", ""},
    {CMD_QUERY_SIMILAR, "QuerySimilar", L"この画像に類似画像を探す（AI）(&A)", ""},
    {CMD_QUERY_TAGS, "QueryTags", L"この画像のタグを生成（AI）(&T)", ""},
    {CMD_SAVE_SMART, "SaveSmartFolder", L"条件をスマートフォルダに保存(&M)...", "Ctrl+Shift+S"},
    {CMD_MANAGE_QUERIES, "ManageQueries", L"スマートフォルダの管理(&Q)...", ""},
    {CMD_SAVE_SELECTION, "SaveSelectionSmartFolder", L"選んだ画像をスマートフォルダに保存(&P)...", "Ctrl+Shift+M"},
    {CMD_DELETE_SMART, "DeleteSmartFolder", L"スマートフォルダを削除(&L)", ""},
    {CMD_EXPAND_LINKS, "ExpandLinks", L"検索結果からリンクフォルダを作成(&X)...", "Ctrl+Shift+E"},
    {CMD_LINKS, "Links", L"リンクフォルダ(&F)...", ""},
    {CMD_UNDO, "Undo", L"タグの操作を元に戻す(&U)", "Ctrl+Z"},
    {CMD_TAG_JAPANESE, "TagJapanese", L"タグを日本語で表示(&J)", "Ctrl+J"},
    {CMD_SETTINGS, "Settings", L"設定(&S)...", "Ctrl+Comma"},
    {CMD_DICTIONARY, "Dictionary", L"AI タグ辞書（行動・アイテム）(&D)...", ""},
    {CMD_TAGDICT, "TagDictionary", L"タグの別名・親子(&T)...", ""},
    {CMD_REFS, "References", L"見本画像のフォルダを開く(&R)", ""},
    {CMD_STATS, "Statistics", L"統計(&G)...", ""},
    {CMD_KEY_SETTINGS, "KeySettings", L"ショートカットキーの設定(&K)...", "Ctrl+K"},
    {CMD_INDEXER_TOGGLE, "Indexer", L"常駐解析を始める(&B)", ""},
    {CMD_INDEXER_AUTOSTART, "IndexerAutoStart", L"Windows と一緒に常駐解析を始める(&A)", ""},
    {CMD_CHECK_UPDATE, "CheckUpdate", L"アップデートを確認(&U)", ""},
    {CMD_MODELS, "AiComponents", L"AI の部品(&M)...", ""},
    {CMD_RELEASES, "Releases", L"最新版のページを開く(&R)", ""},
    {CMD_HELP, "Help", L"使い方(&H)", "F1"},
    {CMD_COMPACT_DB, "CompactDb", L"DB を詰めて小さくする(&Z)...", ""},
    {CMD_ARCHIVE, "Archive", L"退避した解析結果(&H)...", ""},
    {CMD_REFRESH, "Refresh", L"更新(&F)", "Ctrl+F5"},
    {CMD_INFER_RULES, "InferRules", L"フォルダ名のルール(&N)...", ""},
    {CMD_OPEN_DATA, "OpenDataFolder", L"設定のフォルダを開く(&D)", ""},
    {CMD_OPEN_LOGS, "OpenLogs", L"動作履歴（ログ）のフォルダを開く(&L)", ""},
    {CMD_ABOUT, "About", L"バージョン情報(&A)", ""},
    {CMD_TOGGLE_VARIANTS, "IncludeVariants", L"差分（AI）も出す(&V)", ""},
    {CMD_SEARCH_GO, "SearchRun", L"検索する(&S)", ""},
    {CMD_SEARCH_END, "SearchEnd", L"検索を終了(&E)", ""},
    {CMD_SEARCH_CLEAR, "SearchClear", L"条件をクリア(&C)", ""},
    {CMD_SEARCH_ADVANCED, "SearchAdvanced", L"詳細検索を開く / 閉じる(&A)", ""},
    {CMD_TAG_EDIT, "TagEdit", L"タグの手動編集(&E)", ""},
    {CMD_VIEW_TREE, "ShowFolderTree", L"フォルダ一覧(&T)", ""},
    {CMD_VIEW_PREVIEW, "ShowPreview", L"プレビュー(&P)", ""},
    {CMD_VIEW_DETAILS, "ViewDetails", L"詳細(&L)", ""},
    {CMD_VIEW_SMALL, "ViewSmall", L"サムネイル（小）(&1)", ""},
    {CMD_VIEW_MEDIUM, "ViewMedium", L"サムネイル（普通）(&2)", ""},
    {CMD_VIEW_LARGE, "ViewLarge", L"サムネイル（大）(&3)", ""},
    {CMD_VIEW_CUSTOM, "ViewCustom", L"サムネイル（可変）(&4)", ""},
    {CMD_SORT_NAME, "SortName", L"名前(&N)", ""},
    {CMD_SORT_DATE, "SortDate", L"更新日時(&D)", ""},
    {CMD_SORT_TYPE, "SortType", L"種類(&T)", ""},
    {CMD_SORT_SIZE, "SortSize", L"サイズ(&S)", ""},
    {CMD_SORT_RESULT, "SortResult", L"検索結果の順(&R)", ""},
    {CMD_SORT_ASC, "SortAscending", L"昇順(&A)", ""},
    {CMD_SORT_DESC, "SortDescending", L"降順(&E)", ""},
    {CMD_COVER_1, "Cover1", L"1 枚目(&1)", ""},
    {CMD_COVER_2, "Cover2", L"1・2 枚目(&2)", ""},
    {CMD_COVER_3, "Cover3", L"最後と 1 枚目(&3)", ""},
    {CMD_COVER_4, "Cover4", L"最後(&4)", ""},
    {CMD_COVER_5, "Cover5", L"最後の一つ前と最後(&5)", ""},
    {CMD_DELETE, "Delete", L"削除（ごみ箱へ）(&D)", "Delete"},
};

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool startsWithI(const std::string& s, const char* prefix, size_t n) {
    return s.size() > n && iequals(s.substr(0, n), prefix);
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!trim(cur).empty()) out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

}  // namespace

const std::vector<CmdInfo>& allCommands() { return kCommands; }

const CmdInfo* findCommand(int id) {
    for (const auto& c : kCommands)
        if (c.id == id) return &c;
    return nullptr;
}

const CmdInfo* findCommand(const std::string& name) {
    for (const auto& c : kCommands)
        if (iequals(name, c.name)) return &c;
    return nullptr;
}

bool parseKey(const std::string& text, KeyStroke& out) {
    KeyStroke k;
    std::string s = trim(text);
    for (;;) {
        if (startsWithI(s, "Ctrl+", 5)) {
            k.mods |= KMOD_CTRL;
            s = s.substr(5);
        } else if (startsWithI(s, "Shift+", 6)) {
            k.mods |= KMOD_SHIFT;
            s = s.substr(6);
        } else if (startsWithI(s, "Alt+", 4)) {
            k.mods |= KMOD_ALT;
            s = s.substr(4);
        } else {
            break;
        }
    }
    if (s.size() == 1 && std::isalnum(static_cast<unsigned char>(s[0]))) {
        k.code = static_cast<unsigned>(std::toupper(static_cast<unsigned char>(s[0])));
    } else if (s.size() >= 2 && (s[0] == 'F' || s[0] == 'f') && std::isdigit(static_cast<unsigned char>(s[1]))) {
        const int n = std::atoi(s.c_str() + 1);
        if (n < 1 || n > 24) return false;
        k.code = K_F1 + n - 1;
    } else if (s.size() == 4 && iequals(s.substr(0, 3), "Num") && std::isdigit(static_cast<unsigned char>(s[3]))) {
        k.code = K_NUMPAD0 + (s[3] - '0');
    } else {
        for (const auto& n : kKeyNames)
            if (iequals(s, n.name)) k.code = n.vk;
        if (!k.code && s.size() > 2 && iequals(s.substr(0, 2), "VK"))
            k.code = static_cast<unsigned>(std::strtoul(s.c_str() + 2, nullptr, 16));
        if (!k.code) return false;
    }
    out = k;
    return true;
}

std::string keyName(const KeyStroke& k) {
    std::string s;
    if (k.mods & KMOD_CTRL) s += "Ctrl+";
    if (k.mods & KMOD_SHIFT) s += "Shift+";
    if (k.mods & KMOD_ALT) s += "Alt+";
    if ((k.code >= 'A' && k.code <= 'Z') || (k.code >= '0' && k.code <= '9')) return s + static_cast<char>(k.code);
    if (k.code >= K_F1 && k.code <= K_F24) return s + "F" + std::to_string(k.code - K_F1 + 1);
    if (k.code >= K_NUMPAD0 && k.code <= K_NUMPAD0 + 9) return s + "Num" + static_cast<char>('0' + k.code - K_NUMPAD0);
    for (const auto& n : kKeyNames)
        if (n.vk == k.code) return s + n.name;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "VK%02X", k.code);
    return s + buf;
}

KeyMap defaultKeys() {
    KeyMap m;
    for (const auto& c : kCommands)
        for (const auto& t : splitList(c.keys)) {
            KeyStroke k;
            if (parseKey(t, k)) m[k] = c.id;
        }
    return m;
}

std::vector<std::string> keyLines(const KeyMap& keys) {
    std::vector<std::string> lines;
    for (const auto& c : kCommands) {
        std::string ks;
        for (const auto& [k, id] : keys) {
            if (id != c.id) continue;
            if (!ks.empty()) ks += ", ";
            ks += keyName(k);
        }
        if (!ks.empty()) lines.push_back(std::string(c.name) + " = " + ks);
    }
    return lines;
}

KeyMap keysFromLines(const std::vector<std::string>& lines) {
    KeyMap m;
    for (const auto& line : lines) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const CmdInfo* c = findCommand(trim(line.substr(0, eq)));
        if (!c) continue;
        for (const auto& t : splitList(line.substr(eq + 1))) {
            KeyStroke k;
            if (parseKey(t, k)) m[k] = c->id;
        }
    }
    return m;
}

std::string keyLabel(const KeyMap& keys, int cmd) {
    // 修飾キーの少ないものを優先して 1 つ
    const KeyStroke* best = nullptr;
    for (const auto& [k, id] : keys)
        if (id == cmd && (!best || __builtin_popcount(k.mods) < __builtin_popcount(best->mods))) best = &k;
    return best ? keyName(*best) : std::string();
}

}  // namespace imgdupe
