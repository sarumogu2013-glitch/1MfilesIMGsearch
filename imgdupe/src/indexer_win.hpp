// 常駐する indexer（imgdupe.exe --indexer）。タスクトレイに常駐し、フォルダの変化を見て段階を裏で進める。
// GUI とは直接やりとりせず、DB の settings（"roots": 調べるフォルダ）と commands テーブルで指示を受ける。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "drivestore.hpp"

namespace indexer {

// 常駐を始める（すでに動いていれば何もせず 0）。dataDir: DB とパックの場所。
// waitForOld: 古い版が終わるのを少し待つ（アップデートの後に起動し直す時）
int runResident(HINSTANCE inst, const std::wstring& dataDir, bool waitForOld = false);

// 動いているか（同じユーザーの別のプロセスを含む）
bool isRunning();

// GUI から: 起動する（--indexer で自分を起動）。restart: 動いている古い版が終わるのを待ってから始める
bool start(bool restart = false);
// GUI から: 終わるよう頼む（commands に "quit" を入れる）
void requestQuit(const std::wstring& dataDir);
// GUI から: 今すぐ調べ直すよう頼む
void requestRescan(const std::wstring& dataDir);
// フォルダ名からの推し量りだけやり直す（教えた形を変えた時）
void requestInfer(const std::wstring& dataDir);
// DB を詰めて小さくする（バックグラウンドの解析が、手が空いた時に）
void requestCompact(const std::wstring& dataDir);
// GUI から: 調べるフォルダを DB に書く（indexer はこれを見る）
void setRoots(const std::wstring& dataDir, const std::vector<std::wstring>& roots);
// GUI から: スキャンタスクを足す（paths はフォルダかファイル。top なら一番上 = 今のタスクを止めてでも先に）。
// label は一覧に出す名前。indexer が動いていなければ呼び出し側が起動する
void addScanTask(const std::wstring& dataDir, const std::wstring& label, const std::vector<std::wstring>& paths, bool top);
// GUI から: タスクトレイの状況の窓を「タスク」の表示で出してもらう
void requestShowTasks(const std::wstring& dataDir);

// ドライブの正本（データ保護の実装仕様 3）: 調べるフォルダのあるドライブのうち、正本を置けるもの
// （システムのドライブ・ネットワーク・CD などと、直下に書けないドライブは除く。<ドライブ>:\.imgdupe を作って隠す）
std::vector<imgdupe::DriveRef> masterDrives(const std::vector<std::wstring>& roots);
// つながっているドライブの正本と合わせ（無ければ作る・クローンが無ければ読み戻す・食い違いの印）、writeback なら書き戻す。
// 解析と画面が同時にしないよう、名前付きのミューテックスで順番にする。食い違っているドライブの数を返す
int syncDrives(const std::wstring& dataDir, const std::vector<std::wstring>& roots, bool writeback, const std::atomic<bool>* cancel);

// 処理の負荷（imgdupe.ini の indexerLoad）。GUI の設定の画面とタスクトレイのメニューで変える
enum LoadLevel : int {
    kLoadLight = 0,   // 控えめ（既定）: 操作中は 1 スレッド、バックグラウンドの優先度、全画面のアプリ中・バッテリーでは止める
    kLoadMedium = 1,  // そこそこ: 操作中もコアの半分、優先度は「通常以下」。全画面のアプリ中・バッテリーでは止める
    kLoadHeavy = 2,   // しっかり: いつも全部のコア、優先度は「通常」（ゲームと同じくらい）。全画面のアプリ中・バッテリーでも止めない
};
int loadLevel(const std::wstring& dataDir);
void setLoadLevel(const std::wstring& dataDir, int level);
const wchar_t* loadLevelName(int level);

// Windows と一緒に起動する（HKCU の Run キー。管理者権限は要らない）
bool autoStartEnabled();
void setAutoStart(bool on);

}  // namespace indexer
