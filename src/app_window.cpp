// The window itself, following Discord, pasting, searching and settings.

#include "app.h"
#include <algorithm>
#include <cmath>
#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>

#include <dwmapi.h>
#include <windowsx.h>
#include "json.h"

namespace {

App* g_app = nullptr;  // for the Windows event callbacks

std::wstring settings_path() { return exe_dir() + L"settings.json"; }

// The window's visible edges. GetWindowRect would include Windows 10/11's invisible resize
// borders, which would leave a gap.
RECT window_rect(HWND h) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        GetWindowRect(h, &r);
    return r;
}

void add_key(std::vector<INPUT>& out, WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    out.push_back(in);
}

std::wstring replace_all(std::wstring s, std::wstring_view from, std::wstring_view to) {
    for (size_t at = s.find(from); at != std::wstring::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
    return s;
}

std::wstring trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

// ---- files (file mode) --------------------------------------------------------------------

// A file name made from an emote's name: characters Windows doesn't allow in names become _.
std::wstring safe_file_name(const std::wstring& name) {
    std::wstring out;
    for (wchar_t c : name) out += c < 32 || wcschr(L"<>:\"/\\|?*", c) ? L'_' : c;
    if (out.size() > 80) out.resize(80);
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
    if (out.empty()) out = L"emote";
    static const std::set<std::wstring> devices = [] {  // names Windows keeps for devices
        std::set<std::wstring> d{L"con", L"prn", L"aux", L"nul"};
        for (wchar_t n = L'1'; n <= L'9'; n++) d.insert({L"com" + std::wstring(1, n), L"lpt" + std::wstring(1, n)});
        return d;
    }();
    if (devices.count(lowercase(out.substr(0, out.find(L'.'))))) out = L"_" + out;
    return out;
}

// Whether path is a file with something in it.
bool file_exists(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) &&
           !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (a.nFileSizeLow || a.nFileSizeHigh);
}

// Makes the folder dir and the ones it's in, where they're missing.
void make_dirs(const std::wstring& dir) {
    for (size_t at = dir.find(L'\\', 3);; at = dir.find(L'\\', at + 1)) {
        CreateDirectoryW(dir.substr(0, at).c_str(), nullptr);
        if (at == std::wstring::npos) break;
    }
}

// Saves data as path, through a temporary file so a half-written file never has its name.
// A file already there is kept as is: Discord may be about to upload it, and a file that
// changed after it was pasted fails to upload.
bool save_file(const std::wstring& path, const Bytes& data) {
    make_dirs(path.substr(0, path.find_last_of(L'\\')));
    std::wstring part = path + L"." + std::to_wstring(GetCurrentThreadId()) + L".part";
    HANDLE h = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr) && wrote == data.size();
    CloseHandle(h);
    if (ok && MoveFileExW(part.c_str(), path.c_str(), 0)) return true;
    DeleteFileW(part.c_str());
    return ok && file_exists(path);  // another download of it got there first
}

// Deletes downloaded files older than FILE_KEEP_DAYS and pieces of downloads that never
// finished, then the folders that leaves empty.
void prune_files(const std::wstring& dir) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    auto ticks = [](const FILETIME& t) { return (ULONGLONG)t.dwHighDateTime << 32 | t.dwLowDateTime; };
    ULONGLONG cutoff = ticks(ft) - (ULONGLONG)FILE_KEEP_DAYS * 24 * 3600 * 10000000;  // 100ns units
    auto each = [](const std::wstring& pattern, auto&& fn) {
        WIN32_FIND_DATAW f;
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (wcscmp(f.cFileName, L".") && wcscmp(f.cFileName, L"..")) fn(f);
        } while (FindNextFileW(h, &f));
        FindClose(h);
    };
    each(dir + L"*", [&](const WIN32_FIND_DATAW& folder) {
        if (!(folder.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return;
        std::wstring sub = dir + folder.cFileName + L"\\";
        each(sub + L"*", [&](const WIN32_FIND_DATAW& f) {
            std::wstring name = f.cFileName;
            if (name.ends_with(L".part") || ticks(f.ftLastWriteTime) < cutoff) DeleteFileW((sub + name).c_str());
        });
        RemoveDirectoryW(sub.c_str());  // only if it's empty now
    });
}

// Whether any key is held down right now (mouse buttons, and the codes input methods and
// typed-in characters use, aside).
bool keys_held() {
    for (int vk = VK_BACK; vk <= 0xFE; vk++)
        if (vk != VK_PROCESSKEY && vk != VK_PACKET && GetAsyncKeyState(vk) < 0) return true;
    return false;
}

}  // namespace

