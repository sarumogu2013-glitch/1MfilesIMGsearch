// エクスプローラーの画面（mgview の browser.cpp から移植。書き方も mgview に合わせてある）。
// 上に操作のバー (戻る・進む・上へ・アドレスバー・サムネイルの大きさ・フォルダの表紙の出し方・パネルの開け閉め)、
// その下は左からフォルダ一覧 (ツリー)、サムネイルの一覧、プレビュー、いちばん下に「同じ画像」の一覧。境目はドラッグで動かせる。
// サムネイルは裏のスレッドで WIC で読み、プレビュー、見えているものの順に出す。
// mgview との違い: PDF は扱わない。サムネイルの右下にインデックスの状態の印。画像・URL のドロップ、アドレスバーの URL、
// Ctrl+V で「同じ画像を探す」。右クリックのメニュー。

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "explorer_win.hpp"

#include <commctrl.h>
#include <commoncontrols.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "core.hpp"
#include "image_win.hpp"
#include "win_util.hpp"

namespace {

constexpr wchar_t kHostClass[] = L"imgdupe_explorer";
constexpr wchar_t kGridClass[] = L"imgdupe_explorer_grid";
constexpr wchar_t kPreviewClass[] = L"imgdupe_explorer_preview";
constexpr wchar_t kTrayClass[] = L"imgdupe_explorer_tray";
constexpr wchar_t kViewBarClass[] = L"imgdupe_explorer_viewbar";
const wchar_t kListMarker[] = L"\x01list";  // 戻るの履歴で「検索の結果」を表す
constexpr UINT WM_THUMB_DONE = WM_APP + 41;  // lParam = ThumbResult*
constexpr UINT_PTR kStateTimer = 1;
enum {
  ID_UP = 1, ID_PATH, ID_SIZE, ID_SLIDER, ID_FOLDER, ID_SIZE_LABEL, ID_FOLDER_LABEL,
  ID_BACK, ID_FORWARD, ID_TREE_BTN, ID_PREVIEW_BTN, ID_TREE, ID_SAME_TEXT, ID_SAME_VIEW, ID_SAME_LIST,
  ID_VIEW, ID_SORT, ID_SORT_DIR
};
enum { MENU_OPEN = 1, MENU_SHOW, MENU_COPY, MENU_SAME, MENU_ADD_ROOT, MENU_TAG_SEARCH, MENU_TAG_TRAY, MENU_TAG_COPY, MENU_TAG_RENAME, MENU_SAVE_SMART, MENU_BULK_TAG, MENU_SIMILAR_AI,
       MENU_RELOOKUP, MENU_SCAN_TOP, MENU_REMOVE_ROOT, MENU_TAG_REMOVE };
constexpr int kListOrder = 4;  // 並び順の「検索結果の順」

const int kPresetSizes[3] = {96, 160, 256};
// IImageList の IID (mingw のライブラリには無いのでここで定義する)
const GUID kIID_IImageList = {0x46EB5926, 0x582E, 0x4017, {0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50}};
constexpr int kMinSize = 48, kMaxSize = 512;

bool IsSupportedImage(const std::wstring& name) { return imgdupe::isImageExt(winutil::narrow(name)); }

bool IsUrl(const std::wstring& s) { return _wcsnicmp(s.c_str(), L"http://", 7) == 0 || _wcsnicmp(s.c_str(), L"https://", 8) == 0; }

struct Item {
  std::wstring name, path;
  bool dir = false;
  bool image = false;
  int state = kStateNoBadge;  // インデックスの状態
  uint64_t mtime = 0, size = 0;  // 更新日時 (FILETIME)・大きさ
  size_t order = 0;              // 読んだ順 (検索の結果は関連度などの順)
  bool picked = false;           // 複数選んでいる時に選ばれている (並べ替えても付いていく)
  std::vector<std::wstring> members;  // 検索の結果のまとまり (重複・差分)。2 つ以上ならまとまりの項目 (path は先頭)
  int entry = -1;                     // 検索の結果の何番目のまとまりか
  bool Stack() const { return members.size() > 1; }
  bool Thumbable() const { return dir || image; }
};

// 読み込んだサムネイル (フォルダは 2 枚のこともある)
struct ThumbPic {
  HBITMAP bmp = nullptr;
  int w = 0, h = 0;
};
struct Thumb {
  int bucket = 0;    // 読んだ大きさ
  int style = 0;     // フォルダのサムネイルの出し方 (読んだ時)
  bool none = false;  // 画像が見つからなかった (フォルダ) / 読めなかった
  int fullW = 0, fullH = 0;  // 元の大きさ
  std::vector<ThumbPic> pics;
  size_t bytes() const {
    size_t b = 0;
    for (auto& p : pics) b += size_t(p.w) * p.h * 4;
    return b;
  }
  void Free() {
    for (auto& p : pics) {
      if (p.bmp) DeleteObject(p.bmp);
    }
    pics.clear();
  }
};

struct ThumbJob {
  std::wstring path;
  bool dir = false;
  int bucket = 0;
  int style = 0;
  unsigned gen = 0;
  bool preview = false;  // プレビュー用 (大きい)
};

// 読んだ画素 (32bpp PBGRA、向きは直してある)
struct Pixels {
  int w = 0, h = 0;
  std::vector<uint8_t> px;
};

struct ThumbResult {
  ThumbJob job;
  bool none = false;
  int fullW = 0, fullH = 0;
  std::vector<Pixels> images;
};

bool IsHidden(const WIN32_FIND_DATAW& fd) {
  return (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0 || wcscmp(fd.cFileName, L".") == 0 ||
         wcscmp(fd.cFileName, L"..") == 0;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
  if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) return dir + name;
  return dir + L"\\" + name;
}

std::wstring ParentDir(const std::wstring& dir) {
  std::wstring d = dir;
  while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
  size_t pos = d.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return std::wstring();
  if (pos == 2 && d.size() > 3 && d[1] == L':') return d.substr(0, 3);  // C:\ まで
  if (pos < 2) return std::wstring();
  return d.substr(0, pos);
}

bool NaturalLess(const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; }

// フォルダの中を名前順に (サブフォルダ・画像ファイル)
void ListDir(const std::wstring& dir, std::vector<std::wstring>* subdirs, std::vector<std::wstring>* images) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW(Join(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                              FIND_FIRST_EX_LARGE_FETCH);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (IsHidden(fd)) continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (subdirs && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) subdirs->push_back(fd.cFileName);
    } else if (images && IsSupportedImage(fd.cFileName)) {
      images->push_back(fd.cFileName);
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  if (subdirs) std::sort(subdirs->begin(), subdirs->end(), NaturalLess);
  if (images) std::sort(images->begin(), images->end(), NaturalLess);
}

// フォルダのサムネイルにする画像: そのフォルダの画像。無ければ下の階層を名前順に探して最初に見つかったフォルダのもの
bool FolderImages(const std::wstring& dir, int depth, std::vector<std::wstring>* out, int* budget) {
  if (depth > 8 || --*budget < 0) return false;
  std::vector<std::wstring> subdirs, images;
  ListDir(dir, &subdirs, &images);
  if (!images.empty()) {
    for (auto& i : images) out->push_back(Join(dir, i));
    return true;
  }
  for (auto& s : subdirs) {
    if (FolderImages(Join(dir, s), depth + 1, out, budget)) return true;
  }
  return false;
}

HBITMAP ToDib(const Pixels& img) {
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = img.w;
  bi.bmiHeader.biHeight = -img.h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bmp && bits) memcpy(bits, img.px.data(), size_t(img.w) * img.h * 4);
  return bmp;
}

// 読む大きさは段階に丸める (大きさを少し変えるたびに読み直さない)
int BucketFor(int size) {
  static const int kBuckets[] = {64, 96, 128, 160, 192, 256, 320, 384, 512};
  for (int b : kBuckets) {
    if (b >= size) return b;
  }
  return 512;
}

std::wstring FolderOf(const std::wstring& path) {
  size_t sep = path.find_last_of(L"\\/");
  return sep == std::wstring::npos ? std::wstring() : path.substr(0, sep);
}

std::wstring NameOf(const std::wstring& path) {
  size_t sep = path.find_last_of(L"\\/");
  return sep == std::wstring::npos ? path : path.substr(sep + 1);
}

std::wstring FormatBytes(ULONGLONG b) {
  wchar_t buf[64];
  if (b < 1024) swprintf(buf, 64, L"%llu バイト", b);
  else if (b < (1ull << 20)) swprintf(buf, 64, L"%.1f KB", b / 1024.0);
  else if (b < (1ull << 30)) swprintf(buf, 64, L"%.1f MB", b / 1048576.0);
  else swprintf(buf, 64, L"%.2f GB", b / 1073741824.0);
  return buf;
}

bool g_draggingOut = false;  // 自分からファイルをドラッグしている (自分の上に落としても受け取らない)

// 外へドラッグするファイル (CF_HDROP だけを渡す)
class FileDataObject final : public IDataObject {
 public:
  explicit FileDataObject(const std::vector<std::wstring>& paths) {
    // DROPFILES の後ろに、0 で区切ったパスを並べ、最後にもう 1 つ 0
    size_t chars = 1;
    for (const auto& p : paths) chars += p.size() + 1;
    hdrop_ = GlobalAlloc(GHND, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    if (auto* df = static_cast<DROPFILES*>(GlobalLock(hdrop_))) {
      df->pFiles = sizeof(DROPFILES);
      df->fWide = TRUE;
      auto* out = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
      for (const auto& p : paths) {
        memcpy(out, p.c_str(), (p.size() + 1) * sizeof(wchar_t));
        out += p.size() + 1;
      }
      *out = 0;
      GlobalUnlock(hdrop_);
    }
  }
  ~FileDataObject() {
    if (hdrop_) GlobalFree(hdrop_);
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
      *ppv = static_cast<IDataObject*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&ref_)); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG r = InterlockedDecrement(&ref_);
    if (!r) delete this;
    return ULONG(r);
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fe, STGMEDIUM* sm) override {
    if (QueryGetData(fe) != S_OK) return DV_E_FORMATETC;
    // 受け取る側が解放するので複製を渡す
    SIZE_T n = GlobalSize(hdrop_);
    HGLOBAL copy = GlobalAlloc(GHND, n);
    if (!copy) return E_OUTOFMEMORY;
    void* dst = GlobalLock(copy);
    void* src = GlobalLock(hdrop_);
    memcpy(dst, src, n);
    GlobalUnlock(hdrop_);
    GlobalUnlock(copy);
    sm->tymed = TYMED_HGLOBAL;
    sm->hGlobal = copy;
    sm->pUnkForRelease = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override {
    return fe && fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL) && fe->dwAspect == DVASPECT_CONTENT ? S_OK : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
    if (out) out->ptd = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override {
    if (dir != DATADIR_GET) return E_NOTIMPL;
    FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return SHCreateStdEnumFmtEtc(1, &fe, out);
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

 private:
  LONG ref_ = 1;
  HGLOBAL hdrop_ = nullptr;
};

class DropSource final : public IDropSource {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IUnknown || riid == IID_IDropSource) {
      *ppv = static_cast<IDropSource*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&ref_)); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG r = InterlockedDecrement(&ref_);
    if (!r) delete this;
    return ULONG(r);
  }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
    if (escape) return DRAGDROP_S_CANCEL;
    if (!(keys & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

 private:
  LONG ref_ = 1;
};

// ドロップされたもの (ファイルか URL) を受け取る
class DropTarget final : public IDropTarget {
 public:
  explicit DropTarget(std::function<void(const std::vector<std::wstring>&, const std::wstring&)> onDrop) : onDrop_(std::move(onDrop)) {}
  virtual ~DropTarget() = default;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
      *ppv = static_cast<IDropTarget*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&ref_)); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG r = InterlockedDecrement(&ref_);
    if (!r) delete this;
    return ULONG(r);
  }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
    ok_ = !g_draggingOut && Accepts(data);
    *effect = ok_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
    *effect = ok_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
    std::vector<std::wstring> files;
    std::wstring url;
    FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm = {};
    if (SUCCEEDED(data->GetData(&fe, &sm))) {
      HDROP drop = HDROP(sm.hGlobal);
      UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < n; i++) {
        wchar_t buf[32768];
        if (DragQueryFileW(drop, i, buf, UINT(std::size(buf)))) files.push_back(buf);
      }
      ReleaseStgMedium(&sm);
    }
    if (files.empty()) url = Text(data);
    *effect = files.empty() && url.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
    if (!files.empty() || !url.empty()) onDrop_(files, url);
    return S_OK;
  }

  // ブラウザーからの URL (UniformResourceLocatorW) か、文字
  static std::wstring Text(IDataObject* data) {
    static const CLIPFORMAT urlFormat = CLIPFORMAT(RegisterClipboardFormatW(L"UniformResourceLocatorW"));
    for (CLIPFORMAT cf : {urlFormat, CLIPFORMAT(CF_UNICODETEXT)}) {
      FORMATETC fe = {cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
      STGMEDIUM sm = {};
      if (FAILED(data->GetData(&fe, &sm))) continue;
      std::wstring s;
      if (auto* p = static_cast<const wchar_t*>(GlobalLock(sm.hGlobal))) {
        s.assign(p, wcsnlen(p, GlobalSize(sm.hGlobal) / sizeof(wchar_t)));
        GlobalUnlock(sm.hGlobal);
      }
      ReleaseStgMedium(&sm);
      size_t nl = s.find_first_of(L"\r\n");
      if (nl != std::wstring::npos) s.resize(nl);
      if (IsUrl(s)) return s;
    }
    return std::wstring();
  }

 private:
  static bool Accepts(IDataObject* data) {
    FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (data->QueryGetData(&fe) == S_OK) return true;
    return !Text(data).empty();
  }
  LONG ref_ = 1;
  bool ok_ = false;
  std::function<void(const std::vector<std::wstring>&, const std::wstring&)> onDrop_;
};

}  // namespace

class ExplorerImpl {
 public:
  ExplorerHost* host = nullptr;
  ExplorerSettings settings;
  HWND hwnd = nullptr, grid = nullptr;
  HWND upBtn = nullptr, pathText = nullptr, sizeBox = nullptr, slider = nullptr, folderBox = nullptr;
  HWND sizeLabel = nullptr, folderLabel = nullptr, viewLabel = nullptr, sortLabel = nullptr;
  HWND viewBar = nullptr;  // 表示・並び順・大きさ・表紙・パネルの切り替え（ホームのリボンに置く。親はホストの窓）
  HWND backBtn = nullptr, forwardBtn = nullptr, treeBtn = nullptr, previewBtn = nullptr;
  HWND viewBox = nullptr, sortBox = nullptr, sortDirBtn = nullptr, trayWnd = nullptr;
  HWND tree = nullptr, preview = nullptr;
  HWND sameText = nullptr, sameView = nullptr, sameList = nullptr;
  DropTarget* drop = nullptr;
  // 戻る・進むの履歴 (フォルダと、そこで選んでいたもの)
  std::vector<std::pair<std::wstring, std::wstring>> backStack, forwardStack;
  int dragSplit = 0;  // 境目をドラッグ中: 1 = フォルダ一覧の右、2 = プレビューの左、3 = 「同じ画像」の上
  HFONT font = nullptr, nameFont = nullptr;
  int dpi = 96;
  std::wstring dir;
  std::vector<Item> items;
  int sel = -1, hover = -1;  // sel: 今の項目 (プレビューに出す・キーで動かす)
  bool multi = false;        // 複数選んでいる (Item::picked を使う)。false なら sel の 1 つだけ
  int anchor = -1;           // Shift で範囲を選ぶ時の起点
  // 枠で囲んで選ぶ (空いている所からドラッグ)。位置は中身の座標 (スクロールを足したもの)
  bool banding = false, bandMoved = false;
  POINT bandFrom = {}, bandTo = {};
  std::vector<char> bandBase;  // 囲み始めた時の選び方 (Ctrl を押していれば足す)
  // 項目を押したまま動かすと、外へファイルをドラッグする
  bool dragArmed = false;
  POINT dragFrom = {};
  int collapseOnUp = -1;  // 選んでいるものの上を押した: ドラッグしなければ、離した時にその 1 つだけにする

