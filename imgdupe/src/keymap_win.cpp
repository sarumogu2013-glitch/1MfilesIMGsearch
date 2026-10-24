// ショートカットキーの設定画面: キーボードの図に、今の割り当てを表示する（Massiguraedit から移植）。
// Ctrl / Shift / Alt のチェックで表示する組み合わせを切り替え、キーをクリックすると割り当てを変える。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX

#include "keymap_win.hpp"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "win_util.hpp"

using namespace imgdupe;

namespace {

// キーの位置と大きさ (1 = 普通のキー 1 つ分)。vk = 0 は割り当てられないキー
struct KeyDef {
    float x, y, w, h;
    UINT vk;
    const wchar_t* label;
};

// 日本語 (JIS) キーボード
const KeyDef kKeys[] = {
      // 1 段目
      {0, 0, 1, 1, VK_ESCAPE, L"Esc"},
      {2, 0, 1, 1, VK_F1, L"F1"}, {3, 0, 1, 1, VK_F2, L"F2"}, {4, 0, 1, 1, VK_F3, L"F3"}, {5, 0, 1, 1, VK_F4, L"F4"},
      {6.5f, 0, 1, 1, VK_F5, L"F5"}, {7.5f, 0, 1, 1, VK_F6, L"F6"}, {8.5f, 0, 1, 1, VK_F7, L"F7"},
      {9.5f, 0, 1, 1, VK_F8, L"F8"},
      {11, 0, 1, 1, VK_F9, L"F9"}, {12, 0, 1, 1, VK_F10, L"F10"}, {13, 0, 1, 1, VK_F11, L"F11"},
      {14, 0, 1, 1, VK_F12, L"F12"},
      {15.25f, 0, 1, 1, 0, L"PrtSc"}, {16.25f, 0, 1, 1, VK_SCROLL, L"ScrLk"}, {17.25f, 0, 1, 1, VK_PAUSE, L"Pause"},
      // 2 段目
      {0, 1.25f, 1, 1, 0, L"半/全"},
      {1, 1.25f, 1, 1, '1', L"1"}, {2, 1.25f, 1, 1, '2', L"2"}, {3, 1.25f, 1, 1, '3', L"3"}, {4, 1.25f, 1, 1, '4', L"4"},
      {5, 1.25f, 1, 1, '5', L"5"}, {6, 1.25f, 1, 1, '6', L"6"}, {7, 1.25f, 1, 1, '7', L"7"}, {8, 1.25f, 1, 1, '8', L"8"},
      {9, 1.25f, 1, 1, '9', L"9"}, {10, 1.25f, 1, 1, '0', L"0"},
      {11, 1.25f, 1, 1, VK_OEM_MINUS, L"-"}, {12, 1.25f, 1, 1, VK_OEM_7, L"^"}, {13, 1.25f, 1, 1, VK_OEM_5, L"¥"},
      {14, 1.25f, 1, 1, VK_BACK, L"BS"},
      {15.25f, 1.25f, 1, 1, VK_INSERT, L"Ins"}, {16.25f, 1.25f, 1, 1, VK_HOME, L"Home"},
      {17.25f, 1.25f, 1, 1, VK_PRIOR, L"PgUp"},
      {18.5f, 1.25f, 1, 1, VK_NUMLOCK, L"Num"}, {19.5f, 1.25f, 1, 1, VK_DIVIDE, L"/"},
      {20.5f, 1.25f, 1, 1, VK_MULTIPLY, L"*"}, {21.5f, 1.25f, 1, 1, VK_SUBTRACT, L"-"},
      // 3 段目
      {0, 2.25f, 1.5f, 1, VK_TAB, L"Tab"},
      {1.5f, 2.25f, 1, 1, 'Q', L"Q"}, {2.5f, 2.25f, 1, 1, 'W', L"W"}, {3.5f, 2.25f, 1, 1, 'E', L"E"},
      {4.5f, 2.25f, 1, 1, 'R', L"R"}, {5.5f, 2.25f, 1, 1, 'T', L"T"}, {6.5f, 2.25f, 1, 1, 'Y', L"Y"},
      {7.5f, 2.25f, 1, 1, 'U', L"U"}, {8.5f, 2.25f, 1, 1, 'I', L"I"}, {9.5f, 2.25f, 1, 1, 'O', L"O"},
      {10.5f, 2.25f, 1, 1, 'P', L"P"}, {11.5f, 2.25f, 1, 1, VK_OEM_3, L"@"}, {12.5f, 2.25f, 1, 1, VK_OEM_4, L"["},
      {13.75f, 2.25f, 1.25f, 2, VK_RETURN, L"Enter"},
      {15.25f, 2.25f, 1, 1, VK_DELETE, L"Del"}, {16.25f, 2.25f, 1, 1, VK_END, L"End"},
      {17.25f, 2.25f, 1, 1, VK_NEXT, L"PgDn"},
      {18.5f, 2.25f, 1, 1, VK_NUMPAD7, L"7"}, {19.5f, 2.25f, 1, 1, VK_NUMPAD8, L"8"},
      {20.5f, 2.25f, 1, 1, VK_NUMPAD9, L"9"}, {21.5f, 2.25f, 1, 2, VK_ADD, L"+"},
      // 4 段目
      {0, 3.25f, 1.75f, 1, VK_CAPITAL, L"英数"},
      {1.75f, 3.25f, 1, 1, 'A', L"A"}, {2.75f, 3.25f, 1, 1, 'S', L"S"}, {3.75f, 3.25f, 1, 1, 'D', L"D"},
      {4.75f, 3.25f, 1, 1, 'F', L"F"}, {5.75f, 3.25f, 1, 1, 'G', L"G"}, {6.75f, 3.25f, 1, 1, 'H', L"H"},
      {7.75f, 3.25f, 1, 1, 'J', L"J"}, {8.75f, 3.25f, 1, 1, 'K', L"K"}, {9.75f, 3.25f, 1, 1, 'L', L"L"},
      {10.75f, 3.25f, 1, 1, VK_OEM_PLUS, L";"}, {11.75f, 3.25f, 1, 1, VK_OEM_1, L":"},
      {12.75f, 3.25f, 1, 1, VK_OEM_6, L"]"},
      {18.5f, 3.25f, 1, 1, VK_NUMPAD4, L"4"}, {19.5f, 3.25f, 1, 1, VK_NUMPAD5, L"5"},
      {20.5f, 3.25f, 1, 1, VK_NUMPAD6, L"6"},
      // 5 段目
      {0, 4.25f, 2.25f, 1, VK_SHIFT, L"Shift"},
      {2.25f, 4.25f, 1, 1, 'Z', L"Z"}, {3.25f, 4.25f, 1, 1, 'X', L"X"}, {4.25f, 4.25f, 1, 1, 'C', L"C"},
      {5.25f, 4.25f, 1, 1, 'V', L"V"}, {6.25f, 4.25f, 1, 1, 'B', L"B"}, {7.25f, 4.25f, 1, 1, 'N', L"N"},
      {8.25f, 4.25f, 1, 1, 'M', L"M"}, {9.25f, 4.25f, 1, 1, VK_OEM_COMMA, L","},
      {10.25f, 4.25f, 1, 1, VK_OEM_PERIOD, L"."}, {11.25f, 4.25f, 1, 1, VK_OEM_2, L"/"},
      {12.25f, 4.25f, 1, 1, VK_OEM_102, L"\\"}, {13.25f, 4.25f, 1.75f, 1, VK_SHIFT, L"Shift"},
      {16.25f, 4.25f, 1, 1, VK_UP, L"↑"},
      {18.5f, 4.25f, 1, 1, VK_NUMPAD1, L"1"}, {19.5f, 4.25f, 1, 1, VK_NUMPAD2, L"2"},
      {20.5f, 4.25f, 1, 1, VK_NUMPAD3, L"3"}, {21.5f, 4.25f, 1, 2, 0, L"Enter"},
      // 6 段目
      {0, 5.25f, 1.5f, 1, VK_CONTROL, L"Ctrl"}, {1.5f, 5.25f, 1.25f, 1, 0, L"Win"},
      {2.75f, 5.25f, 1.25f, 1, VK_MENU, L"Alt"}, {4, 5.25f, 1.25f, 1, VK_NONCONVERT, L"無変換"},
      {5.25f, 5.25f, 3.5f, 1, VK_SPACE, L"Space"}, {8.75f, 5.25f, 1.25f, 1, VK_CONVERT, L"変換"},
      {10, 5.25f, 1.25f, 1, VK_KANA, L"かな"}, {11.25f, 5.25f, 1.25f, 1, VK_MENU, L"Alt"},
      {12.5f, 5.25f, 1.25f, 1, VK_APPS, L"App"}, {13.75f, 5.25f, 1.25f, 1, VK_CONTROL, L"Ctrl"},
      {15.25f, 5.25f, 1, 1, VK_LEFT, L"←"}, {16.25f, 5.25f, 1, 1, VK_DOWN, L"↓"}, {17.25f, 5.25f, 1, 1, VK_RIGHT, L"→"},
      {18.5f, 5.25f, 2, 1, VK_NUMPAD0, L"0"}, {20.5f, 5.25f, 1, 1, VK_DECIMAL, L"."},
      // キーボードの外のキー
      {0, 6.75f, 2, 1, VK_BROWSER_BACK, L"戻る (マウス/ブラウザ)"},
      {2, 6.75f, 2, 1, VK_BROWSER_FORWARD, L"進む (マウス/ブラウザ)"},
};
constexpr float kCols = 22.5f, kRows = 7.75f;
constexpr int kKeyCount = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));
// 一時置き場: キーボードの外の段の右側に並べる。番号は kKeyCount の後ろ
constexpr int kSlots = 8;
constexpr float kSlotX = 4.5f, kSlotY = 6.75f, kSlotW = 2.25f;