// ---- settings.json ------------------------------------------------------------------------

Settings load_settings() {
    Settings s;
    std::ifstream f(settings_path(), std::ios::binary);
    if (!f) return s;  // missing: use defaults
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Json j;
    if (!parse_json(text, j) || !j.is(Json::Object)) return s;  // broken: use defaults
    auto pick = [](const Json& v, const auto& options, std::wstring& out) {
        if (v.is(Json::String))
            for (auto o : options)
                if (widen(v.str) == o) out = o;
    };
    pick(j["paste"], PASTE_MODES, s.paste);
    pick(j["size"], LINK_SIZES, s.size);
    pick(j["format"], LINK_FORMATS, s.format);
    auto number = [](const Json& v, int low, int& out) {
        if (v.is(Json::Number) && v.number >= low && v.number < 100000 && v.number == (int)v.number)
            out = (int)v.number;
    };
    number(j["width"], MIN_WIDTH, s.width);
    number(j["height"], MIN_HEIGHT, s.height);
    if (j["scale"].is(Json::Number))
        s.scale = std::clamp(std::round(j["scale"].number / EMOTE_SCALE_STEP) * EMOTE_SCALE_STEP,
                             MIN_EMOTE_SCALE, MAX_EMOTE_SCALE);
    if (j["collapsed"].is(Json::Bool)) s.collapsed = j["collapsed"].boolean;
    return s;
}

void save_settings(const Settings& s) {
    char scale[16];
    snprintf(scale, sizeof scale, "%.2f", s.scale);
    std::string text = "{\n  \"paste\": " + json_quote(narrow(s.paste)) +
                       ",\n  \"size\": " + json_quote(narrow(s.size)) +
                       ",\n  \"format\": " + json_quote(narrow(s.format)) +
                       ",\n  \"width\": " + std::to_string(s.width) +
                       ",\n  \"height\": " + std::to_string(s.height) +
                       ",\n  \"scale\": " + scale +
                       ",\n  \"collapsed\": " + (s.collapsed ? "true" : "false") + "\n}";
    std::wstring path = settings_path(), temp = path + L".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f || !(f << text)) return;
    }
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring emote_format(const Emote& e, const Settings& s) {
    // 7TV only has gif for animated emotes and png for still ones; swap so links work.
    if (s.format == L"gif" && !e.animated) return L"png";
    if (s.format == L"png" && e.animated) return L"gif";
    return s.format;
}

// "auto" links (and downloads) the smallest 7TV size at least as tall as Discord's emotes:
// files resize from it, and a bigger one would only take longer to download.
std::wstring link_size(const Settings& s) {
    if (s.size != L"auto") return s.size;
    return std::to_wstring(std::clamp((DISCORD_EMOTE_SIZE + 31) / 32, 1, 4)) + L"x";
}

// Only gif and png files can be written here, so webp and avif ones stay as they are.
int file_height(const Emote& e, const Settings& s) {
    std::wstring format = emote_format(e, s);
    return s.size == L"auto" && (format == L"gif" || format == L"png") ? DISCORD_EMOTE_SIZE : 0;
}

std::wstring emote_link(const Emote& e, const Settings& s) {
    std::wstring url = replace_all(LINK_URL, L"{id}", widen(e.id));
    url = replace_all(url, L"{size}", link_size(s));
    return replace_all(url, L"{format}", emote_format(e, s));
}

// ---- diagnostics --------------------------------------------------------------------------

