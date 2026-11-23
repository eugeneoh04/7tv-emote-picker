// Layout, the mouse, the keyboard and the search box.

#include "app.h"
#include <algorithm>
#include <cmath>

#include <imm.h>

// v pixels at 100% display scaling, in pixels at Discord's scaling.
double App::px(double v) const { return std::max(1.0, (double)std::lround(v * k)); }

void App::layout() {
    float pad = (float)px(PAD), fh = (float)px(FIELD_H);
    mid = pad + fh / 2;
    // Top row: resize grip in the top-left corner, search box, settings and close buttons.
    float close_x = win_w - pad - fh / 2, gear_x = close_x - fh - (float)px(4);
    close = {close_x - fh / 2, pad, close_x + fh / 2, pad + fh};
    gear = {gear_x - fh / 2, pad, gear_x + fh / 2, pad + fh};
    field = {(float)px(24), pad, std::max((float)px(64), gear_x - fh / 2 - (float)px(6)), pad + fh};
    text_x = field.l + (float)px(32);
    text_r = std::max(text_x + 1, field.r - (float)px(10));
    float y = (float)px(PAD + FIELD_H);

    // The settings under it, when open. First what clicking an emote pastes: its link or the
    // file, which size and which format. The buttons line up after the labels; a group of them
    // that doesn't fit goes on the next line (on a narrow panel, from the left edge). Then the
    // emote size slider.
    segments.clear();
    note_box = {};
    if (settings_open) {
        float row_h = small_line + 2 * (float)px(3), gap = (float)px(8), line_gap = (float)px(4);
        float left = (float)px(12), right = win_w - (float)px(12), pad_x = (float)px(9);
        auto label_w = [&](const wchar_t* s) { return text_width(s, f_small.Get()); };
        auto option_w = [&](const std::wstring& o) { return text_width(o, f_small.Get()) + 2 * pad_x; };
        std::pair<Option, std::vector<std::wstring>> groups[] = {
            {Option::Paste, {std::begin(PASTE_MODES), std::end(PASTE_MODES)}},
            {Option::Size, {std::begin(LINK_SIZES), std::end(LINK_SIZES)}},
            {Option::Format, {std::begin(LINK_FORMATS), std::end(LINK_FORMATS)}}};
        float widest = 0;
        for (auto& [group, options] : groups) {
            float w = 0;
            for (auto& o : options) w += option_w(o);
            widest = std::max(widest, w);
        }
        float labels = std::max(label_w(L"Paste as"), label_w(L"Emote size")) + (float)px(12);
        bool aligned = left + labels + widest <= right;
        float indent = aligned ? left + labels : left;
        y += (float)px(6);
        settings_row = {left, y, right, y + row_h};
        float x = aligned ? indent : left + label_w(L"Paste as") + gap;
        bool first = true;
        for (auto& [group, options] : groups) {
            float w = 0;
            for (auto& o : options) w += option_w(o);
            if (!first && x + gap + w > right) {
                y += row_h + line_gap;
                x = indent;
            } else if (!first) {
                x += gap;
            }
            first = false;
            for (auto& o : options) {
                segments.push_back({group, o, {x, y, x + option_w(o), y + row_h}});
                x += option_w(o);
            }
        }
        if (std::wstring note = format_note(); !note.empty() && x + gap + text_width(note, f_small.Get()) <= right)
            note_box = {x + gap, y, right, y + row_h};
        y += row_h;

        // Then the emote size slider, and its value ("125%").
        y += (float)px(6);
        scale_row = {left, y, right, y + row_h};
        float track_l = aligned ? indent : left + label_w(L"Emote size") + (float)px(14);
        float value_w = text_width(L"300%", f_small.Get());
        float track_r = std::max(track_l + (float)px(40),
                                 std::min(track_l + (float)px(190), scale_row.r - value_w - (float)px(14)));
        float cy = (scale_row.t + scale_row.b) / 2, half = (float)px(2);
        scale_track = {track_l, cy - half, track_r, cy + half};
        scale_value_x = track_r + (float)px(14);
        y += row_h;
    }

    float status_h = (float)(px(4) + px(7)) + small_line;
    status = {0, win_h - status_h, (float)win_w, (float)win_h};
    float bar_w = (float)px(8);
    grid = {(float)px(8), y + (float)px(6), win_w - (float)px(3) - bar_w, 0};
    grid.b = std::max(grid.t, status.t);
    scrollbar = {grid.r, grid.t, grid.r + bar_w, grid.b};

    // Emotes keep one size, whatever the panel's width: a wider panel fits more of them in each
    // row. At 100% on the size slider that's the 7TV size (1x-4x = 32-128px tall) closest to
    // EMOTE_SIZE, shown as is; other sizes come from the sharpest 7TV size at least that tall.
    double want = EMOTE_SIZE * k;
    int base = 1;
    for (int c = 2; c <= 4; c++)
        if (std::abs(c * 32 - want) <= std::abs(base * 32 - want)) base = c;
    int new_h = std::max(8, (int)std::lround(base * 32 * settings.scale));
    int s = std::clamp((new_h + 31) / 32, 1, 4);
    int width = std::max(1, (int)grid.w());
    int new_cols = std::max(1, width / (new_h + 2 * (int)px(CELL_PAD)));
    int new_cell = std::max(1, width / new_cols);
    int new_room = std::max(1, new_cell - 2 * (int)px(3));
    if (s != thumb_s || new_room != room || new_h != emote_h) started.clear();  // redo the previews in view
    thumb_s = s;
    emote_h = new_h;
    cols = new_cols;
    cell = new_cell;
    x0 = (width - cols * cell) / 2;
    room = new_room;

    scroll_goal = std::clamp(scroll_goal, 0.0, scroll_max());
    scroll = std::clamp(scroll, 0.0, scroll_max());
    scroll_px = (int)std::lround(scroll);
    keep_caret_visible();
    view_changed();
    request_frame();
}

