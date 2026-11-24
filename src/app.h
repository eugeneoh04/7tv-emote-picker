// The picker: a borderless panel docked to Discord's bottom-right corner.
#pragma once

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_3.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "clipboard.h"
#include "common.h"
#include "config.h"
#include "images.h"
#include "net.h"
#include "textbox.h"

inline constexpr wchar_t WINDOW_CLASS[] = L"SevenTVEmotePicker";
inline constexpr wchar_t WINDOW_TITLE[] = L"7TV";  // a newly started picker finds the running one by it
inline constexpr UINT WM_APP_RESULTS = WM_APP + 1;  // the download/search threads have news
namespace timer {
enum : UINT_PTR { poll = 1, search, flash, paste, restore, blink, prefetch, deliver };
}

struct Settings {
    std::wstring paste = DEFAULT_PASTE, size = DEFAULT_SIZE, format = DEFAULT_FORMAT;
    int width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT;  // at 100% display scaling
    double scale = 1.0;  // emote size slider: 1 = EMOTE_SIZE's nearest 7TV size, as is
};
Settings load_settings();
void save_settings(const Settings& s);
std::wstring emote_link(const Emote& e, const Settings& s);
std::wstring emote_format(const Emote& e, const Settings& s);  // the link's file type
std::wstring link_size(const Settings& s);  // the link's 7TV size, 1x-4x ("auto" picks one)
// How tall file mode makes emote e's file (the "auto" size), or 0: as 7TV made it.
int file_height(const Emote& e, const Settings& s);

struct Box {
    float l = 0, t = 0, r = 0, b = 0;
    bool contains(float x, float y) const { return x >= l && x < r && y >= t && y < b; }
    float w() const { return r - l; }
    float h() const { return b - t; }
    D2D1_RECT_F d2d() const { return {l, t, r, b}; }
};

// Set when a download is no longer wanted: it stops part-way.
using CancelFlag = std::shared_ptr<std::atomic<bool>>;

// Work for the download threads, and what comes back.
struct Job {
    enum Kind { Still, Moving } kind = Still;  // Moving: an animated emote's animation
    std::string key, id;  // key: emote id and preview size
    int s = 1;
    bool animated = false;
    SharedBytes bytes;    // set: already downloaded, just decode
    int height = 0, room = 0, max_texture = 4096;  // shown height asked for, room it must fit
    CancelFlag cancel;
};
struct JobResult {
    Job job;
    SharedBytes bytes;    // what was downloaded; null if that failed
    bool ok = false;      // decoded
    Image still;
    Animation anim;
};
struct SearchResult {
    int gen = 0;
    bool ok = false;
    std::vector<Emote> emotes;
    std::wstring error;
};

class App {
public:
    // headless: no window on screen; draws into an offscreen picture of width x height at
    // display scale (for tests).
    // follow: track Discord (off only in tests, which dock to a stand-in window themselves).
    bool init(bool headless = false, int width = 0, int height = 0, double scale = 1.0,
              bool follow = true);
    int run();                 // until the picker is closed
    bool pump(DWORD timeout);  // handles what's waiting (or waits up to timeout); false on quit

private:
    friend struct Tester;
    enum class Hot { None, Grip, Field, Gear, Close, Segment, Slider, Grid, Scrollbar };
    enum class Drag { None, Resize, Thumb, Select, Slider };
    struct Still {
        ComPtr<ID2D1Bitmap1> bitmap;
        int w = 0, h = 0, height = 0, room = 0;  // size, and the height and room it was made for
        bool by_room = false;
    };
    struct Anim {                 // an animated emote's animation; its frames while near the screen
        ComPtr<ID2D1Bitmap1> atlas;
        int w = 0, h = 0, count = 0, cols = 1, height = 0, room = 0;
        bool by_room = false;
        std::vector<int> ends;
        double start = -1;        // when it started playing (ms)
    };
    enum class Option { Paste, Size, Format };  // the settings' groups of buttons
    struct Segment {
        Option group;
        std::wstring option;
        Box box;
    };
    struct FileDone {             // a file download finished (error empty: it's saved)
        std::wstring path, error;
        bool ahead = false;       // one of the files in view, fetched before any click
    };
    struct Pending {              // a clicked emote whose file is still downloading
        std::wstring path, name;
        double since = 0;
        HWND target = nullptr;    // the window it was clicked for (Discord)
    };

