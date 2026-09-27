#pragma once
// Small shared text-conversion helpers (header-only).

#include <string>
#include <windows.h>

// Wide string (UTF-16) -> UTF-8.
inline std::string WideToUtf8(const wchar_t* w, int len) {
    if (!w || len <= 0) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), size, nullptr, nullptr);
    return out;
}

inline std::string WideToUtf8(const std::wstring& w) {
    return WideToUtf8(w.c_str(), static_cast<int>(w.size()));
}

// UTF-8 -> wide string (UTF-16).
inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size);
    return out;
}