double App::scroll_max() const {
    int rows = ((int)emotes.size() + cols - 1) / cols;
    return std::max(0.0, (double)rows * cell - grid.h());
}

Box App::thumb_box() const {
    double content = scroll_max() + grid.h();
    float len = (float)std::max(px(20), grid.h() * grid.h() / std::max(1.0, content));
    len = std::min(len, grid.h());
    float top = grid.t + (float)((grid.h() - len) * (scroll_max() > 0 ? scroll / scroll_max() : 0));
    float inset = (float)px(1);
    return {scrollbar.l + inset, top, scrollbar.r - inset, top + len};
}

App::Hot App::hit(float x, float y, int* segment) const {
    if (segment) *segment = -1;
    for (int i = 0; i < (int)segments.size(); i++)
        if (segments[i].box.contains(x, y)) {
            if (segment) *segment = i;
            return Hot::Segment;
        }
    float reach = (float)px(9);  // the slider's thumb sticks out past its track
    if (settings_open && x >= scale_track.l - reach && x < scale_track.r + reach && y >= scale_row.t &&
        y < scale_row.b)
        return Hot::Slider;
    if (x < px(22) && y < px(22)) return Hot::Grip;
    if (gear.contains(x, y)) return Hot::Gear;
    if (close.contains(x, y)) return Hot::Close;
    if (field.contains(x, y)) return Hot::Field;
    if (scroll_max() > 0 && scrollbar.contains(x, y)) return Hot::Scrollbar;
    if (grid.contains(x, y)) return Hot::Grid;
    return Hot::None;
}

// Index of the emote at x, y, or -1.
int App::cell_at(float x, float y) const {
    if (!grid.contains(x, y)) return -1;
    float gx = x - grid.l - x0, gy = y - grid.t + scroll_px;
    if (gx < 0) return -1;
    int col = (int)(gx / cell), row = (int)(gy / cell);
    int i = row * cols + col;
    return col < cols && i < (int)emotes.size() ? i : -1;
}

LPCWSTR App::cursor_at(float x, float y) const {
    if (drag == Drag::Resize) return IDC_SIZENWSE;
    if (drag == Drag::Select) return IDC_IBEAM;
    if (drag == Drag::Slider) return IDC_HAND;
    switch (hit(x, y)) {
        case Hot::Grip: return IDC_SIZENWSE;
        case Hot::Gear:
        case Hot::Close:
        case Hot::Slider:
        case Hot::Segment: return IDC_HAND;
        case Hot::Field: return IDC_IBEAM;
        case Hot::Grid: return cell_at(x, y) >= 0 ? IDC_HAND : IDC_ARROW;
        default: return IDC_ARROW;
    }
}

