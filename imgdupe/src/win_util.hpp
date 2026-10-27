// Windows 用の小さな共通部品（COM ポインタ、UTF-8 ⇔ UTF-16）。
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <string>

namespace winutil {

template <class T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    T** put() {
        reset();
        return &p_;
    }
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// マウスを乗せた時の説明（ツールチップ）。makeTooltip で 1 つ作り、部品ごとに addTooltip で足す。
// text は呼び出し側がずっと持っている文字列（文字列リテラル）
inline HWND makeTooltip(HWND owner, int maxWidth) {
    HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr, nullptr, nullptr);
    if (tip) {
        SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, maxWidth);  // 長い説明は折り返す
        SendMessageW(tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
    }
    return tip;
}

inline void addTooltip(HWND tip, HWND ctrl, const wchar_t* text) {
    if (!tip || !ctrl) return;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = GetParent(ctrl);
    ti.uId = reinterpret_cast<UINT_PTR>(ctrl);
    ti.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

}  // namespace winutil