constexpr wchar_t kClass[] = L"imgdupe_keymap";
enum { ID_CTRL = 1, ID_SHIFT, ID_ALT };

bool isModifier(UINT vk) { return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU; }

std::wstring modsText(unsigned mods) {
    std::wstring s;
    if (mods & KMOD_CTRL) s += L"Ctrl+";
    if (mods & KMOD_SHIFT) s += L"Shift+";
    if (mods & KMOD_ALT) s += L"Alt+";
    return s;
}

// メニュー用の (&X) や「...」を外した機能の名前
std::wstring cmdLabel(int cmd) {
    const CmdInfo* ci = findCommand(cmd);
    if (!ci) return L"(不明な機能)";
    std::wstring s = ci->label;
    const size_t amp = s.find(L"(&");
    if (amp != std::wstring::npos && amp + 4 <= s.size() && s[amp + 3] == L')') s.erase(amp, 4);
    while (!s.empty() && s.back() == L'.') s.pop_back();
    return s;
}

// マウスを合わせた時の説明（名前だけでは分かりにくい機能）
std::wstring cmdDetail(int cmd) {
    switch (cmd) {
        case CMD_SEARCH: return L"検索していない時は検索を始め、検索中なら中止する";
        case CMD_QUERY: return L"選んだ画像に似た画像を、追加したフォルダから探す";
        case CMD_OPEN: return L"右に選んでいる画像を、既定のアプリで開く";
        case CMD_PREV_GROUP: case CMD_NEXT_GROUP: return L"左の一覧で、前・次のグループを選ぶ";
        case CMD_THRESHOLD_DOWN: case CMD_THRESHOLD_UP: return L"しきい値を 1 つ変える（次の検索から効く）";
        default: return std::wstring();
    }
}