void App::mouse_move(float x, float y) {
    mouse_x = x;
    mouse_y = y;
    if (!mouse_inside && hwnd && !headless) {
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&t);
    }
    mouse_inside = true;
    if (drag == Drag::Resize) {  // the panel hangs from Discord's corner: it grows up and left
        POINT p;
        GetCursorPos(&p);
        settings.width = std::max(MIN_WIDTH, (int)std::lround((drag_w0 - (p.x - drag_start.x)) / k));
        settings.height = std::max(MIN_HEIGHT, (int)std::lround((drag_h0 - (p.y - drag_start.y)) / k));
        dock();
        return;
    }
    if (drag == Drag::Thumb) {
        Box t = thumb_box();
        double track = grid.h() - t.h();
        if (track > 0) scroll_to((y - drag_grab - grid.t) / track * scroll_max(), false);
        return;
    }
    if (drag == Drag::Slider) {
        set_emote_scale(scale_at(x));
        return;
    }
    if (drag == Drag::Select) {
        box.place(text_index_at(x), true);
        text_edited(false);
        return;
    }
    int seg;
    Hot h = hit(x, y, &seg);
    if (h != hot || seg != hot_segment) {
        hot = h;
        hot_segment = seg;
        request_frame();
    }
    hover_at(x, y);
}

void App::mouse_down(float x, float y, bool shift, bool double_click) {
    int seg;
    switch (hit(x, y, &seg)) {
        case Hot::Segment:
            set_option(segments[seg]);
            break;
        case Hot::Slider:
            if (double_click) {  // back to 100%
                set_emote_scale(1.0);
                save_settings(settings);
                break;
            }
            drag = Drag::Slider;
            fg_at_press = GetForegroundWindow();
            SetCapture(hwnd);
            set_emote_scale(scale_at(x));
            request_frame();
            break;
        case Hot::Grip:
            drag = Drag::Resize;
            fg_at_press = GetForegroundWindow();
            note(L"grip pressed; in front: %ls", describe(fg_at_press).c_str());
            GetCursorPos(&drag_start);
            drag_w0 = win_w;
            drag_h0 = win_h;
            SetCapture(hwnd);
            break;
        case Hot::Gear:
            toggle_settings();
            break;
        case Hot::Close:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        case Hot::Field: {
            // The panel never comes to the front by itself (WS_EX_NOACTIVATE), so typing in the
            // search box means bringing it there. Allowed: you just clicked it.
            if (!headless && GetForegroundWindow() != hwnd) {
                BOOL ok = SetForegroundWindow(hwnd);
                note(L"search box clicked: picker to the front %ls", ok ? L"ok" : L"refused");
            }
            double now = now_ms();
            clicks = double_click ? 2 : clicks == 2 && now - last_click <= GetDoubleClickTime() ? 3 : 1;
            last_click = now;
            size_t pos = text_index_at(x);
            if (clicks == 3) {
                box.select_all();
            } else if (clicks == 2) {
                box.select_word(pos);
            } else {
                box.place(pos, shift);
                drag = Drag::Select;
                SetCapture(hwnd);
            }
            text_edited(false);
            break;
        }
        case Hot::Scrollbar: {
            Box t = thumb_box();
            if (t.contains(x, y) || (y >= t.t && y < t.b)) {
                drag = Drag::Thumb;
                fg_at_press = GetForegroundWindow();
                drag_grab = y - t.t;
                SetCapture(hwnd);
                request_frame();
            } else {
                scroll_to(scroll_goal + (y < t.t ? -1 : 1) * grid.h(), true);  // a page
            }
            break;
        }
        case Hot::Grid:
            if (int i = cell_at(x, y); i >= 0) paste(i);
            break;
        default:
            break;
    }
}