  void StartDragOut() {
    std::vector<std::wstring> paths;
    for (auto& p : SelectedPaths())
      if (!p.empty()) paths.push_back(p);
    if (paths.empty()) return;
    auto* data = new FileDataObject(paths);
    auto* source = new DropSource();
    DWORD effect = DROPEFFECT_NONE;
    g_draggingOut = true;
    // コピーかショートカットだけ (移動はさせない。DB のパスがずれないように)
    DoDragDrop(data, source, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
    g_draggingOut = false;
    data->Release();
    source->Release();
  }
  int scrollY = 0;
  bool tracking = false;

  // 「同じ画像」
  std::wstring sameTitle;
  std::vector<ExplorerMatch> same;
  struct SameFolderRow {
    std::wstring folder;
    int exact = 0, similar = 0;
    std::wstring first;  // 最初の一致
  };
  std::vector<SameFolderRow> sameFolders;
  // 重複のまとまり (下の一覧の行: 見出しか、パス)
  int resultsMode = 0;  // 0 = 同じ画像、1 = 重複
  struct GroupRow {
    bool header = false;
    std::wstring text, path;
  };
  std::vector<GroupRow> groupRows;
  bool fillingList = false;

  // サムネイル
  std::map<std::wstring, Thumb> cache;
  std::list<std::wstring> lru;  // 古い順
  size_t cacheBytes = 0;
  std::map<std::wstring, HICON> icons;  // 拡張子 -> アイコン
  std::mutex mu;
  std::condition_variable cv;
  std::deque<ThumbJob> queue;
  std::set<std::wstring> inflight;
  std::vector<std::thread> workers;
  bool quit = false;
  std::atomic<unsigned> gen{1};

  ~ExplorerImpl() {
    {
      std::lock_guard<std::mutex> lk(mu);
      quit = true;
      queue.clear();
    }
    cv.notify_all();
    for (auto& t : workers) t.join();
    for (auto& kv : cache) kv.second.Free();
    previewThumb.Free();
    for (auto& kv : icons) {
      if (kv.second) DestroyIcon(kv.second);
    }
    if (font) DeleteObject(font);
    if (nameFont) DeleteObject(nameFont);
    for (HBITMAP b : dots) {
      if (b) DeleteObject(b);
    }
  }

  int Px(int v) const { return MulDiv(v, dpi, 96); }
  int ThumbSize() const {
    return settings.sizeMode >= 0 && settings.sizeMode < 3 ? Px(kPresetSizes[settings.sizeMode])
                                                           : Px(std::max(kMinSize, std::min(kMaxSize, settings.customSize)));
  }
  int Pad() const { return Px(8); }
  int TextH() const { return Px(34); }  // 名前 2 行
  // 詳細表示 (1 行に 1 つ、上に列の見出し)
  bool Details() const { return settings.viewMode == 1; }
  int HeaderH() const { return Details() ? Px(24) : 0; }
  int GridW() const {
    RECT rc;
    GetClientRect(grid, &rc);
    return rc.right;
  }
  int CellW() const { return Details() ? GridW() : ThumbSize() + Pad() * 2; }
  int CellH() const { return Details() ? Px(24) : ThumbSize() + Pad() * 2 + TextH(); }
  int Cols() const {
    if (Details()) return 1;
    return std::max(1, int((GridW() - Pad()) / CellW()));
  }
  int OffsetX() const {
    if (Details()) return 0;
    return std::max(0, int((GridW() - Cols() * CellW()) / 2));
  }
  RECT CellRect(int i) const {
    int c = Cols();
    int top = Details() ? HeaderH() : Pad();
    int x = OffsetX() + (i % c) * CellW(), y = top + (i / c) * CellH() - scrollY;
    return RECT{x, y, x + CellW(), y + CellH()};
  }
  int TotalHeight() const {
    int rows = (int(items.size()) + Cols() - 1) / Cols();
    return (Details() ? HeaderH() : Pad()) + Pad() + rows * CellH();
  }
  int HitTest(POINT pt) const {
    if (pt.y < HeaderH()) return -1;
    for (int i = 0; i < int(items.size()); i++) {
      RECT r = CellRect(i);
      if (PtInRect(&r, pt)) return i;
    }
    return -1;
  }

  // ---------------------------------------------------------------- 一覧

  // 並べ替え (フォルダを先に)。一覧 (検索の結果) で「検索結果の順」なら読んだ順
  int SortKey() const { return listMode ? listSort : settings.sortKey; }
  void SortItems() {
    int key = SortKey();
    bool desc = settings.sortDesc;
    std::stable_sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
      if (a.dir != b.dir) return a.dir;
      int c = 0;
      switch (key) {
        case 1: c = a.mtime < b.mtime ? -1 : a.mtime > b.mtime ? 1 : 0; break;
        case 2: c = _wcsicmp(TypeName(a).c_str(), TypeName(b).c_str()); break;
        case 3: c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
        case kListOrder: c = a.order < b.order ? -1 : a.order > b.order ? 1 : 0; break;
      }
      if (c == 0 && key != kListOrder) c = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
      return desc ? c > 0 : c < 0;
    });
  }

  // 種類 (Windows のエクスプローラーと同じ「JPEG ファイル」など)
  std::map<std::wstring, std::wstring> typeNames;
  const std::wstring& TypeName(const Item& it) {
    std::wstring ext = it.dir ? L"<dir>" : PathFindExtensionW(it.name.c_str());
    CharLowerBuffW(&ext[0], DWORD(ext.size()));
    auto f = typeNames.find(ext);
    if (f != typeNames.end()) return f->second;
    SHFILEINFOW fi = {};
    std::wstring name;
    if (SHGetFileInfoW(it.dir ? L"folder" : (L"x" + ext).c_str(), it.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &fi,
                       sizeof(fi), SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
      name = fi.szTypeName;
    if (name.empty()) name = it.dir ? L"ファイル フォルダー" : ext.size() > 1 ? ext.substr(1) + L" ファイル" : L"ファイル";
    return typeNames[ext] = name;
  }

  void SetSort(int key, bool desc) {
    if (listMode) {
      listSort = key;
    } else {
      if (key == kListOrder) key = 0;  // フォルダでは「検索結果の順」は名前
      settings.sortKey = key;
    }
    settings.sortDesc = desc;
    host->ExplorerSettingsChanged(settings);
    UpdateSortBox();
    Resort();
  }

  void UpdateSortBox() {
    if (!sortBox) return;
    SendMessageW(sortBox, CB_SETCURSEL, SortKey(), 0);
    SetWindowTextW(sortDirBtn, settings.sortDesc ? L"降順" : L"昇順");
  }

  void Resort() {
    std::wstring keep = SelectedPath();
    SortItems();
    sel = -1;
    for (int i = 0; i < int(items.size()); i++)
      if (items[size_t(i)].path == keep) sel = i;
    if (sel < 0 && !items.empty()) sel = 0;
    UpdateScroll();
    EnsureVisible(sel);
    InvalidateRect(grid, nullptr, FALSE);
  }

  // ---------------------------------------------------------------- 一覧 (検索の結果)

  bool listMode = false;
  std::wstring listTitle;
  std::vector<std::vector<std::wstring>> listEntries;  // 検索の結果 (まとまりごと。1 枚だけのものも 1 つの束)
  int stackIndex = -1;                                 // まとまりの中を見ている時は、その番号 (-1 = 結果の一覧)
  int listSort = kListOrder;

  void LoadList(const std::wstring& select) {
    listMode = true;
    dir.clear();
    items.clear();
    sel = hover = -1;
    multi = false;
    anchor = -1;
    banding = false;
    scrollY = 0;
    gen++;
    {
      std::lock_guard<std::mutex> lk(mu);
      queue.clear();
    }
    auto addItem = [&](const std::vector<std::wstring>& members, int entry) {
      const std::wstring& p = members.front();
      Item it;
      it.path = p;
      it.name = NameOf(p);
      it.image = true;
      it.order = items.size();
      it.entry = entry;
      if (members.size() > 1) it.members = members;
      WIN32_FILE_ATTRIBUTE_DATA a = {};
      if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) {
        it.mtime = (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
        it.size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
      }
      it.state = kStateIndexed;  // 検索の結果は DB の画像
      items.push_back(std::move(it));
    };
    if (stackIndex >= 0 && stackIndex < int(listEntries.size())) {
      for (const auto& p : listEntries[size_t(stackIndex)]) addItem({p}, stackIndex);
    } else {
      stackIndex = -1;
      for (size_t e = 0; e < listEntries.size(); e++) addItem(listEntries[e], int(e));
    }
    SortItems();
    // 選ぶもの: そのパスの項目。まとまりの中にあれば、そのまとまり
    for (int i = 0; i < int(items.size()) && !select.empty(); i++) {
      const Item& it = items[size_t(i)];
      bool hit = _wcsicmp(it.path.c_str(), select.c_str()) == 0;
      for (const auto& m : it.members) hit = hit || _wcsicmp(m.c_str(), select.c_str()) == 0;
      if (hit) {
        sel = i;
        break;
      }
    }
    if (sel < 0 && !items.empty()) sel = 0;
    std::wstring title = listTitle;
    if (stackIndex >= 0) title += L"　＞　まとまり（" + std::to_wstring(items.size()) + L" 枚）";
    SetWindowTextW(pathText, title.c_str());
    UpdateNavButtons();
    UpdateSortBox();
    UpdateScroll();
    EnsureVisible(sel);
    InvalidateRect(grid, nullptr, FALSE);
    UpdatePreview();
    NotifySelected();
  }

  void ShowList(const std::wstring& title, std::vector<std::vector<std::wstring>> entries, int select = -1) {
    if (!listMode && !dir.empty()) {
      backStack.push_back({dir, SelectedPath()});
      forwardStack.clear();
    } else if (listMode) {
      forwardStack.clear();
    }
    listTitle = title;
    listEntries = std::move(entries);
    stackIndex = -1;
    listSort = kListOrder;
    LoadList(select >= 0 && select < int(listEntries.size()) ? listEntries[size_t(select)].front() : std::wstring());
  }

  // まとまりを開く (中の画像を一覧に出す。「戻る」で結果の一覧へ)
  void OpenStack(int i) {
    const Item& it = items[size_t(i)];
    backStack.push_back({Here(), SelectedPath()});
    forwardStack.clear();
    stackIndex = it.entry;
    LoadList(std::wstring());
  }

  void Load(const std::wstring& d, const std::wstring& select) {
    if (d.compare(0, wcslen(kListMarker), kListMarker) == 0) {
      // 結果の一覧か、その中のまとまり (印の後ろに番号)
      stackIndex = d.size() > wcslen(kListMarker) ? _wtoi(d.c_str() + wcslen(kListMarker)) : -1;
      LoadList(select);
      return;
    }
    if (listMode) {
      listMode = false;
      UpdateSortBox();
    }
    dir = d;
    items.clear();
    sel = hover = -1;
    multi = false;
    anchor = -1;
    banding = false;
    scrollY = 0;
    gen++;
    {
      std::lock_guard<std::mutex> lk(mu);
      queue.clear();
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Join(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (IsHidden(fd)) continue;
        Item it;
        it.name = fd.cFileName;
        it.path = Join(dir, it.name);
        it.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        it.image = !it.dir && IsSupportedImage(it.name);
        it.mtime = (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        it.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        it.order = items.size();
        items.push_back(std::move(it));
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    SortItems();
    for (int i = 0; i < int(items.size()); i++) {
      if (!select.empty() && _wcsicmp(items[i].path.c_str(), select.c_str()) == 0) sel = i;
    }
    if (sel < 0 && !items.empty()) sel = 0;
    LoadStates();
    SetWindowTextW(pathText, dir.c_str());
    SendMessageW(pathText, EM_SETSEL, WPARAM(dir.size()), LPARAM(dir.size()));  // 長い時は末尾 (今のフォルダ名) が見えるように
    UpdateNavButtons();
    UpdateScroll();
    EnsureVisible(sel);
    InvalidateRect(grid, nullptr, FALSE);
    SyncTree(dir);
    UpdatePreview();
    NotifySelected();
  }

  // インデックスの状態を読み直す。変わっていれば描き直して true
  bool LoadStates() {
    if (listMode) return false;
    std::vector<std::pair<std::wstring, bool>> names;
    names.reserve(items.size());
    for (auto& it : items) names.push_back({it.name, it.dir});
    std::vector<int> st = host->ExplorerStates(dir, names);
    bool changed = false;
    for (size_t i = 0; i < items.size(); i++) {
      int s = i < st.size() ? st[i] : kStateNoBadge;
      if (!items[i].dir && !items[i].image) s = kStateNoBadge;
      if (items[i].state != s) changed = true;
      items[i].state = s;
    }
    return changed;
  }

  std::wstring SelectedPath() const { return sel >= 0 && sel < int(items.size()) ? items[size_t(sel)].path : std::wstring(); }

  bool IsPicked(int i) const { return i >= 0 && i < int(items.size()) && (multi ? items[size_t(i)].picked : i == sel); }

  // 選んでいるものの番号 (並んでいる順)
  std::vector<int> PickedIndices() const {
    std::vector<int> out;
    for (int i = 0; i < int(items.size()); i++)
      if (IsPicked(i)) out.push_back(i);
    return out;
  }

  // まとまりは中の画像を全部
  std::vector<std::wstring> SelectedPaths() const {
    std::vector<std::wstring> out;
    for (int i : PickedIndices()) {
      const Item& it = items[size_t(i)];
      if (it.Stack()) out.insert(out.end(), it.members.begin(), it.members.end());
      else out.push_back(it.path);
    }
    return out;
  }

  // 1 つだけ選んでいる形から、複数の形へ (今の項目を選んだまま)
  void BeginMulti() {
    if (multi) return;
    for (auto& it : items) it.picked = false;
    if (sel >= 0 && sel < int(items.size())) items[size_t(sel)].picked = true;
    multi = true;
  }

  // Ctrl: 1 つずつ足す・外す。Shift: 起点から i までを選ぶ (Ctrl も押していれば今の選択に足す)
  void SelectWith(int i, bool ctrl, bool shift) {
    if (items.empty()) return;
    if (!ctrl && !shift) {
      Select(i);
      return;
    }
    i = std::max(0, std::min(i, int(items.size()) - 1));
    int old = sel;
    BeginMulti();
    if (shift) {
      if (anchor < 0 || anchor >= int(items.size())) anchor = sel >= 0 ? sel : i;
      if (!ctrl)
        for (auto& it : items) it.picked = false;
      for (int k = std::min(anchor, i); k <= std::max(anchor, i); k++) items[size_t(k)].picked = true;
    } else {
      items[size_t(i)].picked = !items[size_t(i)].picked;
      anchor = i;
    }
    sel = i;
    EnsureVisible(sel);
    InvalidateRect(grid, nullptr, FALSE);
    UpdatePreview();
    if (preview) InvalidateRect(preview, nullptr, FALSE);  // 選んでいる数
    if (sel != old) NotifySelected();
  }

  void SelectAll() {
    if (items.empty()) return;
    multi = true;
    for (auto& it : items) it.picked = true;
    if (sel < 0) sel = 0;
    InvalidateRect(grid, nullptr, FALSE);
    if (preview) InvalidateRect(preview, nullptr, FALSE);
  }

  // 枠で囲んで選ぶ
  RECT BandRect() const {
    RECT r = {std::min(bandFrom.x, bandTo.x), std::min(bandFrom.y, bandTo.y) - scrollY, std::max(bandFrom.x, bandTo.x),
              std::max(bandFrom.y, bandTo.y) - scrollY};
    return r;
  }

  void UpdateBand(POINT client) {
    RECT rc;
    GetClientRect(grid, &rc);
    // 上下の端の外へ出たら少しずつ送る
    if (client.y < HeaderH()) ScrollTo(scrollY - Px(24));
    else if (client.y > rc.bottom) ScrollTo(scrollY + Px(24));
    bandTo = {client.x, client.y + scrollY};
    if (abs(bandTo.x - bandFrom.x) > Px(4) || abs(bandTo.y - bandFrom.y) > Px(4)) bandMoved = true;
    if (!bandMoved) return;
    multi = true;  // 囲んでいる間も選んだものを見せる
    RECT band = BandRect(), tmp;
    int first = -1;
    for (int i = 0; i < int(items.size()); i++) {
      RECT cell = CellRect(i);
      bool in = IntersectRect(&tmp, &band, &cell) != 0;
      items[size_t(i)].picked = (i < int(bandBase.size()) && bandBase[size_t(i)]) || in;
      if (in && first < 0) first = i;
    }
    if (first >= 0 && !IsPicked(sel)) {
      sel = first;
      anchor = first;
    }
    InvalidateRect(grid, nullptr, FALSE);
  }

  void PaintBand(HDC dc) {
    if (!banding || !bandMoved) return;
    RECT r = BandRect();
    if (r.right <= r.left || r.bottom <= r.top) return;
    // 半透明の青
    HDC src = CreateCompatibleDC(dc);
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), 1, 1, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP one = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (one && bits) {
      *static_cast<uint32_t*>(bits) = 0xFF0078D7;  // 0xAARRGGBB: RGB(0, 120, 215)
      HGDIOBJ old = SelectObject(src, one);
      BLENDFUNCTION bf = {AC_SRC_OVER, 0, 48, 0};
      AlphaBlend(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, src, 0, 0, 1, 1, bf);
      SelectObject(src, old);
    }
    if (one) DeleteObject(one);
    DeleteDC(src);
    Frame(dc, r, RGB(0, 120, 215));
  }

  void NotifySelected() {
    const Item* it = sel >= 0 && sel < int(items.size()) ? &items[size_t(sel)] : nullptr;
    host->ExplorerSelected(it ? it->path : std::wstring(), it && it->image);
  }

  void UpdateNavButtons() {
    EnableWindow(upBtn, listMode || !ParentDir(dir).empty());
    EnableWindow(backBtn, !backStack.empty());
    EnableWindow(forwardBtn, !forwardStack.empty());
  }

  // 別のフォルダへ (戻るの履歴に今の場所を積む)
  std::wstring Here() const {
    if (!listMode) return dir;
    return stackIndex >= 0 ? std::wstring(kListMarker) + std::to_wstring(stackIndex) : std::wstring(kListMarker);
  }

  void Navigate(const std::wstring& d, const std::wstring& select) {
    if (listMode) {
      backStack.push_back({Here(), SelectedPath()});
      if (backStack.size() > 100) backStack.erase(backStack.begin());
      forwardStack.clear();
    } else if (!dir.empty() && _wcsicmp(d.c_str(), dir.c_str()) != 0) {
      backStack.push_back({dir, SelectedPath()});
      if (backStack.size() > 100) backStack.erase(backStack.begin());
      forwardStack.clear();
    }
    if (!listMode && _wcsicmp(d.c_str(), dir.c_str()) == 0 && !select.empty()) {
      // 同じフォルダ: 選び直すだけ
      for (int i = 0; i < int(items.size()); i++) {
        if (_wcsicmp(items[i].path.c_str(), select.c_str()) == 0) {
          Select(i);
          return;
        }
      }
    }
    Load(d, select);
  }

  void Back() {
    if (backStack.empty()) {
      MessageBeep(MB_OK);
      return;
    }
    forwardStack.push_back({Here(), SelectedPath()});
    auto to = backStack.back();
    backStack.pop_back();
    Load(to.first, to.second);
  }

  void Forward() {
    if (forwardStack.empty()) {
      MessageBeep(MB_OK);
      return;
    }
    backStack.push_back({Here(), SelectedPath()});
    auto to = forwardStack.back();
    forwardStack.pop_back();
    Load(to.first, to.second);
  }

  // アドレスバーに打った場所へ。ファイルならそのフォルダを開いて選ぶ。画像の URL なら同じ画像を探す
  void GoToAddress() {
    int len = GetWindowTextLengthW(pathText);
    std::wstring text(size_t(len) + 1, L'\0');
    GetWindowTextW(pathText, &text[0], len + 1);
    text.resize(size_t(len));
    while (!text.empty() && (text.back() == L' ' || text.back() == L'"')) text.pop_back();
    while (!text.empty() && (text.front() == L' ' || text.front() == L'"')) text.erase(text.begin());
    if (IsUrl(text)) {
      SetWindowTextW(pathText, dir.c_str());
      host->ExplorerFindSameUrl(text);
      SetFocus(grid);
      return;
    }
    wchar_t expanded[MAX_PATH * 4];
    if (ExpandEnvironmentStringsW(text.c_str(), expanded, DWORD(std::size(expanded)))) text = expanded;
    if (!text.empty() && PathIsRelativeW(text.c_str()) && !dir.empty()) text = Join(dir, text);
    wchar_t full[MAX_PATH * 4];
    if (!text.empty() && GetFullPathNameW(text.c_str(), DWORD(std::size(full)), full, nullptr)) text = full;
    DWORD attr = text.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(text.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
      MessageBoxW(hwnd, (L"見つかりません。\n" + text).c_str(), L"imgdupe", MB_ICONWARNING);
      SetWindowTextW(pathText, dir.c_str());
      SetFocus(pathText);
      SendMessageW(pathText, EM_SETSEL, 0, -1);
      return;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
      while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
      Navigate(text, std::wstring());
    } else {
      Navigate(ParentDir(text), text);
    }
    SetFocus(grid);
  }

  // ドロップ・貼り付け: 画像なら同じ画像を探す、フォルダならそこを開く、URL なら同じ画像を探す
  void Dropped(const std::vector<std::wstring>& files, const std::wstring& url) {
    for (auto& f : files) {
      DWORD attr = GetFileAttributesW(f.c_str());
      if (attr == INVALID_FILE_ATTRIBUTES) continue;
      if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        Navigate(f, std::wstring());
        return;
      }
      if (IsSupportedImage(f)) {
        host->ExplorerFindSameFile(f);
        return;
      }
    }
    if (!url.empty()) {
      host->ExplorerFindSameUrl(url);
      return;
    }
    if (!files.empty()) MessageBoxW(hwnd, L"画像のファイルか、画像の URL をドロップしてください。", L"imgdupe", MB_ICONINFORMATION);
  }

  void Paste() {
    if (!OpenClipboard(hwnd)) return;
    std::vector<std::wstring> files;
    std::wstring url;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
      HDROP d = HDROP(h);
      UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < n; i++) {
        wchar_t buf[32768];
        if (DragQueryFileW(d, i, buf, UINT(std::size(buf)))) files.push_back(buf);
      }
    } else if (HANDLE t = GetClipboardData(CF_UNICODETEXT)) {
      if (auto* p = static_cast<const wchar_t*>(GlobalLock(t))) {
        url = p;
        GlobalUnlock(t);
      }
      size_t nl = url.find_first_of(L"\r\n");
      if (nl != std::wstring::npos) url.resize(nl);
      while (!url.empty() && url.back() == L' ') url.pop_back();
      if (!IsUrl(url)) {
        // ファイルのパスの文字
        while (!url.empty() && url.front() == L'"') url.erase(url.begin());
        while (!url.empty() && url.back() == L'"') url.pop_back();
        if (!url.empty()) files.push_back(url);
        url.clear();
      }
    }
    CloseClipboard();
    if (!files.empty() || !url.empty()) Dropped(files, url);
  }

  // ---------------------------------------------------------------- フォルダ一覧 (ツリー)

  std::vector<std::wstring> treePaths;  // 項目の lParam = ここの番号
  std::set<HTREEITEM> treeFilled;       // 子を読んだ項目
  bool treeSyncing = false;
  int folderIconIndex = 0, folderOpenIconIndex = 0;

  HTREEITEM TreeAdd(HTREEITEM parent, const std::wstring& label, const std::wstring& path, int icon, int openIcon) {
    treePaths.push_back(path);
    TVINSERTSTRUCTW ins = {};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    ins.item.pszText = const_cast<wchar_t*>(label.c_str());
    ins.item.lParam = LPARAM(treePaths.size() - 1);
    ins.item.cChildren = 1;  // 開く時に調べる
    ins.item.iImage = icon;
    ins.item.iSelectedImage = openIcon;
    return HTREEITEM(SendMessageW(tree, TVM_INSERTITEMW, 0, LPARAM(&ins)));
  }

  std::wstring TreePath(HTREEITEM h) const {
    TVITEMW it = {};
    it.mask = TVIF_PARAM;
    it.hItem = h;
    if (!SendMessageW(tree, TVM_GETITEMW, 0, LPARAM(&it))) return std::wstring();
    size_t i = size_t(it.lParam);
    return i < treePaths.size() ? treePaths[i] : std::wstring();
  }

  void TreeFill(HTREEITEM h) {
    if (!h || treeFilled.count(h)) return;
    treeFilled.insert(h);
    std::vector<std::wstring> subdirs;
    std::wstring path = TreePath(h);
    ListDir(path, &subdirs, nullptr);
    for (auto& sd : subdirs) TreeAdd(h, sd, Join(path, sd), folderIconIndex, folderOpenIconIndex);
    if (subdirs.empty()) {
      TVITEMW it = {};
      it.mask = TVIF_CHILDREN;
      it.hItem = h;
      it.cChildren = 0;
      SendMessageW(tree, TVM_SETITEMW, 0, LPARAM(&it));
    }
  }

  void TreeInit() {
    SHFILEINFOW fi = {};
    HIMAGELIST small = HIMAGELIST(SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &fi, sizeof(fi),
                                                  SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));
    folderIconIndex = fi.iIcon;
    if (SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &fi, sizeof(fi),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES | SHGFI_OPENICON))
      folderOpenIconIndex = fi.iIcon;
    else
      folderOpenIconIndex = folderIconIndex;
    if (small) SendMessageW(tree, TVM_SETIMAGELIST, TVSIL_NORMAL, LPARAM(small));
    auto addRoot = [&](const std::wstring& path, std::wstring label) {
      SHFILEINFOW f = {};
      int icon = folderIconIndex;
      if (SHGetFileInfoW(path.c_str(), 0, &f, sizeof(f), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_DISPLAYNAME)) {
        icon = f.iIcon;
        if (label.empty()) label = f.szDisplayName;
      }
      if (label.empty()) label = path;
      TreeAdd(TVI_ROOT, label, path, icon, icon);
    };
    // よく使う場所、続いてドライブ
    const KNOWNFOLDERID* known[] = {&FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Pictures, &FOLDERID_Downloads};
    for (auto* id : known) {
      PWSTR p = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, nullptr, &p)) && p) {
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) addRoot(p, std::wstring());
      }
      CoTaskMemFree(p);
    }
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
      if (!(drives & (1u << i))) continue;
      std::wstring root = std::wstring(1, wchar_t(L'A' + i)) + L":\\";
      UINT type = GetDriveTypeW(root.c_str());
      if (type == DRIVE_NO_ROOT_DIR) continue;
      addRoot(root, std::wstring());
    }
  }

  static bool IsUnder(const std::wstring& path, const std::wstring& base) {
    if (base.empty() || path.size() < base.size() || _wcsnicmp(path.c_str(), base.c_str(), base.size()) != 0) return false;
    return path.size() == base.size() || base.back() == L'\\' || path[base.size()] == L'\\';
  }

  // 今のフォルダをツリーで選ぶ (途中を開く)
  void SyncTree(const std::wstring& d) {
    if (!tree || !settings.showTree) return;
    HTREEITEM best = nullptr;
    size_t bestLen = 0;
    for (HTREEITEM h = HTREEITEM(SendMessageW(tree, TVM_GETNEXTITEM, TVGN_ROOT, 0)); h;
         h = HTREEITEM(SendMessageW(tree, TVM_GETNEXTITEM, TVGN_NEXT, LPARAM(h)))) {
      std::wstring p = TreePath(h);
      if (IsUnder(d, p) && p.size() > bestLen) {
        best = h;
        bestLen = p.size();
      }
    }
    if (!best) return;
    treeSyncing = true;
    HTREEITEM cur = best;
    for (;;) {
      std::wstring p = TreePath(cur);
      if (_wcsicmp(p.c_str(), d.c_str()) == 0 || p.size() >= d.size()) break;
      TreeFill(cur);
      SendMessageW(tree, TVM_EXPAND, TVE_EXPAND, LPARAM(cur));
      HTREEITEM next = nullptr;
      for (HTREEITEM c = HTREEITEM(SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CHILD, LPARAM(cur))); c;
           c = HTREEITEM(SendMessageW(tree, TVM_GETNEXTITEM, TVGN_NEXT, LPARAM(c)))) {
        if (IsUnder(d, TreePath(c))) {
          next = c;
          break;
        }
      }
      if (!next) break;
      cur = next;
    }
    SendMessageW(tree, TVM_SELECTITEM, TVGN_CARET, LPARAM(cur));
    SendMessageW(tree, TVM_ENSUREVISIBLE, 0, LPARAM(cur));
    treeSyncing = false;
  }

  LRESULT TreeNotify(NMHDR* nm) {
    if (nm->code == TVN_ITEMEXPANDINGW) {
      auto* tv = reinterpret_cast<NMTREEVIEWW*>(nm);
      if (tv->action == TVE_EXPAND) TreeFill(tv->itemNew.hItem);
    } else if (nm->code == TVN_SELCHANGEDW) {
      auto* tv = reinterpret_cast<NMTREEVIEWW*>(nm);
      if (!treeSyncing && (tv->action == TVC_BYMOUSE || tv->action == TVC_BYKEYBOARD)) {
        std::wstring p = TreePath(tv->itemNew.hItem);
        if (!p.empty() && _wcsicmp(p.c_str(), dir.c_str()) != 0) Navigate(p, std::wstring());
      }
    }
    return 0;
  }

  void Open(int i) {
    if (i < 0 || i >= int(items.size())) return;
    const Item& it = items[size_t(i)];
    if (it.dir) Navigate(it.path, std::wstring());
    else if (it.Stack()) OpenStack(i);
    else host->ExplorerOpen(it.path);
  }

  void Up() {
    if (listMode) {  // 検索の結果からは、前のフォルダへ
      Back();
      return;
    }
    std::wstring parent = ParentDir(dir);
    if (parent.empty()) {
      MessageBeep(MB_OK);
      return;
    }
    std::wstring from = dir;
    while (from.size() > 3 && (from.back() == L'\\' || from.back() == L'/')) from.pop_back();
    Navigate(parent, from);
  }

  // いくつも選んでいる時の右クリックのメニュー
  void MultiMenu(const std::vector<int>& picked, POINT screen) {
    size_t images = 0, dirCount = 0;
    std::vector<std::wstring> scan;  // スキャンタスクに足すもの（フォルダと画像。まとまりは中の画像）
    for (int k : picked) {
      const Item& it = items[size_t(k)];
      images += it.image ? (it.Stack() ? it.members.size() : 1) : 0;
      if (it.dir) {
        scan.push_back(it.path);
        ++dirCount;
      } else if (it.Stack()) {
        scan.insert(scan.end(), it.members.begin(), it.members.end());
      } else if (it.image) {
        scan.push_back(it.path);
      }
    }
    const std::wstring n = std::to_wstring(picked.size()), ni = std::to_wstring(images);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MENU_COPY, (L"パスをコピー（" + n + L" 個）(&C)").c_str());
    AppendMenuW(menu, MF_STRING, MENU_SHOW, L"エクスプローラーで表示(&E)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    UINT on = images ? MF_STRING : MF_STRING | MF_GRAYED;
    AppendMenuW(menu, on, MENU_SAVE_SMART, (L"選んだ画像（" + ni + L" 枚）をスマートフォルダに保存(&M)...").c_str());
    AppendMenuW(menu, on, MENU_BULK_TAG, (L"選んだ画像（" + ni + L" 枚）にタグを付ける・外す(&T)...").c_str());
    if (!scan.empty()) {
      std::wstring what = dirCount ? L"フォルダ " + std::to_wstring(dirCount) + L" 個" : L"";
      if (images) what += (what.empty() ? L"" : L"・") + std::wstring(L"画像 ") + ni + L" 枚";
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, MENU_ADD_ROOT, (L"選んだもの（" + what + L"）をスキャンタスクに追加(&I)").c_str());
      AppendMenuW(menu, MF_STRING, MENU_SCAN_TOP, (L"選んだもの（" + what + L"）をスキャンタスクの一番上に追加(&P)").c_str());
    }
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
      case MENU_COPY: {
        std::wstring text;
        for (int k : picked) text += (text.empty() ? L"" : L"\r\n") + items[size_t(k)].path;
        CopyText(text);
        break;
      }
      case MENU_SHOW: ShowInShell(SelectedPath()); break;
      case MENU_SAVE_SMART: host->ExplorerSaveSelection(); break;
      case MENU_BULK_TAG: host->ExplorerTagSelection(); break;
      case MENU_ADD_ROOT:
      case MENU_SCAN_TOP:
        host->ExplorerAddScanTask(scan, cmd == MENU_SCAN_TOP);
        RefreshStates();
        break;
    }
  }

  // 右クリックのメニュー
  void ContextMenu(int i, POINT screen) {
    if (i < 0 || i >= int(items.size())) return;
    {
      auto picked = PickedIndices();
      if (picked.size() > 1 && IsPicked(i)) {
        MultiMenu(picked, screen);
        return;
      }
    }
    const Item& it = items[size_t(i)];
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MENU_OPEN, L"開く(&O)");
    AppendMenuW(menu, MF_STRING, MENU_SHOW, L"エクスプローラーで表示(&E)");
    AppendMenuW(menu, MF_STRING, MENU_COPY, L"パスをコピー(&C)");
    if (it.image) {
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, MENU_SAME, L"同じ画像を探す(&S)");
      AppendMenuW(menu, MF_STRING, MENU_SIMILAR_AI, L"似た画像を探す（AI）(&A)");
      AppendMenuW(menu, MF_STRING, MENU_RELOOKUP, L"出典を調べ直す(&L)");
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, MENU_SAVE_SMART, L"スマートフォルダに保存(&M)...");
      AppendMenuW(menu, MF_STRING, MENU_BULK_TAG, L"タグを付ける・外す(&T)...");
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, MENU_SCAN_TOP, L"この画像を解析する（最優先）(&I)");
    } else if (it.Stack()) {
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, MENU_SCAN_TOP, L"このまとまりの画像を解析する（最優先）(&I)");
    }
    if (it.dir) {
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, MENU_ADD_ROOT, L"スキャンタスクに追加(&I)");
      AppendMenuW(menu, MF_STRING, MENU_SCAN_TOP, L"スキャンタスクの一番上に追加（最優先）(&P)");
      if (host->ExplorerIsRoot(it.path)) AppendMenuW(menu, MF_STRING, MENU_REMOVE_ROOT, L"調べるフォルダから外す(&X)...");
    }
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    std::wstring path = it.path;
    switch (cmd) {
      case MENU_OPEN: Open(i); break;
      case MENU_SHOW: ShowInShell(path); break;
      case MENU_COPY: CopyText(path); break;
      case MENU_SAME: host->ExplorerFindSameFile(path); break;
      case MENU_SIMILAR_AI: host->ExplorerFindSimilarAi(path); break;
      case MENU_RELOOKUP: host->ExplorerRelookup(path); break;
      case MENU_SAVE_SMART: host->ExplorerSaveSelection(); break;
      case MENU_BULK_TAG: host->ExplorerTagSelection(); break;
      case MENU_ADD_ROOT:
      case MENU_SCAN_TOP:
        host->ExplorerAddScanTask(it.Stack() ? it.members : std::vector<std::wstring>{path}, cmd == MENU_SCAN_TOP);
        RefreshStates();
        break;
      case MENU_REMOVE_ROOT:
        host->ExplorerRemoveRoot(path);
        RefreshStates();
        break;
    }
  }

  static void ShowInShell(const std::wstring& path) {
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
  }

  void CopyText(const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (g) {
      memcpy(GlobalLock(g), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
      GlobalUnlock(g);
      if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
  }

  void RefreshStates() {
    if (LoadStates()) {
      InvalidateRect(grid, nullptr, FALSE);
      InvalidateRect(preview, nullptr, FALSE);
    }
  }

  // ---------------------------------------------------------------- スクロール

  void UpdateScroll() {
    RECT rc;
    GetClientRect(grid, &rc);
    int total = TotalHeight();
    scrollY = std::max(0, std::min(scrollY, std::max(0, total - int(rc.bottom))));
    SCROLLINFO si = {sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
    si.nMin = 0;
    si.nMax = std::max(0, total - 1);
    si.nPage = UINT(rc.bottom);
    si.nPos = scrollY;
    SetScrollInfo(grid, SB_VERT, &si, TRUE);
  }

  void ScrollTo(int y) {
    scrollY = y;
    UpdateScroll();
    InvalidateRect(grid, nullptr, FALSE);
  }

  void EnsureVisible(int i) {
    if (i < 0) return;
    RECT rc;
    GetClientRect(grid, &rc);
    RECT r = CellRect(i);
    if (r.top < HeaderH()) ScrollTo(scrollY + r.top - HeaderH() - (Details() ? 0 : Pad()));
    else if (r.bottom > rc.bottom) ScrollTo(scrollY + (r.bottom - rc.bottom) + Pad());
  }

  void Select(int i) {
    if (items.empty()) return;
    int old = sel;
    sel = std::max(0, std::min(i, int(items.size()) - 1));
    anchor = sel;
    if (multi) {
      multi = false;
      if (preview) InvalidateRect(preview, nullptr, FALSE);
    }
    EnsureVisible(sel);
    InvalidateRect(grid, nullptr, FALSE);
    UpdatePreview();
    if (sel != old) NotifySelected();
  }

  // ---------------------------------------------------------------- 大きさ

  void SetSize(int mode, int custom) {
    // 選んでいるものが見えたままになるよう、大きさを変えたら選んでいるものへ戻す
    settings.sizeMode = mode;
    settings.customSize = std::max(kMinSize, std::min(kMaxSize, custom));
    SendMessageW(sizeBox, CB_SETCURSEL, settings.sizeMode, 0);
    SendMessageW(slider, TBM_SETPOS, TRUE, settings.sizeMode < 3 ? kPresetSizes[settings.sizeMode] : settings.customSize);
    UpdateScroll();
    EnsureVisible(sel);
    InvalidateRect(grid, nullptr, FALSE);
    host->ExplorerSettingsChanged(settings);
  }

  int CurrentLogicalSize() const {
    return settings.sizeMode < 3 ? kPresetSizes[settings.sizeMode] : settings.customSize;
  }

  // ---------------------------------------------------------------- サムネイルの読み込み

  static std::wstring JobKey(const ThumbJob& j) {
    return j.path + L"|" + std::to_wstring(j.bucket) + L"|" + std::to_wstring(j.style) + (j.preview ? L"|p" : L"");
  }

  void StartWorkers() {
    for (int i = 0; i < 3; i++) {
      workers.emplace_back([this] {
        imgwin::threadBegin();  // WIC に COM を使う
        for (;;) {
          ThumbJob job;
          {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return quit || !queue.empty(); });
            if (quit) {
              lk.unlock();
              imgwin::threadEnd();
              return;
            }
            job = queue.front();
            queue.pop_front();
            inflight.insert(JobKey(job));
          }
          auto* r = new ThumbResult;
          r->job = job;
          std::vector<std::wstring> pics;
          if (job.dir) {
            std::vector<std::wstring> all;
            int budget = 200;  // 調べるフォルダの数の上限 (深い・広い階層で時間をかけない)
            FolderImages(job.path, 0, &all, &budget);
            if (!all.empty()) {
              switch (job.style) {
                case 1:  // 1・2 枚目
                  pics.push_back(all[0]);
                  if (all.size() > 1) pics.push_back(all[1]);
                  break;
                case 2:  // 最後と 1 枚目 (左に最後、右に 1 枚目)
                  if (all.size() > 1) pics.push_back(all.back());
                  pics.push_back(all[0]);
                  break;
                case 3:  // 最後
                  pics.push_back(all.back());
                  break;
                case 4:  // 最後の一つ前と最後 (左に一つ前、右に最後)
                  if (all.size() > 1) pics.push_back(all[all.size() - 2]);
                  pics.push_back(all.back());
                  break;
                default:  // 1 枚目
                  pics.push_back(all[0]);
                  break;
              }
            }
          } else {
            pics.push_back(job.path);
          }
          for (auto& p : pics) {
            int box = pics.size() > 1 ? job.bucket * 3 / 4 : job.bucket;
            Pixels img;
            int ow = 0, oh = 0;
            if (imgwin::decodeBgra(p, box, img.px, img.w, img.h, &ow, &oh)) {
              if (r->images.empty()) {
                // decodeBgra は向きを直してあるので、縦横は縮小版に合わせる
                bool swap = (img.w > img.h) != (ow > oh) && ow != oh;
                r->fullW = swap ? oh : ow;
                r->fullH = swap ? ow : oh;
              }
              r->images.push_back(std::move(img));
            }
          }
          r->none = r->images.empty();
          {
            std::lock_guard<std::mutex> lk(mu);
            inflight.erase(JobKey(job));
          }
          if (!PostMessageW(grid, WM_THUMB_DONE, 0, LPARAM(r))) delete r;
        }
      });
    }
  }

  // 見えているもの (と少し先) で、まだ無い・小さいサムネイルを頼む。見えているものを先に
  void RequestVisible() {
    if (items.empty()) return;
    RECT rc;
    GetClientRect(grid, &rc);
    int bucket = BucketFor(ThumbSize());
    int cols = Cols();
    int first = std::max(0, (scrollY - Pad()) / CellH() * cols);
    int last = std::min(int(items.size()) - 1, int((scrollY + rc.bottom) / CellH() + 2) * cols);
    std::vector<ThumbJob> jobs;
    for (int i = first; i <= last; i++) {
      const Item& it = items[size_t(i)];
      if (!it.Thumbable()) continue;
      auto c = cache.find(it.path);
      if (c != cache.end() && c->second.bucket >= bucket && (!it.dir || c->second.style == settings.folderStyle)) continue;
      jobs.push_back({it.path, it.dir, bucket, settings.folderStyle, gen.load()});
    }
    {
      std::lock_guard<std::mutex> lk(mu);
      // 見えなくなったものは後回し (また見えたら頼む)。プレビューは残す
      queue.erase(std::remove_if(queue.begin(), queue.end(), [](const ThumbJob& q) { return !q.preview; }), queue.end());
      for (auto& j : jobs) {
        if (inflight.count(JobKey(j))) continue;
        queue.push_back(j);
      }
    }
    cv.notify_all();
  }

  void OnThumb(ThumbResult* r) {
    std::unique_ptr<ThumbResult> res(r);
    Thumb t;
    t.bucket = r->job.bucket;
    t.style = r->job.style;
    t.none = r->none;
    t.fullW = r->fullW;
    t.fullH = r->fullH;
    for (auto& img : r->images) {
      ThumbPic p;
      p.bmp = ToDib(img);
      p.w = img.w;
      p.h = img.h;
      if (p.bmp) t.pics.push_back(p);
    }
    if (r->job.preview) {
      OnPreviewThumb(r, t);
      return;
    }
    auto it = cache.find(r->job.path);
    if (it != cache.end()) {
      if (it->second.bucket > t.bucket && it->second.style == t.style) {  // もっと大きいのがある
        t.Free();
        return;
      }
      cacheBytes -= it->second.bytes();
      it->second.Free();
      lru.remove(r->job.path);
      it->second = t;
    } else {
      cache[r->job.path] = t;
    }
    cacheBytes += t.bytes();
    lru.push_back(r->job.path);
    // 多すぎたら古いものから捨てる (256MB)
    while (cacheBytes > (256u << 20) && lru.size() > 1) {
      auto old = cache.find(lru.front());
      if (old != cache.end()) {
        cacheBytes -= old->second.bytes();
        old->second.Free();
        cache.erase(old);
      }
      lru.pop_front();
    }
    InvalidateRect(grid, nullptr, FALSE);
    if (r->job.path == previewPath && !previewReady) InvalidateRect(preview, nullptr, FALSE);  // 大きいのが来るまでこれを出す
  }

  HICON IconFor(const Item& it) {
    std::wstring key = it.dir ? L"<dir>" : PathFindExtensionW(it.name.c_str());
    CharLowerBuffW(&key[0], DWORD(key.size()));
    auto f = icons.find(key);
    if (f != icons.end()) return f->second;
    SHFILEINFOW fi = {};
    HICON icon = nullptr;
    DWORD attr = it.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (SHGetFileInfoW(it.dir ? L"folder" : (L"x" + key).c_str(), attr, &fi, sizeof(fi),
                       SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES)) {
      IImageList* list = nullptr;
      if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, kIID_IImageList, reinterpret_cast<void**>(&list))) && list) {
        list->GetIcon(fi.iIcon, ILD_TRANSPARENT, &icon);
        list->Release();
      }
    }
    if (!icon && SHGetFileInfoW((L"x" + key).c_str(), attr, &fi, sizeof(fi),
                                SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES))
      icon = fi.hIcon;
    icons[key] = icon;
    return icon;
  }

  // ---------------------------------------------------------------- 描画

  // bottom: 下に揃える (本棚に立てたように)
  static RECT FitRect(int pw, int ph, RECT box, bool bottom = false) {
    double s = std::min(double(box.right - box.left) / pw, double(box.bottom - box.top) / ph);
    s = std::min(s, 1.0);
    int w = std::max(1, int(pw * s)), h = std::max(1, int(ph * s));
    int x = box.left + (box.right - box.left - w) / 2;
    int y = bottom ? box.bottom - h : box.top + (box.bottom - box.top - h) / 2;
    return {x, y, x + w, y + h};
  }

  static void Blit(HDC dc, HDC mem, const ThumbPic& p, RECT r) {
    HGDIOBJ old = SelectObject(mem, p.bmp);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    SetStretchBltMode(dc, HALFTONE);
    AlphaBlend(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, mem, 0, 0, p.w, p.h, bf);
    SelectObject(mem, old);
  }

  static void DrawPic(HDC dc, HDC mem, const ThumbPic& p, RECT box) { Blit(dc, mem, p, FitRect(p.w, p.h, box)); }

  static void Fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &r, br);
    DeleteObject(br);
  }

  static void Frame(HDC dc, RECT r, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    FrameRect(dc, &r, br);
    DeleteObject(br);
  }

  // 黒か白を半透明に重ねる (影や背の陰影)
  HBITMAP dots[2] = {};
  void Shade(HDC dc, HDC mem, RECT r, bool white, int alpha) {
    if (r.right <= r.left || r.bottom <= r.top || alpha <= 0) return;
    HBITMAP& b = dots[white ? 1 : 0];
    if (!b) {
      BITMAPINFO bi = {};
      bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
      bi.bmiHeader.biWidth = 1;
      bi.bmiHeader.biHeight = 1;
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      void* bits = nullptr;
      b = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
      if (!b) return;
      *static_cast<DWORD*>(bits) = white ? 0x00FFFFFF : 0;
    }
    HGDIOBJ old = SelectObject(mem, b);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, BYTE(std::min(alpha, 255)), 0};
    AlphaBlend(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, mem, 0, 0, 1, 1, bf);
    SelectObject(mem, old);
  }

  // 柔らかい影 (右下にずらして 2 段)
  void DropShadow(HDC dc, HDC mem, RECT r) {
    int o = Px(2);
    Shade(dc, mem, {r.left + o * 2, r.top + o * 2, r.right + o * 2, r.bottom + o * 2}, false, 22);
    Shade(dc, mem, {r.left + o, r.top + o, r.right + o, r.bottom + o}, false, 34);
  }

  // 小口 (紙の重なり) を表す細い線
  void PageLines(HDC dc, int x0, int y0, int x1, int y1, bool vertical, int depth) {
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(214, 208, 196));
    HGDIOBJ op = SelectObject(dc, pen);
    for (int k = Px(2); k < depth; k += Px(2)) {
      if (vertical) {
        MoveToEx(dc, x0 + k, y0 + k, nullptr);
        LineTo(dc, x0 + k, y1 + k);
      } else {
        MoveToEx(dc, x0 + k, y0 + k, nullptr);
        LineTo(dc, x1 + k, y0 + k);
      }
    }
    SelectObject(dc, op);
    DeleteObject(pen);
  }

  // 閉じた本: 表紙 (画像か無地) と背の陰影、右と下に小口、影
  // 描いた本の外形を返す
  RECT DrawBook(HDC dc, HDC mem, RECT box, const ThumbPic* pic, COLORREF blank) {
    int ts = box.right - box.left;
    int d = std::max(Px(3), ts / 26);  // 本の厚み
    RECT area = {box.left, box.top, box.right - d - Px(4), box.bottom - d - Px(4)};
    RECT cr;
    if (pic) {
      cr = FitRect(pic->w, pic->h, area, true);
    } else {
      int h = (area.bottom - area.top) * 9 / 10, w = h * 7 / 10;
      int x = (area.left + area.right - w) / 2, y = area.bottom - h;
      cr = {x, y, x + w, y + h};
    }
    RECT pages = {cr.left + Px(2), cr.top + d, cr.right + d, cr.bottom + d};
    DropShadow(dc, mem, pages);
    Fill(dc, pages, RGB(250, 248, 241));
    PageLines(dc, cr.right, cr.top + Px(1), 0, cr.bottom - Px(1), true, d);
    PageLines(dc, cr.left + Px(2), cr.bottom, cr.right - Px(1), 0, false, d);
    Frame(dc, pages, RGB(176, 170, 158));
    Fill(dc, cr, pic ? RGB(255, 255, 255) : blank);
    if (pic) Blit(dc, mem, *pic, cr);
    // 背: 左端が暗く、綴じ目に細い溝
    int sw = std::max(Px(3), int(cr.right - cr.left) / 12);
    for (int x = 0; x < sw; x++)
      Shade(dc, mem, {cr.left + x, cr.top, cr.left + x + 1, cr.bottom}, false, 90 * (sw - x) / sw);
    Shade(dc, mem, {cr.left + sw, cr.top, cr.left + sw + 1, cr.bottom}, false, 60);
    Shade(dc, mem, {cr.left + sw + 1, cr.top, cr.left + sw + 2, cr.bottom}, true, 110);
    Frame(dc, cr, RGB(120, 116, 108));
    return {cr.left, cr.top, pages.right, pages.bottom};
  }

  // 開いた本: 左右のページに 1 枚ずつ、ノドの陰影、下に小口、まわりに表紙
  RECT DrawSpread(HDC dc, HDC mem, RECT box, const ThumbPic& left, const ThumbPic& right) {
    int ts = box.right - box.left;
    int d = std::max(Px(3), ts / 30), bd = std::max(Px(2), ts / 48);
    RECT area = {box.left + bd, box.top + Px(2), box.right - bd - Px(4), box.bottom - d - bd - Px(4)};
    int mid = (area.left + area.right) / 2;
    RECT ra = FitRect(left.w, left.h, {area.left, area.top, mid, area.bottom}, true);
    RECT rb = FitRect(right.w, right.h, {mid, area.top, area.right, area.bottom}, true);
    int pw = std::max(ra.right - ra.left, rb.right - rb.left), ph = std::max(ra.bottom - ra.top, rb.bottom - rb.top);
    RECT L = {mid - pw, area.bottom - ph, mid, area.bottom}, R = {mid, L.top, mid + pw, L.bottom};
    RECT board = {L.left - bd, L.top + Px(2), R.right + bd, L.bottom + d + bd};
    DropShadow(dc, mem, board);
    Fill(dc, board, RGB(72, 82, 104));
    RECT stack = {L.left, L.bottom, R.right, L.bottom + d};
    Fill(dc, stack, RGB(250, 248, 241));
    PageLines(dc, L.left, L.bottom - Px(2), R.right - d, 0, false, d);
    Fill(dc, L, RGB(255, 255, 255));
    Fill(dc, R, RGB(255, 255, 255));
    DrawPic(dc, mem, left, L);
    DrawPic(dc, mem, right, R);
    // ノド: 真ん中に向かって暗く
    int gw = std::max(Px(3), pw / 7);
    for (int x = 0; x < gw; x++) {
      int a = 70 * (x + 1) / gw;
      Shade(dc, mem, {mid - gw + x, L.top, mid - gw + x + 1, L.bottom}, false, a);
      Shade(dc, mem, {mid + gw - x - 1, L.top, mid + gw - x, L.bottom}, false, a);
    }
    Frame(dc, {L.left, L.top, R.right, L.bottom}, RGB(150, 146, 138));
    return board;
  }

  // フォルダを表す小さな札 (添え物)
  void DrawFolderShape(HDC dc, RECT r) {
    int tabW = int(r.right - r.left) * 2 / 5, tabH = std::max(2, int(r.bottom - r.top) / 6);
    HBRUSH back = CreateSolidBrush(RGB(232, 190, 80));
    HBRUSH front = CreateSolidBrush(RGB(252, 214, 110));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(186, 146, 50));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, back);
    int rr = std::max(2, int(r.bottom - r.top) / 6);
    RoundRect(dc, r.left, r.top, r.left + tabW, r.top + tabH * 2, rr, rr);
    RoundRect(dc, r.left, r.top + tabH, r.right, r.bottom, rr, rr);
    SelectObject(dc, front);
    RoundRect(dc, r.left, r.top + tabH * 2, r.right, r.bottom, rr, rr);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(back);
    DeleteObject(front);
    DeleteObject(pen);
  }

  // book: 本の外形。その左下の角に重ねる
  void FolderBadge(HDC dc, HDC mem, int ts, RECT book) {
    int bw = std::clamp(ts / 5, Px(16), Px(40)), bh = bw * 4 / 5;
    int x = std::max(int(book.left) - bw / 4, 0), y = book.bottom - bh + bh / 5;
    RECT r = {x, y, x + bw, y + bh};
    Shade(dc, mem, {r.left + Px(1), r.top + Px(2), r.right + Px(1), r.bottom + Px(1)}, false, 50);
    DrawFolderShape(dc, r);
  }

  // インデックスの状態の印 (右下の丸): 緑 = 作成済み、橙 = 処理待ち、白 = 未作成、左半分が緑 = 一部
  void StateBadge(HDC dc, RECT box, int state) {
    if (state < 0) return;
    int ts = std::min(box.right - box.left, box.bottom - box.top);
    int r = std::clamp(ts / 10, Px(6), Px(11));
    int cx = box.right - r - Px(1), cy = box.bottom - r - Px(1);
    RECT c = {cx - r, cy - r, cx + r + 1, cy + r + 1};
    COLORREF green = RGB(36, 160, 72), orange = RGB(232, 140, 24), gray = RGB(140, 140, 140);
    HPEN outline = CreatePen(PS_SOLID, std::max(1, Px(1)), RGB(255, 255, 255));
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    HBRUSH fill = CreateSolidBrush(state == kStateIndexed ? green : state == kStatePending ? orange : RGB(255, 255, 255));
    HGDIOBJ op = SelectObject(dc, outline);
    HGDIOBJ ob = SelectObject(dc, fill);
    Ellipse(dc, c.left - Px(1), c.top - Px(1), c.right + Px(1), c.bottom + Px(1));  // 白い縁
    HPEN ring = CreatePen(PS_SOLID, std::max(1, Px(1)), state == kStateNone ? gray : state == kStatePending ? orange : green);
    SelectObject(dc, ring);
    if (state == kStatePartial) {
      SelectObject(dc, white);
      Ellipse(dc, c.left, c.top, c.right, c.bottom);
      HBRUSH g = CreateSolidBrush(green);
      SelectObject(dc, g);
      Pie(dc, c.left, c.top, c.right, c.bottom, cx, c.top, cx, c.bottom);  // 左半分
      SelectObject(dc, white);
      DeleteObject(g);
    } else {
      Ellipse(dc, c.left, c.top, c.right, c.bottom);
    }
    if (state == kStateIndexed) {
      // 白いチェック
      HPEN check = CreatePen(PS_SOLID, std::max(2, r / 3), RGB(255, 255, 255));
      SelectObject(dc, check);
      POINT pts[3] = {{cx - r / 2, cy}, {cx - r / 8, cy + r * 2 / 5}, {cx + r / 2, cy - r * 2 / 5}};
      Polyline(dc, pts, 3);
      SelectObject(dc, ring);
      DeleteObject(check);
    } else if (state == kStatePending) {
      // 白い点 3 つ (待っている)
      for (int k = -1; k <= 1; k++) {
        RECT d = {cx + k * r / 2 - Px(1), cy - Px(1), cx + k * r / 2 + Px(1) + 1, cy + Px(1) + 1};
        Fill(dc, d, RGB(255, 255, 255));
      }
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(outline);
    DeleteObject(white);
    DeleteObject(fill);
    DeleteObject(ring);
  }

  void DrawIconIn(HDC dc, const Item& it, RECT box) {
    HICON icon = IconFor(it);
    if (!icon) return;
    int is = std::min({int(box.right - box.left), int(box.bottom - box.top), Px(256)}) * 2 / 3;
    DrawIconEx(dc, (box.left + box.right - is) / 2, box.bottom - is - Px(4), icon, is, is, 0, nullptr, DI_NORMAL);
  }

  // 1 つ分のサムネイルを box に描く (一覧とプレビューで同じ見た目)。t は無ければ読み込み中
  // まとまりの枚数 (左上の丸)
  void StackBadge(HDC dc, RECT r, int n) {
    std::wstring text = std::to_wstring(n);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &sz);
    int h = Px(18), w = std::max(h, int(sz.cx) + Px(10));
    RECT b = {r.left + Px(4), r.top + Px(4), r.left + Px(4) + w, r.top + Px(4) + h};
    HBRUSH br = CreateSolidBrush(RGB(0, 120, 215));
    HGDIOBJ oldBr = SelectObject(dc, br), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, b.left, b.top, b.right, b.bottom, h, h);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(br);
    COLORREF oldColor = SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, text.c_str(), -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, oldColor);
  }

  // 一覧に出す名前 (まとまりは「名前 ほか N 枚」)
  static std::wstring DisplayName(const Item& it) {
    return it.Stack() ? it.name + L" ほか " + std::to_wstring(it.members.size() - 1) + L" 枚" : it.name;
  }

  void DrawThumb(HDC dc, HDC mem, const Item& it, const Thumb* t, RECT box) {
    int ts = std::min(box.right - box.left, box.bottom - box.top);
    if (it.dir) {
      // フォルダは本として描き、隅にフォルダの札を添える
      RECT book = t && t->pics.size() >= 2
                      ? DrawSpread(dc, mem, box, t->pics[0], t->pics[1])
                      : DrawBook(dc, mem, box, t && !t->pics.empty() ? &t->pics[0] : nullptr,
                                 t ? RGB(126, 142, 170) : RGB(228, 228, 228));
      FolderBadge(dc, mem, ts, book);
    } else if (it.image) {
      if (t && !t->pics.empty()) {
        // 一枚の紙のように: 白地と影、細い縁
        const ThumbPic& p = t->pics[0];
        // まとまりは、後ろに紙を 2 枚重ねて見せる
        int lift = it.Stack() ? Px(8) : 0;
        RECT r = FitRect(p.w, p.h, {box.left, box.top + lift, box.right - Px(4) - lift, box.bottom - Px(4)}, true);
        for (int k = 2; k >= 1 && it.Stack(); k--) {
          RECT back = {r.left + k * Px(4), r.top - k * Px(4), r.right + k * Px(4), r.bottom - k * Px(4)};
          Fill(dc, back, RGB(250, 250, 250));
          Frame(dc, back, RGB(180, 180, 180));
        }
        DropShadow(dc, mem, r);
        Fill(dc, r, RGB(255, 255, 255));
        Blit(dc, mem, p, r);
        Frame(dc, r, RGB(200, 200, 200));
        if (it.Stack()) StackBadge(dc, r, int(it.members.size()));
      } else if (t && t->none) {
        DrawIconIn(dc, it, box);  // 読めなかった
      } else {
        // 読み込み中
        RECT ph = {box.left + ts / 6, box.top + ts / 6, box.right - ts / 6, box.bottom - ts / 6};
        Fill(dc, ph, RGB(236, 236, 236));
      }
    } else {
      DrawIconIn(dc, it, box);
    }
    StateBadge(dc, box, it.state);
  }

  // 詳細表示の列: 名前・更新日時・種類 (検索の結果ではフォルダ)・サイズ・インデックス。右端の x を返す
  struct Column {
    const wchar_t* title;
    int sortKey;  // 見出しをクリックした時の並び順 (-1 = 並べ替えない)
    int left, right;
  };
  std::vector<Column> DetailColumns() const {
    int w = GridW();
    int dateW = Px(140), typeW = listMode ? Px(280) : Px(150), sizeW = Px(90), stateW = Px(96);
    int nameW = std::max(Px(160), w - dateW - typeW - sizeW - stateW);
    std::vector<Column> cols = {{L"名前", 0, 0, 0}, {L"更新日時", 1, 0, 0}, {listMode ? L"フォルダ" : L"種類", listMode ? -1 : 2, 0, 0},
                                {L"サイズ", 3, 0, 0}, {L"インデックス", -1, 0, 0}};
    int widths[] = {nameW, dateW, typeW, sizeW, stateW};
    int x = 0;
    for (size_t i = 0; i < cols.size(); i++) {
      cols[i].left = x;
      x += widths[i];
      cols[i].right = x;
    }
    return cols;
  }

  void PaintDetails(HDC dc, const RECT& rc) {
    auto cols = DetailColumns();
    int rowH = CellH(), pad = Px(6);
    for (int i = 0; i < int(items.size()); i++) {
      RECT row = CellRect(i);
      if (row.bottom < HeaderH()) continue;
      if (row.top > rc.bottom) break;
      const Item& it = items[size_t(i)];
      if (IsPicked(i) || i == hover) {
        Fill(dc, row, IsPicked(i) ? RGB(204, 232, 255) : RGB(229, 243, 255));
        if (IsPicked(i)) Frame(dc, row, i == sel ? RGB(0, 120, 215) : RGB(153, 209, 255));
      }
      // 名前: 小さいアイコンと名前
      HICON icon = IconFor(it);
      int is = Px(16);
      if (icon) DrawIconEx(dc, cols[0].left + pad, row.top + (rowH - is) / 2, icon, is, is, 0, nullptr, DI_NORMAL);
      SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
      auto cell = [&](int c, const std::wstring& text, UINT align) {
        RECT r = {cols[size_t(c)].left + pad, row.top, cols[size_t(c)].right - pad, row.bottom};
        DrawTextW(dc, text.c_str(), -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
      };
      RECT nameR = {cols[0].left + pad + is + Px(6), row.top, cols[0].right - pad, row.bottom};
      DrawTextW(dc, DisplayName(it).c_str(), -1, &nameR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
      FILETIME ft = {DWORD(it.mtime), DWORD(it.mtime >> 32)};
      if (it.mtime) cell(1, FormatTime(ft), DT_LEFT);
      SetTextColor(dc, RGB(96, 96, 96));
      cell(2, listMode ? FolderOf(it.path) : TypeName(const_cast<Item&>(it)), DT_LEFT);
      if (!it.dir) cell(3, FormatBytes(it.size), DT_RIGHT);
      if (it.state >= 0) {
        int r = Px(5), cx = cols[4].left + pad + r, cy = (row.top + row.bottom) / 2;
        StateBadge(dc, {cx - r * 2, cy - r * 2, cx + r * 2 - Px(1), cy + r * 2 - Px(1)}, it.state);
        RECT tr = {cols[4].left + pad + r * 3, row.top, cols[4].right - pad, row.bottom};
        static const wchar_t* names[] = {L"未作成", L"処理待ち", L"作成済み", L"一部"};
        DrawTextW(dc, names[it.state], -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      }
    }
    // 列の見出し (スクロールしない)
    RECT hr = {0, 0, rc.right, HeaderH()};
    Fill(dc, hr, RGB(250, 250, 250));
    Fill(dc, {0, hr.bottom - 1, rc.right, hr.bottom}, RGB(220, 220, 220));
    SetTextColor(dc, RGB(76, 96, 122));
    int key = SortKey();
    for (const auto& c : cols) {
      std::wstring t = c.title;
      if (c.sortKey >= 0 && c.sortKey == key) t += settings.sortDesc ? L" ▼" : L" ▲";
      RECT r = {c.left + pad, 0, c.right - pad, hr.bottom};
      DrawTextW(dc, t.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      if (c.right < rc.right) Fill(dc, {c.right - 1, Px(4), c.right, hr.bottom - Px(4)}, RGB(225, 225, 225));
    }
    if (items.empty()) {
      RECT tr = rc;
      SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
      DrawTextW(dc, L"(何もありません)", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
  }

  void Paint(HDC dc, const RECT& rc) {
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    if (Details()) {
      SetBkMode(dc, TRANSPARENT);
      HGDIOBJ oldFont = SelectObject(dc, nameFont);
      PaintDetails(dc, rc);
      SelectObject(dc, oldFont);
      return;
    }
    HDC mem = CreateCompatibleDC(dc);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, nameFont);
    int ts = ThumbSize();
    bool requested = false;
    int bucket = BucketFor(ts);
    for (int i = 0; i < int(items.size()); i++) {
      RECT cell = CellRect(i);
      if (cell.bottom < 0) continue;
      if (cell.top > rc.bottom) break;
      const Item& it = items[size_t(i)];
      RECT inner = {cell.left + Px(2), cell.top + Px(2), cell.right - Px(2), cell.bottom - Px(2)};
      if (IsPicked(i) || i == hover) {
        Fill(dc, inner, IsPicked(i) ? RGB(204, 232, 255) : RGB(229, 243, 255));
        if (IsPicked(i)) Frame(dc, inner, i == sel && multi ? RGB(0, 120, 215) : RGB(153, 209, 255));
      }
      RECT box = {cell.left + Pad(), cell.top + Pad(), cell.left + Pad() + ts, cell.top + Pad() + ts};
      const Thumb* t = CachedThumb(it);
      if (it.Thumbable() && (!t || t->bucket < bucket)) requested = true;
      DrawThumb(dc, mem, it, t, box);
      RECT tr = {cell.left + Px(4), box.bottom + Px(4), cell.right - Px(4), cell.bottom - Px(2)};
      SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
      DrawTextW(dc, DisplayName(it).c_str(), -1, &tr, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
    }
    if (items.empty()) {
      RECT tr = rc;
      SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
      DrawTextW(dc, L"(何もありません)", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, oldFont);
    DeleteDC(mem);
    if (requested) RequestVisible();
  }

  const Thumb* CachedThumb(const Item& it) const {
    auto c = cache.find(it.path);
    const Thumb* t = c != cache.end() ? &c->second : nullptr;
    if (t && it.dir && t->style != settings.folderStyle) t = nullptr;  // 出し方を変えた: 読み直すまで形だけ
    return t;
  }

  // ---------------------------------------------------------------- プレビュー

  std::wstring previewPath;  // 出している (読んでいる) もの
  Thumb previewThumb;
  bool previewReady = false;
  bool previewInfoOk = false;
  WIN32_FILE_ATTRIBUTE_DATA previewInfo = {};

  // プレビューの上の切り替え (画像だけ・タグ・プロパティ) の高さ
  int PreviewTabsH() const { return Px(30); }

  int PreviewBox() const {
    RECT rc;
    GetClientRect(preview, &rc);
    int w = rc.right - Px(24);
    int h = int(rc.bottom) - PreviewTabsH();
    if (settings.previewMode == 0) return std::max(Px(48), std::min({w, h - Px(64), 1024}));  // 画像だけ: 名前の分を残して全部
    if (settings.previewMode == 2) return std::max(Px(48), std::min({w, h / 3, 1024}));       // プロパティ: 下に表を出すので 3 分の 1
    return std::max(Px(48), std::min({w, h * 2 / 5, 1024}));  // タグ: 下にタグと情報を出すので 4 割まで
  }

  // 切り替えのボタン
  std::vector<std::pair<RECT, int>> previewTabHits;

  void SetPreviewMode(int mode) {
    if (mode == settings.previewMode) return;
    settings.previewMode = mode;
    host->ExplorerSettingsChanged(settings);
    InvalidateRect(preview, nullptr, FALSE);
    UpdatePreview(true);  // 大きさが変わるので読み直す
  }

  // プロパティ (画像は DB から。開いている画像の分だけ持つ)
  std::vector<std::pair<std::wstring, std::wstring>> previewProps;
  std::wstring previewPropsPath;

  void ClearPreviewThumb() {
    previewThumb.Free();
    previewThumb = Thumb();
    previewReady = false;
  }

  // 選んでいるものが変わった・パネルの大きさが変わった: 大きい縮小版を頼む
  void UpdatePreview(bool force = false) {
    if (!settings.showPreview || !preview) return;
    const Item* it = sel >= 0 && sel < int(items.size()) ? &items[size_t(sel)] : nullptr;
    std::wstring path = it ? it->path : std::wstring();
    int box = PreviewBox();
    int bucket = std::min(1024, (box + 63) / 64 * 64);
    bool same = path == previewPath && previewReady && previewThumb.bucket >= bucket &&
                (!it || !it->dir || previewThumb.style == settings.folderStyle);
    if (same && !force) return;
    LoadPreviewTags(it);
    if (path != previewPath) {
      ClearPreviewThumb();
      previewPath = path;
      previewInfoOk = !path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &previewInfo);
    }
    if (it && it->Thumbable()) {
      ThumbJob j{path, it->dir, bucket, settings.folderStyle, gen.load(), true};
      {
        std::lock_guard<std::mutex> lk(mu);
        queue.erase(std::remove_if(queue.begin(), queue.end(), [](const ThumbJob& q) { return q.preview; }), queue.end());
        if (!inflight.count(JobKey(j))) queue.push_front(j);
      }
      cv.notify_all();
    }
    InvalidateRect(preview, nullptr, FALSE);
  }

  void OnPreviewThumb(ThumbResult* r, Thumb& t) {
    if (r->job.path != previewPath) {
      t.Free();
      return;
    }
    if (previewReady && previewThumb.bucket > t.bucket && previewThumb.style == t.style) {
      t.Free();
      return;
    }
    previewThumb.Free();
    previewThumb = t;
    previewReady = true;
    InvalidateRect(preview, nullptr, FALSE);
  }

  static std::wstring FormatTime(const FILETIME& ft) {
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return std::wstring();
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d/%02d/%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
  }

  static const wchar_t* StateText(int s) {
    switch (s) {
      case kStateIndexed: return L"インデックス: 作成済み";
      case kStatePending: return L"インデックス: 処理待ち";
      case kStatePartial: return L"インデックス: 一部のフォルダだけ";
      case kStateNone: return L"インデックス: 未作成";
    }
    return L"";
  }

  void PaintPreview(HDC dc, const RECT& rc) {
    Fill(dc, rc, RGB(246, 246, 246));
    // 左の境目
    Fill(dc, {rc.left, rc.top, rc.left + 1, rc.bottom}, RGB(220, 220, 220));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, nameFont);
    const Item* it = sel >= 0 && sel < int(items.size()) ? &items[size_t(sel)] : nullptr;
    if (!it) {
      RECT tr = rc;
      SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
      DrawTextW(dc, L"(選んでいません)", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(dc, oldFont);
      return;
    }
    // 上に切り替え (画像だけ・タグ・プロパティ)
    previewTabHits.clear();
    {
      int x = Px(8), y = Px(5);
      const wchar_t* names[] = {L"画像だけ", L"タグ", L"プロパティ"};
      for (int k = 0; k < 3; k++) {
        bool on = settings.previewMode == k;
        RECT r = DrawChip(dc, x, y, names[k], on ? RGB(0, 100, 220) : RGB(130, 130, 130), on);
        previewTabHits.push_back({r, k});
        x = r.right + Px(4);
      }
    }
    HDC mem = CreateCompatibleDC(dc);
    int boxSize = PreviewBox();
    int m = Px(12);
    int top = PreviewTabsH() + Px(4);
    RECT box = {(rc.right - boxSize) / 2, top, (rc.right - boxSize) / 2 + boxSize, top + boxSize};
    const Thumb* t = previewReady && it->path == previewPath ? &previewThumb : nullptr;
    if (!t && it->Thumbable()) {
      // 大きいのを読むまでは一覧の縮小版で
      t = CachedThumb(*it);
    }
    DrawThumb(dc, mem, *it, t, box);
    DeleteDC(mem);

    // 名前と情報
    std::wstring info;
    auto line = [&](const std::wstring& s) {
      if (s.empty()) return;
      if (!info.empty()) info += L"\n";
      info += s;
    };
    if (multi) {
      const size_t n = PickedIndices().size();
      if (n > 1) line(std::to_wstring(n) + L" 個を選んでいます");
    }
    if (it->dir) {
      line(L"フォルダ");
    } else {
      std::wstring ext = PathFindExtensionW(it->name.c_str());
      if (!ext.empty()) ext = ext.substr(1);
      CharUpperBuffW(&ext[0], DWORD(ext.size()));
      line(ext.empty() ? L"ファイル" : ext + (it->image ? L" 画像" : L" ファイル"));
    }
    if (t && it->image && t->fullW > 0) line(std::to_wstring(t->fullW) + L" × " + std::to_wstring(t->fullH) + L" px");
    if (previewInfoOk && it->path == previewPath) {
      if (!it->dir) line(FormatBytes((ULONGLONG(previewInfo.nFileSizeHigh) << 32) | previewInfo.nFileSizeLow));
      line(L"更新: " + FormatTime(previewInfo.ftLastWriteTime));
    }
    line(StateText(it->state));
    RECT nr = {rc.left + m, box.bottom + Px(10), rc.right - m, rc.bottom - m};
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    RECT measure = nr;
    DrawTextW(dc, it->name.c_str(), -1, &measure, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_CALCRECT);
    measure.right = nr.right;
    measure.left = nr.left;
    DrawTextW(dc, it->name.c_str(), -1, &measure, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    // タグ (クリックで検索、ドラッグで取り置き)。ファイルのタグ、続いて検索用のタグ。名前のすぐ下に
    tagHits.clear();
    int y = measure.bottom + Px(8);
    if (settings.previewMode == 0) {
      // 画像だけ: いくつも選んでいる時の数だけ
      if (multi && PickedIndices().size() > 1) {
        RECT ir = {nr.left, y, nr.right, nr.bottom};
        SetTextColor(dc, RGB(96, 96, 96));
        DrawTextW(dc, (std::to_wstring(PickedIndices().size()) + L" 個を選んでいます").c_str(), -1, &ir, DT_CENTER | DT_SINGLELINE);
      }
      SelectObject(dc, oldFont);
      return;
    }
    if (settings.previewMode == 2) {
      // プロパティ: 名前と値の表。画像は DB の情報、ほかはファイルの情報
      if (it->image) {
        if (previewPropsPath != it->path) {
          previewPropsPath = it->path;
          previewProps = host->ExplorerProperties(it->path);
        }
      } else {
        previewProps.clear();
        previewPropsPath.clear();
      }
      std::vector<std::pair<std::wstring, std::wstring>> rows = previewProps;
      if (rows.empty()) {
        // DB に無い: 分かることだけ
        for (size_t a = 0, b; a < info.size(); a = b + 1) {
          b = info.find(L'\n', a);
          if (b == std::wstring::npos) b = info.size();
          rows.push_back({L"", info.substr(a, b - a)});
        }
      } else if (multi && PickedIndices().size() > 1) {
        rows.insert(rows.begin(), {L"選択", std::to_wstring(PickedIndices().size()) + L" 個"});
      }
      const int labelW = Px(100);
      for (const auto& [k, v] : rows) {
        if (y > nr.bottom) break;
        RECT vr = {nr.left + (k.empty() ? 0 : labelW), y, nr.right, nr.bottom};
        RECT calc = vr;
        DrawTextW(dc, v.c_str(), -1, &calc, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_CALCRECT);
        if (!k.empty()) {
          RECT kr = {nr.left, y, nr.left + labelW - Px(6), y + Px(20)};
          SetTextColor(dc, RGB(110, 110, 110));
          DrawTextW(dc, k.c_str(), -1, &kr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        DrawTextW(dc, v.c_str(), -1, &vr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        y += std::max(int(calc.bottom - calc.top), Px(18)) + Px(4);
      }
      SelectObject(dc, oldFont);
      return;
    }
    if (it->image && it->path == previewTagsPath) {
      for (int pass = 0; pass < 2; pass++) {
        bool any = false;
        for (const auto& t : previewTags) any = any || t.file == (pass == 0);
        if (!any) continue;
        RECT hr = {nr.left, y, nr.right, y + Px(18)};
        SetTextColor(dc, RGB(90, 90, 90));
        DrawTextW(dc, pass == 0 ? L"ファイルのタグ" : L"検索用タグ", -1, &hr, DT_LEFT | DT_SINGLELINE);
        y += Px(20);
        int x = nr.left;
        for (size_t i = 0; i < previewTags.size(); i++) {
          const ExplorerTag& t = previewTags[i];
          if (t.file != (pass == 0)) continue;
          SIZE sz = {};
          const std::wstring& label = t.label.empty() ? t.tag : t.label;
          GetTextExtentPoint32W(dc, label.c_str(), int(label.size()), &sz);
          int w = sz.cx + Px(14);
          if (x > nr.left && x + w > nr.right) {
            x = nr.left;
            y += Px(24);
          }
          if (y + Px(20) > rc.bottom) break;
          RECT r = DrawChip(dc, x, y, label, KindColor(t.kind), false);
          tagHits.push_back({r, i});
          x = r.right + Px(5);
        }
        y += Px(28);
      }
      if (previewTags.empty()) {
        RECT tr = {nr.left, y, nr.right, y + Px(20)};
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextW(dc, L"タグはまだありません", -1, &tr, DT_CENTER | DT_SINGLELINE);
        y += Px(26);
      }
    }
    RECT ir = {nr.left, y, nr.right, nr.bottom};
    SetTextColor(dc, RGB(96, 96, 96));
    DrawTextW(dc, info.c_str(), -1, &ir, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, oldFont);
  }

  // ---------------------------------------------------------------- タグ (プレビュー) と取り置き

  std::vector<ExplorerTag> previewTags;
  std::wstring previewTagsPath;
  struct TagHit {
    RECT r;
    size_t index;
  };
  std::vector<TagHit> tagHits;  // プレビューに描いたタグ
  int tagDown = -1;             // 押したタグ (tagHits の番号)
  bool tagDragging = false;
  POINT tagDownPt = {};

  std::vector<std::pair<std::wstring, bool>> tray;  // 取り置いたタグと、検索に使うか
  struct TrayHit {
    RECT chip, close;
    size_t index;
  };
  std::vector<TrayHit> trayHits;
  RECT traySearchR = {}, trayClearR = {};
  bool trayHot = false;  // タグをドラッグして上に来ている

  // タグの種類の色 (Danbooru に近い): 作者 赤、キャラ 緑、作品 紫、一般 青、ほか 灰か橙
  static COLORREF KindColor(const std::wstring& kind) {
    if (kind == L"artist") return RGB(192, 0, 4);
    if (kind == L"character") return RGB(0, 150, 32);
    if (kind == L"copyright") return RGB(160, 0, 170);
    if (kind == L"general") return RGB(0, 100, 220);
    if (kind == L"action" || kind == L"item") return RGB(204, 110, 0);
    return RGB(110, 110, 110);
  }

  // 角の丸いタグの札を描いて、その四角を返す
  RECT DrawChip(HDC dc, int x, int y, const std::wstring& text, COLORREF color, bool filled, bool closeMark = false) {
    SIZE sz = {};
    GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &sz);
    int h = Px(20), padX = Px(7), closeW = closeMark ? Px(14) : 0;
    RECT r = {x, y, x + sz.cx + padX * 2 + closeW, y + h};
    HBRUSH br = CreateSolidBrush(filled ? color : RGB(255, 255, 255));
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, Px(8), Px(8));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
    SetTextColor(dc, filled ? RGB(255, 255, 255) : color);
    RECT tr = {r.left + padX, r.top, r.right - padX - closeW, r.bottom};
    DrawTextW(dc, text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (closeMark) {
      RECT cr = {r.right - padX - closeW, r.top, r.right - Px(4), r.bottom};
      DrawTextW(dc, L"×", -1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    return r;
  }

  void LoadPreviewTags(const Item* it) {
    std::wstring path = it && it->image ? it->path : std::wstring();
    if (path == previewTagsPath) return;
    previewTagsPath = path;
    previewTags = path.empty() ? std::vector<ExplorerTag>() : host->ExplorerTags(path);
  }

  // タグの表示名が変わった: プレビューのタグを読み直して描き直す
  void RefreshTags() {
    previewTagsPath.clear();
    previewPropsPath.clear();
    const Item* it = sel >= 0 && sel < int(items.size()) ? &items[size_t(sel)] : nullptr;
    LoadPreviewTags(it);
    if (preview) InvalidateRect(preview, nullptr, FALSE);
    InvalidateRect(trayWnd, nullptr, FALSE);
  }

  void SaveTray() {
    settings.tray.clear();
    for (auto& t : tray) settings.tray.push_back(t.first);
    host->ExplorerSettingsChanged(settings);
    InvalidateRect(trayWnd, nullptr, FALSE);
  }

  void AddTray(const std::wstring& tag) {
    for (auto& t : tray)
      if (_wcsicmp(t.first.c_str(), tag.c_str()) == 0) {
        t.second = true;
        SaveTray();
        return;
      }
    tray.push_back({tag, true});
    SaveTray();
  }

  void TraySearch() {
    std::vector<std::wstring> tags;
    for (auto& t : tray)
      if (t.second) tags.push_back(t.first);
    if (tags.empty()) {
      MessageBeep(MB_OK);
      return;
    }
    host->ExplorerSearchTags(tags);
  }

  void PaintTray(HDC dc, const RECT& rc) {
    Fill(dc, rc, trayHot ? RGB(222, 238, 255) : RGB(244, 246, 250));
    Fill(dc, {rc.left, rc.bottom - 1, rc.right, rc.bottom}, RGB(220, 220, 220));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, nameFont);
    int x = Px(8), y = (rc.bottom - Px(20)) / 2;
    RECT lr = {x, 0, x + Px(70), rc.bottom};
    SetTextColor(dc, RGB(90, 90, 90));
    DrawTextW(dc, L"取り置き", -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    x += Px(64);
    // 右端に「組み合わせて検索」「クリア」（文字の幅に合わせる）
    auto chipW = [&](const wchar_t* t) {
      SIZE sz = {};
      GetTextExtentPoint32W(dc, t, int(wcslen(t)), &sz);
      return int(sz.cx) + Px(14);
    };
    int cw = chipW(L"クリア"), sw = chipW(L"組み合わせて検索");
    trayClearR = {rc.right - Px(8) - cw, y, rc.right - Px(8), y + Px(20)};
    traySearchR = {trayClearR.left - Px(6) - sw, y, trayClearR.left - Px(6), y + Px(20)};
    trayHits.clear();
    if (tray.empty()) {
      RECT tr = {x, 0, traySearchR.left, rc.bottom};
      SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
      DrawTextW(dc, L"プレビューのタグをここにドラッグすると取り置けます。取り置いたタグを組み合わせて検索できます（クリックで使う・使わない、× で外す）",
                -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    for (size_t i = 0; i < tray.size(); i++) {
      if (x > traySearchR.left - Px(40)) break;
      RECT r = DrawChip(dc, x, y, host->ExplorerTagLabel(tray[i].first), tray[i].second ? RGB(0, 100, 220) : RGB(150, 150, 150),
                        tray[i].second, true);
      trayHits.push_back({r, {r.right - Px(20), r.top, r.right, r.bottom}, i});
      x = r.right + Px(6);
    }
    DrawChip(dc, traySearchR.left, traySearchR.top, L"組み合わせて検索", RGB(0, 120, 60), true);
    DrawChip(dc, trayClearR.left, trayClearR.top, L"クリア", RGB(120, 120, 120), false);
    SelectObject(dc, oldFont);
  }

  void TrayClick(POINT pt) {
    if (PtInRect(&traySearchR, pt)) {
      TraySearch();
      return;
    }
    if (PtInRect(&trayClearR, pt)) {
      tray.clear();
      SaveTray();
      return;
    }
    for (const auto& h : trayHits) {
      if (PtInRect(&h.close, pt)) {
        tray.erase(tray.begin() + long(h.index));
        SaveTray();
        return;
      }
      if (PtInRect(&h.chip, pt)) {
        tray[h.index].second = !tray[h.index].second;
        SaveTray();
        return;
      }
    }
  }

  static LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<ExplorerImpl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
      self = reinterpret_cast<ExplorerImpl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(self));
      self->trayWnd = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC buf = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ old = SelectObject(buf, bmp);
        self->PaintTray(buf, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, buf, 0, 0, SRCCOPY);
        SelectObject(buf, old);
        DeleteObject(bmp);
        DeleteDC(buf);
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_LBUTTONUP:
        self->TrayClick(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  // プレビューのタグを押した・離した
  int TagAt(POINT pt) const {
    for (size_t i = 0; i < tagHits.size(); i++)
      if (PtInRect(&tagHits[i].r, pt)) return int(i);
    return -1;
  }

  void TagMenu(int hit, POINT screen) {
    const ExplorerTag& t = previewTags[tagHits[size_t(hit)].index];
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MENU_TAG_SEARCH, L"このタグで検索(&S)");
    AppendMenuW(menu, MF_STRING, MENU_TAG_TRAY, L"取り置く(&K)");
    std::wstring shown;  // メニューでは & が下線の印になるので重ねる
    for (wchar_t c : t.tag) shown += c == L'&' ? std::wstring(L"&&") : std::wstring(1, c);
    AppendMenuW(menu, MF_STRING, MENU_TAG_COPY, (L"コピー（" + shown + L"）(&C)").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MENU_TAG_RENAME, L"表示名を変える(&R)...");
    if (!previewTagsPath.empty()) {  // タグを出している画像
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, MENU_TAG_REMOVE, L"この画像からこのタグを外す(&X)");
    }
    const std::wstring tag = t.tag;  // 下で previewTags が読み直されることがある
    const std::wstring path = previewTagsPath;
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == MENU_TAG_SEARCH) host->ExplorerSearchTags({tag});
    if (cmd == MENU_TAG_TRAY) AddTray(tag);
    if (cmd == MENU_TAG_COPY) CopyText(tag);
    if (cmd == MENU_TAG_RENAME) host->ExplorerRenameTag(tag);
    if (cmd == MENU_TAG_REMOVE) host->ExplorerRemoveTag(path, tag);
  }

  // ---------------------------------------------------------------- 「同じ画像」

  void FillSameList() {
    fillingList = true;
    ShowWindow(sameView, resultsMode == 0 ? SW_SHOW : SW_HIDE);
    SendMessageW(sameList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(sameList);
    while (Header_GetItemCount(ListView_GetHeader(sameList)) > 0) ListView_DeleteColumn(sameList, 0);
    auto column = [&](int i, const wchar_t* title, int width, int fmt) {
      LVCOLUMNW c = {};
      c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
      c.fmt = fmt;
      c.cx = Px(width);
      c.pszText = const_cast<wchar_t*>(title);
      ListView_InsertColumn(sameList, i, &c);
    };
    auto add = [&](int row, std::initializer_list<std::wstring> cells) {
      int col = 0;
      for (const auto& s : cells) {
        std::wstring text = s;
        if (col == 0) {
          LVITEMW li = {};
          li.mask = LVIF_TEXT;
          li.iItem = row;
          li.pszText = text.data();
          ListView_InsertItem(sameList, &li);
        } else {
          ListView_SetItemText(sameList, row, col, text.data());
        }
        col++;
      }
    };
    if (resultsMode == 1) {
      column(0, L"重複（パスをクリックすると、エクスプローラーで表示します）", 900, LVCFMT_LEFT);
      for (size_t i = 0; i < groupRows.size(); i++)
        add(int(i), {groupRows[i].header ? L"■ " + groupRows[i].text : L"　　" + groupRows[i].path});
    } else if (settings.resultsView == 1) {
      column(0, L"フォルダ", 520, LVCFMT_LEFT);
      column(1, L"完全一致", 80, LVCFMT_RIGHT);
      column(2, L"見た目一致", 90, LVCFMT_RIGHT);
      for (size_t i = 0; i < sameFolders.size(); i++) {
        const auto& f = sameFolders[i];
        add(int(i), {f.folder, std::to_wstring(f.exact), std::to_wstring(f.similar)});
      }
    } else {
      column(0, L"一致", 110, LVCFMT_LEFT);
      column(1, L"名前", 220, LVCFMT_LEFT);
      column(2, L"フォルダ", 360, LVCFMT_LEFT);
      column(3, L"大きさ", 80, LVCFMT_RIGHT);
      column(4, L"解像度", 100, LVCFMT_RIGHT);
      for (size_t i = 0; i < same.size(); i++) {
        const auto& m = same[i];
        std::wstring kind = m.exact ? L"完全一致" : L"見た目 (距離 " + std::to_wstring(m.distance) + L")";
        std::wstring res = m.width > 0 ? std::to_wstring(m.width) + L" × " + std::to_wstring(m.height) : L"";
        add(int(i), {kind, NameOf(m.path), FolderOf(m.path), FormatBytes(m.size), res});
      }
    }
    SendMessageW(sameList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(sameList, nullptr, TRUE);
    fillingList = false;
  }

  void SetGroups(const std::wstring& title, std::vector<ExplorerGroup> groups) {
    resultsMode = 1;
    groupRows.clear();
    size_t paths = 0;
    for (auto& g : groups) {
      groupRows.push_back({true, g.title, L""});
      for (auto& p : g.paths) groupRows.push_back({false, L"", p});
      paths += g.paths.size();
    }
    SetWindowTextW(sameText, (title + L"　" + std::to_wstring(groups.size()) + L" グループ・" + std::to_wstring(paths) + L" 枚").c_str());
    FillSameList();
  }

  void SetSame(const std::wstring& title, std::vector<ExplorerMatch> matches) {
    resultsMode = 0;
    same = std::move(matches);
    sameFolders.clear();
    std::map<std::wstring, size_t> index;
    for (const auto& m : same) {
      std::wstring folder = FolderOf(m.path), key = folder;
      CharLowerBuffW(&key[0], DWORD(key.size()));
      auto it = index.find(key);
      if (it == index.end()) {
        it = index.emplace(key, sameFolders.size()).first;
        sameFolders.push_back({folder, 0, 0, m.path});
      }
      (m.exact ? sameFolders[it->second].exact : sameFolders[it->second].similar)++;
    }
    std::stable_sort(sameFolders.begin(), sameFolders.end(), [](const SameFolderRow& a, const SameFolderRow& b) {
      if (a.exact != b.exact) return a.exact > b.exact;
      return a.exact + a.similar > b.exact + b.similar;
    });
    int exact = 0;
    for (const auto& m : same) exact += m.exact ? 1 : 0;
    sameTitle = title;
    std::wstring text = title + L"　";
    if (same.empty()) {
      text += L"同じ画像は見つかりませんでした（インデックスを作ったフォルダの中だけを探します）";
    } else {
      text += std::to_wstring(same.size()) + L" 枚（完全一致 " + std::to_wstring(exact) + L"・見た目一致 " +
              std::to_wstring(same.size() - exact) + L"）、" + std::to_wstring(sameFolders.size()) + L" フォルダ";
    }
    SetWindowTextW(sameText, text.c_str());
    FillSameList();
  }

  // 一覧の i 番目へ移る (そのフォルダを開いて選ぶ)
  void GoToSame(int i) {
    if (i < 0) return;
    std::wstring target;
    if (resultsMode == 1) {
      if (i < int(groupRows.size())) {
        // 見出しなら、そのまとまりの最初の画像
        size_t k = size_t(i);
        while (k < groupRows.size() && groupRows[k].header) k++;
        if (k < groupRows.size()) target = groupRows[k].path;
      }
    } else if (settings.resultsView == 1) {
      if (i < int(sameFolders.size())) target = sameFolders[size_t(i)].first;
    } else if (i < int(same.size())) {
      target = same[size_t(i)].path;
    }
    if (target.empty()) return;
    Navigate(FolderOf(target), target);
  }

  void SameContextMenu(int i, POINT screen) {
    if (i < 0) return;
    std::wstring target;
    if (resultsMode == 1) {
      if (i < int(groupRows.size()) && !groupRows[size_t(i)].header) target = groupRows[size_t(i)].path;
    } else {
      target = settings.resultsView == 1 ? (i < int(sameFolders.size()) ? sameFolders[size_t(i)].folder : L"")
                                         : (i < int(same.size()) ? same[size_t(i)].path : L"");
    }
    if (target.empty()) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MENU_OPEN, L"ここへ移る(&G)");
    bool folderRow = resultsMode == 0 && settings.resultsView == 1;
    if (!folderRow) AppendMenuW(menu, MF_STRING, MENU_SAME, L"開く(&O)");
    AppendMenuW(menu, MF_STRING, MENU_SHOW, L"エクスプローラーで表示(&E)");
    AppendMenuW(menu, MF_STRING, MENU_COPY, L"パスをコピー(&C)");
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
      case MENU_OPEN: GoToSame(i); break;
      case MENU_SAME: host->ExplorerOpen(target); break;
      case MENU_SHOW:
        if (folderRow) ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else ShowInShell(target);
        break;
      case MENU_COPY: CopyText(target); break;
    }
  }

  // ---------------------------------------------------------------- 操作のバー

  int BarH() const { return Px(34); }
  int SplitW() const { return Px(5); }
  int SameHeaderH() const { return Px(28); }
  int TrayH() const { return Px(30); }

  // パネルの幅 (窓が狭い時はサムネイルの一覧に Px(160) は残す)
  void PanelWidths(int total, int* tw, int* pw) const {
    *tw = settings.showTree ? Px(settings.treeWidth) : 0;
    *pw = settings.showPreview ? Px(settings.previewWidth) : 0;
    int room = total - Px(160) - (settings.showTree ? SplitW() : 0) - (settings.showPreview ? SplitW() : 0);
    if (*tw + *pw > room) {
      double k = room > 0 && *tw + *pw > 0 ? double(room) / (*tw + *pw) : 0;
      *tw = int(*tw * k);
      *pw = int(*pw * k);
    }
  }

  // 下の「同じ画像」の高さ (窓が低い時は上に Px(160) は残す)
  int ResultsH(int total) const {
    return std::max(SameHeaderH() + Px(40), std::min(Px(settings.resultsHeight), total - BarH() - TrayH() - Px(160)));
  }

  void LayoutBar() {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int h = BarH(), m = Px(4), bh = h - m * 2, x = m;
    int nav = Px(30);
    MoveWindow(backBtn, x, m, nav, bh, TRUE);
    x += nav + Px(2);
    MoveWindow(forwardBtn, x, m, nav, bh, TRUE);
    x += nav + Px(2);
    MoveWindow(upBtn, x, m, nav, bh, TRUE);
    x += nav + Px(6);
    // 表示・並び順・大きさ・表紙・パネルの切り替えは、ホームのリボン（viewBar）にある
    MoveWindow(pathText, x, m + Px(1), std::max(Px(60), int(rc.right - m - x)), bh - Px(2), TRUE);
    // 取り置いたタグ
    MoveWindow(trayWnd, 0, h, rc.right, TrayH(), TRUE);
    h += TrayH();

    int resultsH = ResultsH(rc.bottom);
    int top = h, bodyH = std::max(0, int(rc.bottom - h - resultsH - SplitW()));
    int tw, pw;
    PanelWidths(rc.right, &tw, &pw);
    int gx = 0, gr = rc.right;
    ShowWindow(tree, settings.showTree ? SW_SHOW : SW_HIDE);
    ShowWindow(preview, settings.showPreview ? SW_SHOW : SW_HIDE);
    if (settings.showTree) {
      MoveWindow(tree, 0, top, tw, bodyH, TRUE);
      gx = tw + SplitW();
    }
    if (settings.showPreview) {
      MoveWindow(preview, rc.right - pw, top, pw, bodyH, TRUE);
      gr = rc.right - pw - SplitW();
    }
    MoveWindow(grid, gx, top, std::max(0, gr - gx), bodyH, TRUE);
    // 「同じ画像」
    int sy = top + bodyH + SplitW();
    int sameViewW = Px(120);
    MoveWindow(sameText, m, sy + Px(5), std::max(0, int(rc.right - sameViewW - m * 3)), SameHeaderH() - Px(6), TRUE);
    MoveWindow(sameView, rc.right - m - sameViewW, sy + Px(2), sameViewW, Px(200), TRUE);
    MoveWindow(sameList, 0, sy + SameHeaderH(), rc.right, std::max(0, int(rc.bottom - sy - SameHeaderH())), TRUE);
    InvalidateRect(hwnd, nullptr, TRUE);
  }

  // 境目の上か: 1 = フォルダ一覧の右、2 = プレビューの左、3 = 「同じ画像」の上
  int SplitAt(POINT pt) const {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (pt.y < BarH() + TrayH()) return 0;
    int resultsH = ResultsH(rc.bottom);
    int splitY = rc.bottom - resultsH - SplitW();
    if (pt.y >= splitY && pt.y < splitY + SplitW()) return 3;
    if (pt.y > splitY) return 0;
    int tw, pw;
    PanelWidths(rc.right, &tw, &pw);
    if (settings.showTree && pt.x >= tw && pt.x < tw + SplitW()) return 1;
    if (settings.showPreview && pt.x >= rc.right - pw - SplitW() && pt.x < rc.right - pw) return 2;
    return 0;
  }

  // ホームのリボンに置く部品（3 段: 表示・並び順 / 大きさ・表紙 / フォルダ一覧・プレビュー）
  SIZE ViewBarSize() const { return SIZE{Px(400), Px(80)}; }
  void LayoutViewBar() {
    const int rowH = Px(24), gap = Px(3), labelW = Px(46);
    int y = 0, x = 0;
    auto label = [&](HWND h, int w) {
      MoveWindow(h, x, y + Px(4), w, rowH - Px(4), TRUE);
      x += w;
    };
    auto put = [&](HWND h, int w, int dropH = 0) {
      MoveWindow(h, x, y, w, dropH ? dropH : rowH, TRUE);
      x += w + Px(4);
    };
    label(viewLabel, labelW);
    put(viewBox, Px(100), Px(200));
    x += Px(8);
    label(sortLabel, Px(50));
    put(sortBox, Px(126), Px(220));
    put(sortDirBtn, Px(48));
    y += rowH + gap;
    x = 0;
    label(sizeLabel, labelW);
    put(sizeBox, Px(64), Px(200));
    put(slider, Px(90));
    x += Px(4);
    label(folderLabel, Px(40));
    put(folderBox, Px(140), Px(200));
    y += rowH + gap;
    x = labelW;
    put(treeBtn, Px(96));
    put(previewBtn, Px(96));
  }

  // viewBar: 部品の通知を、エクスプローラーの窓（HostProc）に回す
  static LRESULT CALLBACK ViewBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<ExplorerImpl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->hwnd)
      switch (msg) {
        case WM_COMMAND:
        case WM_HSCROLL:
        case WM_NOTIFY:
        case WM_CTLCOLORSTATIC:
          return SendMessageW(self->hwnd, msg, wp, lp);
        case WM_SIZE:
          self->LayoutViewBar();
          return 0;
      }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  void TogglePanel(bool treePanel) {
    if (treePanel) settings.showTree = !settings.showTree;
    else settings.showPreview = !settings.showPreview;
    SendMessageW(treeBtn, BM_SETCHECK, settings.showTree ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(previewBtn, BM_SETCHECK, settings.showPreview ? BST_CHECKED : BST_UNCHECKED, 0);
    LayoutBar();
    if (treePanel && settings.showTree) SyncTree(dir);
    if (!treePanel && settings.showPreview) UpdatePreview(true);
    host->ExplorerSettingsChanged(settings);
    SetFocus(grid);
  }

  // ---------------------------------------------------------------- ウィンドウ

  static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<ExplorerImpl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
      self = reinterpret_cast<ExplorerImpl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
      case WM_SIZE:
        self->LayoutBar();
        return 0;
      case WM_TIMER:
        if (wp == kStateTimer && IsWindowVisible(hwnd)) self->RefreshStates();
        return 0;
      case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (HWND(wp) == hwnd && (self->dragSplit || self->SplitAt(pt))) {
          int s = self->dragSplit ? self->dragSplit : self->SplitAt(pt);
          SetCursor(LoadCursor(nullptr, s == 3 ? IDC_SIZENS : IDC_SIZEWE));
          return TRUE;
        }
        break;
      }
      case WM_LBUTTONDOWN:
        self->dragSplit = self->SplitAt(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (self->dragSplit) SetCapture(hwnd);
        return 0;
      case WM_MOUSEMOVE:
        if (self->dragSplit) {
          RECT rc;
          GetClientRect(hwnd, &rc);
          int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
          if (self->dragSplit == 3) {
            int logical = MulDiv(int(rc.bottom) - y - self->SplitW() / 2, 96, self->dpi);
            self->settings.resultsHeight = std::max(80, std::min(1200, logical));
          } else {
            int logical = self->dragSplit == 1 ? MulDiv(x, 96, self->dpi) : MulDiv(int(rc.right) - x - self->SplitW() / 2, 96, self->dpi);
            logical = std::max(120, std::min(800, logical));
            (self->dragSplit == 1 ? self->settings.treeWidth : self->settings.previewWidth) = logical;
          }
          self->LayoutBar();
          UpdateWindow(hwnd);
        }
        return 0;
      case WM_LBUTTONUP:
      case WM_CAPTURECHANGED:
        if (self->dragSplit) {
          self->dragSplit = 0;
          if (msg == WM_LBUTTONUP) ReleaseCapture();
          self->host->ExplorerSettingsChanged(self->settings);
          self->UpdatePreview();
        }
        return 0;
      case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->hwndFrom == self->tree) return self->TreeNotify(nm);
        if (nm->hwndFrom == self->sameList) {
          // 行を選んだら (クリック・矢印キー)、エクスプローラーでその画像を表示する
          if (nm->code == LVN_ITEMCHANGED && !self->fillingList) {
            auto* lv = reinterpret_cast<NMLISTVIEW*>(lp);
            if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)) self->GoToSame(lv->iItem);
          }
          if (nm->code == NM_DBLCLK || nm->code == NM_RETURN) {
            int i = ListView_GetNextItem(self->sameList, -1, LVNI_SELECTED);
            self->GoToSame(i);
            if (self->resultsMode == 1 || self->settings.resultsView == 0) {
              // ダブルクリックは開く
              std::wstring t = self->resultsMode == 1 ? (i >= 0 && i < int(self->groupRows.size()) ? self->groupRows[size_t(i)].path : L"")
                                                      : (i >= 0 && i < int(self->same.size()) ? self->same[size_t(i)].path : L"");
              if (!t.empty()) self->host->ExplorerOpen(t);
            }
          }
          if (nm->code == NM_RCLICK) {
            POINT pt;
            GetCursorPos(&pt);
            self->SameContextMenu(ListView_GetNextItem(self->sameList, -1, LVNI_SELECTED), pt);
          }
          return 0;
        }
        break;
      }
      case WM_COMMAND:
        switch (LOWORD(wp)) {
          case ID_UP:
            self->Up();
            SetFocus(self->grid);
            return 0;
          case ID_BACK:
            self->Back();
            SetFocus(self->grid);
            return 0;
          case ID_FORWARD:
            self->Forward();
            SetFocus(self->grid);
            return 0;
          case ID_TREE_BTN:
            self->TogglePanel(true);
            return 0;
          case ID_PREVIEW_BTN:
            self->TogglePanel(false);
            return 0;
          case ID_PATH:
            if (HIWORD(wp) == EN_SETFOCUS) PostMessageW(self->pathText, EM_SETSEL, 0, -1);
            return 0;
          case ID_SIZE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
              self->SetSize(int(SendMessageW(self->sizeBox, CB_GETCURSEL, 0, 0)), self->settings.customSize);
              SetFocus(self->grid);
            }
            return 0;
          case ID_FOLDER:
            if (HIWORD(wp) == CBN_SELCHANGE) {
              self->settings.folderStyle = int(SendMessageW(self->folderBox, CB_GETCURSEL, 0, 0));
              self->host->ExplorerSettingsChanged(self->settings);
              InvalidateRect(self->grid, nullptr, FALSE);
              self->UpdatePreview();
              SetFocus(self->grid);
            }
            return 0;
          case ID_VIEW:
            if (HIWORD(wp) == CBN_SELCHANGE) {
              self->settings.viewMode = int(SendMessageW(self->viewBox, CB_GETCURSEL, 0, 0));
              self->host->ExplorerSettingsChanged(self->settings);
              self->scrollY = 0;
              self->UpdateScroll();
              self->EnsureVisible(self->sel);
              InvalidateRect(self->grid, nullptr, FALSE);
              SetFocus(self->grid);
            }
            return 0;
          case ID_SORT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
              self->SetSort(int(SendMessageW(self->sortBox, CB_GETCURSEL, 0, 0)), self->settings.sortDesc);
              SetFocus(self->grid);
            }
            return 0;
          case ID_SORT_DIR:
            self->SetSort(self->SortKey(), !self->settings.sortDesc);
            SetFocus(self->grid);
            return 0;
          case ID_SAME_VIEW:
            if (HIWORD(wp) == CBN_SELCHANGE) {
              self->settings.resultsView = int(SendMessageW(self->sameView, CB_GETCURSEL, 0, 0));
              self->host->ExplorerSettingsChanged(self->settings);
              self->FillSameList();
            }
            return 0;
        }
        break;
      case WM_HSCROLL:
        if (HWND(lp) == self->slider) {
          int v = int(SendMessageW(self->slider, TBM_GETPOS, 0, 0));
          if (v != self->CurrentLogicalSize()) self->SetSize(3, v);
          if (LOWORD(wp) == TB_ENDTRACK) SetFocus(self->grid);
          return 0;
        }
        break;
      case WM_CTLCOLORSTATIC:
        SetBkMode(HDC(wp), TRANSPARENT);
        return LRESULT(GetSysColorBrush(COLOR_BTNFACE));
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  // アドレスバーとツリーのキー: Enter で移動、Esc で戻す、Tab で一覧へ
  static LRESULT CALLBACK ChildKeyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<ExplorerImpl*>(ref);
    if (msg == WM_GETDLGCODE && hwnd == self->pathText) return DefSubclassProc(hwnd, msg, wp, lp) | DLGC_WANTALLKEYS;
    if (msg == WM_KEYDOWN) {
      if (wp == VK_TAB) {
        SetFocus(self->grid);
        return 0;
      }
      if (hwnd == self->pathText && wp == VK_RETURN) {
        self->GoToAddress();
        return 0;
      }
      if (wp == VK_ESCAPE) {
        if (hwnd == self->pathText) SetWindowTextW(self->pathText, self->dir.c_str());
        SetFocus(self->grid);
        return 0;
      }
    }
    if (msg == WM_CHAR && hwnd == self->pathText && (wp == L'\r' || wp == 27 || wp == L'\t')) return 0;  // ビープを鳴らさない
    if (msg == WM_XBUTTONUP) {
      if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) self->Back();
      else self->Forward();
      return TRUE;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
  }

  static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<ExplorerImpl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
      self = reinterpret_cast<ExplorerImpl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(self));
      self->preview = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC buf = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ old = SelectObject(buf, bmp);
        self->PaintPreview(buf, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, buf, 0, 0, SRCCOPY);
        SelectObject(buf, old);
        DeleteObject(bmp);
        DeleteDC(buf);
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        if (!self->dragSplit) self->UpdatePreview();
        return 0;
      case WM_LBUTTONDBLCLK:
        if (self->TagAt(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}) < 0) self->Open(self->sel);
        return 0;
      case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (self->tagDragging || self->TagAt(pt) >= 0) {
          SetCursor(LoadCursor(nullptr, self->tagDragging ? IDC_SIZEALL : IDC_HAND));
          return TRUE;
        }
        break;
      }
      case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (const auto& [r, mode] : self->previewTabHits)
          if (PtInRect(&r, pt)) {
            self->SetPreviewMode(mode);
            return 0;
          }
        self->tagDown = self->TagAt(pt);
        self->tagDragging = false;
        self->tagDownPt = pt;
        if (self->tagDown >= 0) SetCapture(hwnd);
        else SetFocus(self->grid);
        return 0;
      }
      case WM_MOUSEMOVE:
        if (self->tagDown >= 0 && GetCapture() == hwnd) {
          POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
          if (!self->tagDragging && (abs(pt.x - self->tagDownPt.x) > self->Px(4) || abs(pt.y - self->tagDownPt.y) > self->Px(4)))
            self->tagDragging = true;
          if (self->tagDragging) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            POINT sp = pt;
            ClientToScreen(hwnd, &sp);
            bool hot = WindowFromPoint(sp) == self->trayWnd;
            if (hot != self->trayHot) {
              self->trayHot = hot;
              InvalidateRect(self->trayWnd, nullptr, FALSE);
            }
          }
        }
        return 0;
      case WM_LBUTTONUP:
        if (self->tagDown >= 0 && GetCapture() == hwnd) {
          ReleaseCapture();
          int hit = self->tagDown;
          self->tagDown = -1;
          if (hit < int(self->tagHits.size())) {
            const std::wstring tag = self->previewTags[self->tagHits[size_t(hit)].index].tag;
            if (self->tagDragging) {
              POINT sp = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
              ClientToScreen(hwnd, &sp);
              if (WindowFromPoint(sp) == self->trayWnd) self->AddTray(tag);
            } else {
              self->host->ExplorerSearchTags({tag});
            }
          }
          self->tagDragging = false;
          if (self->trayHot) {
            self->trayHot = false;
            InvalidateRect(self->trayWnd, nullptr, FALSE);
          }
        }
        return 0;
      case WM_RBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hit = self->TagAt(pt);
        if (hit >= 0) {
          ClientToScreen(hwnd, &pt);
          self->TagMenu(hit, pt);
        }
        return 0;
      }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  static LRESULT CALLBACK GridProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<ExplorerImpl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
      self = reinterpret_cast<ExplorerImpl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(self));
      self->grid = hwnd;  // CreateWindow から戻る前のメッセージでも使えるように
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->Grid(msg, wp, lp);
  }

  LRESULT Grid(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(grid, &ps);
        RECT rc;
        GetClientRect(grid, &rc);
        HDC buf = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ old = SelectObject(buf, bmp);
        Paint(buf, rc);
        PaintBand(buf);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, buf, 0, 0, SRCCOPY);
        SelectObject(buf, old);
        DeleteObject(bmp);
        DeleteDC(buf);
        EndPaint(grid, &ps);
        return 0;
      }
      case WM_SIZE:
        UpdateScroll();
        InvalidateRect(grid, nullptr, FALSE);
        return 0;
      case WM_THUMB_DONE:
        OnThumb(reinterpret_cast<ThumbResult*>(lp));
        return 0;
      case WM_VSCROLL: {
        RECT rc;
        GetClientRect(grid, &rc);
        int y = scrollY;
        switch (LOWORD(wp)) {
          case SB_LINEUP: y -= CellH() / 3; break;
          case SB_LINEDOWN: y += CellH() / 3; break;
          case SB_PAGEUP: y -= rc.bottom; break;
          case SB_PAGEDOWN: y += rc.bottom; break;
          case SB_TOP: y = 0; break;
          case SB_BOTTOM: y = TotalHeight(); break;
          case SB_THUMBTRACK:
          case SB_THUMBPOSITION: {
            SCROLLINFO si = {sizeof(si), SIF_TRACKPOS};
            GetScrollInfo(grid, SB_VERT, &si);
            y = si.nTrackPos;
            break;
          }
        }
        ScrollTo(y);
        return 0;
      }
      case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
          // Ctrl + ホイールで大きさを変える (可変になる)
          int step = delta > 0 ? 16 : -16;
          SetSize(3, CurrentLogicalSize() + step);
        } else {
          ScrollTo(scrollY - delta * CellH() / 2 / WHEEL_DELTA);
        }
        return 0;
      }
      case WM_MOUSEMOVE: {
        if (banding) {
          UpdateBand(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
          return 0;
        }
        if (dragArmed && (wp & MK_LBUTTON) &&
            (abs(GET_X_LPARAM(lp) - dragFrom.x) > GetSystemMetrics(SM_CXDRAG) ||
             abs(GET_Y_LPARAM(lp) - dragFrom.y) > GetSystemMetrics(SM_CYDRAG))) {
          dragArmed = false;
          collapseOnUp = -1;
          ReleaseCapture();
          StartDragOut();
          return 0;
        }
        if (!tracking) {
          TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, grid, 0};
          TrackMouseEvent(&tme);
          tracking = true;
        }
        int h = HitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (h != hover) {
          hover = h;
          InvalidateRect(grid, nullptr, FALSE);
        }
        return 0;
      }
      case WM_MOUSELEAVE:
        tracking = false;
        hover = -1;
        InvalidateRect(grid, nullptr, FALSE);
        return 0;
      case WM_LBUTTONDOWN:
      case WM_RBUTTONDOWN: {
        SetFocus(grid);
        if (msg == WM_LBUTTONDOWN && GET_Y_LPARAM(lp) < HeaderH()) {
          // 列の見出し: その列で並べる (同じ列なら向きを変える)
          for (const auto& c : DetailColumns())
            if (GET_X_LPARAM(lp) >= c.left && GET_X_LPARAM(lp) < c.right && c.sortKey >= 0)
              SetSort(c.sortKey, c.sortKey == SortKey() ? !settings.sortDesc : false);
          return 0;
        }
        int h = HitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        bool ctrl = (wp & MK_CONTROL) != 0, shift = (wp & MK_SHIFT) != 0;
        if (h >= 0) {
          // 右クリックは、選んでいるものの上なら選び方を変えない (選んだものに対するメニュー)
          if (msg == WM_RBUTTONDOWN && IsPicked(h)) {
            if (h != sel) {
              sel = h;
              InvalidateRect(grid, nullptr, FALSE);
              UpdatePreview();
              NotifySelected();
            }
          } else if (msg == WM_LBUTTONDOWN && !ctrl && !shift && multi && IsPicked(h)) {
            // いくつも選んだまま外へドラッグできるよう、1 つにするのは離した時
            collapseOnUp = h;
          } else {
            SelectWith(h, msg == WM_LBUTTONDOWN && ctrl, msg == WM_LBUTTONDOWN && shift);
          }
          if (msg == WM_LBUTTONDOWN && !shift) {
            dragArmed = true;
            dragFrom = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            SetCapture(grid);
          }
        } else if (msg == WM_LBUTTONDOWN && !items.empty()) {
          // 空いている所から: 枠で囲んで選ぶ
          banding = true;
          bandMoved = false;
          bandFrom = bandTo = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp) + scrollY};
          bandBase.assign(items.size(), 0);
          if (ctrl)
            for (int i = 0; i < int(items.size()); i++) bandBase[size_t(i)] = IsPicked(i);
          SetCapture(grid);
        }
        return 0;
      }
      case WM_LBUTTONUP:
        if (dragArmed) {
          dragArmed = false;
          ReleaseCapture();
          if (collapseOnUp >= 0) Select(collapseOnUp);
          collapseOnUp = -1;
        }
        if (banding) {
          banding = false;
          ReleaseCapture();
          if (bandMoved) {
            multi = true;
            if (!IsPicked(sel)) {
              auto picked = PickedIndices();
              sel = picked.empty() ? sel : picked.front();
            }
            InvalidateRect(grid, nullptr, FALSE);
            UpdatePreview();
            if (preview) InvalidateRect(preview, nullptr, FALSE);
            NotifySelected();
          }
        }
        return 0;
      case WM_CAPTURECHANGED:
        dragArmed = false;
        if (banding) {
          banding = false;
          InvalidateRect(grid, nullptr, FALSE);
        }
        return 0;
      case WM_RBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int h = HitTest(pt);
        if (h >= 0) {
          ClientToScreen(grid, &pt);
          ContextMenu(h, pt);
        }
        return 0;
      }
      case WM_LBUTTONDBLCLK: {
        int h = HitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (h >= 0) Open(h);
        return 0;
      }
      case WM_XBUTTONUP:
        // マウスの戻る・進むボタン
        if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) Back();
        else Forward();
        return TRUE;
      case WM_GETDLGCODE: {
        // いくつも選んでいる時の Esc (選ぶのをやめる) は、ダイアログの「キャンセル」にしない
        const MSG* m = reinterpret_cast<const MSG*>(lp);
        if (m && m->message == WM_KEYDOWN && m->wParam == VK_ESCAPE && multi)
          return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB | DLGC_WANTMESSAGE;
        return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
      }
      case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
        int cols = Cols();
        RECT rc;
        GetClientRect(grid, &rc);
        int page = std::max(1, int(rc.bottom / CellH())) * cols;
        bool shift = GetKeyState(VK_SHIFT) < 0;
        auto go = [&](int i) { SelectWith(i, false, shift); };  // Shift を押していれば範囲を選ぶ
        switch (wp) {
          case VK_LEFT: go(sel - 1); return 0;
          case VK_RIGHT: go(sel + 1); return 0;
          case VK_UP:
            if (alt) Up();
            else go(sel - cols);
            return 0;
          case VK_DOWN: go(sel + cols); return 0;
          case VK_PRIOR: go(sel - page); return 0;
          case VK_NEXT: go(sel + page); return 0;
          case VK_HOME: go(0); return 0;
          case VK_END: go(int(items.size()) - 1); return 0;
          case 'A':
            if (ctrl) {
              SelectAll();
              return 0;
            }
            break;
          case VK_ESCAPE:
            if (multi) {
              Select(sel);
              return 0;
            }
            break;
          case VK_RETURN: Open(sel); return 0;
          case VK_BACK: Up(); return 0;
          case VK_F5: Load(dir, SelectedPath()); return 0;
          case VK_TAB:
            // Tab でフォルダ一覧へ、Shift + Tab でアドレスバーへ
            if (GetKeyState(VK_SHIFT) < 0 || !settings.showTree) SetFocus(pathText);
            else SetFocus(tree);
            return 0;
          case 'V':
            if (ctrl) {
              Paste();
              return 0;
            }
            break;
          case 'L':
          case 'D':
            if ((ctrl && wp == 'L') || (alt && wp == 'D')) {
              SetFocus(pathText);
              return 0;
            }
            break;
          case VK_ADD:
          case VK_OEM_PLUS:
            if (ctrl) {
              SetSize(3, CurrentLogicalSize() + 16);
              return 0;
            }
            break;
          case VK_SUBTRACT:
          case VK_OEM_MINUS:
            if (ctrl) {
              SetSize(3, CurrentLogicalSize() - 16);
              return 0;
            }
            break;
        }
        return 0;
      }
      case WM_SYSKEYDOWN:
        if (wp == VK_UP) {
          Up();
          return 0;
        }
        if (wp == VK_LEFT) {
          Back();
          return 0;
        }
        if (wp == VK_RIGHT) {
          Forward();
          return 0;
        }
        if (wp == 'D') {
          SetFocus(pathText);
          return 0;
        }
        break;
    }
    return DefWindowProcW(grid, msg, wp, lp);
  }

  bool Create(HWND parent, ExplorerHost* h, const ExplorerSettings& s) {
    host = h;
    settings = s;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);
    WNDCLASSEXW gc = {sizeof(gc)};
    gc.style = CS_DBLCLKS;
    gc.lpfnWndProc = GridProc;
    gc.hInstance = inst;
    gc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    gc.lpszClassName = kGridClass;
    RegisterClassExW(&gc);
    WNDCLASSEXW pc = {sizeof(pc)};
    pc.style = CS_DBLCLKS;
    pc.lpfnWndProc = PreviewProc;
    pc.hInstance = inst;
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.lpszClassName = kPreviewClass;
    RegisterClassExW(&pc);
    WNDCLASSEXW vc = {sizeof(vc)};
    vc.lpfnWndProc = ViewBarProc;
    vc.hInstance = inst;
    vc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    vc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    vc.lpszClassName = kViewBarClass;
    RegisterClassExW(&vc);
    WNDCLASSEXW tc = {sizeof(tc)};
    tc.lpfnWndProc = TrayProc;
    tc.hInstance = inst;
    tc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    tc.lpszClassName = kTrayClass;
    RegisterClassExW(&tc);
    HDC sdc = GetDC(nullptr);
    dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(nullptr, sdc);
    hwnd = CreateWindowExW(0, kHostClass, nullptr, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 100, 100, parent, nullptr, inst, this);
    if (!hwnd) return false;
    font = CreateFontW(-Px(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Meiryo UI");
    nameFont = CreateFontW(-Px(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Meiryo UI");
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
      HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd, HMENU(INT_PTR(id)), inst, nullptr);
      SendMessageW(c, WM_SETFONT, WPARAM(font), TRUE);
      return c;
    };
    // 表示の設定の部品はホームのリボンに置く: 親はホストの窓の子の入れ物。入れ物は通知をこの窓へ回す
    viewBar = CreateWindowExW(WS_EX_CONTROLPARENT, kViewBarClass, nullptr, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, parent, nullptr,
                              inst, this);
    auto makeView = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
      HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 10, 10, viewBar, HMENU(INT_PTR(id)), inst,
                               nullptr);
      SendMessageW(c, WM_SETFONT, WPARAM(font), TRUE);
      return c;
    };
    backBtn = make(L"BUTTON", L"←", BS_PUSHBUTTON, ID_BACK);
    forwardBtn = make(L"BUTTON", L"→", BS_PUSHBUTTON, ID_FORWARD);
    upBtn = make(L"BUTTON", L"↑", BS_PUSHBUTTON, ID_UP);
    treeBtn = makeView(L"BUTTON", L"フォルダ一覧", BS_AUTOCHECKBOX | BS_PUSHLIKE, ID_TREE_BTN);
    previewBtn = makeView(L"BUTTON", L"プレビュー", BS_AUTOCHECKBOX | BS_PUSHLIKE, ID_PREVIEW_BTN);
    SendMessageW(treeBtn, BM_SETCHECK, settings.showTree ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(previewBtn, BM_SETCHECK, settings.showPreview ? BST_CHECKED : BST_UNCHECKED, 0);
    pathText = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
                               HMENU(INT_PTR(ID_PATH)), inst, nullptr);
    SendMessageW(pathText, WM_SETFONT, WPARAM(font), TRUE);
    SendMessageW(pathText, EM_SETCUEBANNER, TRUE, LPARAM(L"フォルダ、または画像の URL（同じ画像を探す）"));
    SHAutoComplete(pathText, SHACF_FILESYS_DIRS);  // フォルダの名前を補う
    SetWindowSubclass(pathText, ChildKeyProc, 1, DWORD_PTR(this));
    sizeLabel = makeView(L"STATIC", L"大きさ", SS_LEFT, ID_SIZE_LABEL);
    sizeBox = makeView(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_SIZE);
    for (const wchar_t* t : {L"小", L"普通", L"大", L"可変"}) SendMessageW(sizeBox, CB_ADDSTRING, 0, LPARAM(t));
    slider = makeView(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, ID_SLIDER);
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(kMinSize, kMaxSize));
    SendMessageW(slider, TBM_SETPAGESIZE, 0, 32);
    folderLabel = makeView(L"STATIC", L"表紙", SS_LEFT, ID_FOLDER_LABEL);
    viewLabel = makeView(L"STATIC", L"表示", SS_LEFT, ID_FOLDER_LABEL);
    sortLabel = makeView(L"STATIC", L"並び順", SS_LEFT, ID_FOLDER_LABEL);
    viewBox = makeView(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_VIEW);
    for (const wchar_t* t : {L"サムネイル", L"詳細"}) SendMessageW(viewBox, CB_ADDSTRING, 0, LPARAM(t));
    settings.viewMode = std::max(0, std::min(1, settings.viewMode));
    SendMessageW(viewBox, CB_SETCURSEL, settings.viewMode, 0);
    sortBox = makeView(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_SORT);
    for (const wchar_t* t : {L"名前順", L"更新日時順", L"種類順", L"サイズ順", L"検索結果の順"}) SendMessageW(sortBox, CB_ADDSTRING, 0, LPARAM(t));
    settings.sortKey = std::max(0, std::min(3, settings.sortKey));
    sortDirBtn = makeView(L"BUTTON", L"昇順", BS_PUSHBUTTON, ID_SORT_DIR);
    folderBox = makeView(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_FOLDER);
    for (const wchar_t* t : {L"1 枚目", L"1・2 枚目", L"最後と 1 枚目", L"最後", L"最後の一つ前と最後"})
      SendMessageW(folderBox, CB_ADDSTRING, 0, LPARAM(t));
    settings.folderStyle = std::max(0, std::min(4, settings.folderStyle));
    SendMessageW(folderBox, CB_SETCURSEL, settings.folderStyle, 0);
    grid = CreateWindowExW(WS_EX_CLIENTEDGE, kGridClass, nullptr, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP, 0, 0, 10,
                           10, hwnd, nullptr, inst, this);
    tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, nullptr,
                           WS_CHILD | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_TRACKSELECT,
                           0, 0, 10, 10, hwnd, HMENU(INT_PTR(ID_TREE)), inst, nullptr);
    SendMessageW(tree, WM_SETFONT, WPARAM(font), TRUE);
    SetWindowSubclass(tree, ChildKeyProc, 1, DWORD_PTR(this));
    TreeInit();
    CreateWindowExW(0, kPreviewClass, nullptr, WS_CHILD, 0, 0, 10, 10, hwnd, nullptr, inst, this);
    CreateWindowExW(0, kTrayClass, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, nullptr, inst, this);
    for (const auto& t : settings.tray) tray.push_back({t, true});
    // 「同じ画像」
    sameText = make(L"STATIC", L"画像を選ぶか、画像のファイル・URL をここにドロップすると、同じ画像（インデックスを作ったフォルダの中）を探します。"
                               L"　右下の印 — 緑: インデックス作成済み、橙: 処理待ち、白: 未作成、半分: 一部",
                    SS_LEFTNOWORDWRAP | SS_NOPREFIX | SS_ENDELLIPSIS, ID_SAME_TEXT);
    sameView = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_SAME_VIEW);
    for (const wchar_t* t : {L"画像ごと", L"フォルダごと"}) SendMessageW(sameView, CB_ADDSTRING, 0, LPARAM(t));
    settings.resultsView = std::max(0, std::min(1, settings.resultsView));
    SendMessageW(sameView, CB_SETCURSEL, settings.resultsView, 0);
    // マウスを乗せた時の説明
    {
      HWND tip = winutil::makeTooltip(hwnd, Px(360));
      const std::pair<HWND, const wchar_t*> tips[] = {
          {backBtn, L"前に見ていた場所に戻る"},
          {forwardBtn, L"戻る前の場所に進む"},
          {upBtn, L"1 つ上のフォルダへ（検索の結果からは前のフォルダへ）"},
          {treeBtn, L"左のフォルダ一覧を出す・隠す"},
          {previewBtn, L"右のプレビュー（画像・タグ・プロパティ）を出す・隠す"},
          {pathText, L"フォルダのパスを入れて Enter で開きます。画像の URL を入れると、同じ画像を探します"},
          {viewBox, L"サムネイルで並べるか、詳細（名前・日時・大きさなどの列）で並べるか"},
          {sortBox, L"並べ順"},
          {sortDirBtn, L"昇順と降順を切り替えます"},
          {sizeBox, L"サムネイルの大きさ（可変ならスライダーで変えられます）"},
          {slider, L"サムネイルの大きさ（「可変」の時）"},
          {folderBox, L"フォルダの表紙に、中のどの画像を出すか"},
          {sameView, L"同じ画像の一覧を、画像ごとに並べるか、フォルダごとにまとめるか"},
      };
      for (const auto& [h, text] : tips) winutil::addTooltip(tip, h, text);
    }
    sameList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0, 10, 10,
                               hwnd, HMENU(INT_PTR(ID_SAME_LIST)), inst, nullptr);
    SendMessageW(sameList, WM_SETFONT, WPARAM(font), TRUE);
    ListView_SetExtendedListViewStyle(sameList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    FillSameList();
    settings.treeWidth = std::max(120, std::min(800, settings.treeWidth));
    settings.previewWidth = std::max(120, std::min(800, settings.previewWidth));
    settings.resultsHeight = std::max(80, std::min(1200, settings.resultsHeight));
    settings.sizeMode = std::max(0, std::min(3, settings.sizeMode));
    settings.customSize = std::max(kMinSize, std::min(kMaxSize, settings.customSize));
    SendMessageW(sizeBox, CB_SETCURSEL, settings.sizeMode, 0);
    SendMessageW(slider, TBM_SETPOS, TRUE, CurrentLogicalSize());
    UpdateSortBox();
    // ドロップ (ファイル・ブラウザーからの画像の URL)
    OleInitialize(nullptr);
    drop = new DropTarget([this](const std::vector<std::wstring>& files, const std::wstring& url) { Dropped(files, url); });
    RegisterDragDrop(hwnd, drop);
    RegisterDragDrop(grid, drop);
    RegisterDragDrop(sameList, drop);
    SetTimer(hwnd, kStateTimer, 5000, nullptr);
    StartWorkers();
    return true;
  }
};

