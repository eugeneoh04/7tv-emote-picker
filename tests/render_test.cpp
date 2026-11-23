// Offscreen checks for the picker: no window is ever shown and nothing takes the focus.
//   render_test.exe <out dir> [--clipboard] [--gifs N] [--cpu SECONDS]
//   render_test.exe <out dir> --hold-file <path> <seconds>
// Draws the picker at 1451x341 (829x195 at 175%) into PNGs while driving it like a user,
// writes decoded GIF frames for comparison with the Python version's decoder, measures CPU
// use while emotes animate, and with --clipboard checks the clipboard borrow round trip.
// --hold-file puts a file on the clipboard for a while (to try pasting it into a browser by
// hand), then puts back what was there.
#include "app.h"

#include <dxgi1_4.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <tuple>

#include <winhttp.h>

struct Tester {
    App& app;
    std::wstring out;

    void pump_for(double ms) {
        double end = now_ms() + ms;
        while (now_ms() < end) app.pump(5);
    }
    // Waits until the search is in and the previews in view have arrived.
    bool settle(double timeout_ms = 20000) {
        double end = now_ms() + timeout_ms;
        while (now_ms() < end) {
            app.pump(10);
            bool searching = app.status_text == L"Searching…";
            if (!searching && app.fetching.empty()) {
                pump_for(50);
                if (app.fetching.empty()) return true;
            }
        }
        return false;
    }
    void shot(const wchar_t* name) {
        app.render();
        bool ok = app.save_png(out + L"\\" + name + L".png");
        wprintf(L"  %-22ls %ls\n", name, ok ? L"saved" : L"FAILED");
    }
    void type(const std::wstring& s) {
        for (wchar_t c : s) app.char_typed(c);
    }
    float cell_x(int i) { return app.grid.l + app.x0 + (i % app.cols + 0.5f) * app.cell; }
    float cell_y(int i) { return app.grid.t + (i / app.cols + 0.5f) * app.cell - app.scroll_px; }
    int playing() {
        int n = 0;
        auto [from, to] = app.on_screen();
        for (int i = from; i < to; i++) n += app.anim_at(i) != nullptr;
        return n;
    }
    int animated_on_screen() {
        int n = 0;
        auto [from, to] = app.on_screen();
        for (int i = from; i < to; i++) n += app.emotes[i].animated;
        return n;
    }

    void wait_search() {
        for (int i = 0; i < 300 && app.status_text == L"Searching…"; i++) app.pump(10);
    }

