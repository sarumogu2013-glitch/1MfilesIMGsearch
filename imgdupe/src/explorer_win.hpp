// エクスプローラーの画面（mgview の browser.cpp「ファイルエクスプローラーモード」から移植。Windows 専用）。アプリの基本の画面。
// 上に戻る・進む・上へ・アドレスバー・表示・並び順、その下に取り置いたタグ、左にフォルダ一覧、真ん中にサムネイル（か詳細）、
// 右にプレビュー（画像の情報とタグ。タグをクリックで検索、ドラッグで取り置き）、下にステータスバー。同じ画像・重複の結果も真ん中の一覧に出す。
// サムネイルの右下の印で、インデックス（DB への登録）の状態を出す。
// 右クリックの「同じ画像を探す」・画像や URL のドロップ・アドレスバーの URL で、同じ画像を探してもらう（ホストが探して SetSame で返す。一覧に出す）。
// 検索の結果は ShowList で真ん中に出す（「戻る」で元のフォルダへ）。
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <memory>
#include <functional>
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
  int previewMode = 1;      // 右のプレビュー: 0 = 画像だけ、1 = タグ、2 = プロパティ
  bool sortDesc = false;
  std::vector<int> detailCols;  // 詳細表示の列の幅（96 dpi の px。名前・更新日時・種類・サイズ・インデックス。0 か無ければ既定）
  std::vector<std::wstring> tray;  // 取り置いたタグ
  std::vector<std::pair<std::wstring, int>> frequent;  // よく行く場所（パスと開いた回数。起動した時に中身を先読みする）
};

// インデックスの進み具合（詳細表示の「インデックス」の列）
struct ExplorerProgress {
  std::wstring name;  // フォルダの直下の名前
  double ratio = 0;   // 0〜1
  std::wstring text;  // 「[55%] 1,234 枚」など
};

// 画像のタグ（プレビューに出す）
struct ExplorerTag {
  std::wstring tag, kind;
  std::wstring label;  // 画面に出す名前（日本語の表示名など。空ならタグそのもの）
  double confidence = 0;
  bool file = false;  // ファイルに書き込まれているタグ（実体のタグ）
};

// 重複のまとまり（下の一覧に出す）
struct ExplorerGroup {
  std::wstring title;
  std::vector<std::wstring> paths;
};

// インデックスの状態の印 (sameimg.hpp の IndexState と同じ値。-1 = 印を出さない)
enum ExplorerState { kStateNoBadge = -1, kStateNone = 0, kStatePending = 1, kStateIndexed = 2, kStatePartial = 3,
                     kStateOffline = 4 };  // 外してある（インデックスはあるが、調べるフォルダの外・つながっていない。結果に出ない）

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
  // フォルダ・画像をスキャンタスクに足す（いくつも選んだ時は 1 つのタスクにまとめる）。top なら一番上（最優先）
  virtual void ExplorerAddScanTask(const std::vector<std::wstring>& paths, bool top) = 0;
  virtual bool ExplorerIsRoot(const std::wstring& dir) = 0;      // 調べるフォルダ（インデックスの対象）そのものか
  virtual void ExplorerRemoveRoot(const std::wstring& dir) = 0;  // 調べるフォルダから外す（確かめてから）
  virtual void ExplorerSettingsChanged(const ExplorerSettings& s) = 0;
  // dir の中の items（名前・フォルダか）の状態（ExplorerState）
  virtual std::vector<int> ExplorerStates(const std::wstring& dir, const std::vector<std::pair<std::wstring, bool>>& items) = 0;
  // 詳細表示で、フォルダ dir の直下の進み具合を求める（裏で数えて Explorer::SetProgress で返す）
  virtual void ExplorerRequestProgress(const std::wstring& dir) { (void)dir; }
  virtual std::vector<ExplorerTag> ExplorerTags(const std::wstring& path) = 0;      // 画像のタグ（確度の高い順）
  virtual void ExplorerSearchTags(const std::vector<std::wstring>& tags) = 0;       // タグ（すべてを持つ画像）で検索する
  virtual void ExplorerFindSimilarAi(const std::wstring& path) = 0;                 // 似た画像（CLIP）を探す
  virtual void ExplorerRelookup(const std::wstring& path) = 0;                      // 出典を調べ直す
  virtual std::wstring ExplorerTagLabel(const std::wstring& tag) = 0;               // タグの表示名（取り置きに出す）
  virtual void ExplorerRenameTag(const std::wstring& tag) = 0;                      // タグの表示名を変える
  virtual void ExplorerRemoveTag(const std::wstring& path, const std::wstring& tag) = 0;  // この画像からタグを外す（手動で「違う」）
  virtual void ExplorerAddTag(const std::vector<std::wstring>& paths, const std::wstring& tag) = 0;  // 画像にタグを付ける（手動で「正しい」）
  virtual void ExplorerDropTag(const std::wstring& tag, POINT screen) = 0;  // 取り置いたタグをエクスプローラーの外に落とした
  virtual std::vector<std::pair<std::wstring, std::wstring>> ExplorerProperties(const std::wstring& path) = 0;  // 画像のプロパティ（名前と値）
  virtual void ExplorerSaveSelection() = 0;                                         // 選んだ画像をスマートフォルダに保存する
  virtual void ExplorerTagSelection() = 0;                                          // 選んだ画像にタグを付ける・外す
  virtual void ExplorerTeachFolder(const std::wstring& dir) = 0;                   // フォルダ名から作者などを読み取る形を教える
};

