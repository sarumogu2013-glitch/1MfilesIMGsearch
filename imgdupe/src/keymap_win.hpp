// ショートカットキーの設定画面（キーボードの図に今の割り当てを出す。Massiguraedit と同じ作り）。
#pragma once

#include <windows.h>

#include <vector>

#include "commands.hpp"

// 設定画面が本体に頼むこと
class KeyMapHost {
public:
    virtual ~KeyMapHost() = default;
    virtual imgdupe::KeyMap& keys() = 0;
    // 割り当てる機能を選ぶメニューの中身（本体のメニューと同じ並び）
    virtual void buildCommandMenu(HMENU root) = 0;
    // 割り当てを変えた後（設定ファイルへの保存、メニューの表示の更新）
    virtual void onKeysChanged() = 0;
    // 一時置き場（キーから外した機能を置いておく枠。機能の ID、0 = 空き）
    virtual std::vector<int>& keyTmp() = 0;
};

// 開いていれば前に出す
void showKeyMapWindow(HWND owner, KeyMapHost* host);