Explorer::Explorer() : impl_(new ExplorerImpl) {}
Explorer::~Explorer() {
  if (impl_->hwnd) {
    RevokeDragDrop(impl_->grid);
    RevokeDragDrop(impl_->sameList);
    RevokeDragDrop(impl_->hwnd);
    DestroyWindow(impl_->hwnd);
  }
  if (impl_->drop) impl_->drop->Release();
}

IDropTarget* MakeFileDropTarget(std::function<void(const std::vector<std::wstring>&, const std::wstring&)> onDrop) {
  return new DropTarget(std::move(onDrop));
}

bool Explorer::Create(HWND parent, ExplorerHost* host, const ExplorerSettings& settings) {
  return impl_->Create(parent, host, settings);
}

void Explorer::Show(const std::wstring& dir, const std::wstring& select) {
  ShowWindow(impl_->hwnd, SW_SHOW);
  impl_->LayoutBar();
  if (!dir.empty() && (impl_->dir.empty() || _wcsicmp(dir.c_str(), impl_->dir.c_str()) != 0 || !select.empty()))
    impl_->Navigate(dir, select);
  else if (!impl_->dir.empty())
    impl_->RefreshStates();
  SetFocus(impl_->grid);
}

void Explorer::Hide() {
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->queue.clear();
  }
  ShowWindow(impl_->hwnd, SW_HIDE);
}