void App::note(const wchar_t* fmt, ...) {
    if (!log_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(log_file, L"%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfwprintf(log_file, fmt, args);
    va_end(args);
    fputwc(L'\n', log_file);
    fflush(log_file);
}

// A window, for the log: handle, class and program (no titles: they can name your chats).
std::wstring App::describe(HWND h) const {
    if (!h) return L"(nothing)";
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    DWORD pid = window_pid(h);
    wchar_t buf[256];
    swprintf_s(buf, L"%p [%ls] %ls", (void*)h, cls, pid == my_pid ? L"the picker" : exe_name(pid).c_str());
    return buf;
}

// ---- the window ---------------------------------------------------------------------------

bool App::init(bool headless_, int width, int height, double scale, bool follow) {
    headless = headless_;
    g_app = this;
    my_pid = GetCurrentProcessId();
    if (!headless && GetEnvironmentVariableW(L"PICKER_LOG", nullptr, 0)) {
        log_file = _wfsopen((exe_dir() + L"picker.log").c_str(), L"a, ccs=UTF-8", _SH_DENYNO);  // readable while open
        note(L"---- started");
    }
    settings = load_settings();
    // File mode's downloads live in your local app data (the program's own folder may be
    // somewhere synced or shared).
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    files_dir = n && n < MAX_PATH ? std::wstring(local) + L"\\7TV Emote Picker\\uploads\\" : exe_dir() + L"uploads\\";
    if (!headless) std::thread([dir = files_dir] { prune_files(dir); }).detach();
    warm_connections(false);  // (the search for popular emotes, at the end, opens the search's)
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&wc);
    if (headless) {
        k = scale;
        win_w = width;
        win_h = height;
        hwnd = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                               inst, this);
    } else {
        // Made hidden; dock() shows it over Discord without ever taking the focus.
        // WS_EX_NOACTIVATE: clicking it leaves Discord in front, with the keyboard. (Without it,
        // even a click that doesn't activate the panel took Discord out of the foreground and
        // left nothing in front.) The search box brings the panel to the front itself.
        hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WINDOW_CLASS,
                               WINDOW_TITLE, WS_POPUP, -32000, -32000, settings.width,
                               settings.height, nullptr, nullptr, inst, this);
        if (hwnd) {
            // Windows 11: rounded corners and a thin dark border, like Discord's own popups.
            DWORD corners = 2;  // DWMWCP_ROUND
            COLORREF border = RGB(BORDER >> 16, BORDER >> 8 & 0xFF, BORDER & 0xFF);
            DwmSetWindowAttribute(hwnd, 33, &corners, sizeof(corners));  // corner preference
            DwmSetWindowAttribute(hwnd, 34, &border, sizeof(border));    // border color
            RECT rc;
            GetClientRect(hwnd, &rc);
            win_w = std::max(1L, rc.right);
            win_h = std::max(1L, rc.bottom);
        }
    }
    if (!hwnd) return false;
    anim_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
    if (!anim_timer) anim_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)dwrite.GetAddressOf())) ||
        !create_device())
        return false;
    make_fonts();
    layout();
    start_workers();
    following = !headless && follow;
    if (following) {
        // Follow Discord through Windows' events: instant, and nothing runs while idle.
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, win_event, 0, 0,
                        WINEVENT_OUTOFCONTEXT);
        SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, win_event, 0,
                        0, WINEVENT_OUTOFCONTEXT);
        SetTimer(hwnd, timer::poll, FOLLOW_POLL_MS, nullptr);
        track_foreground();
    }
    search_now();  // show popular emotes on start
    return true;
}

bool App::pump(DWORD timeout) {
    HANDLE wait[] = {anim_timer};
    if (MsgWaitForMultipleObjectsEx(1, wait, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE) ==
        WAIT_OBJECT_0)
        request_frame();  // an animated emote's next frame is due
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) return false;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (headless && frame_pending) render();
    return true;
}

int App::run() {
    while (pump(INFINITE)) {
    }
    return 0;
}

