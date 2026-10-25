// エクスプローラーの画面（mgview の browser.cpp「ファイルエクスプローラーモード」から移植。Windows 専用）。アプリの基本の画面。
// 上に戻る・進む・上へ・アドレスバー・表示・並び順、その下に取り置いたタグ、左にフォルダ一覧、真ん中にサムネイル（か詳細）、
// 右にプレビュー（画像の情報とタグ。タグをクリックで検索、ドラッグで取り置き）、下に結果の一覧（同じ画像・重複）。
// サムネイルの右下の印で、インデックス（DB への登録）の状態を出す。
// 右クリックの「同じ画像を探す」・画像や URL のドロップ・アドレスバーの URL で、同じ画像を探してもらう（ホストが探して SetSame で返す）。
// 検索の結果は ShowList で真ん中に出す（「戻る」で元のフォルダへ）。
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ExplorerSettings {
  int sizeMode = 1;      // 0 = 小、1 = 普通、2 = 大、3 = 可変
  int customSize = 200;  // 可変の時の大きさ (px、48-512)
  // フォルダのサムネイル: 0 = 1 枚目、1 = 1・2 枚目、2 = 最後と 1 枚目、3 = 最後、4 = 最後の一つ前と最後
  int folderStyle = 0;
  bool showTree = true;     // 左のフォルダ一覧
  bool showPreview = true;  // 右のプレビュー
  int treeWidth = 220;      // 幅 (96 dpi の px)
  int previewWidth = 300;
  int resultsHeight = 200;  // 下の結果の一覧の高さ
  int resultsView = 0;      // 同じ画像の一覧: 0 = 画像ごと、1 = フォルダごと
  int viewMode = 0;         // 0 = サムネイル、1 = 詳細（Windows のエクスプローラーの「詳細」と同じ）
  int sortKey = 0;          // 0 = 名前、1 = 更新日時、2 = 種類、3 = サイズ
  bool sortDesc = false;
  std::vector<std::wstring> tray;  // 取り置いたタグ
};

// 画像のタグ（プレビューに出す）
struct ExplorerTag {
  std::wstring tag, kind;
  double confidence = 0;
  bool file = false;  // ファイルに書き込まれているタグ（実体のタグ）
};

// 重複のまとまり（下の一覧に出す）
struct ExplorerGroup {
  std::wstring title;
  std::vector<std::wstring> paths;
};

// インデックスの状態の印 (sameimg.hpp の IndexState と同じ値。-1 = 印を出さない)
enum ExplorerState { kStateNoBadge = -1, kStateNone = 0, kStatePending = 1, kStateIndexed = 2, kStatePartial = 3 };

struct ExplorerMatch {
  std::wstring path;
  bool exact = false;
  int distance = 0;
  uint64_t size = 0;
  int width = 0, height = 0;
};

class ExplorerHost {
 public:
  virtual ~ExplorerHost() = default;
  virtual void ExplorerOpen(const std::wstring& path) = 0;                     // 画像・ファイルを開く
  virtual void ExplorerSelected(const std::wstring& path, bool image) = 0;     // 選んでいるものが変わった
  virtual void ExplorerFindSameFile(const std::wstring& path) = 0;             // この画像と同じ画像を探す（ドロップなど）
  virtual void ExplorerFindSameUrl(const std::wstring& url) = 0;               // URL の画像と同じ画像を探す
  virtual void ExplorerAddRoot(const std::wstring& dir) = 0;                   // 調べるフォルダに足す
  virtual void ExplorerSettingsChanged(const ExplorerSettings& s) = 0;
  // dir の中の items（名前・フォルダか）の状態（ExplorerState）
  virtual std::vector<int> ExplorerStates(const std::wstring& dir, const std::vector<std::pair<std::wstring, bool>>& items) = 0;
  virtual std::vector<ExplorerTag> ExplorerTags(const std::wstring& path) = 0;      // 画像のタグ（確度の高い順）
  virtual void ExplorerSearchTags(const std::vector<std::wstring>& tags) = 0;       // タグ（すべてを持つ画像）で検索する
  virtual void ExplorerFindSimilarAi(const std::wstring& path) = 0;                 // 似た画像（CLIP）を探す
  virtual void ExplorerRelookup(const std::wstring& path) = 0;                      // 出典を調べ直す
};

class ExplorerImpl;

class Explorer {
 public:
  Explorer();
  ~Explorer();
  bool Create(HWND parent, ExplorerHost* host, const ExplorerSettings& settings);
  void Show(const std::wstring& dir, const std::wstring& select);  // dir を開く。select があればそれを選ぶ
  void Hide();
  bool Visible() const;
  void Move(const RECT& r);
  HWND Window() const;
  std::wstring Dir() const;
  std::wstring Selected() const;
  void RefreshStates();  // インデックスの状態を読み直す
  // 真ん中に画像の一覧（検索の結果）を出す。「戻る」で前のフォルダへ
  // select: 選ぶもの（paths の番号。-1 なら先頭）
  void ShowList(const std::wstring& title, std::vector<std::wstring> paths, int select = -1);
  // 下の結果の一覧
  void SetSameStatus(const std::wstring& text);
  void SetSame(const std::wstring& title, std::vector<ExplorerMatch> matches);
  void SetGroups(const std::wstring& title, std::vector<ExplorerGroup> groups);
  void AddTrayTag(const std::wstring& tag);

 private:
  std::unique_ptr<ExplorerImpl> impl_;
};
