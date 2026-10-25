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
constexpr UINT WM_THUMB_DONE = WM_APP + 41;  // lParam = ThumbResult*
constexpr UINT_PTR kStateTimer = 1;
enum {
  ID_UP = 1, ID_PATH, ID_SIZE, ID_SLIDER, ID_FOLDER, ID_SIZE_LABEL, ID_FOLDER_LABEL,
  ID_BACK, ID_FORWARD, ID_TREE_BTN, ID_PREVIEW_BTN, ID_TREE, ID_SAME_TEXT, ID_SAME_VIEW, ID_SAME_LIST
};
enum { MENU_OPEN = 1, MENU_SHOW, MENU_COPY, MENU_SAME, MENU_ADD_ROOT };

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
    ok_ = Accepts(data);
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
  HWND sizeLabel = nullptr, folderLabel = nullptr;
  HWND backBtn = nullptr, forwardBtn = nullptr, treeBtn = nullptr, previewBtn = nullptr;
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
  int sel = -1, hover = -1;
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
  int CellW() const { return ThumbSize() + Pad() * 2; }
  int CellH() const { return ThumbSize() + Pad() * 2 + TextH(); }
  int Cols() const {
    RECT rc;
    GetClientRect(grid, &rc);
    return std::max(1, int((rc.right - Pad()) / CellW()));
  }
  int OffsetX() const {
    RECT rc;
    GetClientRect(grid, &rc);
    return std::max(0, int((rc.right - Cols() * CellW()) / 2));
  }
  RECT CellRect(int i) const {
    int c = Cols();
    int x = OffsetX() + (i % c) * CellW(), y = Pad() + (i / c) * CellH() - scrollY;
    return RECT{x, y, x + CellW(), y + CellH()};
  }
  int TotalHeight() const {
    int rows = (int(items.size()) + Cols() - 1) / Cols();
    return Pad() * 2 + rows * CellH();
  }
  int HitTest(POINT pt) const {
    for (int i = 0; i < int(items.size()); i++) {
      RECT r = CellRect(i);
      if (PtInRect(&r, pt)) return i;
    }
    return -1;
  }

  // ---------------------------------------------------------------- 一覧

  void Load(const std::wstring& d, const std::wstring& select) {
    dir = d;
    items.clear();
    sel = hover = -1;
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
        items.push_back(std::move(it));
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    // フォルダを先に、それぞれ名前順
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
      if (a.dir != b.dir) return a.dir;
      return NaturalLess(a.name, b.name);
    });
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

  void NotifySelected() {
    const Item* it = sel >= 0 && sel < int(items.size()) ? &items[size_t(sel)] : nullptr;
    host->ExplorerSelected(it ? it->path : std::wstring(), it && it->image);
  }

  void UpdateNavButtons() {
    EnableWindow(upBtn, !ParentDir(dir).empty());
    EnableWindow(backBtn, !backStack.empty());
    EnableWindow(forwardBtn, !forwardStack.empty());
  }

  // 別のフォルダへ (戻るの履歴に今の場所を積む)
  void Navigate(const std::wstring& d, const std::wstring& select) {
    if (!dir.empty() && _wcsicmp(d.c_str(), dir.c_str()) != 0) {
      backStack.push_back({dir, SelectedPath()});
      if (backStack.size() > 100) backStack.erase(backStack.begin());
      forwardStack.clear();
    }
    if (_wcsicmp(d.c_str(), dir.c_str()) == 0 && !select.empty()) {
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
    forwardStack.push_back({dir, SelectedPath()});
    auto to = backStack.back();
    backStack.pop_back();
    Load(to.first, to.second);
  }

  void Forward() {
    if (forwardStack.empty()) {
      MessageBeep(MB_OK);
      return;
    }
    backStack.push_back({dir, SelectedPath()});
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
    else host->ExplorerOpen(it.path);
  }

  void Up() {
    std::wstring parent = ParentDir(dir);
    if (parent.empty()) {
      MessageBeep(MB_OK);
      return;
    }
    std::wstring from = dir;
    while (from.size() > 3 && (from.back() == L'\\' || from.back() == L'/')) from.pop_back();
    Navigate(parent, from);
  }

  // 右クリックのメニュー
  void ContextMenu(int i, POINT screen) {
    if (i < 0 || i >= int(items.size())) return;
    const Item& it = items[size_t(i)];
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MENU_OPEN, L"開く(&O)");
    AppendMenuW(menu, MF_STRING, MENU_SHOW, L"エクスプローラーで表示(&E)");
    AppendMenuW(menu, MF_STRING, MENU_COPY, L"パスをコピー(&C)");
    if (it.image) {
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, MENU_SAME, L"同じ画像を探す(&S)");
    }
    if (it.dir && it.state != kStateIndexed) {
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, MENU_ADD_ROOT, L"このフォルダのインデックスを作る(&I)");
    }
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    std::wstring path = it.path;
    switch (cmd) {
      case MENU_OPEN: Open(i); break;
      case MENU_SHOW: ShowInShell(path); break;
      case MENU_COPY: CopyText(path); break;
      case MENU_SAME: host->ExplorerFindSameFile(path); break;
      case MENU_ADD_ROOT:
        host->ExplorerAddRoot(path);
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
    if (r.top < 0) ScrollTo(scrollY + r.top - Pad());
    else if (r.bottom > rc.bottom) ScrollTo(scrollY + (r.bottom - rc.bottom) + Pad());
  }

  void Select(int i) {
    if (items.empty()) return;
    int old = sel;
    sel = std::max(0, std::min(i, int(items.size()) - 1));
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
        RECT r = FitRect(p.w, p.h, {box.left, box.top, box.right - Px(4), box.bottom - Px(4)}, true);
        DropShadow(dc, mem, r);
        Fill(dc, r, RGB(255, 255, 255));
        Blit(dc, mem, p, r);
        Frame(dc, r, RGB(200, 200, 200));
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

  void Paint(HDC dc, const RECT& rc) {
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
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
      if (i == sel || i == hover) {
        Fill(dc, inner, i == sel ? RGB(204, 232, 255) : RGB(229, 243, 255));
        if (i == sel) Frame(dc, inner, RGB(153, 209, 255));
      }
      RECT box = {cell.left + Pad(), cell.top + Pad(), cell.left + Pad() + ts, cell.top + Pad() + ts};
      const Thumb* t = CachedThumb(it);
      if (it.Thumbable() && (!t || t->bucket < bucket)) requested = true;
      DrawThumb(dc, mem, it, t, box);
      RECT tr = {cell.left + Px(4), box.bottom + Px(4), cell.right - Px(4), cell.bottom - Px(2)};
      SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
      DrawTextW(dc, it.name.c_str(), -1, &tr, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
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

  int PreviewBox() const {
    RECT rc;
    GetClientRect(preview, &rc);
    int w = rc.right - Px(24);
    return std::max(Px(48), std::min({w, int(rc.bottom) * 2 / 3, 1024}));
  }

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
    HDC mem = CreateCompatibleDC(dc);
    int boxSize = PreviewBox();
    int m = Px(12);
    RECT box = {(rc.right - boxSize) / 2, m, (rc.right - boxSize) / 2 + boxSize, m + boxSize};
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
    RECT ir = {nr.left, measure.bottom + Px(8), nr.right, nr.bottom};
    SetTextColor(dc, RGB(96, 96, 96));
    DrawTextW(dc, info.c_str(), -1, &ir, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, oldFont);
  }

  // ---------------------------------------------------------------- 「同じ画像」

  void FillSameList() {
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
    if (settings.resultsView == 1) {
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
  }

  void SetSame(const std::wstring& title, std::vector<ExplorerMatch> matches) {
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
    if (settings.resultsView == 1) {
      if (i < int(sameFolders.size())) target = sameFolders[size_t(i)].first;
    } else if (i < int(same.size())) {
      target = same[size_t(i)].path;
    }
    if (target.empty()) return;
    Navigate(FolderOf(target), target);
    SetFocus(grid);
  }

  void SameContextMenu(int i, POINT screen) {
    if (i < 0) return;
    std::wstring target = settings.resultsView == 1 ? (i < int(sameFolders.size()) ? sameFolders[size_t(i)].folder : L"")
                                                    : (i < int(same.size()) ? same[size_t(i)].path : L"");
    if (target.empty()) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MENU_OPEN, L"ここへ移る(&G)");
    if (settings.resultsView == 0) AppendMenuW(menu, MF_STRING, MENU_SAME, L"開く(&O)");
    AppendMenuW(menu, MF_STRING, MENU_SHOW, L"エクスプローラーで表示(&E)");
    AppendMenuW(menu, MF_STRING, MENU_COPY, L"パスをコピー(&C)");
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
      case MENU_OPEN: GoToSame(i); break;
      case MENU_SAME: host->ExplorerOpen(target); break;
      case MENU_SHOW:
        if (settings.resultsView == 1) ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else ShowInShell(target);
        break;
      case MENU_COPY: CopyText(target); break;
    }
  }

  // ---------------------------------------------------------------- 操作のバー

  int BarH() const { return Px(34); }
  int SplitW() const { return Px(5); }
  int SameHeaderH() const { return Px(28); }

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
    return std::max(SameHeaderH() + Px(40), std::min(Px(settings.resultsHeight), total - BarH() - Px(160)));
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
    int toggleW = Px(84);
    MoveWindow(treeBtn, x, m, toggleW, bh, TRUE);
    x += toggleW + Px(6);
    int right = rc.right - m;
    MoveWindow(previewBtn, right - toggleW, m, toggleW, bh, TRUE);
    right -= toggleW + Px(8);
    int folderW = Px(150), sizeW = Px(64), sliderW = Px(120), labelW = Px(44);
    int fx = right - folderW;
    MoveWindow(folderBox, fx, m, folderW, Px(200), TRUE);
    MoveWindow(folderLabel, fx - labelW, m + Px(5), labelW - Px(4), bh - Px(5), TRUE);
    int sx = fx - labelW - Px(6) - sliderW;
    MoveWindow(slider, sx, m, sliderW, bh, TRUE);
    int bx = sx - sizeW - Px(2);
    MoveWindow(sizeBox, bx, m, sizeW, Px(200), TRUE);
    MoveWindow(sizeLabel, bx - labelW, m + Px(5), labelW - Px(4), bh - Px(5), TRUE);
    MoveWindow(pathText, x, m + Px(1), std::max(Px(60), int(bx - labelW - x - Px(6))), bh - Px(2), TRUE);

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
    int viewW = Px(120);
    MoveWindow(sameText, m, sy + Px(5), std::max(0, int(rc.right - viewW - m * 3)), SameHeaderH() - Px(6), TRUE);
    MoveWindow(sameView, rc.right - m - viewW, sy + Px(2), viewW, Px(200), TRUE);
    MoveWindow(sameList, 0, sy + SameHeaderH(), rc.right, std::max(0, int(rc.bottom - sy - SameHeaderH())), TRUE);
    InvalidateRect(hwnd, nullptr, TRUE);
  }

  // 境目の上か: 1 = フォルダ一覧の右、2 = プレビューの左、3 = 「同じ画像」の上
  int SplitAt(POINT pt) const {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (pt.y < BarH()) return 0;
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
          if (nm->code == NM_DBLCLK || nm->code == NM_RETURN)
            self->GoToSame(ListView_GetNextItem(self->sameList, -1, LVNI_SELECTED));
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
        self->Open(self->sel);
        return 0;
      case WM_LBUTTONDOWN:
        SetFocus(self->grid);
        return 0;
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
        int h = HitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (h >= 0) Select(h);
        return 0;
      }
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
      case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
      case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
        int cols = Cols();
        RECT rc;
        GetClientRect(grid, &rc);
        int page = std::max(1, int(rc.bottom / CellH())) * cols;
        switch (wp) {
          case VK_LEFT: Select(sel - 1); return 0;
          case VK_RIGHT: Select(sel + 1); return 0;
          case VK_UP:
            if (alt) Up();
            else Select(sel - cols);
            return 0;
          case VK_DOWN: Select(sel + cols); return 0;
          case VK_PRIOR: Select(sel - page); return 0;
          case VK_NEXT: Select(sel + page); return 0;
          case VK_HOME: Select(0); return 0;
          case VK_END: Select(int(items.size()) - 1); return 0;
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
    backBtn = make(L"BUTTON", L"←", BS_PUSHBUTTON, ID_BACK);
    forwardBtn = make(L"BUTTON", L"→", BS_PUSHBUTTON, ID_FORWARD);
    upBtn = make(L"BUTTON", L"↑", BS_PUSHBUTTON, ID_UP);
    treeBtn = make(L"BUTTON", L"フォルダ一覧", BS_AUTOCHECKBOX | BS_PUSHLIKE, ID_TREE_BTN);
    previewBtn = make(L"BUTTON", L"プレビュー", BS_AUTOCHECKBOX | BS_PUSHLIKE, ID_PREVIEW_BTN);
    SendMessageW(treeBtn, BM_SETCHECK, settings.showTree ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(previewBtn, BM_SETCHECK, settings.showPreview ? BST_CHECKED : BST_UNCHECKED, 0);
    pathText = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
                               HMENU(INT_PTR(ID_PATH)), inst, nullptr);
    SendMessageW(pathText, WM_SETFONT, WPARAM(font), TRUE);
    SendMessageW(pathText, EM_SETCUEBANNER, TRUE, LPARAM(L"フォルダ、または画像の URL（同じ画像を探す）"));
    SHAutoComplete(pathText, SHACF_FILESYS_DIRS);  // フォルダの名前を補う
    SetWindowSubclass(pathText, ChildKeyProc, 1, DWORD_PTR(this));
    sizeLabel = make(L"STATIC", L"大きさ", SS_RIGHT, ID_SIZE_LABEL);
    sizeBox = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_SIZE);
    for (const wchar_t* t : {L"小", L"普通", L"大", L"可変"}) SendMessageW(sizeBox, CB_ADDSTRING, 0, LPARAM(t));
    slider = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, ID_SLIDER);
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(kMinSize, kMaxSize));
    SendMessageW(slider, TBM_SETPAGESIZE, 0, 32);
    folderLabel = make(L"STATIC", L"表紙", SS_RIGHT, ID_FOLDER_LABEL);
    folderBox = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_FOLDER);
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
    // 「同じ画像」
    sameText = make(L"STATIC", L"画像を選ぶか、画像のファイル・URL をここにドロップすると、同じ画像（インデックスを作ったフォルダの中）を探します。"
                               L"　右下の印 — 緑: インデックス作成済み、橙: 処理待ち、白: 未作成、半分: 一部",
                    SS_LEFTNOWORDWRAP | SS_NOPREFIX | SS_ENDELLIPSIS, ID_SAME_TEXT);
    sameView = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_SAME_VIEW);
    for (const wchar_t* t : {L"画像ごと", L"フォルダごと"}) SendMessageW(sameView, CB_ADDSTRING, 0, LPARAM(t));
    settings.resultsView = std::max(0, std::min(1, settings.resultsView));
    SendMessageW(sameView, CB_SETCURSEL, settings.resultsView, 0);
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

std::wstring Explorer::Dir() const { return impl_->dir; }

std::wstring Explorer::Selected() const { return impl_->SelectedPath(); }

void Explorer::RefreshStates() { impl_->RefreshStates(); }

void Explorer::SetSameStatus(const std::wstring& text) { SetWindowTextW(impl_->sameText, text.c_str()); }

void Explorer::SetSame(const std::wstring& title, std::vector<ExplorerMatch> matches) { impl_->SetSame(title, std::move(matches)); }