LRESULT CALLBACK App::wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* app = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        app = (App*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)app);
        app->hwnd = h;
    }
    return app ? app->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_RESULTS:
            drain_results();
            return 0;
        case WM_TIMER:
            switch (wp) {
                case timer::poll: track_foreground(); break;
                case timer::search: search_now(); break;
                case timer::flash:
                    KillTimer(hwnd, timer::flash);
                    flashing = false;
                    request_frame();
                    break;
                case timer::paste:
                    KillTimer(hwnd, timer::paste);
                    send_ctrl_v();
                    break;
                case timer::restore: restore_clipboard(); break;
                case timer::prefetch:
                    KillTimer(hwnd, timer::prefetch);
                    // Two at a time: they share a connection with the one you click.
                    if (settings.paste == L"file" && hover_i >= 0 && hover_i < (int)emotes.size() &&
                        (int)downloading.size() - files_ahead < 2)
                        fetch_file(emotes[hover_i]);
                    break;
                case timer::deliver: deliver_pending(); break;
                case timer::blink:
                    caret_on = !caret_on;
                    request_frame();
                    break;
            }
            return 0;
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            render();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED && LOWORD(lp) && HIWORD(lp)) {
                win_w = LOWORD(lp);
                win_h = HIWORD(lp);
                resize_target();
                if (!settings.collapsed) layout();  // the button has nothing to lay out
                render();
            }
            return 0;
        case WM_MOUSEMOVE:
            mouse_move((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            mouse_down((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp), (wp & MK_SHIFT) != 0,
                       msg == WM_LBUTTONDBLCLK);
            return 0;
        case WM_LBUTTONUP:
            mouse_up();
            return 0;
        case WM_RBUTTONUP: {
            Hot h = hit((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            if (h == Hot::Button || h == Hot::Close) show_menu();
            return 0;
        }
        case WM_CAPTURECHANGED:
            note(L"mouse capture lost to %ls", describe((HWND)lp).c_str());
            if ((HWND)lp != hwnd && drag != Drag::None) mouse_up();
            return 0;
        case WM_CANCELMODE:
            note(L"cancel mode");
            break;
        case WM_ACTIVATEAPP:
            note(L"picker %ls", wp ? L"activated" : L"deactivated");
            break;
        case WM_MOUSELEAVE:
            mouse_leave();
            return 0;
        case WM_MOUSEWHEEL:
            wheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                SetCursor(LoadCursorW(nullptr, cursor_at((float)p.x, (float)p.y)));
                return TRUE;
            }
            break;
        case WM_MOUSEACTIVATE: {
            // Only the search box takes the keyboard. Clicking anything else leaves Discord
            // focused, so an emote pastes straight away.
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            bool field_hit = !settings.collapsed && hit((float)p.x, (float)p.y) == Hot::Field;
            note(L"click: %ls; in front: %ls", field_hit ? L"activate (search box)" : L"don't activate",
                 describe(GetForegroundWindow()).c_str());
            return field_hit ? MA_ACTIVATE : MA_NOACTIVATE;
        }
        case WM_ACTIVATE:
            note(L"WM_ACTIVATE %u, other window %ls", LOWORD(wp), describe((HWND)lp).c_str());
            focused = LOWORD(wp) != WA_INACTIVE;
            caret_on = true;
            if (focused && GetCaretBlinkTime() != INFINITE)
                SetTimer(hwnd, timer::blink, GetCaretBlinkTime(), nullptr);
            else
                KillTimer(hwnd, timer::blink);
            request_frame();
            return 0;
        case WM_KEYDOWN:
            key_down((UINT)wp);
            return 0;
        case WM_CHAR:
            char_typed((wchar_t)wp);
            return 0;
        case WM_IME_STARTCOMPOSITION:
            place_ime();
            break;
        case WM_DPICHANGED:
            return 0;  // the panel follows Discord's scaling and position, not its own
        case WM_RENDERFORMAT:  // a program reads what the clipboard was lent (clipboard_lend)
            render_lent((UINT)wp);
            return 0;
        case WM_RENDERALLFORMATS:  // quitting while the clipboard is lent and not read yet
            if (borrowing) {
                restore_clipboard();  // your copy back now
            } else if (OpenClipboard(hwnd)) {
                if (GetClipboardOwner() == hwnd)
                    for (auto& [format, data] : lent) clipboard_render(format, lent);
                CloseClipboard();
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (borrowing) restore_clipboard();  // quitting right after a paste
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- following Discord --------------------------------------------------------------------

void CALLBACK App::win_event(HWINEVENTHOOK, DWORD event, HWND h, LONG object, LONG child, DWORD,
                             DWORD) {
    App* app = g_app;
    if (!app) return;
    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        if (h && h == app->discord_hwnd && object == OBJID_WINDOW && child == CHILDID_SELF)
            app->dock();  // Discord moved or resized: move along
    } else {
        app->note(L"event: %ls %ls",
                  event == EVENT_SYSTEM_FOREGROUND     ? L"in front now:"
                  : event == EVENT_SYSTEM_MINIMIZESTART ? L"minimizing:"
                                                        : L"restored:",
                  app->describe(h).c_str());
        app->track_foreground();
    }
}

// Remember the last window that had focus that isn't us, and follow Discord.
void App::track_foreground() {
    HWND fg = GetForegroundWindow();
    DWORD pid = fg ? window_pid(fg) : 0;
    if (fg != noted_fg) {
        noted_fg = fg;
        note(L"in front: %ls%ls", describe(fg).c_str(), drag != Drag::None ? L" (mid-drag: ignored)" : L"");
    }
    // Keep the old state while you're typing in the picker or dragging its corner, and while
    // nothing at all is in front (Windows reports that while focus moves): the panel stays put.
    if (fg && pid != my_pid && drag == Drag::None) {
        target_hwnd = fg;
        if (fg != last_fg || pid != last_fg_pid) {
            last_fg = fg;
            last_fg_pid = pid;
            std::wstring exe = fg ? exe_name(pid) : L"";
            last_fg_discord = std::any_of(std::begin(DISCORD_EXES), std::end(DISCORD_EXES),
                                          [&](auto name) { return exe == name; });
        }
        discord_hwnd = fg && last_fg_discord && !IsIconic(fg) ? fg : nullptr;
        if (discord_hwnd) watch_discord(pid);
    }
    dock();
}

// Discord's own moves come as events from its process only, so nothing else wakes us.
void App::watch_discord(DWORD pid) {
    if (pid == watched_pid) return;
    if (location_hook) UnhookWinEvent(location_hook);
    location_hook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                                    nullptr, win_event, pid, 0, WINEVENT_OUTOFCONTEXT);
    watched_pid = pid;
}

// Pin the panel to Discord's bottom-right corner, or hide it when Discord isn't focused.
// SWP_NOACTIVATE: showing it never takes focus away from Discord.
void App::dock() {
    if (headless) return;
    if (!discord_hwnd || !IsWindow(discord_hwnd) || IsIconic(discord_hwnd)) {
        if (dock_pos) {
            note(L"hide");
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);
            dock_pos.reset();
            schedule_animation(INFINITY);  // nothing moves while hidden
        }
        return;
    }
    RECT r = window_rect(discord_hwnd);
    UINT dpi = GetDpiForWindow(discord_hwnd);
    double scale = (dpi ? dpi : 96) / 96.0;
    if (scale != k) {  // different display scaling: resize fonts and spacing
        k = scale;
        make_fonts();
        layout();
    }
    int mr = (int)std::lround(DOCK_MARGIN_RIGHT * k), mb = (int)std::lround(DOCK_MARGIN_BOTTOM * k);
    // Shrunk to the button: it sits where the panel's bottom-right corner goes.
    int want_w = settings.collapsed ? BUTTON_SIZE : settings.width;
    int want_h = settings.collapsed ? BUTTON_SIZE : settings.height;
    int w = std::max(1, std::min((int)std::lround(want_w * k), (int)(r.right - r.left) - mr));
    int h = std::max(1, std::min((int)std::lround(want_h * k), (int)(r.bottom - r.top) - mb));
    RECT pos{r.right - mr - w, r.bottom - mb - h, w, h};
    if (!dock_pos || memcmp(&pos, &*dock_pos, sizeof(pos))) {
        bool showing = !dock_pos;
        dock_pos = pos;
        if (showing) note(L"show at %ld,%ld, %dx%d", pos.left, pos.top, w, h);
        SetWindowPos(hwnd, HWND_TOPMOST, pos.left, pos.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (showing) {
            request_frame();  // animated emotes move again
            warm_connections();  // a search may come next
        }
    }
}

// ---- pasting ------------------------------------------------------------------------------

// Clicking emote i pastes its link, or in file mode the file itself, into Discord.
void App::paste(int i) {
    const Emote& e = emotes[i];
    KillTimer(hwnd, timer::deliver);
    pending.reset();  // a newer click wins over one still downloading
    if (settings.paste != L"file") {
        paste_items(clipboard_text(emote_link(e, settings) + (ADD_SPACE_AFTER ? L" " : L"")), target_hwnd);
        return;
    }
    // File mode: the file the link points to, downloaded into files_dir and pasted as a file.
    // Discord uploads it with the message and shows it at once, instead of loading the link.
    std::wstring path = file_for(e);
    if (file_exists(path)) {
        paste_items(clipboard_files({path}), target_hwnd);
        return;
    }
    // Not downloaded yet: it's pasted once it is (deliver_pending), into the window it's for
    // now. If you were typing in the search box, Discord gets the focus back right away, while
    // Windows allows it (you just clicked).
    pending = Pending{path, e.name, now_ms(), target_hwnd};
    fetch_file(e);
    SetTimer(hwnd, timer::deliver, FILE_WAIT_MS, nullptr);  // when to give up
    if (!dry_run && target_hwnd && IsWindow(target_hwnd) && GetForegroundWindow() == hwnd) bring_forward(target_hwnd);
    request_frame();  // the status line says it's downloading
}

// Lends the clipboard to items and presses Ctrl+V in window, the one you were typing in.
void App::paste_items(const ClipItems& data, HWND window) {
    if (dry_run) {
        dry_pasted = data;
        flash(L"Pasted!");
        return;
    }
    if (!window || !IsWindow(window)) {
        clipboard_set(hwnd, data, false);
        flash(L"Copied (no target window yet)");
        return;
    }
    // Save what you had copied, then lend the clipboard to the link. If an earlier paste is
    // still borrowing it, keep that one's saved copy: it's the one to put back. The link is
    // handed over when Discord reads it (render_lent), so that's when the copy's time to come
    // back starts.
    if (!borrowing) clip_saved = clipboard_snapshot(hwnd);
    auto seq = clipboard_lend(hwnd, data);
    if (!seq) {
        flash(L"Clipboard busy, click again");
        return;
    }
    lent = data;
    clip_seq = *seq;
    borrowing = true;
    KillTimer(hwnd, timer::restore);
    flash(L"Pasted!");
    if (GetForegroundWindow() == window) {
        send_ctrl_v();  // Discord kept the focus: paste now
        return;
    }
    bring_forward(window);
    SetTimer(hwnd, timer::paste, PASTE_DELAY_MS, nullptr);  // give Discord a moment to take focus
}

void App::bring_forward(HWND window) {
    if (!SetForegroundWindow(window)) {
        // Windows sometimes refuses focus switches; any key we send unlocks it.
        // (F24 rather than Alt: a lone Alt tap opens menu mode and eats Ctrl+V.)
        std::vector<INPUT> keys;
        add_key(keys, VK_F24, false);
        add_key(keys, VK_F24, true);
        SendInput((UINT)keys.size(), keys.data(), sizeof(INPUT));
        SetForegroundWindow(window);
    }
}

// Opens the connections a search, its previews and (file mode) a click will use, unless
// they're open: 7TV closes idle ones within a minute, and opening one takes a second or so
// from here. Called when a search may be coming, so that's done while you type. search false:
// a search is starting and opens its own.
void App::warm_connections(bool search) { net_warm_soon(settings.paste == L"file", search); }

// Where emote e's file goes in file mode: a folder for the emote and size ("2x", or "48px" for
// one resized to that), the file named after the emote (Discord shows that name).
std::wstring App::file_for(const Emote& e) const {
    int height = file_height(e, settings);
    std::wstring size = height ? std::to_wstring(height) + L"px" : link_size(settings);
    return files_dir + widen(e.id) + L"-" + size + L"\\" + safe_file_name(e.name) + L"." +
           emote_format(e, settings);
}

// Downloads emote e's file in the background, unless it's here or on its way already;
// file_done() hears when it's saved. A file clicked (or about to be) gets a connection of its
// own; ones in view fetched ahead share the previews' connections.
void App::fetch_file(const Emote& e, bool ahead) {
    std::wstring path = file_for(e);
    if (downloading.count(path)) return;
    if (file_exists(path)) {
        files_here.insert(path);
        return;
    }
    downloading.insert(path);
    files_ahead += ahead;
    CancelFlag cancel;  // files fetched ahead stop if a new search doesn't have their emote
    if (ahead) {
        cancel = std::make_shared<std::atomic<bool>>(false);
        ahead_files[path] = {e.id, cancel};
    }
    std::thread([this, url = emote_link(e, settings), path, ahead, cancel, height = file_height(e, settings)] {
        FileDone d{path, L"", ahead};
        Bytes data;
        bool got = ahead ? fetch(url, "", data, d.error, cancel.get()) : fetch_alone(url, data, d.error);
        // The "auto" size: made as tall as Discord's own emotes (left as 7TV made it if that
        // fails, so it still pastes). Below normal priority, like the previews.
        if (got && height) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Bytes resized;
            if (resize_image(data, height, resized)) data = std::move(resized);
            if (SUCCEEDED(com)) CoUninitialize();
        }
        if (got && !save_file(path, data)) d.error = L"couldn't save it";
        {
            std::lock_guard lock(result_mutex);
            files_done.push_back(std::move(d));
        }
        PostMessageW(hwnd, WM_APP_RESULTS, 0, 0);
    }).detach();
}

// File mode: the files of the emotes in view [from, to) download too, so clicking one pastes
// at once, four at a time. (load_previews calls this once the stills in view are in: those show
// what there is to click.)
void App::fetch_files_in_view(int from, int to) {
    for (int i = from; i < to && files_ahead < 4; i++) {
        std::wstring path = file_for(emotes[i]);
        if (!files_here.count(path)) fetch_file(emotes[i], true);
    }
}

void App::file_done(const FileDone& d) {
    downloading.erase(d.path);
    files_ahead -= d.ahead;
    if (d.ahead) ahead_files.erase(d.path);
    if (d.error.empty()) files_here.insert(d.path);
    if (!pending || pending->path != d.path) return;  // fetched ahead of a click, or not wanted now
    if (!d.error.empty()) {
        flash(L"Couldn't download " + pending->name + L": " + d.error);
        pending.reset();
        KillTimer(hwnd, timer::deliver);
        return;
    }
    deliver_pending();
}

// Pastes the clicked emote's file once it's downloaded. Only into the window it was clicked
// for, never into one you've switched to since (a game, say), and not while you hold a key:
// it would mix with the Ctrl+V (Ctrl+K opens Discord's quick switcher, for one).
void App::deliver_pending() {
    KillTimer(hwnd, timer::deliver);
    if (!pending) return;
    bool late = now_ms() - pending->since >= FILE_WAIT_MS - 1;
    auto give_up = [&](const std::wstring& why) {
        flash(L"Not pasted: " + why);
        pending.reset();
    };
    if (!file_exists(pending->path)) {  // still downloading: called when it's time to give up
        if (late) give_up(pending->name + L" took too long to download");
        else SetTimer(hwnd, timer::deliver, (UINT)(FILE_WAIT_MS - (now_ms() - pending->since)), nullptr);
        return;
    }
    HWND window = pending->target;
    if (!dry_run && window && IsWindow(window)) {
        HWND fg = GetForegroundWindow();
        if (fg && fg != window) {
            give_up(L"Discord isn't in front");
            return;
        }
        if (!fg || keys_held()) {  // focus on its way, or a key down: try again in a moment
            if (late) give_up(fg ? L"a key was held down" : L"Discord isn't in front");
            else SetTimer(hwnd, timer::deliver, 30, nullptr);
            return;
        }
    }
    std::wstring path = pending->path;
    pending.reset();
    paste_items(clipboard_files({path}), window);
}

void App::send_ctrl_v() {
    std::vector<INPUT> keys;
    add_key(keys, VK_CONTROL, false);
    add_key(keys, 'V', false);
    add_key(keys, 'V', true);
    add_key(keys, VK_CONTROL, true);
    SendInput((UINT)keys.size(), keys.data(), sizeof(INPUT));
    wait_for_paste();
}

// The paste is under way: your copy comes back RESTORE_CLIPBOARD_MS after Discord reads the
// link (render_lent), or after RESTORE_CLIPBOARD_MAX_MS if nothing does.
void App::wait_for_paste() { SetTimer(hwnd, timer::restore, RESTORE_CLIPBOARD_MAX_MS, nullptr); }

// A program (Discord, pasting) asks for what the clipboard was lent: hand it over, and put your
// copy back a moment after that.
void App::render_lent(UINT format) {
    if (!clipboard_render(format, lent)) return;
    clip_seq = GetClipboardSequenceNumber();  // (if handing it over counts as a change, it's ours)
    if (borrowing) SetTimer(hwnd, timer::restore, RESTORE_CLIPBOARD_MS, nullptr);
}

// Put back what you had copied before the paste, unless you've copied something new since.
void App::restore_clipboard() {
    KillTimer(hwnd, timer::restore);
    if (clip_saved && GetClipboardSequenceNumber() == clip_seq) {
        // Hidden: it's already in clipboard history from when you copied it.
        if (!clipboard_set(hwnd, *clip_saved, true)) {
            SetTimer(hwnd, timer::restore, 100, nullptr);  // busy: try again soon
            return;
        }
    }
    clip_saved.reset();
    lent.clear();
    borrowing = false;
}

// ---- status line, search, settings --------------------------------------------------------

// The status line, shown whenever the mouse isn't over an emote.
void App::say(const std::wstring& s) {
    status_text = s;
    request_frame();
}

// Shows text on the status line for a moment (ms).
void App::flash(const std::wstring& s, UINT ms) {
    flash_text = s;
    flashing = true;
    SetTimer(hwnd, timer::flash, ms, nullptr);
    request_frame();
}

void App::query_changed() {
    SetTimer(hwnd, timer::search, SEARCH_DELAY_MS, nullptr);  // search once you stop typing
    warm_connections();
    request_frame();
}

void App::search_now() {
    KillTimer(hwnd, timer::search);
    int gen = ++search_gen;
    std::wstring text = trim(box.text());
    search_text = text;
    say(L"Searching…");
    warm_connections(false);  // previews and (file mode) a click come next
    std::thread([this, gen, text] {
        SearchResult r;
        r.gen = gen;
        r.ok = search_emotes(text, r.emotes, r.error);
        {
            std::lock_guard lock(result_mutex);
            search_results.push_back(std::move(r));
        }
        PostMessageW(hwnd, WM_APP_RESULTS, 0, 0);
    }).detach();
}

void App::show_results(std::vector<Emote> found) {
    // Only keep preview images for these results; the picker runs for days and old ones
    // would pile up. Downloads are kept longer, so going back to a search is quick.
    std::unordered_set<std::string> shown;
    for (auto& e : found) shown.insert(e.id);
    cancel_unshown(shown);
    auto unshown = [&](const auto& entry) { return !shown.count(entry.first.substr(0, entry.first.size() - 2)); };
    std::erase_if(stills, unshown);
    std::erase_if(anims, unshown);
    if (raw.size() > MAX_SAVED_PREVIEWS) std::erase_if(raw, unshown);
    emotes = std::move(found);
    index.clear();
    for (int i = 0; i < (int)emotes.size(); i++) index[emotes[i].id] = i;
    hover_i = -1;
    const std::wstring& q = search_text;
    empty_note = !emotes.empty() ? L""
                 : q.empty()     ? L"No emotes found"
                                 : L"No emotes found for “" + q + L"”";
    say(emotes.empty() ? L""
        : q.empty()    ? L"Popular on 7TV"
                       : std::to_wstring(emotes.size()) + L" results for “" + q + L"”");
    scroll = scroll_goal = 0;
    scroll_px = 0;
    scrolling = false;
    started.clear();
    layout();  // the scroll range changed
    if (mouse_inside) hover_at(mouse_x, mouse_y);
}

std::wstring& App::option_value(Option group) {
    return group == Option::Paste ? settings.paste : group == Option::Size ? settings.size : settings.format;
}

void App::set_option(const Segment& seg) {
    option_value(seg.group) = seg.option;
    // Only files can be resized, and only gif and png ones: picking "auto" switches to those
    // (otherwise it would paste 7TV's 2x, and look no different).
    if (seg.group == Option::Size && seg.option == L"auto") {
        settings.paste = L"file";
        if (settings.format == L"webp" || settings.format == L"avif") settings.format = L"gif";
    }
    save_settings(settings);
    warm_connections();
    layout();  // the note next to the formats changes
    if (settings.size == L"auto") flash(auto_note(), 4000);
}

// What the "auto" size does with the paste mode and format picked now (short enough for the
// status line of the narrowest panel).
std::wstring App::auto_note() const {
    std::wstring size = link_size(settings);
    if (settings.paste != L"file") return L"Only files are resized; links stay " + size;
    if (settings.format == L"webp" || settings.format == L"avif")
        return settings.format + L" stays " + size + L"; gif and png resize";
    return L"Made " + std::to_wstring(DISCORD_EMOTE_SIZE) + L"px tall, like Discord's emotes";
}

// What the format buttons don't say: 7TV has gif only for animated emotes and png only for
// still ones, so the others get the other.
std::wstring App::format_note() const {
    return settings.format == L"gif"   ? L"png for still emotes"
           : settings.format == L"png" ? L"gif for animated ones"
                                       : L"";
}

void App::toggle_settings() {
    settings_open = !settings_open;
    layout();
}

// The X shrinks the panel to a button in the same corner of Discord (shrink), and clicking the
// button opens it again. The picker keeps running either way; it quits from show_menu().
void App::collapse(bool shrink) {
    if (settings.collapsed == shrink) return;
    note(shrink ? L"shrink to the button" : L"open from the button");
    if (drag != Drag::None) mouse_up();
    settings.collapsed = shrink;
    if (!shrink) opened_at = now_ms();
    save_settings(settings);
    hot = Hot::None;
    hot_segment = -1;
    hover_i = -1;
    KillTimer(hwnd, timer::prefetch);
    // Typing in the search box put the picker in front: hand the keyboard back to Discord.
    if (shrink && !headless && GetForegroundWindow() == hwnd && target_hwnd && IsWindow(target_hwnd))
        bring_forward(target_hwnd);
    if (headless) {  // tests: no Discord to dock to, so take the size dock() would give
        win_w = (int)std::lround((shrink ? BUTTON_SIZE : settings.width) * k);
        win_h = (int)std::lround((shrink ? BUTTON_SIZE : settings.height) * k);
        resize_target();
        if (!shrink) layout();
    }
    dock();  // resizes the window; WM_SIZE lays it out and draws it
    if (!shrink && mouse_inside && !headless) {  // the panel is under the mouse now
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        mouse_move((float)p.x, (float)p.y);
    }
    request_frame();
}

// Right-clicking the button or the X: open (or shrink) the panel, or quit.
void App::show_menu() {
    if (headless) return;
    HMENU menu = CreatePopupMenu();
    enum { open_id = 1, quit_id };
    AppendMenuW(menu, MF_STRING, open_id, settings.collapsed ? L"Open emote picker" : L"Shrink to button");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, quit_id, L"Quit");
    POINT p;
    GetCursorPos(&p);
    // A menu only closes properly when its window is in front (allowed: you just clicked it).
    HWND back = GetForegroundWindow();
    SetForegroundWindow(hwnd);
    int picked = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                                p.x, p.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (picked == quit_id) {
        DestroyWindow(hwnd);
        return;
    }
    // Discord gets the keyboard back, as before the right-click.
    if (back && back != hwnd && IsWindow(back) && GetForegroundWindow() == hwnd) bring_forward(back);
    if (picked == open_id) collapse(!settings.collapsed);
}