    static double memory() {
        PROCESS_MEMORY_COUNTERS_EX mem{sizeof(mem)};
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&mem, sizeof(mem));
        return mem.PrivateUsage / 1048576.0;
    }
    // What Task Manager shows: private pages actually in RAM.
    static double ram() {
        PROCESS_MEMORY_COUNTERS_EX2 mem{sizeof(mem)};
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&mem, sizeof(mem));
        return mem.PrivateWorkingSetSize / 1048576.0;
    }

    void startup() {
        wprintf(L"After start: committed %.0f MB, in RAM %.0f MB\n", memory(), ram());
        double t0 = now_ms(), results_at = 0, stills_at = 0;
        while (now_ms() - t0 < 20000 && !stills_at) {
            app.pump(5);
            if (!results_at && !app.emotes.empty()) results_at = now_ms() - t0;
            if (results_at) {
                auto [from, to] = app.on_screen();
                int have = 0;
                for (int i = from; i < to; i++) have += app.stills.count(app.key_of(i)) > 0;
                if (have == to - from) stills_at = now_ms() - t0;
            }
        }
        wprintf(L"Results after %.0f ms, every still in view after %.0f ms\n", results_at, stills_at);
        settle();
        int requests, http2;
        net_stats(requests, http2);
        wprintf(L"GIFs in view ready after %.0f ms; committed %.0f MB, in RAM %.0f MB; %d requests, %d over HTTP/2\n",
                now_ms() - t0, memory(), ram(), requests, http2);
    }

    void ui_shots() {
        wprintf(L"UI at %dx%d, scale %.2f\n", app.win_w, app.win_h, app.k);
        if (!settle()) wprintf(L"  (timed out waiting for previews)\n");
        wprintf(L"  %zu results, %d per row, cell %dpx, previews %dx (room %d)\n", app.emotes.size(),
                app.cols, app.cell, app.thumb_s, app.room);
        shot(L"01-popular");
        app.mouse_inside = true;
        app.mouse_move(cell_x(2), cell_y(2));
        shot(L"02-hover");
        app.mouse_move(app.gear.l + 2, app.gear.t + 2);
        app.toggle_settings();
        shot(L"03-settings");
        if (app.segments.size() > 5) app.mouse_move(app.segments[5].box.l + 2, app.segments[5].box.t + 2);
        shot(L"03b-settings-hover");
        app.toggle_settings();
        app.mouse_leave();
        app.focused = true;
        type(L"catjam");
        app.search_now();
        settle();
        shot(L"04-search");
        app.box.select_all();
        app.text_edited(false);
        shot(L"05-selection");
        app.box.end(false);
        app.text_edited(false);
        type(L" with a very long search text that will not fit in the box at all");
        shot(L"05b-long-text");
        app.box.select_all();
        app.box.erase_back(false);
        app.text_edited(true);
        app.search_now();
        settle();
        app.mouse_inside = true;
        app.mouse_move(cell_x(0), cell_y(0));
        app.wheel(-120);
        pump_for(60);
        shot(L"06a-scrolling");
        pump_for(400);
        settle();
        shot(L"06b-scrolled");
        wprintf(L"  scroll after one notch: %dpx (cell %d)\n", app.scroll_px, app.cell);
        app.scroll_to(app.scroll_max(), false);
        settle();
        shot(L"06c-bottom");
        app.scroll_to(0, false);
        app.mouse_leave();
        type(L"zzqqxxnotanemote");
        app.search_now();
        settle();
        shot(L"07-empty");
        app.box.select_all();
        app.box.erase_back(false);
        app.text_edited(true);
        app.search_now();
        settle();
    }

    // The emote size slider in the settings row: dragged (previews scaled from what's there),
    // let go (made again at the new size), and at the ends of its range.
    void slider_shots() {
        wprintf(L"Emote size slider\n");
        app.toggle_settings();
        app.mouse_inside = true;
        auto x_of = [&](double v) {
            return app.scale_track.l +
                   (float)((v - MIN_EMOTE_SCALE) / (MAX_EMOTE_SCALE - MIN_EMOTE_SCALE)) * app.scale_track.w();
        };
        float y = (app.scale_row.t + app.scale_row.b) / 2;
        auto report = [&](const wchar_t* name) {
            wprintf(L"  %-24ls %3.0f%%: emotes %dpx from %dx previews, %d per row\n", name, app.settings.scale * 100,
                    app.emote_h, app.thumb_s, app.cols);
            shot(name);
        };
        app.mouse_move(x_of(1.0), y);
        app.mouse_down(x_of(1.5), y, false, false);  // press on the track: jumps there
        report(L"09a-slider-dragging");               // old previews drawn scaled for now
        app.mouse_up();
        settle();
        report(L"09b-slider-150");
        app.set_emote_scale(0.75);
        settle();
        report(L"09c-slider-75");
        app.set_emote_scale(3.0);
        settle();
        report(L"09d-slider-300");
        app.set_emote_scale(1.0);
        settle();
        app.mouse_leave();
        app.toggle_settings();
    }

    // The settings at a few panel sizes, in both paste modes: the button groups line up after
    // the labels where they fit, and go on more lines on a narrow panel.
    void settings_shots() {
        wprintf(L"Settings\n");
        int w0 = app.win_w, h0 = app.win_h;
        Settings saved = app.settings;
        auto size_to = [&](int w, int h) {
            app.win_w = w;
            app.win_h = h;
            app.resize_target();
            app.layout();
        };
        app.toggle_settings();
        struct {
            int w, h;
            const wchar_t* name;
        } sizes[] = {{1451, 341, L"strip"}, {595, 735, L"default"}, {385, 590, L"narrowest"}};
        for (auto& s : sizes) {
            size_to(s.w, s.h);
            for (const wchar_t* mode : PASTE_MODES) {
                app.settings.paste = mode;
                app.settings.format = std::wstring(mode) == L"file" ? L"gif" : L"webp";
                app.layout();
                settle();
                std::vector<float> lines;
                bool inside = true;
                for (auto& seg : app.segments) {
                    if (std::find(lines.begin(), lines.end(), seg.box.t) == lines.end()) lines.push_back(seg.box.t);
                    inside &= seg.box.l >= 0 && seg.box.r <= app.win_w;
                }
                bool track_inside = app.scale_track.l > 0 && app.scale_value_x < app.win_w;
                std::wstring name = std::wstring(L"10-settings-") + s.name + L"-" + mode;
                wprintf(L"  %-34ls %4dx%-4d buttons on %zu line(s), all inside: %ls, slider inside: %ls, note: %ls\n",
                        name.c_str(), s.w, s.h, lines.size(), inside ? L"yes" : L"NO", track_inside ? L"yes" : L"NO",
                        app.note_box.w() > 0 ? L"shown" : L"no room");
                shot(name.c_str());
            }
        }
        app.settings = saved;
        size_to(w0, h0);
        app.toggle_settings();
        settle();
    }

    // The file a clipboard CF_HDROP holds (the first one).
    static std::wstring dropped_file(const ClipItems& items) {
        for (auto& [f, d] : items)
            if (f == CF_HDROP && d.size() > sizeof(DROPFILES)) {
                auto* h = (const DROPFILES*)d.data();
                if (!h->fWide || h->pFiles >= d.size()) return L"(not wide)";
                return std::wstring((const wchar_t*)(d.data() + h->pFiles));
            }
        return L"";
    }
    static std::string head(const std::wstring& path, size_t n) {
        std::ifstream f(path, std::ios::binary);
        std::string s(n, '\0');
        f.read(s.data(), n);
        return s;
    }

    // File mode, dry run (nothing goes on the clipboard and no key is pressed): the mouse
    // resting on an emote downloads its file; a click on one that isn't downloaded yet waits for
    // it, then pastes it as a file; a second click pastes at once; the files of the emotes in
    // view download by themselves.
    void file_mode() {
        wprintf(L"File mode (dry run)\n");
        Settings saved = app.settings;
        wchar_t full[MAX_PATH];
        GetFullPathNameW((out + L"\\uploads\\").c_str(), MAX_PATH, full, nullptr);
        app.files_dir = full;
        std::error_code ec;
        std::filesystem::remove_all(app.files_dir, ec);  // start with nothing downloaded
        app.files_here.clear();
        app.dry_run = true;
        app.settings.paste = L"file";  // (not through the button: that would fetch the view's files)
        app.settings.size = L"2x";
        app.settings.format = L"gif";
        app.warm_files();  // as when file mode is picked (or the panel shows, or you search)
        pump_for(1500);
        int still = -1, far_anim = -1;
        auto [from, to] = app.on_screen();
        for (int i = from; i < to && still < 0; i++)
            if (!app.emotes[i].animated) still = i;
        for (int i = to; i < (int)app.emotes.size() && far_anim < 0; i++)
            if (app.emotes[i].animated) far_anim = i;
        if (still < 0 || far_anim < 0) {
            wprintf(L"  need a still emote in view and an animated one below (got %d, %d)\n", still, far_anim);
            app.settings = saved;
            app.dry_run = false;
            return;
        }
        // The mouse resting on a still emote downloads its png, no click needed.
        std::wstring still_path = app.file_for(app.emotes[still]);
        app.mouse_inside = true;
        app.mouse_move(cell_x(still), cell_y(still));
        double t0 = now_ms();
        while (now_ms() - t0 < 1000 && !app.downloading.count(still_path)) app.pump(5);
        double started = now_ms() - t0;
        while (now_ms() - t0 < 15000 && app.downloading.count(still_path)) app.pump(5);
        wprintf(L"  mouse resting on a still emote: download started after %.0f ms, done after %.0f ms, PNG: %ls\n",
                started, now_ms() - t0, head(still_path, 4) == "\x89PNG" ? L"yes" : L"NO");
        app.mouse_leave();
        // A click on one that isn't here (from further down: nothing fetched it ahead): the
        // status line says it's downloading, then it pastes.
        std::wstring path = app.file_for(app.emotes[far_anim]);
        app.dry_pasted.clear();
        t0 = now_ms();
        app.paste(far_anim);
        bool waited = app.pending.has_value();
        shot(L"11a-file-downloading");
        while (now_ms() - t0 < 15000 && app.dry_pasted.empty()) app.pump(5);
        double cold = now_ms() - t0;
        std::wstring pasted = dropped_file(app.dry_pasted);
        std::error_code size_ec;
        auto bytes = std::filesystem::file_size(path, size_ec);
        wprintf(L"  click, not downloaded: waited %ls, pasted after %.0f ms, the file: %ls, %llu KB, GIF: %ls\n",
                waited ? L"yes" : L"NO", cold, pasted == path ? L"yes" : L"NO", (unsigned long long)bytes / 1024,
                head(path, 6) == "GIF89a" ? L"yes" : L"NO");
        wprintf(L"    %ls\n", path.c_str());
        shot(L"11b-file-pasted");
        // Again: it's on disk now.
        app.dry_pasted.clear();
        t0 = now_ms();
        app.paste(far_anim);
        wprintf(L"  click, downloaded before: pasted after %.1f ms\n", now_ms() - t0);
        // A file that arrives after you've switched away from the window it was clicked for
        // (to a game, say) isn't pasted anywhere. Not a dry run: the check under test is skipped
        // in those. The stand-in window is never in front, so this stops before the clipboard.
        HWND stand_in = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        app.dry_run = false;
        app.flashing = false;
        app.pending = App::Pending{path, app.emotes[far_anim].name, now_ms(), stand_in};
        DWORD seq = GetClipboardSequenceNumber();
        app.deliver_pending();
        bool held_back = !app.pending && app.flashing && app.flash_text == L"Not pasted: Discord isn't in front";
        wprintf(L"  file in after switching windows: not pasted: %ls, clipboard untouched: %ls\n",
                held_back ? L"yes" : L"NO", GetClipboardSequenceNumber() == seq ? L"yes" : L"NO");
        app.pending.reset();
        KillTimer(app.hwnd, timer::deliver);
        app.dry_run = true;
        DestroyWindow(stand_in);
        // The files of the emotes in view come by themselves.
        app.layout();
        t0 = now_ms();
        int have = 0, n = to - from;
        for (double report = 5000; now_ms() - t0 < 60000 && have < n; app.pump(20)) {
            have = 0;
            for (int i = from; i < to; i++) have += std::filesystem::exists(app.file_for(app.emotes[i]), ec);
            if (now_ms() - t0 >= report) {
                wprintf(L"  files of the %d emotes in view: %d here after %.0f s\n", n, have, report / 1000);
                report += 5000;
            }
        }
        wprintf(L"  files of the %d emotes in view: %d here after %.1f s, at most %d at once\n", n, have,
                (now_ms() - t0) / 1000, 4);
        // Names Windows can't take as file names.
        Emote odd{"0000TESTID", L"wh?at:<>.", false}, device{"0000TESTID", L"CON", false};
        wprintf(L"  file names: \"%ls\", \"%ls\"\n", std::filesystem::path(app.file_for(odd)).filename().c_str(),
                std::filesystem::path(app.file_for(device)).filename().c_str());
        app.settings = saved;
        app.dry_run = false;
        settle();
    }

    // A GET on a session of its own (its own connection), for comparing with fetch()'s shared ones.
    static bool fetch_own(HINTERNET session, const std::wstring& path, Bytes& out) {
        static std::map<HINTERNET, HINTERNET> conns;
        HINTERNET& c = conns[session];
        if (!c) c = WinHttpConnect(session, L"cdn.7tv.app", INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        bool ok = r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
                  WinHttpReceiveResponse(r, nullptr);
        out.clear();
        static uint8_t buf[65536];
        for (DWORD got = 0; ok && WinHttpReadData(r, buf, sizeof(buf), &got) && got;) out.insert(out.end(), buf, buf + got);
        if (r) WinHttpCloseHandle(r);
        return ok && !out.empty();
    }

    // How long single GIF downloads take: fetch()'s shared HTTP/2 connections, or a connection
    // of their own (HTTP/1.1, and HTTP/2).
    void file_speed(int n) {
        wprintf(L"File download speed, %d animated emotes, 2x.gif\n", n);
        wait_search();
        HINTERNET own[2];
        for (int h = 0; h < 2; h++) {
            own[h] = WinHttpOpen(L"emote-picker/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
            DWORD protocols = h ? WINHTTP_PROTOCOL_FLAG_HTTP2 : 0;
            WinHttpSetOption(own[h], WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols));
            Bytes warm;
            fetch_own(own[h], L"/emote/" + widen(app.emotes[0].id) + L"/1x.png", warm);
        }
        std::vector<double> times[3];
        int done = 0;
        for (int i = (int)app.emotes.size() - 1; i >= 0 && done < n; i--) {
            if (!app.emotes[i].animated) continue;
            std::wstring path = L"/emote/" + widen(app.emotes[i].id) + L"/2x.gif";
            double t[3] = {};
            size_t size = 0;
            for (int k = 0; k < 3; k++) {
                int way = done % 2 ? 2 - k : k;  // alternate the order: the first one may wake 7TV's cache
                Bytes data;
                std::wstring err;
                double t0 = now_ms();
                bool ok = way == 0 ? fetch(L"https://cdn.7tv.app" + path, "", data, err) : fetch_own(own[way - 1], path, data);
                t[way] = ok ? now_ms() - t0 : -1;
                size = std::max(size, data.size());
            }
            if (size < 100000) continue;  // small ones say little
            for (int k = 0; k < 3; k++) times[k].push_back(t[k]);
            wprintf(L"  %4zu KB: shared HTTP/2 %5.0f ms, own HTTP/1.1 %5.0f ms, own HTTP/2 %5.0f ms\n", size / 1024,
                    t[0], t[1], t[2]);
            done++;
        }
        const wchar_t* names[] = {L"shared HTTP/2", L"own HTTP/1.1", L"own HTTP/2"};
        for (int k = 0; k < 3; k++) {
            std::sort(times[k].begin(), times[k].end());
            if (!times[k].empty()) wprintf(L"  median %-14ls %5.0f ms\n", names[k], times[k][times[k].size() / 2]);
        }
    }

    void animation() {
        wprintf(L"Animation\n");
        double end = now_ms() + 20000;
        while (now_ms() < end && (playing() < animated_on_screen() || !app.fetching.empty())) app.pump(10);
        wprintf(L"  %d of %d animated emotes on screen are moving\n", playing(), animated_on_screen());
        double atlas = 0, gif = 0, raw = 0, still = 0;
        for (auto& [key, a] : app.anims)
            if (a) {
                gif += a->gif ? a->gif->size() : 0;
                if (a->atlas) atlas += a->atlas->GetPixelSize().width * a->atlas->GetPixelSize().height * 4.0;
            }
        for (auto& [key, b] : app.raw) raw += b->size();
        for (auto& [key, s] : app.stills) still += s.w * s.h * 4.0;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIAdapter3> adapter3;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
        if (SUCCEEDED(app.d3d.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter.As(&adapter3))) {
            adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
            adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
        }
        wprintf(L"  frames on the GPU %.1f MB, stills %.1f MB; kept downloads: GIFs %.1f MB, PNGs %.1f MB\n",
                atlas / 1048576, still / 1048576, gif / 1048576, raw / 1048576);
        wprintf(L"  video memory used: %.0f MB on the card, %.0f MB shared; process private %.0f MB\n",
                local.CurrentUsage / 1048576.0, shared.CurrentUsage / 1048576.0, memory());
        shot(L"08a-frame");
        pump_for(120);
        shot(L"08b-frame");
    }

    void cpu(double seconds) {
        FILETIME c, e, k0, u0, k1, u1;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k0, &u0);
        auto ticks = [](FILETIME f) { return (double)(((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime); };
        int frames = 0;
        double seen = app.last_frame, end = now_ms() + seconds * 1000;
        while (now_ms() < end) {
            app.pump(50);
            if (app.last_frame != seen) frames++, seen = app.last_frame;
        }
        wprintf(L"  %.0f frames drawn per second\n", frames / seconds);
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k1, &u1);
        double used_ms = (ticks(k1) - ticks(k0) + ticks(u1) - ticks(u0)) / 10000.0;
        PROCESS_MEMORY_COUNTERS_EX mem{sizeof(mem)};
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&mem, sizeof(mem));
        wprintf(L"CPU while %d emotes animate: %.1f%% of one core over %.0fs; private memory %.0f MB\n",
                playing(), used_ms / (seconds * 10), seconds, mem.PrivateUsage / 1048576.0);
    }

    // Decoded frames of the first n animated results, for tests/compare_gifs.py.
    void dump_gifs(int n) {
        wprintf(L"GIF frames for comparison\n");
        CreateDirectoryW((out + L"\\gifs").c_str(), nullptr);
        int done = 0;
        for (auto& e : app.emotes) {
            if (!e.animated || done >= n) continue;
            Bytes data;
            std::wstring err;
            std::wstring url = L"https://cdn.7tv.app/emote/" + widen(e.id) + L"/2x.gif";
            Animation a;
            if (!fetch(url, "", data, err) || !decode_gif(data, 0, 0, 16384, a)) {
                wprintf(L"  %hs: %ls\n", e.id.c_str(), err.empty() ? L"didn't decode" : err.c_str());
                continue;
            }
            std::wstring base = out + L"\\gifs\\" + widen(e.id);
            std::ofstream(base + L".gif", std::ios::binary).write((const char*)data.data(), data.size());
            std::ofstream raw(base + L".bgra", std::ios::binary);
            raw.write((const char*)a.atlas.px.data(), a.atlas.px.size() * 4);
            std::ofstream meta(base + L".txt");
            meta << a.w << " " << a.h << " " << a.count << " " << a.cols << " " << a.atlas.w;
            for (int t : a.ends) meta << " " << t;
            done++;
        }
        wprintf(L"  wrote %d\n", done);
    }

    // Downloads previews straight through fetch() from 12 threads, to time the network alone.
    void net(bool gifs) {
        std::vector<std::wstring> urls;
        for (auto& e : app.emotes) {
            if (gifs && !e.animated) continue;
            urls.push_back(L"https://cdn.7tv.app/emote/" + widen(e.id) +
                           (gifs ? L"/2x.gif" : e.animated ? L"/2x_static.png" : L"/2x.png"));
            if (urls.size() >= 48) break;
        }
        std::atomic<size_t> next{0}, bytes{0};
        std::vector<double> took(urls.size());
        double t0 = now_ms();
        std::vector<std::thread> threads;
        for (int k = 0; k < PREVIEW_DOWNLOADS; k++)
            threads.emplace_back([&] {
                for (size_t i; (i = next++) < urls.size();) {
                    double s = now_ms();
                    Bytes data;
                    std::wstring err;
                    fetch(urls[i], "", data, err);
                    took[i] = now_ms() - s;
                    bytes += data.size();
                }
            });
        for (auto& t : threads) t.join();
        std::sort(took.begin(), took.end());
        wprintf(L"%zu %ls: %.0f ms total, %.0f KB; each took median %.0f ms, slowest %.0f ms\n", urls.size(),
                gifs ? L"GIFs" : L"stills", now_ms() - t0, bytes / 1024.0, took[took.size() / 2], took.back());
    }

    // Many searches, each scrolled through to the bottom, watching memory (the picker runs for days).
    void soak(int rounds) {
        const wchar_t* words[] = {L"pepe", L"cat", L"dance", L"kekw", L"monka", L"pog", L"sad", L"wave",
                                  L"jam", L"hello", L"lul", L"clap", L"dog", L"love", L"no", L"yes"};
        wprintf(L"Soak test: %d searches, each scrolled to the bottom\n", rounds);
        app.mouse_leave();
        for (int r = 0; r < rounds; r++) {
            app.box.select_all();
            app.box.insert(words[r % std::size(words)] + std::wstring(r >= (int)std::size(words) ? L"s" : L""));
            app.search_now();
            settle(15000);
            for (int step = 0; step < 12 && app.scroll < app.scroll_max(); step++) {
                app.scroll_to(app.scroll_goal + app.grid.h(), true);
                pump_for(250);
            }
            settle(15000);
            if (r % 4 == 3)
                wprintf(L"  after %2d searches: committed %.0f MB, in RAM %.0f MB, %zu GIFs kept, %zu PNGs kept\n",
                        r + 1, memory(), ram(), app.anims.size(), app.raw.size());
        }
    }

    // A window as Windows composes it (DirectX content included), saved as a PNG.
    static bool save_window_png(HWND hwnd, const std::wstring& path) {
        RECT r;
        GetWindowRect(hwnd, &r);
        int w = r.right - r.left, h = r.bottom - r.top;
        HDC wdc = GetWindowDC(hwnd), mdc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
        HGDIOBJ old = SelectObject(mdc, bmp);
        bool ok = PrintWindow(hwnd, mdc, PW_RENDERFULLCONTENT);
        if (!ok) wprintf(L"  (PrintWindow failed: error %lu)\n", GetLastError());
        SelectObject(mdc, old);
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        int step = 0;
        HRESULT hr = S_OK;
        auto go = [&](HRESULT r) { step++; hr = r; return SUCCEEDED(r); };
        ok = ok && go(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
             go(wic->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, &bitmap)) &&
             go(wic->CreateStream(&stream)) && go(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
             go(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
             go(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
             go(encoder->CreateNewFrame(&frame, nullptr)) && go(frame->Initialize(nullptr)) &&
             go(frame->SetSize(w, h)) && go(frame->SetPixelFormat(&format)) &&
             go(frame->WriteSource(bitmap.Get(), nullptr)) && go(frame->Commit()) && go(encoder->Commit());
        if (!ok) wprintf(L"  (saving failed at step %d: 0x%08lx)\n", step, (unsigned long)hr);
        DeleteObject(bmp);
        DeleteDC(mdc);
        ReleaseDC(hwnd, wdc);
        return ok;
    }

    // The real window (swap chain and all), docked to a hidden stand-in for Discord placed past
    // the right edge of every screen, resized step by step the way dragging the grip does.
    // Nothing is shown on a monitor and nothing is activated.
    void resize_drag() {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        HWND stand_in = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"stand-in",
                                        WS_POPUP, vx + vw + 400, 0, 3840, 2076, nullptr, nullptr, nullptr, nullptr);
        app.discord_hwnd = stand_in;
        app.dock();
        settle();
        auto check = [&](const wchar_t* when) {
            DXGI_SWAP_CHAIN_DESC1 d{};
            app.swap->GetDesc1(&d);
            UINT sw = d.Width, sh = d.Height;  // what's shown: all of the buffers, or part of them
            if (app.swap2) app.swap2->GetSourceSize(&sw, &sh);
            RECT r;
            GetWindowRect(app.hwnd, &r);
            bool ok = IsWindowVisible(app.hwnd) && app.target && (int)sw == app.win_w && (int)sh == app.win_h &&
                      r.right - r.left == app.win_w && r.bottom - r.top == app.win_h;
            if (!ok || when)
                wprintf(L"  %-10ls window %ldx%ld visible=%d | target %ls | shows %ux%u of %ux%u buffers | "
                        L"buffer reallocations %d, failures %d\n",
                        when ? when : L"(problem)", r.right - r.left, r.bottom - r.top, IsWindowVisible(app.hwnd),
                        app.target ? L"ok" : L"GONE", sw, sh, d.Width, d.Height, app.buffer_resizes,
                        app.resize_failures);
            return ok;
        };
        wprintf(L"Resize drag on the real window, off screen\n");
        check(L"docked");
        int bad = 0, frames0 = 0, realloc0 = app.buffer_resizes;
        double seen = app.last_frame;
        for (int i = 0; i < 240; i++) {
            int d = i < 120 ? 6 : -6;  // grow up-left a long way, then shrink back
            app.settings.width += d;
            app.settings.height += d / 2;
            app.dock();
            app.pump(0);
            if (app.last_frame != seen) frames0++, seen = app.last_frame;
            bad += !check(nullptr);
        }
        pump_for(200);
        check(L"after");
        wprintf(L"  %d of 240 resize steps had a problem; %d frames drawn; buffers reallocated %d times\n", bad,
                frames0, app.buffer_resizes - realloc0);
        app.drag = App::Drag::Resize;  // letting go of the grip hands back the spare buffer space
        app.mouse_up();
        check(L"released");
        // What Windows actually composes for the window at a few sizes (PrintWindow).
        int n = 0;
        for (auto [w, h] : {std::pair{615, 203}, std::pair{400, 160}, std::pair{900, 300}, std::pair{615, 203}}) {
            app.settings.width = w;
            app.settings.height = h;
            app.dock();
            pump_for(150);
            std::wstring name = L"composed-" + std::to_wstring(++n) + L"-" + std::to_wstring(app.win_w) + L"x" +
                                std::to_wstring(app.win_h) + L".png";
            wprintf(L"  %-28ls %ls\n", name.c_str(), save_window_png(app.hwnd, out + L"\\" + name) ? L"captured" : L"FAILED");
        }
        DestroyWindow(stand_in);
    }

    // How much process memory GPU bitmaps cost: makes some, measures, frees them, measures.
    void bitmaps() {
        auto flush = [&] {
            ComPtr<ID3D11DeviceContext> c;
            app.d3d->GetImmediateContext(&c);
            c->Flush();
            pump_for(200);
        };
        flush();
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(app.d3d.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter))) adapter->GetDesc(&desc);
        wprintf(L"  drawing on: %ls (%.0f MB dedicated video memory, %.0f MB shared), feature level %x\n",
                desc.Description, desc.DedicatedVideoMemory / 1048576.0, desc.SharedSystemMemory / 1048576.0,
                app.d3d->GetFeatureLevel());
        ComPtr<IDXGIFactory1> factory;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; factory && SUCCEEDED(factory->EnumAdapters1(i, &a)); i++) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            wprintf(L"  adapter %u: %ls, %.0f MB dedicated\n", i, d.Description, d.DedicatedVideoMemory / 1048576.0);
        }
        {  // textures made directly, filled through one reused staging texture
            ComPtr<ID3D11DeviceContext> ctx;
            app.d3d->GetImmediateContext(&ctx);
            D3D11_TEXTURE2D_DESC sd{1024, 1024, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0,
                                    D3D11_CPU_ACCESS_WRITE, 0};
            double before = memory();
            ComPtr<ID3D11Texture2D> staging;
            app.d3d->CreateTexture2D(&sd, nullptr, &staging);
            flush();
            double with_staging = memory();
            for (auto [w, h, n] : {std::tuple{64, 64, 400}, std::tuple{1024, 1024, 25}}) {
                double b0 = memory();
                std::vector<uint32_t> px((size_t)w * h, 0x80402010);
                std::vector<ComPtr<ID2D1Bitmap1>> made(n);
                for (auto& b : made) {
                    D3D11_TEXTURE2D_DESC td{(UINT)w, (UINT)h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                                            D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
                    ComPtr<ID3D11Texture2D> tex;
                    app.d3d->CreateTexture2D(&td, nullptr, &tex);
                    D3D11_MAPPED_SUBRESOURCE m{};
                    ctx->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &m);
                    for (int y = 0; y < h; y++) memcpy((uint8_t*)m.pData + y * m.RowPitch, &px[(size_t)y * w], w * 4);
                    ctx->Unmap(staging.Get(), 0);
                    D3D11_BOX box{0, 0, 0, (UINT)w, (UINT)h, 1};
                    ctx->CopySubresourceRegion(tex.Get(), 0, 0, 0, 0, staging.Get(), 0, &box);
                    ComPtr<IDXGISurface> surface;
                    tex.As(&surface);
                    app.dc->CreateBitmapFromDxgiSurface(surface.Get(), D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
                }
                flush();
                double with = memory();
                made.clear();
                flush();
                wprintf(L"  own staging (%.0f MB): %d of %dx%d (%.0f MB of pixels): private +%.0f MB, after freeing +%.0f MB\n",
                        with_staging - before, n, w, h, n * w * h * 4 / 1048576.0, with - b0, memory() - b0);
            }
        }
        auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        for (bool copy_after : {true, false})
            for (auto [w, h, n] : {std::tuple{64, 64, 400}, std::tuple{1024, 1024, 25}}) {
                double before = memory();
                std::vector<uint32_t> px((size_t)w * h, 0x80402010);
                std::vector<ComPtr<ID2D1Bitmap1>> made(n);
                for (auto& b : made) {
                    app.dc->CreateBitmap(D2D1::SizeU(w, h), copy_after ? nullptr : px.data(), w * 4, props, &b);
                    if (copy_after) b->CopyFromMemory(nullptr, px.data(), w * 4);
                }
                flush();
                double with = memory();
                made.clear();
                flush();
                wprintf(L"  %ls: %d bitmaps of %dx%d (%.0f MB of pixels): private +%.0f MB, after freeing +%.0f MB\n",
                        copy_after ? L"empty, then copy" : L"made with pixels", n, w, h, n * w * h * 4 / 1048576.0,
                        with - before, memory() - before);
            }
    }

    void clipboard() {
        wprintf(L"Clipboard borrow round trip\n");
        auto before = clipboard_snapshot(app.hwnd);
        if (!before) {
            wprintf(L"  clipboard busy\n");
            return;
        }
        auto seq = clipboard_set(app.hwnd, clipboard_text(L"https://cdn.7tv.app/emote/test/2x.webp"), true);
        std::wstring now = clipboard_get_text(app.hwnd);
        bool hidden = IsClipboardFormatAvailable(RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"));
        bool same_seq = seq && GetClipboardSequenceNumber() == *seq;
        auto restored = clipboard_set(app.hwnd, *before, true);
        auto after = clipboard_snapshot(app.hwnd);
        // Every format you had comes back byte for byte (plus the "keep out of history" marks).
        bool equal = after.has_value();
        for (size_t i = 0; equal && i < before->size(); i++) {
            auto& [f1, d1] = (*before)[i];
            bool found = false;
            for (auto& [f2, d2] : *after)
                if (f1 == f2) found = d1.size() <= d2.size() && std::equal(d1.begin(), d1.end(), d2.begin());
            if (!found) {
                wchar_t name[64] = L"?";
                GetClipboardFormatNameW(f1, name, 64);
                wprintf(L"  format %u (%ls) differs\n", f1, name);
            }
            equal = found;
        }
        wprintf(L"  link on clipboard: %ls, hidden from history: %ls, sequence matched: %ls\n",
                now == L"https://cdn.7tv.app/emote/test/2x.webp" ? L"yes" : L"NO", hidden ? L"yes" : L"NO",
                same_seq ? L"yes" : L"NO");
        wprintf(L"  restored %zu formats, identical: %ls\n", before->size(), restored && equal ? L"yes" : L"NO");

        // A file, read back the way programs read pasted files (DragQueryFile).
        std::wstring path = L"C:\\Users\\Test\\AppData\\Local\\7TV Emote Picker\\uploads\\x-2x\\catJAM.gif";
        auto before2 = clipboard_snapshot(app.hwnd);
        clipboard_set(app.hwnd, clipboard_files({path}), true);
        std::wstring got;
        DWORD effect = 0;
        if (OpenClipboard(app.hwnd)) {
            if (HANDLE h = GetClipboardData(CF_HDROP)) {
                wchar_t buf[MAX_PATH * 2] = L"";
                UINT n = DragQueryFileW((HDROP)h, 0xFFFFFFFF, nullptr, 0);
                DragQueryFileW((HDROP)h, 0, buf, (UINT)std::size(buf));
                got = n == 1 ? buf : L"(" + std::to_wstring(n) + L" files)";
            }
            if (HANDLE h = GetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)))
                if (auto* p = (DWORD*)GlobalLock(h)) {
                    effect = *p;
                    GlobalUnlock(h);
                }
            CloseClipboard();
        }
        if (before2) clipboard_set(app.hwnd, *before2, true);
        wprintf(L"  file on clipboard: %ls, as a copy: %ls, put back: %ls\n", got == path ? L"yes" : got.c_str(),
                effect == DROPEFFECT_COPY ? L"yes" : L"NO", before2 ? L"yes" : L"NO");
    }
};

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: render_test <out dir> [--clipboard] [--gifs N] [--cpu SECONDS]\n");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // like the picker
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    net_init();
    // Start from the default settings: the tests' own settings.json (next to this program, in
    // build\) keeps whatever the last run left, like the size slider's position.
    DeleteFileW((exe_dir() + L"settings.json").c_str());
    CreateDirectoryW(argv[1], nullptr);
    for (int i = 2; i + 2 < argc; i++)
        if (std::wstring(argv[i]) == L"--hold-file") {
            HWND owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
            auto before = clipboard_snapshot(owner);
            auto seq = clipboard_set(owner, clipboard_files({argv[i + 1]}), true);
            wprintf(L"File on the clipboard: %ls\n", seq ? L"yes" : L"NO (busy)");
            fflush(stdout);
            Sleep((DWORD)(_wtof(argv[i + 2]) * 1000));
            bool untouched = seq && GetClipboardSequenceNumber() == *seq;
            if (before && untouched) clipboard_set(owner, *before, true);
            wprintf(L"Put back what was there: %ls\n", before && untouched ? L"yes" : L"no (something new was copied)");
            fflush(stdout);
            ExitProcess(0);
        }
    for (int i = 2; i < argc; i++)
        if (std::wstring(argv[i]) == L"--resize") {
            auto* windowed = new App;
            if (!windowed->init(false, 0, 0, 1.0, false)) return 1;
            Tester{*windowed, argv[1]}.resize_drag();
            fflush(stdout);
            ExitProcess(0);
        }
    wprintf(L"Private memory before the picker: %.0f MB\n", Tester::memory());
    if (!GetEnvironmentVariableW(L"NO_WARM_UP", nullptr, 0)) net_warm_up(L"https://cdn.7tv.app/");
    CreateDirectoryW(argv[1], nullptr);
    auto* app = new App;
    if (!app->init(true, 1451, 341, 1.75)) {
        wprintf(L"init failed\n");
        return 1;
    }
    Tester t{*app, argv[1]};
    bool clip = false, net = false;
    int gifs = 0, soak = 0;
    double cpu_s = 0;
    for (int i = 2; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--clipboard") clip = true;
        else if (a == L"--net") net = true;
        else if (a == L"--soak" && i + 1 < argc) soak = _wtoi(argv[++i]);
        else if (a == L"--gifs" && i + 1 < argc) gifs = _wtoi(argv[++i]);
        else if (a == L"--cpu" && i + 1 < argc) cpu_s = _wtof(argv[++i]);
    }
    if (GetEnvironmentVariableW(L"BITMAPS", nullptr, 0)) {
        t.bitmaps();
        fflush(stdout);
        ExitProcess(0);
    }
    for (int i = 2; i + 1 < argc; i++)
        if (std::wstring(argv[i]) == L"--file-speed") {
            t.file_speed(_wtoi(argv[i + 1]));
            fflush(stdout);
            ExitProcess(0);
        }
    if (net) {  // the network alone, before the picker's own downloads warm anything up
        t.wait_search();
        t.net(false);
        t.net(true);
        t.net(false);
        fflush(stdout);
        ExitProcess(0);
    }
    t.startup();
    t.ui_shots();
    t.slider_shots();
    t.settings_shots();
    t.file_mode();
    t.animation();
    if (cpu_s > 0) t.cpu(cpu_s);
    if (gifs > 0) t.dump_gifs(gifs);
    if (soak > 0) t.soak(soak);
    if (clip) t.clipboard();
    fflush(stdout);
    ExitProcess(0);
}
