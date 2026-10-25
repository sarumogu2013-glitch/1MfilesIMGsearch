// エクスプローラーの画面（mgview の browser.cpp「ファイルエクスプローラーモード」から移植。Windows 専用）。
// 上に戻る・進む・上へ・アドレスバー、左にフォルダ一覧、真ん中にサムネイル、右にプレビュー、下に「同じ画像」の一覧。
// サムネイルの右下の印で、インデックス（DB への登録）の状態を出す。
// 画像を選ぶ・画像や URL をドロップする・アドレスバーに画像の URL を打つと、その画像と同じ画像を探してもらう（ホストが探して SetSame で返す）。
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
  int resultsHeight = 200;  // 下の「同じ画像」の高さ
  int resultsView = 0;      // 0 = 画像ごと、1 = フォルダごと
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
  // 「同じ画像」の一覧
  void SetSameStatus(const std::wstring& text);
  void SetSame(const std::wstring& title, std::vector<ExplorerMatch> matches);

 private:
  std::unique_ptr<ExplorerImpl> impl_;
};