    // ---- window, docking, pasting (app_window.cpp)
    // Diagnostics: started with the PICKER_LOG environment variable set, focus and docking
    // decisions are written to picker.log next to the program.
    void note(const wchar_t* fmt, ...);
    std::wstring describe(HWND h) const;
    static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);
    static void CALLBACK win_event(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void track_foreground();
    void dock();
    void watch_discord(DWORD pid);
    void paste(int i);
    void paste_items(const ClipItems& items, HWND window);
    void bring_forward(HWND window);
    void send_ctrl_v();
    void restore_clipboard();
    std::wstring file_for(const Emote& e) const;
    void warm_connections(bool search = true);
    void fetch_file(const Emote& e, bool ahead = false);
    void fetch_files_in_view(int from, int to);
    void file_done(const FileDone& d);
    void deliver_pending();
    void say(const std::wstring& text);
    void flash(const std::wstring& text, UINT ms = 1500);
    void query_changed();
    void search_now();
    void show_results(std::vector<Emote> found);
    void set_option(const Segment& seg);
    std::wstring& option_value(Option group);
    std::wstring format_note() const;
    std::wstring auto_note() const;
    void toggle_settings();

    // ---- layout and input (app_input.cpp)
    double px(double v) const;
    void layout();
    Hot hit(float x, float y, int* segment = nullptr) const;
    int cell_at(float x, float y) const;
    void mouse_move(float x, float y);
    void mouse_down(float x, float y, bool shift, bool double_click);
    void mouse_up();
    void mouse_leave();
    void wheel(int delta);
    void key_down(UINT vk);
    void char_typed(wchar_t c);
    void hover_at(float x, float y);
    void scroll_to(double pos, bool smooth);
    bool step_scroll(double now);
    double scroll_max() const;
    Box thumb_box() const;
    void view_changed();
    void text_edited(bool changed);
    size_t text_index_at(float x) const;
    float caret_x(size_t pos) const;
    void keep_caret_visible();
    void place_ime();
    LPCWSTR cursor_at(float x, float y) const;

    // ---- drawing (app_render.cpp)
    bool create_device();
    void discard_device();
    bool resize_target();
    bool resize_buffers(int w, int h);
    void trim_buffers();
    void make_fonts();
    void render();
    void request_frame();
    void draw_header();
    void draw_settings();
    void draw_grid(double now, double& next_due);
    void draw_status();
    void fill_round(const Box& b, float radius, uint32_t color);
    void text(const std::wstring& s, IDWriteTextFormat* format, const Box& b, uint32_t color);
    float text_width(const std::wstring& s, IDWriteTextFormat* format) const;
    IDWriteTextLayout* box_layout() const;
    void schedule_animation(double due_ms);
    bool save_png(const std::wstring& path);

    // ---- previews (app_previews.cpp)
    void start_workers();
    void worker();
    void drain_results();
    void got_still(JobResult& r);
    void got_moving(JobResult& r);
    void load_previews();
    void load_stills(int from, int to);
    void submit(Job job);
    void done_fetching(const std::string& what, const CancelFlag& cancel);
    void cancel_unshown(const std::unordered_set<std::string>& shown);
    void keep_moving_file(const std::string& key, SharedBytes bytes);
    std::pair<int, int> on_screen() const;
    std::string key_of(int i) const;
    Anim* anim_at(int i, bool* exact = nullptr);
    void forget_far_animations();
    bool fits(int w, int h, bool by_room, int made_height, int made_room) const;
    const Still* still_for(int i, bool& exact) const;
    void set_emote_scale(double scale);
    double scale_at(float x) const;

    // window and display
    HWND hwnd = nullptr;
    bool headless = false, following = false;  // following: tracking Discord's window
    int win_w = 1, win_h = 1;
    double k = 1.0;                // display scaling: 1.75 at 175%
    double refresh_ms = 1000.0 / 60;
    double last_frame = 0;
    bool frame_pending = false;
    HANDLE anim_timer = nullptr;

    // Direct3D / Direct2D / DirectWrite
    ComPtr<ID2D1Factory1> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID2D1Device> d2d_device;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> target;
    ComPtr<ID2D1SolidColorBrush> brush, white;
    int max_texture = 4096;
    ComPtr<IDXGISwapChain2> swap2;  // set when only part of the buffers can be shown
    int buf_w = 0, buf_h = 0;       // the swap chain's buffers (can be bigger than the window)
    int buffer_resizes = 0, resize_failures = 0;  // (tests check these)
    std::wstring text_family = L"Segoe UI", icon_family;
    ComPtr<IDWriteTextFormat> f_text, f_small, f_small_center, f_icon, f_note, f_status;
    float text_line = 19, small_line = 16;
    std::wstring glyph_search, glyph_gear, glyph_close;
    mutable ComPtr<IDWriteTextLayout> layout_cache;
    mutable std::wstring layout_text;
    std::unordered_map<std::wstring, ComPtr<IDWriteTextLayout>> text_layouts;  // see text()

    // layout (pixels)
    Box field, gear, close, grid, scrollbar, status;
    Box settings_row;             // the line "Paste as" is on
    Box note_box;                 // where format_note() goes; empty if it doesn't fit
    float text_x = 0, text_r = 0, mid = 0;
    std::vector<Segment> segments;
    int thumb_s = 1, cols = 1, cell = 44, x0 = 0, room = 38;
    int emote_h = 32;             // emotes show this tall (pixels), from the size slider
    Box scale_row, scale_track;   // the size slider's row in settings, and its track
    float scale_value_x = 0;      // where "125%" goes

    // interaction
    Hot hot = Hot::None;
    int hot_segment = -1;
    int hover_i = -1;              // emote under the mouse
    float mouse_x = -1, mouse_y = -1;
    bool mouse_inside = false;
    Drag drag = Drag::None;
    HWND fg_at_press = nullptr;  // what was in front when a drag started
    POINT drag_start{};
    int drag_w0 = 0, drag_h0 = 0;
    float drag_grab = 0;
    bool focused = false, caret_on = true;
    float text_scroll = 0;
    double last_click = 0;
    int clicks = 0;
    TextBox box;
    bool settings_open = false;

    // scrolling (pixels from the top of the grid)
    double scroll = 0, scroll_goal = 0, scroll_step_at = 0;
    int scroll_px = 0;
    bool scrolling = false;

    // results
    std::vector<Emote> emotes;
    std::unordered_map<std::string, int> index;  // emote id -> place in emotes
    int search_gen = 0;
    std::wstring search_text, status_text, empty_note, flash_text;
    bool flashing = false;

    // previews
    std::unordered_map<std::string, SharedBytes> raw;              // key -> downloaded still
    std::unordered_map<std::string, Still> stills;                 // key -> on the GPU
    std::unordered_map<std::string, std::unique_ptr<Anim>> anims;  // null: can't play
    struct Kept {
        SharedBytes bytes;
        uint64_t used = 0;  // use_tick when last wanted
    };
    // key -> downloaded animation, kept when the results change (see keep_moving_file)
    std::unordered_map<std::string, Kept> moving_files;
    uint64_t use_tick = 0;
    // key + "s" (still) / "g" (animation) in the works, and the flag that stops its download
    std::unordered_map<std::string, CancelFlag> fetching;
    std::unordered_set<std::string> started;   // emote ids whose preview shows or is coming
    std::vector<std::thread> workers;
    std::mutex job_mutex, result_mutex;
    std::condition_variable job_ready;
    std::deque<Job> jobs;
    std::vector<JobResult> results;
    std::vector<SearchResult> search_results;

    // Discord and the clipboard
    Settings settings;
    DWORD my_pid = 0;
    HWND target_hwnd = nullptr;    // the window the user was typing in
    HWND discord_hwnd = nullptr;   // Discord's window while it's focused
    HWND last_fg = nullptr;
    DWORD last_fg_pid = 0;
    bool last_fg_discord = false;
    std::optional<RECT> dock_pos;  // left, top, width, height; empty while hidden
    HWINEVENTHOOK location_hook = nullptr;
    DWORD watched_pid = 0;
    FILE* log_file = nullptr;      // see note()
    HWND noted_fg = (HWND)-1;
    std::optional<ClipItems> clip_saved;  // what you had copied, while a link borrows it
    DWORD clip_seq = 0;
    bool borrowing = false;        // a link is on the clipboard and the copy isn't back yet

    // File mode (see paste())
    std::wstring files_dir;        // downloaded files go here, with a trailing backslash
    std::unordered_set<std::wstring> downloading;  // files on their way (paths)
    std::unordered_set<std::wstring> files_here;   // files seen on disk (paths)
    int files_ahead = 0;                           // downloads of files in view, under way
    // those downloads: path -> emote id, and the flag that stops one no longer in the results
    std::unordered_map<std::wstring, std::pair<std::string, CancelFlag>> ahead_files;
    std::vector<FileDone> files_done;              // from the download threads (result_mutex)
    std::optional<Pending> pending;
    bool dry_run = false;          // tests: pasting only records what would go on the clipboard
    ClipItems dry_pasted;
};
