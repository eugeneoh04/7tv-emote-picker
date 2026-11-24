// Offscreen checks for the picker: no window is ever shown and nothing takes the focus.
//   render_test.exe <out dir> [--clipboard] [--gifs N] [--cpu SECONDS]
//   render_test.exe <out dir> --hold-file <path> <seconds>
//   render_test.exe <out dir> --auto-size N   (file mode's "auto" size on N animated emotes)
//   render_test.exe <out dir> --search-speed [ROUNDS] [--pause SECONDS]   (see search_speed)
//   render_test.exe <out dir> --webp N   (WebP previews decoded, for checking against ffmpeg)
//   render_test.exe <out dir> --clipboard-only   (the clipboard borrow checks, nothing else)
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
#include <iterator>
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

    // Files the tests download (file mode) go in <out dir>\uploads, never in your own folder.
    void own_uploads() {
        wchar_t full[MAX_PATH];
        GetFullPathNameW((out + L"\\uploads\\").c_str(), MAX_PATH, full, nullptr);
        app.files_dir = full;
        app.files_here.clear();
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
        NetStats n = net_stats();
        wprintf(L"Animations in view ready after %.0f ms; committed %.0f MB, in RAM %.0f MB; %d requests, %d over HTTP/2\n",
                now_ms() - t0, memory(), ram(), n.requests, n.http2);
    }

    // ---- how fast a search shows its emotes (--search-speed) ----------------------------------

    struct Phase {
        double results = -1, first = -1, stills = -1, anims = -1;  // ms after the last letter typed
        int in_view = 0, animated = 0, moving = 0, blank = 0;
        NetStats net;  // during the phase
    };
    static NetStats minus(const NetStats& a, const NetStats& b) {
        return {a.requests - b.requests, a.http2 - b.http2, a.bytes - b.bytes, a.cancelled - b.cancelled};
    }
    // Clicks the search box, then types q one letter every 120 ms (quick typing). Times are from
    // the last letter: the results, the first still in view, every still in view, every animated
    // emote in view moving. lead_ms > 0: stops that long after the results (to type the next
    // search while this one still loads).
    Phase search_timed(const std::wstring& q, double lead_ms = 0) {
        Phase p;
        NetStats before = net_stats();
        app.mouse_down(app.field.l + app.field.w() / 2, app.field.t + app.field.h() / 2, false, false);
        app.mouse_up();
        app.focused = true;
        pump_for(400);  // the hand moves to the keyboard
        app.box.select_all();
        for (wchar_t c : q) {
            app.char_typed(c);
            pump_for(120);
        }
        double t0 = now_ms() - 120;
        while (now_ms() - t0 < 30000) {
            app.pump(2);
            double t = now_ms() - t0;
            if (p.results < 0) {
                if (app.search_text != q || app.status_text == L"Searching…") continue;
                p.results = t;
            }
            if (lead_ms > 0 && t - p.results >= lead_ms) break;
            auto [from, to] = app.on_screen();
            int stills = 0, have = 0, animated = 0, moving = 0, done = 0;
            for (int i = from; i < to; i++) {
                const Emote& e = app.emotes[i];
                std::string key = app.key_of(i);
                bool still = app.stills.count(key) > 0;
                have += still;
                // A still that failed to download stays blank: done too.
                stills += still || (app.started.count(e.id) && !app.fetching.count(key + "s"));
                if (!e.animated) continue;
                animated++;
                auto a = app.anims.find(key);
                bool plays = app.anim_at(i) != nullptr;
                moving += plays;
                done += plays || (a != app.anims.end() && !a->second && !app.fetching.count(key + "g"));
            }
            p.in_view = to - from;
            p.animated = animated;
            p.moving = moving;
            p.blank = p.in_view - have;
            if (p.first < 0 && have) p.first = t;
            if (p.stills < 0 && stills == p.in_view) p.stills = t;
            if (p.stills >= 0 && done == animated) {
                p.anims = t;
                break;
            }
        }
        p.net = minus(net_stats(), before);
        return p;
    }
    static void print_phase(const wchar_t* what, const std::wstring& q, const Phase& p) {
        auto ms = [](double v) { return v < 0 ? std::wstring(L"    -") : std::to_wstring((int)std::lround(v)); };
        wprintf(L"  %-16ls %-9ls results %5ls, first still %5ls, %2d stills %5ls, %2d/%2d moving %5ls ms; "
                L"%5.1f MB in %d requests, %d stopped part-way\n",
                what, (L"\"" + q + L"\"").c_str(), ms(p.results).c_str(), ms(p.first).c_str(), p.in_view,
                ms(p.stills).c_str(), p.moving, p.animated, ms(p.anims).c_str(), p.net.bytes / 1048576.0,
                p.net.requests, p.net.cancelled);
    }

    // Each round: a pause long enough for 7TV to close idle connections (its CDN after 30 s, its
    // search server after 50 s), then a search; a second search typed 1.5 s after another one's
    // results came in (while that one's previews still load); then the first search again.
    // The panel the results in docs/search-speed.md were measured with: 577x569 at 175%, the
    // size slider at 240% (154px emotes from 4x previews), pasting "auto" gif files (so the files
    // in view download too). Downloads several MB a round, slowing anything else on the
    // connection meanwhile.
    void search_speed(int rounds, double pause_s) {
        own_uploads();
        std::error_code ec;
        std::filesystem::remove_all(app.files_dir, ec);  // no files downloaded yet
        app.settings.scale = 2.4;
        app.settings.paste = L"file";
        app.settings.size = L"auto";
        app.settings.format = L"gif";
        app.layout();
        wait_search();
        settle();
        wprintf(L"Search speed: panel %dx%d, %dpx emotes from %dx previews, %d per row\n", app.win_w, app.win_h,
                app.emote_h, app.thumb_s, app.cols);
        const wchar_t* queries[][3] = {{L"pepe", L"kekw", L"catjam"}, {L"peepo", L"monka", L"pog"},
                                       {L"cat", L"dance", L"sad"},    {L"lul", L"hype", L"wave"}};
        std::vector<Phase> phases[3];
        for (int r = 0; r < rounds; r++) {
            const wchar_t** q = queries[r % std::size(queries)];
            wprintf(L"Round %d: waiting %.0f s for connections to go idle\n", r + 1, pause_s);
            pump_for(pause_s * 1000);
            phases[0].push_back(search_timed(q[0]));
            print_phase(L"after a pause", q[0], phases[0].back());
            settle();
            search_timed(q[1], 1500);
            phases[1].push_back(search_timed(q[2]));
            print_phase(L"during another", q[2], phases[1].back());
            settle();
            phases[2].push_back(search_timed(q[0]));
            print_phase(L"again", q[0], phases[2].back());
            settle();
        }
        const wchar_t* names[] = {L"after a pause", L"during another", L"again"};
        wprintf(L"Medians over %d rounds (ms after the last letter; >30000: not all done within 30 s):\n", rounds);
        // The middle value (the mean of the middle two for an even count). A round that didn't
        // finish within 30 s counts as longer than any that did.
        auto median = [](std::vector<double> v) {
            for (double& x : v) x = x < 0 ? INFINITY : x;
            std::sort(v.begin(), v.end());
            size_t n = v.size();
            return n == 0 ? NAN : n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
        };
        auto ms = [](double v) {
            return std::isnan(v) ? std::wstring(L"    -") : std::isinf(v) ? std::wstring(L">30000") : std::to_wstring((int)std::lround(v));
        };
        for (int k = 0; k < 3; k++) {
            auto of = [&](double Phase::*field) {
                std::vector<double> v;
                for (auto& p : phases[k]) v.push_back(p.*field);
                return ms(median(v));
            };
            std::vector<double> mb;
            for (auto& p : phases[k]) mb.push_back(p.net.bytes / 1048576.0);
            wprintf(L"  %-16ls results %5ls, first still %5ls, all stills %5ls, all moving %6ls; %.1f MB\n", names[k],
                    of(&Phase::results).c_str(), of(&Phase::first).c_str(), of(&Phase::stills).c_str(),
                    of(&Phase::anims).c_str(), mb.empty() ? 0.0 : median(mb));
        }
    }

    // The X shrinks the panel to a button; clicking the button opens it again.
    void button_shots() {
        int w = app.win_w, h = app.win_h;
        // headless, the panel reopens at the saved size: the size this run started at
        app.settings.width = (int)std::lround(w / app.k);
        app.settings.height = (int)std::lround(h / app.k);
        app.mouse_inside = true;
        app.mouse_down(app.close.l + 2, app.close.t + 2, false, false);
        app.mouse_up();
        app.mouse_leave();
        shot(L"07a-button");
        bool shrunk = app.settings.collapsed && app.win_w == (int)std::lround(BUTTON_SIZE * app.k) && app.win_h == app.win_w;
        app.mouse_move(app.win_w / 2.f, app.win_h / 2.f);
        shot(L"07b-button-hover");
        bool saved = load_settings().collapsed;
        int emotes_before = (int)app.emotes.size();
        app.mouse_down(app.win_w / 2.f, app.win_h / 2.f, false, false);
        app.mouse_up();
        // A quick second click (a double-click) must not paste whatever opened under the mouse.
        app.dry_run = true;
        app.dry_pasted = {};
        app.mouse_down(cell_x(0), cell_y(0), false, true);
        bool pasted = !app.dry_pasted.empty();
        app.dry_run = false;
        app.mouse_leave();
        settle();
        shot(L"07c-opened-again");
        wprintf(L"  X -> button %dx%d (%ls), saved: %ls; reopened %dx%d (%ls), %d emotes kept (%ls), "
                L"double-click pasted: %ls\n",
                shrunk ? (int)std::lround(BUTTON_SIZE * app.k) : -1, shrunk ? (int)std::lround(BUTTON_SIZE * app.k) : -1,
                shrunk ? L"ok" : L"WRONG", saved ? L"yes" : L"NO", app.win_w, app.win_h,
                app.win_w == w && app.win_h == h && !app.settings.collapsed ? L"ok" : L"WRONG",
                (int)app.emotes.size(), (int)app.emotes.size() == emotes_before ? L"ok" : L"WRONG",
                pasted ? L"YES (wrong)" : L"no (ok)");
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
        // "auto" from a fresh settings file (link, 4x, webp): it switches to gif files, since only
        // those resize. Picking link or webp after it says what that does, on the status line.
        // At the narrowest width, where the status line is shortest.
        app.settings = Settings{};
        app.layout();
        auto click = [&](const wchar_t* option, const wchar_t* name) {
            for (auto& seg : app.segments)
                if (seg.option == option) {
                    app.set_option(seg);
                    break;
                }
            wprintf(L"  %-34ls now %ls, %ls, %ls: \"%ls\"\n", name, app.settings.paste.c_str(), app.settings.size.c_str(),
                    app.settings.format.c_str(), app.flashing ? app.flash_text.c_str() : L"(nothing)");
            shot(name);
        };
        click(L"auto", L"10b-auto-from-link-webp");
        click(L"link", L"10c-auto-then-link");
        click(L"file", L"10d-auto-then-file");
        click(L"webp", L"10e-auto-then-webp");
        app.settings = saved;
        save_settings(app.settings);
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
    static Bytes read_file(const std::wstring& path) {
        std::ifstream f(path, std::ios::binary);
        return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
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
        app.warm_connections();  // as when file mode is picked (or the panel shows, or you search)
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
        // The "auto" size: the same two emotes' files, resized to Discord's emote size on the way
        // in (the 2x ones above are what they're made from). The GIF keeps every frame and its
        // timing.
        app.settings.size = L"auto";
        std::wstring auto_gif = app.file_for(app.emotes[far_anim]), auto_png = app.file_for(app.emotes[still]);
        app.dry_pasted.clear();
        t0 = now_ms();
        app.paste(far_anim);
        while (now_ms() - t0 < 15000 && app.dry_pasted.empty()) app.pump(5);
        double auto_ms = now_ms() - t0;
        bool gif_pasted = dropped_file(app.dry_pasted) == auto_gif;
        app.dry_pasted.clear();
        app.paste(still);
        for (double t1 = now_ms(); now_ms() - t1 < 15000 && app.dry_pasted.empty();) app.pump(5);
        bool png_pasted = dropped_file(app.dry_pasted) == auto_png;
        Animation a2, a48;
        Image s2, s48;
        bool gif_ok = decode_gif(read_file(path), 0, 0, 16384, a2) && decode_gif(read_file(auto_gif), 0, 0, 16384, a48);
        bool png_ok = decode_still(read_file(still_path), 0, 0, s2) && decode_still(read_file(auto_png), 0, 0, s48);
        wprintf(L"  auto size, animated: pasted %ls after %.0f ms, GIF %dx%d from %dx%d, %d of %d frames, same timing: %ls, "
                L"%llu KB (2x: %llu KB)\n",
                gif_pasted ? L"yes" : L"NO", auto_ms, a48.w, a48.h, a2.w, a2.h, a48.count, a2.count,
                gif_ok && a48.ends == a2.ends ? L"yes" : L"NO",
                (unsigned long long)std::filesystem::file_size(auto_gif, ec) / 1024,
                (unsigned long long)std::filesystem::file_size(path, ec) / 1024);
        wprintf(L"  auto size, still: pasted %ls, PNG %dx%d from %dx%d, PNG file: %ls\n", png_pasted ? L"yes" : L"NO",
                s48.w, s48.h, s2.w, s2.h, png_ok && head(auto_png, 4) == "\x89PNG" ? L"yes" : L"NO");
        wprintf(L"    %ls\n", auto_gif.c_str());
        app.settings.paste = L"link";
        wprintf(L"  auto size, link mode: %ls\n", emote_link(app.emotes[far_anim], app.settings).c_str());
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
        double atlas = 0, moving = 0, raw = 0, still = 0;
        for (auto& [key, a] : app.anims)
            if (a && a->atlas) atlas += a->atlas->GetPixelSize().width * a->atlas->GetPixelSize().height * 4.0;
        for (auto& [key, f] : app.moving_files) moving += f.bytes->size();
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
        wprintf(L"  frames on the GPU %.1f MB, stills %.1f MB; kept downloads: animations %.1f MB, stills %.1f MB\n",
                atlas / 1048576, still / 1048576, moving / 1048576, raw / 1048576);
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

    // WebP previews decoded by the picker (libwebp), as-is, for checking against another
    // decoder (ffmpeg's own): for n of these animated emotes their 2x.webp (every frame, in an
    // atlas) and 2x_static.webp, and for n/2 still ones their 2x.webp. In <out dir>\webp: the
    // .webp, pixels as .bgra (premultiplied), and .txt: frame w h count cols atlas-width, then
    // when each frame ends (ms).
    static void dump_webp(const std::wstring& out, const std::vector<Emote>& emotes, int n) {
        wprintf(L"WebP frames for comparison\n");
        CreateDirectoryW((out + L"\\webp").c_str(), nullptr);
        int moving = 0, stills = 0, failed = 0;
        auto save = [&](const std::wstring& name, const Bytes& data, const std::vector<uint32_t>& px,
                        const std::string& meta) {
            std::wstring base = out + L"\\webp\\" + name;
            std::ofstream(base + L".webp", std::ios::binary).write((const char*)data.data(), data.size());
            std::ofstream(base + L".bgra", std::ios::binary).write((const char*)px.data(), px.size() * 4);
            std::ofstream(base + L".txt") << meta;
        };
        for (auto& e : emotes) {
            bool want = e.animated ? moving < n : stills < n / 2;
            if (!want) continue;
            std::wstring id = widen(e.id), url = L"https://cdn.7tv.app/emote/" + id;
            Bytes data, still;
            std::wstring err;
            if (e.animated) {
                Animation a;
                Image s;
                if (!fetch(url + L"/2x.webp", "", data, err) || !decode_animation(data, 0, 0, 16384, a) ||
                    !fetch(url + L"/2x_static.webp", "", still, err) || !decode_still(still, 0, 0, s)) {
                    wprintf(L"  %ls: %ls\n", id.c_str(), err.empty() ? L"didn't decode" : err.c_str());
                    failed++;
                    continue;
                }
                std::string meta = std::to_string(a.w) + " " + std::to_string(a.h) + " " + std::to_string(a.count) +
                                   " " + std::to_string(a.cols) + " " + std::to_string(a.atlas.w);
                for (int t : a.ends) meta += " " + std::to_string(t);
                save(id + L"-moving", data, a.atlas.px, meta);
                save(id + L"-static", still, s.px,
                     std::to_string(s.w) + " " + std::to_string(s.h) + " 1 1 " + std::to_string(s.w));
                moving++;
            } else {
                Image s;
                if (!fetch(url + L"/2x.webp", "", data, err) || !decode_still(data, 0, 0, s)) {
                    wprintf(L"  %ls: %ls\n", id.c_str(), err.empty() ? L"didn't decode" : err.c_str());
                    failed++;
                    continue;
                }
                save(id + L"-still", data, s.px, std::to_string(s.w) + " " + std::to_string(s.h) + " 1 1 " + std::to_string(s.w));
                stills++;
            }
        }
        wprintf(L"  wrote %d animated (with their stills) and %d still emotes; %d failed\n", moving, stills, failed);
    }

    // A w x h picture (premultiplied BGRA) scaled to fw x fh.
    static std::vector<uint32_t> scaled(const std::vector<uint32_t>& px, int w, int h, int fw, int fh,
                                        WICBitmapInterpolationMode mode) {
        std::vector<uint32_t> to((size_t)fw * fh, 0);
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4, (UINT)px.size() * 4,
                                                  (BYTE*)px.data(), &bitmap)) &&
            SUCCEEDED(wic->CreateBitmapScaler(&scaler)) && SUCCEEDED(scaler->Initialize(bitmap.Get(), fw, fh, mode)))
            scaler->CopyPixels(nullptr, fw * 4, (UINT)to.size() * 4, (BYTE*)to.data());
        return to;
    }
    // Frame f of an animation, on its own.
    static std::vector<uint32_t> frame_of(const Animation& a, int f) {
        std::vector<uint32_t> px((size_t)a.w * a.h);
        const uint32_t* cell = &a.atlas.px[(size_t)(f / a.cols) * a.h * a.atlas.w + (size_t)(f % a.cols) * a.w];
        for (int y = 0; y < a.h; y++) std::copy_n(cell + (size_t)y * a.atlas.w, a.w, &px[(size_t)y * a.w]);
        return px;
    }
    // Draws px (premultiplied BGRA, w x h) over the opaque picture `to` (to_w wide) at x, y.
    static void paint(std::vector<uint32_t>& to, int to_w, int x, int y, const std::vector<uint32_t>& px, int w, int h) {
        int to_h = (int)(to.size() / to_w);
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                if (x + i < 0 || x + i >= to_w || y + j < 0 || y + j >= to_h) continue;
                uint32_t s = px[(size_t)j * w + i], a = s >> 24, blended = 0xFF000000;
                uint32_t& d = to[(size_t)(y + j) * to_w + x + i];
                for (int sh = 0; sh < 24; sh += 8)
                    blended |= std::min(255u, (s >> sh & 0xFF) + (d >> sh & 0xFF) * (255 - a) / 255) << sh;
                d = blended;
            }
    }
    static bool save_picture(const std::wstring& path, std::vector<uint32_t>& px, int w, int h) {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
               SUCCEEDED(wic->CreateStream(&stream)) &&
               SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
               SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
               SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
               SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
               SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
               SUCCEEDED(frame->WritePixels(h, w * 4, w * h * 4, (BYTE*)px.data())) && SUCCEEDED(frame->Commit()) &&
               SUCCEEDED(encoder->Commit());
    }

    // File mode's "auto" size on the first n animated results: each 2x GIF resized to Discord's
    // emote size, read back (by WIC), and compared with the same frames scaled without the GIF
    // step: same size, frames and timing, the same pixels transparent, colors close. Saves the
    // GIFs in <out>\resized (to try other decoders on) and, for the six whose colors came out
    // furthest off, frames zoomed in 4x (scaled, then as written) and 1x, auto and 2x side by
    // side the way Discord shows them at 175%.
    void resize_check(int n) {
        wprintf(L"Auto size: the first %d animated emotes, 2x GIF to %dpx tall\n", n, DISCORD_EMOTE_SIZE);
        std::wstring dir = out + L"\\resized";
        CreateDirectoryW(dir.c_str(), nullptr);
        struct Sample {
            Animation want, got, one, two;
        };
        std::vector<std::pair<double, const Emote*>> by_error;
        std::vector<double> times;
        int good = 0, bad = 0, worst_max = 0;
        double worst_mean = 0;
        size_t bytes_in = 0, bytes_out = 0;
        for (auto& e : app.emotes) {
            if (!e.animated || good + bad >= n) continue;
            std::wstring url = L"https://cdn.7tv.app/emote/" + widen(e.id), err;
            Bytes src, made;
            if (!fetch(url + L"/2x.gif", "", src, err)) {
                wprintf(L"  %hs: %ls\n", e.id.c_str(), err.c_str());
                continue;
            }
            double t0 = now_ms();
            bool resized = resize_image(src, DISCORD_EMOTE_SIZE, made);
            double took = now_ms() - t0;
            times.push_back(took);
            Sample s;
            if (!resized || !decode_gif(src, DISCORD_EMOTE_SIZE, 0, 16384, s.want) ||
                !decode_gif(made, 0, 0, 16384, s.got) || !decode_gif(src, 0, 0, 16384, s.two)) {
                wprintf(L"  %-24ls %ls\n", e.name.substr(0, 24).c_str(), resized ? L"made, but doesn't read back" : L"NOT resized");
                bad++;
                continue;
            }
            std::ofstream(dir + L"\\" + widen(e.id) + L".gif", std::ios::binary).write((const char*)made.data(), made.size());
            std::ofstream(dir + L"\\" + widen(e.id) + L"-2x.gif", std::ios::binary).write((const char*)src.data(), src.size());
            {  // its frames as read back, one after another (BGRA), to compare with other decoders'
                std::ofstream raw(dir + L"\\" + widen(e.id) + L".bgra", std::ios::binary);
                for (int f = 0; f < s.got.count; f++) {
                    auto px = frame_of(s.got, f);
                    raw.write((const char*)px.data(), px.size() * 4);
                }
            }
            bytes_in += src.size();
            bytes_out += made.size();
            bool same = s.want.w == s.got.w && s.want.h == s.got.h && s.want.count == s.got.count && s.want.ends == s.got.ends;
            // Pixel by pixel: transparent where the scaled frame is under half opaque, otherwise its
            // color, give or take what bringing it down to 255 colors costs.
            long long opaque = 0, alpha_wrong = 0;
            double err_sum = 0;
            int err_max = 0;
            for (size_t i = 0; same && i < s.want.atlas.px.size(); i++) {
                uint32_t w = s.want.atlas.px[i], g = s.got.atlas.px[i], a = w >> 24;
                bool want_opaque = a >= 128, got_opaque = g >> 24 == 255;
                if (want_opaque != got_opaque || (!got_opaque && g != 0)) {
                    alpha_wrong++;
                    continue;
                }
                if (!want_opaque) continue;
                opaque++;
                for (int sh = 0; sh < 24; sh += 8) {
                    int wc = (int)std::min(255u, ((w >> sh & 0xFF) * 255 + a / 2) / a), gc = g >> sh & 0xFF;
                    err_sum += std::abs(wc - gc);
                    err_max = std::max(err_max, std::abs(wc - gc));
                }
            }
            double err_mean = opaque ? err_sum / (opaque * 3.0) : 0;
            bool ok = same && alpha_wrong == 0;
            good += ok;
            bad += !ok;
            worst_mean = std::max(worst_mean, err_mean);
            worst_max = std::max(worst_max, err_max);
            wprintf(L"  %-24ls %3dx%-3d -> %3dx%-3d %3d frames, timing %ls, transparency wrong %lld px, color off %.2f "
                    L"on average (at most %3d), %4zu -> %4zu KB, %4.0f ms\n",
                    e.name.substr(0, 24).c_str(), s.two.w, s.two.h, s.got.w, s.got.h, s.got.count,
                    same ? L"same" : L"DIFFERENT", alpha_wrong, err_mean, err_max, src.size() / 1024, made.size() / 1024,
                    took);
            if (ok) by_error.push_back({err_mean, &e});
        }
        std::sort(times.begin(), times.end());
        wprintf(L"  %d of %d fine; color off %.2f on average at worst (at most %d); %zu KB of 2x GIFs made %zu KB; "
                L"resizing took median %.0f ms, slowest %.0f ms\n",
                good, good + bad, worst_mean, worst_max, bytes_in / 1024, bytes_out / 1024,
                times.empty() ? 0 : times[times.size() / 2], times.empty() ? 0 : times.back());
        // The pictures: the six furthest off, read back from the files saved above.
        std::sort(by_error.rbegin(), by_error.rend());
        std::vector<Sample> samples;
        for (auto& [off, e] : by_error) {
            if (samples.size() >= 6) break;
            Sample s;
            Bytes one;
            std::wstring err, base = dir + L"\\" + widen(e->id);
            if (decode_gif(read_file(base + L"-2x.gif"), DISCORD_EMOTE_SIZE, 0, 16384, s.want) &&
                decode_gif(read_file(base + L".gif"), 0, 0, 16384, s.got) &&
                decode_gif(read_file(base + L"-2x.gif"), 0, 0, 16384, s.two) &&
                fetch(L"https://cdn.7tv.app/emote/" + widen(e->id) + L"/1x.gif", "", one, err) &&
                decode_gif(one, 0, 0, 16384, s.one)) {
                wprintf(L"  in the pictures: %ls (color off %.2f)\n", e->name.c_str(), off);
                samples.push_back(std::move(s));
            }
        }
        if (samples.empty()) return;

        const uint32_t discord_bg = 0xFF313338, outline = 0xFF4E5058;
        // Zoomed in 4x, frames 0 and the middle one: scaled (soft edges), then as written.
        int zoom = 4, cell_w = 0, cell_h = zoom * DISCORD_EMOTE_SIZE, gap = 8;
        for (auto& s : samples) cell_w = std::max(cell_w, zoom * s.got.w);
        int zw = 4 * (cell_w + gap) + gap, zh = (int)samples.size() * (cell_h + gap) + gap;
        std::vector<uint32_t> z((size_t)zw * zh, discord_bg);
        for (int r = 0; r < (int)samples.size(); r++) {
            auto& s = samples[r];
            int x = gap, y = gap + r * (cell_h + gap);
            for (int f : {0, s.got.count / 2})
                for (const Animation* a : {&s.want, &s.got}) {
                    paint(z, zw, x, y, scaled(frame_of(*a, f), a->w, a->h, zoom * a->w, zoom * a->h,
                                              WICBitmapInterpolationModeNearestNeighbor),
                          zoom * a->w, zoom * a->h);
                    x += cell_w + gap;
                }
        }
        bool z_ok = save_picture(out + L"\\12a-auto-zoomed.png", z, zw, zh);
        // As Discord shows them at 175%: a box the size of Discord's own emotes (48px there is
        // 84 here), then 1x, auto and 2x, standing on one line.
        double k = 1.75;
        auto at_k = [&](int v) { return (int)std::lround(v * k); };
        int box = at_k(DISCORD_EMOTE_SIZE), row_h = at_k(64) + 2 * gap, dw = 1200, dh = (int)samples.size() * row_h + gap;
        std::vector<uint32_t> d((size_t)dw * dh, discord_bg);
        for (int r = 0; r < (int)samples.size(); r++) {
            auto& s = samples[r];
            int base = gap + r * row_h + at_k(64), x = gap;
            for (int i = 0; i < box; i++) {  // the outline
                d[(size_t)(base - box) * dw + x + i] = d[(size_t)(base - 1) * dw + x + i] = outline;
                d[(size_t)(base - box + i) * dw + x] = d[(size_t)(base - box + i) * dw + x + box - 1] = outline;
            }
            x += box + 3 * gap;
            for (const Animation* a : {&s.one, &s.got, &s.two}) {
                int w = at_k(a->w), h = at_k(a->h);
                paint(d, dw, x, base - h, scaled(frame_of(*a, 0), a->w, a->h, w, h, WICBitmapInterpolationModeCubic), w, h);
                x += w + 3 * gap;
            }
        }
        bool d_ok = save_picture(out + L"\\12b-auto-as-discord-shows.png", d, dw, dh);
        wprintf(L"  12a-auto-zoomed         %ls (frame 0 and the middle one: scaled, then as written, 4x)\n",
                z_ok ? L"saved" : L"FAILED");
        wprintf(L"  12b-auto-as-discord-shows %ls (Discord's emote size, then 1x, auto, 2x, at 175%%)\n",
                d_ok ? L"saved" : L"FAILED");
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
                wprintf(L"  after %2d searches: committed %.0f MB, in RAM %.0f MB, %zu animations (%zu downloads kept), "
                        L"%zu stills kept\n",
                        r + 1, memory(), ram(), app.anims.size(), app.moving_files.size(), app.raw.size());
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
        lend_checks();
    }

    // Every format in `before` is in `after`, byte for byte (Windows may round sizes up).
    static bool same_clip(const ClipItems& before, const ClipItems& after) {
        for (auto& [f1, d1] : before) {
            bool found = false;
            for (auto& [f2, d2] : after)
                if (f1 == f2) found = d1.size() <= d2.size() && std::equal(d1.begin(), d1.end(), d2.begin());
            if (!found) return false;
        }
        return true;
    }

    // The link lent the way paste_items does it (without pressing Ctrl+V anywhere): read only
    // after 1.5 s (with a screenshot copied, Discord got to a paste after the old fixed 1 s, and
    // found the screenshot back already), then never read at all; and a screenshot-like virtual
    // file without its contents. Puts back what you had copied, unless you copy something meanwhile.
    void lend_checks() {
        auto mine = clipboard_snapshot(app.hwnd);
        if (!mine) {
            wprintf(L"  clipboard busy\n");
            return;
        }
        const std::wstring url = L"https://cdn.7tv.app/emote/test/4x.webp";
        auto lend = [&] {
            ClipItems link = clipboard_text(url);
            auto seq = clipboard_lend(app.hwnd, link);
            app.clip_saved = mine;
            app.lent = link;
            app.clip_seq = seq.value_or(0);
            app.borrowing = seq.has_value();
            app.wait_for_paste();
            return seq.has_value();
        };
        DWORD last = 0;
        if (lend()) {
            double t0 = now_ms();
            pump_for(1500);
            bool waiting = app.borrowing;
            std::wstring read = clipboard_get_text(app.hwnd);  // the late reader
            double read_at = now_ms() - t0;
            while (app.borrowing && now_ms() - t0 < 10000) app.pump(10);
            double back = now_ms() - t0 - read_at;
            auto after = clipboard_snapshot(app.hwnd);
            last = GetClipboardSequenceNumber();
            wprintf(L"  link read after %.0f ms: still there: %ls; your copy back %.0f ms after that, identical: %ls\n",
                    read_at, waiting && read == url ? L"yes" : L"NO", back, after && same_clip(*mine, *after) ? L"yes" : L"NO");
        }
        if (lend()) {
            double t0 = now_ms();
            while (app.borrowing && now_ms() - t0 < 10000) app.pump(10);
            auto after = clipboard_snapshot(app.hwnd);
            last = GetClipboardSequenceNumber();
            wprintf(L"  link never read: your copy back after %.0f ms (RESTORE_CLIPBOARD_MAX_MS %d), identical: %ls\n",
                    now_ms() - t0, RESTORE_CLIPBOARD_MAX_MS, after && same_clip(*mine, *after) ? L"yes" : L"NO");
        }
        // One FILEDESCRIPTORW (592 bytes after the count) and some text; no FileContents.
        UINT fgd = RegisterClipboardFormatW(L"FileGroupDescriptorW");
        ClipItems fake = clipboard_text(L"screenshot stand-in");
        fake.push_back({fgd, Bytes(4 + 592, 0)});
        fake.back().second[0] = 1;
        auto fake_seq = clipboard_set(app.hwnd, fake, true);
        auto snap = clipboard_snapshot(app.hwnd);
        bool dropped = snap && std::none_of(snap->begin(), snap->end(), [&](const auto& i) { return i.first == fgd; });
        bool text_kept = snap && std::any_of(snap->begin(), snap->end(), [](const auto& i) { return i.first == CF_UNICODETEXT; });
        wprintf(L"  virtual file without its contents: left out of the copy: %ls, the rest kept: %ls\n",
                dropped ? L"yes" : L"NO", text_kept ? L"yes" : L"NO");
        (void)last;
        bool untouched = fake_seq && GetClipboardSequenceNumber() == *fake_seq;  // nothing copied since
        if (untouched) clipboard_set(app.hwnd, *mine, true);
        wprintf(L"  what you had copied put back: %ls\n", untouched ? L"yes" : L"no (something new was copied)");
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
    for (int i = 2; i + 1 < argc; i++)
        if (std::wstring(argv[i]) == L"--webp") {  // WebP decoding alone, without the picker's own downloads
            std::vector<Emote> popular;
            std::wstring err;
            if (!search_emotes(L"", popular, err)) {
                wprintf(L"search failed: %ls\n", err.c_str());
                return 1;
            }
            Tester::dump_webp(argv[1], popular, _wtoi(argv[i + 1]));
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
    for (int i = 2; i < argc; i++)
        if (std::wstring(argv[i]) == L"--search-speed") {  // [rounds] [--pause seconds]
            int rounds = i + 1 < argc && iswdigit(argv[i + 1][0]) ? _wtoi(argv[i + 1]) : 2;
            double pause = 60;
            for (int j = 2; j + 1 < argc; j++)
                if (std::wstring(argv[j]) == L"--pause") pause = _wtof(argv[j + 1]);
            auto* timed = new App;
            if (!timed->init(true, 1010, 996, 1.75)) return 1;  // 577x569 at 175%
            Tester{*timed, argv[1]}.search_speed(rounds, pause);
            fflush(stdout);
            ExitProcess(0);
        }
    auto* app = new App;
    if (!app->init(true, 1451, 341, 1.75)) {
        wprintf(L"init failed\n");
        return 1;
    }
    Tester t{*app, argv[1]};
    t.own_uploads();
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
    for (int i = 2; i < argc; i++)
        if (std::wstring(argv[i]) == L"--clipboard-only") {  // the clipboard borrow checks alone
            t.clipboard();
            fflush(stdout);
            ExitProcess(0);
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
    for (int i = 2; i + 1 < argc; i++)
        if (std::wstring(argv[i]) == L"--auto-size") {  // file mode's "auto" size alone
            t.wait_search();
            t.resize_check(_wtoi(argv[i + 1]));
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
    t.button_shots();
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
