/* 7TV emote picker.

A tiny borderless panel docked to the bottom-right of Discord, just above the message box.
It only shows while Discord (or the picker itself) is focused. Type to search 7TV, click an
emote and its link is pasted into Discord. The link only borrows the clipboard: whatever you
had copied before is put back right after. Drag the corner grip to resize. The X shrinks it to a
small button in the same corner of Discord (click it to open the panel again); right-click
the button or the X to quit. Starting it again (say, after rebuilding) replaces the copy that's running, including
the old Python version.

Build with build.cmd; run "7TV Emote Picker.exe". Settings you might change are in config.h.
*/
#include "app.h"

namespace {

// Close any picker that's already running, as if its X was clicked, and wait for it to
// exit. One that's stuck gets ended after 3 seconds.
void close_running_picker() {
    std::vector<std::pair<HWND, DWORD>> running;
    for (auto cls : {WINDOW_CLASS, L"TkTopLevel"}) {  // this one, and the old Python one
        HWND h = nullptr;
        while ((h = FindWindowExW(nullptr, h, cls, WINDOW_TITLE))) {
            DWORD pid = window_pid(h);
            if (pid == GetCurrentProcessId()) continue;
            if (cls == WINDOW_CLASS || exe_name(pid).starts_with(L"python")) running.push_back({h, pid});
        }
    }
    for (auto [h, pid] : running) {
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        PostMessageW(h, WM_CLOSE, 0, 0);
        if (process) {
            if (WaitForSingleObject(process, 3000) != WAIT_OBJECT_0) {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 2000);
            }
            CloseHandle(process);
        }
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // also in the manifest
    // One picker at a time: it also starts with Windows, and starting it again should give
    // you the new version, not a second panel or nothing.
    close_running_picker();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    net_init();
    net_warm_up(L"https://cdn.7tv.app/");  // previews come from there once the search is in
    auto* app = new App;  // lives as long as the process
    if (!app->init()) {
        MessageBoxW(nullptr, L"The emote picker couldn't start its window or graphics.",
                    L"7TV Emote Picker", MB_ICONERROR);
        return 1;
    }
    int code = app->run();
    ExitProcess(code);  // the download threads are still waiting for work
}