void App::mouse_up() {
    Drag was = drag;
    drag = Drag::None;
    if (GetCapture() == hwnd) ReleaseCapture();
    if (was == Drag::Slider) {
        save_settings(settings);
        load_previews();  // now make them at the size picked
    }
    if (was == Drag::Resize || was == Drag::Thumb || was == Drag::Slider) {
        // A drag can leave nothing in front: Windows takes Discord out of the foreground and
        // keys go nowhere (and the panel, which follows Discord, would hide). Put back
        // whatever was in front when the drag started, unless something else took over.
        HWND fg = GetForegroundWindow();
        note(L"released; in front: %ls", describe(fg).c_str());
        if (following && fg != fg_at_press && fg_at_press && IsWindow(fg_at_press) &&
            (!fg || window_pid(fg) == my_pid)) {
            BOOL ok = SetForegroundWindow(fg_at_press);
            note(L"gave the foreground back to %ls: %ls", describe(fg_at_press).c_str(), ok ? L"ok" : L"refused");
        }
    }
    if (was == Drag::Resize) {
        save_settings(settings);
        trim_buffers();
        if (following) track_foreground();  // catch up on anything that changed meanwhile
    }
    request_frame();
}

void App::mouse_leave() {
    mouse_inside = false;
    if (drag != Drag::None) return;
    hot = Hot::None;
    hot_segment = -1;
    hover_at(-1, -1);
    request_frame();
}

void App::wheel(int delta) {
    if (mouse_inside && hit(mouse_x, mouse_y) == Hot::Slider) {  // over the size slider: 10% a notch
        set_emote_scale(settings.scale + (delta > 0 ? 2 : -2) * EMOTE_SCALE_STEP);
        save_settings(settings);
        return;
    }
    if (scroll_max() > 0) scroll_to(scroll_goal - delta / 120.0 * cell, true);  // a row per notch
}

// The slider's value at x: from the track's left (smallest) to right (biggest), in steps.
double App::scale_at(float x) const {
    double t = std::clamp((x - scale_track.l) / std::max(1.0f, scale_track.w()), 0.0f, 1.0f);
    return MIN_EMOTE_SCALE + t * (MAX_EMOTE_SCALE - MIN_EMOTE_SCALE);
}

// Sets the emote size, keeping the emotes that were at the top of the grid there.
void App::set_emote_scale(double scale) {
    scale = std::clamp(std::round(scale / EMOTE_SCALE_STEP) * EMOTE_SCALE_STEP, MIN_EMOTE_SCALE, MAX_EMOTE_SCALE);
    if (std::abs(scale - settings.scale) < 1e-6) return;
    int first = scroll_px / std::max(1, cell) * cols;
    settings.scale = scale;
    layout();
    scroll_to((double)(first / cols) * cell, false);
    request_frame();
}

// Emote under the mouse: rounded highlight behind it, its name on the status line.
void App::hover_at(float x, float y) {
    int i = mouse_inside && x >= 0 ? cell_at(x, y) : -1;
    if (i == hover_i) return;
    hover_i = i;
    flashing = false;  // the name replaces a "Pasted!"
    // File mode: the emote the mouse rests on starts downloading, to be there when it's clicked.
    if (i >= 0 && settings.paste == L"file") SetTimer(hwnd, timer::prefetch, FILE_PREFETCH_MS, nullptr);
    else KillTimer(hwnd, timer::prefetch);
    request_frame();
}

void App::scroll_to(double pos, bool smooth) {
    scroll_goal = std::clamp(pos, 0.0, scroll_max());
    if (!smooth || SCROLL_EASE_MS <= 0) {
        scroll = scroll_goal;
        scrolling = false;
        if (int p = (int)std::lround(scroll); p != scroll_px) {
            scroll_px = p;
            view_changed();
        }
    } else if (!scrolling) {
        scrolling = true;
        scroll_step_at = now_ms();
    }
    request_frame();
}

// Moves the smooth scroll along; true while it's still moving.
bool App::step_scroll(double now) {
    if (!scrolling) return false;
    double dt = std::min(now - scroll_step_at, 100.0);
    scroll_step_at = now;
    scroll += (scroll_goal - scroll) * (1 - std::exp(-dt / SCROLL_EASE_MS));
    if (std::abs(scroll_goal - scroll) < 0.5) {
        scroll = scroll_goal;
        scrolling = false;
    }
    if (int p = (int)std::lround(scroll); p != scroll_px) {
        scroll_px = p;
        view_changed();
    }
    return scrolling;
}

