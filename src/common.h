// Shared includes and small helpers.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

using Bytes = std::vector<uint8_t>;
using SharedBytes = std::shared_ptr<const Bytes>;

inline std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

inline std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

inline std::wstring lowercase(std::wstring_view s) {
    std::wstring out(s);
    if (!out.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), (int)s.size(),
                      out.data(), (int)out.size(), nullptr, nullptr, 0);
    return out;
}

// Milliseconds from a steady high-resolution clock.
inline double now_ms() {
    static const double ticks_per_ms = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart / 1000.0;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart / ticks_per_ms;
}

inline DWORD window_pid(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid;
}

// The program a process runs, lowercase, e.g. "discord.exe" ("" if unknown).
inline std::wstring exe_name(DWORD pid) {
    std::wstring name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD size = (DWORD)std::size(buf);
        if (QueryFullProcessImageNameW(h, 0, buf, &size)) {
            std::wstring path(buf, size);
            name = lowercase(path.substr(path.find_last_of(L"\\/") + 1));
        }
        CloseHandle(h);
    }
    return name;
}

// The folder this program is in, with a trailing backslash.
inline std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::wstring path(buf, n);
    return path.substr(0, path.find_last_of(L"\\/") + 1);
}