bool Explorer::Visible() const { return impl_->hwnd && IsWindowVisible(impl_->hwnd); }

void Explorer::Move(const RECT& r) {
  MoveWindow(impl_->hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
}

HWND Explorer::Window() const { return impl_->hwnd; }
HWND Explorer::ViewBar() const { return impl_->viewBar; }
SIZE Explorer::ViewBarSize() const { return impl_->ViewBarSize(); }

std::wstring Explorer::Dir() const { return impl_->dir; }

std::wstring Explorer::Selected() const { return impl_->SelectedPath(); }

std::vector<std::wstring> Explorer::SelectedPaths() const { return impl_->SelectedPaths(); }

bool Explorer::ShowingList() const { return impl_->listMode; }

void Explorer::RefreshStates() { impl_->RefreshStates(); }

void Explorer::RefreshTags() { impl_->RefreshTags(); }

void Explorer::SetSameStatus(const std::wstring& text) { SetWindowTextW(impl_->sameText, text.c_str()); }

void Explorer::SetSame(const std::wstring& title, std::vector<ExplorerMatch> matches) { impl_->SetSame(title, std::move(matches)); }

void Explorer::ShowList(const std::wstring& title, std::vector<std::vector<std::wstring>> entries, int select) {
  impl_->ShowList(title, std::move(entries), select);
  SetFocus(impl_->grid);
}

void Explorer::SetGroups(const std::wstring& title, std::vector<ExplorerGroup> groups) { impl_->SetGroups(title, std::move(groups)); }

void Explorer::AddTrayTag(const std::wstring& tag) { impl_->AddTray(tag); }