class KeyMapWindow {
public:
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    KeyMapHost* host = nullptr;

    void create(HWND ownerWnd, KeyMapHost* h);
    LRESULT proc(UINT msg, WPARAM wp, LPARAM lp);

private:
    unsigned mods() const;
    RECT keyRect(int i) const;
    int hitTest(POINT pt) const;  // キー (0..) か一時置き場 (kKeyCount..)
    bool isSlot(int t) const { return t >= kKeyCount; }
    int targetCmd(int t) const;
    void setTarget(int t, int cmd);
    void setKey(UINT vk, unsigned mods, int cmd);
    bool canHold(int t) const { return t >= 0 && (isSlot(t) || (kKeys[t].vk && !isModifier(kKeys[t].vk))); }
    void drop(int from, int to);
    std::wstring targetName(int t) const;
    int cmdFor(UINT vk, unsigned mods) const;
    void assign(UINT vk, unsigned mods, int cmd);
    void layout();
    void paint(HDC dc);
    void updateTip(POINT pt);
    void onClick(int key);
    void makeFonts();

    HWND check_[3] = {};
    HWND tip_ = nullptr;
    TOOLINFOW ti_ = {};
    std::wstring tipText_;
    int hover_ = -1;
    // ドラッグ（キーの割り当てを別のキーや一時置き場へ動かす）
    int down_ = -1;
    POINT downPt_ = {}, dragPt_ = {};
    bool dragging_ = false;
    bool tracking_ = false;
    float unit_ = 40;
    int left_ = 0, top_ = 0, hintTop_ = 0;
    int dpi_ = 96;
    HFONT uiFont_ = nullptr, labelFont_ = nullptr, cmdFont_ = nullptr;
};

