// Windows 用の小さな共通部品（COM ポインタ、UTF-8 ⇔ UTF-16）。
#pragma once

#include <windows.h>

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

}  // namespace winutil
