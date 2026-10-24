// 常駐する indexer（imgdupe.exe --indexer）。タスクトレイに常駐し、フォルダの変化を見て段階を裏で進める。
// GUI とは直接やりとりせず、DB の settings（"roots": 調べるフォルダ）と commands テーブルで指示を受ける。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace indexer {

// 常駐を始める（すでに動いていれば何もせず 0）。dataDir: DB とパックの場所
int runResident(HINSTANCE inst, const std::wstring& dataDir);

// 動いているか（同じユーザーの別のプロセスを含む）
bool isRunning();

// GUI から: 起動する（--indexer で自分を起動）
bool start();
// GUI から: 終わるよう頼む（commands に "quit" を入れる）
void requestQuit(const std::wstring& dataDir);
// GUI から: 今すぐ調べ直すよう頼む
void requestRescan(const std::wstring& dataDir);
// GUI から: 調べるフォルダを DB に書く（indexer はこれを見る）
void setRoots(const std::wstring& dataDir, const std::vector<std::wstring>& roots);

// Windows と一緒に起動する（HKCU の Run キー。管理者権限は要らない）
bool autoStartEnabled();
void setAutoStart(bool on);

}  // namespace indexer