// ファイルか URL（ブラウザーから）のドロップを受け取る IDropTarget（RegisterDragDrop に渡す。参照は 1 で返す）
IDropTarget* MakeFileDropTarget(std::function<void(const std::vector<std::wstring>& files, const std::wstring& url)> onDrop);

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
  HWND ViewBar() const;            // 表示・並び順・大きさ・表紙・パネルの切り替え（ホストのリボンに置く。親はホストの窓）
  SIZE ViewBarSize() const;
  std::wstring Dir() const;
  std::wstring Selected() const;                    // 今の項目（プレビューに出しているもの）
  std::vector<std::wstring> SelectedPaths() const;  // 選んでいるもの全部（Ctrl・Shift・枠で囲んで、いくつも選べる）
  bool ShowingList() const;                         // 検索の結果を出している
  void RefreshStates();  // インデックスの状態を読み直す
  void Refresh();        // 更新: 中身の控えとサムネイルを捨てて、今の場所を読み直す
  std::vector<std::pair<std::wstring, int>> Frequent() const;  // よく行く場所（多い順。終わる時に保存する）
  void PreviewFile(const std::wstring& path);  // 一覧に無い画像をプレビューに出す（一覧で選び直すと戻る）
  void RefreshTags();    // タグの表示名が変わった
  // 真ん中に画像の一覧（検索の結果）を出す。「戻る」で前のフォルダへ。
  // entries: 1 つずつの項目。2 枚以上の束は「まとまり」（重なった絵で出し、開くと中の画像の一覧）。
  // select: 選ぶもの（entries の番号。-1 なら先頭）
  void ShowList(const std::wstring& title, std::vector<std::vector<std::wstring>> entries, int select = -1);
  // 下のステータスバーの文
  void SetSameStatus(const std::wstring& text);
  void SetProgress(const std::wstring& dir, const std::vector<ExplorerProgress>& progress);  // 詳細表示の進み具合
  // 同じ画像（全部の調べたフォルダから）を真ん中の一覧に出す。original があれば先頭に置いて選ぶ
  void SetSame(const std::wstring& title, std::vector<ExplorerMatch> matches, const std::wstring& original = L"");
  // 重複のまとまりを真ん中の一覧に出す（まとまりは重なった絵）
  void SetGroups(const std::wstring& title, std::vector<ExplorerGroup> groups);
  void AddTrayTag(const std::wstring& tag);

 private:
  std::unique_ptr<ExplorerImpl> impl_;
};