KeyMapWindow* g_window = nullptr;

LRESULT CALLBACK keyMapProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_window && (g_window->hwnd == hwnd || msg == WM_CREATE)) {
        g_window->hwnd = hwnd;
        return g_window->proc(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void KeyMapWindow::create(HWND ownerWnd, KeyMapHost* h) {
    owner = ownerWnd;
    host = h;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = keyMapProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    HDC sdc = GetDC(nullptr);
    dpi_ = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(nullptr, sdc);
    const int w = MulDiv(1180, dpi_, 96), hgt = MulDiv(560, dpi_, 96);
    RECT orc;
    GetWindowRect(owner, &orc);
    const int x = orc.left + std::max(0L, (orc.right - orc.left - w) / 2);
    const int y = orc.top + std::max(0L, (orc.bottom - orc.top - hgt) / 2);
    CreateWindowExW(0, kClass, L"ショートカットキーの設定 - imgdupe", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, hgt,
                    owner, nullptr, inst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
}

unsigned KeyMapWindow::mods() const {
    unsigned m = 0;
    if (Button_GetCheck(check_[0]) == BST_CHECKED) m |= KMOD_CTRL;
    if (Button_GetCheck(check_[1]) == BST_CHECKED) m |= KMOD_SHIFT;
    if (Button_GetCheck(check_[2]) == BST_CHECKED) m |= KMOD_ALT;
    return m;
}

RECT KeyMapWindow::keyRect(int i) const {
    KeyDef k = isSlot(i) ? KeyDef{kSlotX + (i - kKeyCount) * kSlotW, kSlotY, kSlotW, 1, 0, nullptr} : kKeys[i];
    const int gap = std::max(1, static_cast<int>(unit_ * 0.05f));
    return RECT{left_ + static_cast<int>(k.x * unit_) + gap, top_ + static_cast<int>(k.y * unit_) + gap,
                left_ + static_cast<int>((k.x + k.w) * unit_) - gap, top_ + static_cast<int>((k.y + k.h) * unit_) - gap};
}

int KeyMapWindow::hitTest(POINT pt) const {
    for (int i = 0; i < kKeyCount + kSlots; ++i) {
        RECT r = keyRect(i);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

int KeyMapWindow::cmdFor(UINT vk, unsigned m) const {
    if (!vk || isModifier(vk)) return 0;
    auto it = host->keys().find(KeyStroke{m, vk});
    return it == host->keys().end() ? 0 : it->second;
}

void KeyMapWindow::setKey(UINT vk, unsigned m, int cmd) {
    host->keys().erase(KeyStroke{m, vk});
    if (cmd) host->keys()[KeyStroke{m, vk}] = cmd;
}

void KeyMapWindow::assign(UINT vk, unsigned m, int cmd) {
    setKey(vk, m, cmd);
    host->onKeysChanged();
    InvalidateRect(hwnd, nullptr, FALSE);
}

int KeyMapWindow::targetCmd(int t) const {
    if (t < 0) return 0;
    if (isSlot(t)) return host->keyTmp()[static_cast<size_t>(t - kKeyCount)];
    return cmdFor(kKeys[t].vk, mods());
}

void KeyMapWindow::setTarget(int t, int cmd) {
    if (isSlot(t)) host->keyTmp()[static_cast<size_t>(t - kKeyCount)] = cmd;
    else setKey(kKeys[t].vk, mods(), cmd);
}

std::wstring KeyMapWindow::targetName(int t) const {
    if (isSlot(t)) return L"一時置き場 " + std::to_wstring(t - kKeyCount + 1);
    return modsText(mods()) + kKeys[t].label;
}

// from の機能を to へ動かす。to に機能があれば入れ替える
void KeyMapWindow::drop(int from, int to) {
    if (from < 0 || to < 0 || from == to) return;
    if (!canHold(to)) {
        MessageBeep(MB_OK);
        return;
    }
    const int a = targetCmd(from), b = targetCmd(to);
    if (!a) return;
    setTarget(to, a);
    setTarget(from, b);
    host->onKeysChanged();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void KeyMapWindow::makeFonts() {
    if (labelFont_) DeleteObject(labelFont_);
    if (cmdFont_) DeleteObject(cmdFont_);
    labelFont_ = CreateFontW(-std::max(9, static_cast<int>(unit_ * 0.24f)), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0,
                             0, CLEARTYPE_QUALITY, 0, L"Meiryo UI");
    cmdFont_ = CreateFontW(-std::max(8, static_cast<int>(unit_ * 0.2f)), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0,
                           0, CLEARTYPE_QUALITY, 0, L"Meiryo UI");
}

void KeyMapWindow::layout() {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int m = MulDiv(10, dpi_, 96);
    const int rowH = MulDiv(24, dpi_, 96);
    int x = m;
    for (int i = 0; i < 3; ++i) {
        MoveWindow(check_[i], x, m, MulDiv(70, dpi_, 96), rowH, TRUE);
        x += MulDiv(76, dpi_, 96);
    }
    const int hintH = MulDiv(40, dpi_, 96);
    const int availW = rc.right - 2 * m, availH = rc.bottom - (m + rowH + m) - hintH - m;
    unit_ = std::max(16.0f, std::min(availW / kCols, availH / kRows));
    left_ = m + std::max(0, static_cast<int>((availW - unit_ * kCols) / 2));
    top_ = m + rowH + m;
    hintTop_ = top_ + static_cast<int>(unit_ * kRows) + m / 2;
    makeFonts();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void KeyMapWindow::paint(HDC dc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);
    const unsigned m = mods();
    HPEN normalPen = CreatePen(PS_SOLID, 1, RGB(150, 150, 150));
    HPEN assignedPen = CreatePen(PS_SOLID, 1, RGB(70, 120, 200));
    HPEN hoverPen = CreatePen(PS_SOLID, std::max(2, static_cast<int>(unit_ / 20)), RGB(230, 120, 0));
    HGDIOBJ oldPen = SelectObject(dc, normalPen);
    const int round = std::max(4, static_cast<int>(unit_ / 8));
    for (int i = 0; i < kKeyCount; ++i) {
        const KeyDef& k = kKeys[i];
        RECT r = keyRect(i);
        const int cmd = cmdFor(k.vk, m);
        const bool mod = isModifier(k.vk);
        const bool modOn = (k.vk == VK_CONTROL && (m & KMOD_CTRL)) || (k.vk == VK_SHIFT && (m & KMOD_SHIFT)) ||
                           (k.vk == VK_MENU && (m & KMOD_ALT));
        const COLORREF fill = !k.vk ? RGB(225, 225, 225)
                              : modOn ? RGB(255, 205, 110)
                              : mod   ? RGB(245, 235, 210)
                              : cmd   ? RGB(205, 225, 255)
                                      : RGB(255, 255, 255);
        HBRUSH br = CreateSolidBrush(fill);
        SelectObject(dc, i == hover_ ? hoverPen : cmd ? assignedPen : normalPen);
        HGDIOBJ oldBr = SelectObject(dc, br);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, round, round);
        SelectObject(dc, oldBr);
        DeleteObject(br);
        // キーの名前（左上）と割り当てた機能（下）
        const int pad = std::max(2, static_cast<int>(unit_ * 0.08f));
        RECT lr = {r.left + pad, r.top + pad / 2, r.right - pad, r.bottom};
        SelectObject(dc, labelFont_);
        SetTextColor(dc, k.vk ? RGB(60, 60, 60) : RGB(150, 150, 150));
        DrawTextW(dc, k.label, -1, &lr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (cmd) {
            RECT cr = {r.left + pad / 2, r.top + static_cast<int>(unit_ * 0.3f), r.right - pad / 2, r.bottom - pad / 2};
            SelectObject(dc, cmdFont_);
            SetTextColor(dc, RGB(20, 50, 120));
            const std::wstring text = cmdLabel(cmd);
            DrawTextW(dc, text.c_str(), -1, &cr, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS | DT_EDITCONTROL);
        } else if (mod) {
            RECT cr = {r.left, r.top + static_cast<int>(unit_ * 0.3f), r.right, r.bottom};
            SelectObject(dc, cmdFont_);
            SetTextColor(dc, RGB(120, 90, 30));
            DrawTextW(dc, modOn ? L"押している" : L"クリックで押す", -1, &cr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    // 一時置き場
    {
        RECT first = keyRect(kKeyCount);
        RECT lr = {first.left, first.top - static_cast<int>(unit_ * 0.32f), first.left + static_cast<int>(unit_ * 8),
                   first.top};
        SelectObject(dc, labelFont_);
        SetTextColor(dc, RGB(90, 90, 90));
        DrawTextW(dc, L"一時置き場 (キーからドラッグして置いておける)", -1, &lr,
                  DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
    }
    for (int j = 0; j < kSlots; ++j) {
        const int t = kKeyCount + j;
        RECT r = keyRect(t);
        const int cmd = host->keyTmp()[static_cast<size_t>(j)];
        HBRUSH br = CreateSolidBrush(cmd ? RGB(225, 245, 215) : RGB(248, 248, 240));
        HPEN dash = CreatePen(PS_DOT, 1, RGB(140, 140, 120));
        SelectObject(dc, t == hover_ ? hoverPen : cmd ? assignedPen : dash);
        HGDIOBJ oldBr = SelectObject(dc, br);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, round, round);
        SelectObject(dc, oldBr);
        SelectObject(dc, normalPen);
        DeleteObject(br);
        DeleteObject(dash);
        RECT cr = {r.left + 3, r.top + 2, r.right - 3, r.bottom - 2};
        SelectObject(dc, cmdFont_);
        SetTextColor(dc, cmd ? RGB(30, 80, 30) : RGB(170, 170, 160));
        const std::wstring text = cmd ? cmdLabel(cmd) : L"空き";
        DrawTextW(dc, text.c_str(), -1, &cr, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS | DT_EDITCONTROL);
    }
    // ドラッグ中の機能
    if (dragging_) {
        const std::wstring text = cmdLabel(targetCmd(down_));
        SelectObject(dc, cmdFont_);
        RECT tr = {0, 0, static_cast<int>(unit_ * 3), 0};
        DrawTextW(dc, text.c_str(), -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        OffsetRect(&tr, dragPt_.x + 12, dragPt_.y + 12);
        InflateRect(&tr, 4, 3);
        HBRUSH br = CreateSolidBrush(RGB(255, 240, 200));
        SelectObject(dc, hoverPen);
        HGDIOBJ oldBr = SelectObject(dc, br);
        Rectangle(dc, tr.left, tr.top, tr.right, tr.bottom);
        SelectObject(dc, oldBr);
        DeleteObject(br);
        InflateRect(&tr, -4, -3);
        SetTextColor(dc, RGB(60, 40, 0));
        DrawTextW(dc, text.c_str(), -1, &tr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
    SelectObject(dc, oldPen);
    DeleteObject(normalPen);
    DeleteObject(assignedPen);
    DeleteObject(hoverPen);

    RECT hr = {left_, hintTop_, rc.right - 10, rc.bottom};
    SelectObject(dc, uiFont_);
    SetTextColor(dc, RGB(70, 70, 70));
    const std::wstring hint =
        L"表示中: " + (m ? modsText(m) + L"キー" : std::wstring(L"修飾キーなし (キー単独)")) +
        L"    カーソルを合わせると詳細、クリックで割り当てを変更、ドラッグで別のキーや一時置き場へ動かす"
        L" (相手に割り当てがあれば入れ替え)。変更は設定ファイルに保存。"
        L"\n青 = 割り当てあり、灰色 = 割り当てられないキー。";
    DrawTextW(dc, hint.c_str(), -1, &hr, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);
}

void KeyMapWindow::updateTip(POINT pt) {
    const int key = hitTest(pt);
    if (key != hover_) {
        hover_ = key;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (key < 0) {
        SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti_));
        return;
    }
    const unsigned m = mods();
    std::wstring text;
    if (dragging_) {
        text = canHold(key) ? targetName(key) + L" へ動かす" : L"ここには置けません";
        const int b = targetCmd(key);
        if (canHold(key) && b && key != down_) text += L" (" + cmdLabel(b) + L" と入れ替え)";
    } else if (isSlot(key)) {
        const int cmd = targetCmd(key);
        text = targetName(key) + L":  " + (cmd ? cmdLabel(cmd) : std::wstring(L"空き"));
        text += L"\n\nキーからここへドラッグすると、割り当てを外して置いておける。"
                L"\nここからキーへドラッグすると、そのキーに割り当てる。クリックで機能を選んで入れる・空にする。";
    } else {
        const KeyDef& k = kKeys[key];
        if (!k.vk) {
            text = std::wstring(k.label) + L"\nこのキーには割り当てられません。";
        } else if (isModifier(k.vk)) {
            text = std::wstring(k.label) + L"\nクリックすると、" + k.label +
                   L" を押しながらの割り当てを表示します (上のチェックと同じ)。";
        } else {
            const int cmd = cmdFor(k.vk, m);
            text = L"[" + modsText(m) + k.label + L"]  " + (cmd ? cmdLabel(cmd) : std::wstring(L"(割り当てなし)"));
            if (cmd) {
                const std::wstring detail = cmdDetail(cmd);
                if (!detail.empty()) text += L"\n" + detail;
                if (const CmdInfo* ci = findCommand(cmd)) text += L"\n設定ファイルでの名前: " + winutil::widen(ci->name);
            }
            // このキーの他の組み合わせ
            std::wstring others;
            for (unsigned o = 0; o < 8; ++o) {
                if (o == m) continue;
                if (const int c = cmdFor(k.vk, o))
                    others += L"\n  " + (o ? modsText(o) + k.label : std::wstring(k.label) + L" (単独)") + L" : " +
                              cmdLabel(c);
            }
            if (!others.empty()) text += L"\n\nこのキーの他の組み合わせ:" + others;
            text += L"\n\nクリックで割り当てを変更";
        }
    }
    if (text != tipText_) {
        tipText_ = text;
        ti_.lpszText = &tipText_[0];
        SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti_));
    }
    POINT sp = pt;
    ClientToScreen(hwnd, &sp);
    const int dy = dragging_ ? static_cast<int>(unit_ * 1.2f) : 20;  // ドラッグ中は運んでいる機能の名前の下に出す
    SendMessageW(tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(sp.x + 16, sp.y + dy));
    SendMessageW(tip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti_));
}

void KeyMapWindow::onClick(int key) {
    if (key < 0) return;
    if (!isSlot(key)) {
        const KeyDef& k = kKeys[key];
        if (isModifier(k.vk)) {
            HWND chk = check_[k.vk == VK_CONTROL ? 0 : k.vk == VK_SHIFT ? 1 : 2];
            Button_SetCheck(chk, Button_GetCheck(chk) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        if (!k.vk) {
            MessageBeep(MB_OK);
            return;
        }
    }
    SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti_));
    const int current = targetCmd(key);
    HMENU menu = CreatePopupMenu();
    const std::wstring title = targetName(key) + (isSlot(key) ? L" に入れる機能" : L" に割り当てる機能");
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, title.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    constexpr UINT kNone = 1;
    AppendMenuW(menu, MF_STRING | (current ? 0 : MF_CHECKED), kNone, isSlot(key) ? L"空にする" : L"割り当てなし");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    host->buildCommandMenu(menu);
    if (current) CheckMenuItem(menu, static_cast<UINT>(current), MF_BYCOMMAND | MF_CHECKED);
    POINT sp;
    GetCursorPos(&sp);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd < static_cast<int>(kNone)) return;
    const int value = cmd == static_cast<int>(kNone) ? 0 : cmd;
    if (isSlot(key)) {
        setTarget(key, value);
        host->onKeysChanged();
        InvalidateRect(hwnd, nullptr, FALSE);
    } else {
        assign(kKeys[key].vk, mods(), value);
    }
}

LRESULT KeyMapWindow::proc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HINSTANCE inst = GetModuleHandleW(nullptr);
            uiFont_ = CreateFontW(-MulDiv(12, dpi_, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                  CLEARTYPE_QUALITY, 0, L"Meiryo UI");
            const wchar_t* names[] = {L"Ctrl", L"Shift", L"Alt"};
            for (int i = 0; i < 3; ++i) {
                check_[i] = CreateWindowExW(0, L"BUTTON", names[i], WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0,
                                            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CTRL + i)), inst,
                                            nullptr);
                SendMessageW(check_[i], WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
            }
            tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0,
                                   0, 0, hwnd, nullptr, inst, nullptr);
            ti_.cbSize = sizeof(ti_);
            ti_.uFlags = TTF_TRACK | TTF_ABSOLUTE;
            ti_.hwnd = hwnd;
            ti_.lpszText = const_cast<wchar_t*>(L"");
            SendMessageW(tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti_));
            SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, MulDiv(420, dpi_, 96));
            return 0;
        }
        case WM_SIZE:
            layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = MulDiv(640, dpi_, 96);
            mm->ptMinTrackSize.y = MulDiv(320, dpi_, 96);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) >= ID_CTRL && LOWORD(wp) <= ID_ALT) {
                SetFocus(hwnd);  // Esc で閉じられるように
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
            HGDIOBJ old = SelectObject(mem, bmp);
            paint(mem);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (down_ >= 0 && !dragging_ && (std::abs(pt.x - downPt_.x) > 4 || std::abs(pt.y - downPt_.y) > 4) &&
                targetCmd(down_))
                dragging_ = true;
            if (dragging_) {
                dragPt_ = pt;
                SetCursor(LoadCursor(nullptr, canHold(hitTest(pt)) ? IDC_HAND : IDC_NO));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            updateTip(pt);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            down_ = hitTest(pt);
            downPt_ = pt;
            dragging_ = false;
            SetCapture(hwnd);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lp) != hwnd && dragging_) {
                dragging_ = false;
                down_ = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSELEAVE:
            tracking_ = false;
            hover_ = -1;
            SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti_));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int from = down_, to = hitTest(pt);
            const bool drag = dragging_;
            down_ = -1;
            dragging_ = false;
            ReleaseCapture();
            if (drag) {
                drop(from, to);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (from == to) {
                onClick(to);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_DESTROY:
            DestroyWindow(tip_);
            for (HFONT f : {uiFont_, labelFont_, cmdFont_})
                if (f) DeleteObject(f);
            if (owner) SetForegroundWindow(owner);
            g_window = nullptr;
            delete this;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void showKeyMapWindow(HWND owner, KeyMapHost* host) {
    if (g_window) {
        ShowWindow(g_window->hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_window->hwnd);
        return;
    }
    g_window = new KeyMapWindow;
    g_window->create(owner, host);
}