// What's in view changed (scrolling, new results, resizing).
void App::view_changed() {
    if (mouse_inside && drag == Drag::None) hover_at(mouse_x, mouse_y);  // grid moved under it
    forget_far_animations();
    load_previews();
}

// ---- the search box -----------------------------------------------------------------------

void App::key_down(UINT vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    auto copy = [&] {
        if (box.has_selection()) clipboard_set(hwnd, clipboard_text(box.selected()), false);
    };
    bool changed = false;
    switch (vk) {
        case VK_LEFT: box.move(-1, ctrl, shift); break;
        case VK_RIGHT: box.move(1, ctrl, shift); break;
        case VK_HOME: box.home(shift); break;
        case VK_END: box.end(shift); break;
        case VK_BACK: changed = box.erase_back(ctrl); break;
        case VK_DELETE:
            if (shift && !ctrl) copy();
            changed = box.erase_forward(ctrl);
            break;
        case VK_INSERT:
            if (ctrl) copy();
            else if (shift) changed = box.insert(clipboard_get_text(hwnd));
            break;
        case VK_RETURN: search_now(); return;
        case 'A': if (!ctrl) return; box.select_all(); break;
        case 'C': if (!ctrl) return; copy(); break;
        case 'X':
            if (!ctrl) return;
            copy();
            changed = box.has_selection() && box.erase_back(false);
            break;
        case 'V': if (!ctrl) return; changed = box.insert(clipboard_get_text(hwnd)); break;
        case 'Z': if (!ctrl) return; changed = shift ? box.redo() : box.undo(); break;
        case 'Y': if (!ctrl) return; changed = box.redo(); break;
        default: return;
    }
    text_edited(changed);
}

void App::char_typed(wchar_t c) {
    if (c >= 0x20 && c != 0x7F) text_edited(box.insert(std::wstring(1, c)));
}

void App::text_edited(bool changed) {
    caret_on = true;  // the caret shows (and starts blinking again) whenever you do something
    if (focused && GetCaretBlinkTime() != INFINITE) SetTimer(hwnd, timer::blink, GetCaretBlinkTime(), nullptr);
    keep_caret_visible();
    if (changed) query_changed();
    place_ime();
    request_frame();
}

size_t App::text_index_at(float x) const {
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    box_layout()->HitTestPoint(x - text_x + text_scroll, 1, &trailing, &inside, &m);
    return m.textPosition + (trailing ? m.length : 0);
}

float App::caret_x(size_t pos) const {
    float x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS m{};
    box_layout()->HitTestTextPosition((UINT32)pos, FALSE, &x, &y, &m);
    return x;
}

// Scrolls the text sideways so the caret stays in the box.
void App::keep_caret_visible() {
    if (!f_text) return;
    float width = text_r - text_x, caret_w = (float)px(1.2);
    DWRITE_TEXT_METRICS tm{};
    box_layout()->GetMetrics(&tm);
    float full = tm.widthIncludingTrailingWhitespace + caret_w;
    if (full <= width) {
        text_scroll = 0;
        return;
    }
    float cx = caret_x(box.caret());
    if (cx - text_scroll > width - caret_w) text_scroll = cx - width + caret_w;
    if (cx - text_scroll < 0) text_scroll = cx;
    text_scroll = std::clamp(text_scroll, 0.0f, full - width);
}

// Puts the input method's (e.g. Korean or Japanese) typing window at the caret.
void App::place_ime() {
    if (headless || !focused) return;
    if (HIMC imc = ImmGetContext(hwnd)) {
        LONG x = std::lround(text_x + caret_x(box.caret()) - text_scroll);
        COMPOSITIONFORM cf{CFS_POINT, {x, std::lround(mid - text_line / 2)}, {}};
        ImmSetCompositionWindow(imc, &cf);
        CANDIDATEFORM cand{0, CFS_CANDIDATEPOS, {x, std::lround(mid + text_line / 2)}, {}};
        ImmSetCandidateWindow(imc, &cand);
        ImmReleaseContext(hwnd, imc);
    }
}
