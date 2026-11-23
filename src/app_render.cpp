// Drawing: Direct2D on the graphics card, shown through a flip-model swap chain, so frames
// land on the screen's refresh without tearing and cost almost no CPU.

#include "app.h"
#include <algorithm>
#include <cmath>
#include <tuple>

#include <dwmapi.h>
#include <wincodec.h>

namespace {

D2D1_COLOR_F color(uint32_t rgb) {
    return {(rgb >> 16 & 255) / 255.f, (rgb >> 8 & 255) / 255.f, (rgb & 255) / 255.f, 1.f};
}

}  // namespace

bool App::create_device() {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                  D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   (UINT)std::size(levels), D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr))  // no usable graphics card: draw with the CPU instead
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                               (UINT)std::size(levels), D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    ComPtr<IDXGIDevice1> dxgi;
    if (FAILED(hr) || FAILED(d3d.As(&dxgi)) ||
        FAILED(d2d->CreateDevice(dxgi.Get(), &d2d_device)) ||
        FAILED(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)))
        return false;
    dxgi->SetMaximumFrameLatency(1);  // a frame shows at the next refresh, no queue building up
    dc->SetDpi(96, 96);               // one unit is one pixel; scaling is done by px()
    max_texture = (int)dc->GetMaximumBitmapSize();
    dc->CreateSolidColorBrush(color(TEXT), &brush);
    dc->CreateSolidColorBrush(color(SELECTED_TEXT), &white);
    if (!headless) {
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
            return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = win_w;
        desc.Height = win_h;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        // Stretch: the part of the buffers shown (SetSourceSize, see resize_target()) is mapped
        // onto the window, which is the same size, so it shows 1:1. (With DXGI_SCALING_NONE,
        // Windows stretched that part in odd ways and cut off the right side of the panel.)
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = factory->CreateSwapChainForHwnd(d3d.Get(), hwnd, &desc, nullptr, nullptr, &swap);
        if (FAILED(hr)) {  // older Windows
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            hr = factory->CreateSwapChainForHwnd(d3d.Get(), hwnd, &desc, nullptr, nullptr, &swap);
        }
        if (FAILED(hr)) return false;
        swap.As(&swap2);  // can show part of its buffers (Windows 8.1 and later)
        buf_w = win_w;
        buf_h = win_h;
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        DWM_TIMING_INFO timing{sizeof(timing)};
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.qpcRefreshPeriod)
            refresh_ms = timing.qpcRefreshPeriod * 1000.0 / freq.QuadPart;
    }
    return resize_target();
}

// The graphics card was reset or removed (driver update, sleep): start over with a new one.
// Previews come back from the downloads kept in memory.
void App::discard_device() {
    stills.clear();
    for (auto& [key, anim] : anims)
        if (anim) {
            anim->atlas.Reset();
            anim->start = -1;
        }
    started.clear();
    if (dc) dc->SetTarget(nullptr);
    target.Reset();
    swap2.Reset();
    swap.Reset();
    brush.Reset();
    white.Reset();
    dc.Reset();
    d2d_device.Reset();
    d3d.Reset();
    if (create_device()) layout();
}

// Fits the drawing target to the window (win_w x win_h). Call render() right after.
bool App::resize_target() {
    if (!dc) return false;
    if (headless) {
        dc->SetTarget(nullptr);
        target.Reset();
        auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        dc->CreateBitmap(D2D1::SizeU(win_w, win_h), nullptr, 0, props, &target);
        if (target) dc->SetTarget(target.Get());
        return target != nullptr;
    }
    // Reallocating the swap chain's buffers throws away the frame on screen, and the window
    // shows nothing until the next one lands. Dragging the corner resizes on every mouse move,
    // so that used to leave the panel blank for the whole drag. Instead the buffers have room
    // to spare and the swap chain shows just the window-sized part of them.
    if (target && swap2 && win_w <= buf_w && win_h <= buf_h)
        return SUCCEEDED(swap2->SetSourceSize(win_w, win_h));
    if (!swap2) return resize_buffers(win_w, win_h);  // older Windows: buffers match the window
    return resize_buffers(std::max(buf_w, win_w + win_w / 4), std::max(buf_h, win_h + win_h / 4));
}

// Reallocates the swap chain's buffers at w x h, of which the window-sized part is shown.
bool App::resize_buffers(int w, int h) {
    dc->SetTarget(nullptr);
    target.Reset();
    // Nothing may still point at the old buffers, including work Direct3D hasn't let go of.
    ComPtr<ID3D11DeviceContext> context;
    d3d->GetImmediateContext(&context);
    context->ClearState();
    context->Flush();
    // Just after the screen updates, so the new frame (drawn right after this) is in before
    // the next update: otherwise the window can show blank for that moment.
    DwmFlush();
    HRESULT hr = swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        discard_device();
        return target != nullptr;
    }
    ComPtr<IDXGISurface> surface;
    if (FAILED(hr)) resize_failures++;
    if (FAILED(hr) || FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    buf_w = w;
    buf_h = h;
    buffer_resizes++;
    auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
    dc->CreateBitmapFromDxgiSurface(surface.Get(), props, &target);
    if (!target) return false;
    if (swap2) swap2->SetSourceSize(win_w, win_h);
    dc->SetTarget(target.Get());
    return true;
}

// After a resize drag: hand back buffer space far beyond what the window needs now.
void App::trim_buffers() {
    double need = (win_w + win_w / 4.0) * (win_h + win_h / 4.0);
    if (headless || !swap2 || (double)buf_w * buf_h <= 2 * need) return;
    if (resize_buffers(win_w + win_w / 4, win_h + win_h / 4)) render();
}

// Fonts sized in pixels for the display scaling.
void App::make_fonts() {
    ComPtr<IDWriteFontCollection> fonts;
    dwrite->GetSystemFontCollection(&fonts);
    auto has = [&](const wchar_t* name) {
        UINT32 i;
        BOOL exists = FALSE;
        return fonts && SUCCEEDED(fonts->FindFamilyName(name, &i, &exists)) && exists;
    };
    text_family = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    icon_family = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons"
                  : has(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets"
                                              : L"";
    bool icons = !icon_family.empty();
    glyph_search = icons ? L"" : L"⌕";
    glyph_gear = icons ? L"" : L"⚙";
    glyph_close = icons ? L"" : L"✕";
    auto make = [&](const std::wstring& family, double size, DWRITE_TEXT_ALIGNMENT align) {
        ComPtr<IDWriteTextFormat> f;
        dwrite->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 (float)px(size), L"en-us", &f);
        f->SetTextAlignment(align);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return f;
    };
    f_text = make(text_family, 14, DWRITE_TEXT_ALIGNMENT_LEADING);
    f_note = make(text_family, 14, DWRITE_TEXT_ALIGNMENT_CENTER);
    f_small = make(text_family, 12, DWRITE_TEXT_ALIGNMENT_LEADING);
    f_small_center = make(text_family, 12, DWRITE_TEXT_ALIGNMENT_CENTER);
    f_status = make(text_family, 12, DWRITE_TEXT_ALIGNMENT_LEADING);
    f_icon = make(icons ? icon_family : text_family, 14, DWRITE_TEXT_ALIGNMENT_CENTER);
    ComPtr<IDWriteInlineObject> ellipsis;  // long emote names end in "…"
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(f_status.Get(), &ellipsis)))
        f_status->SetTrimming(&trim, ellipsis.Get());
    auto line = [&](IDWriteTextFormat* f) {
        ComPtr<IDWriteTextLayout> l;
        DWRITE_TEXT_METRICS m{};
        dwrite->CreateTextLayout(L"Ag", 2, f, 1000, 1000, &l);
        l->GetMetrics(&m);
        return std::ceil(m.height);
    };
    text_line = line(f_text.Get());
    small_line = line(f_small.Get());
    layout_cache.Reset();
    text_layouts.clear();
}

void App::request_frame() {
    frame_pending = true;
    if (!headless && hwnd) InvalidateRect(hwnd, nullptr, FALSE);  // drawn in WM_PAINT
}

// Wakes the message loop when the next animation frame is due (but never more often than
// the screen refreshes); INFINITY stops it.
void App::schedule_animation(double due) {
    if (!anim_timer) return;
    if (!std::isfinite(due)) {
        CancelWaitableTimer(anim_timer);
        return;
    }
    due = std::max(due, last_frame + refresh_ms * 0.9);
    LARGE_INTEGER rel;
    rel.QuadPart = -std::max(1LL, (long long)((due - now_ms()) * 10000));  // 100ns units
    SetWaitableTimer(anim_timer, &rel, 0, nullptr, nullptr, FALSE);
}

void App::render() {
    frame_pending = false;
    if (!dc || !target) return;
    double now = now_ms();
    bool moving = step_scroll(now);
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->Clear(color(BG));
    draw_header();
    draw_settings();
    double next_due = INFINITY;
    draw_grid(now, next_due);
    draw_status();
    HRESULT hr = dc->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discard_device();
        return;
    }
    if (swap) {
        hr = swap->Present(1, 0);  // waits for the refresh if a frame is already waiting
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            discard_device();
            return;
        }
    } else {  // offscreen (tests): nothing presents, so hand the work over like Present would
        ComPtr<ID3D11DeviceContext> context;
        d3d->GetImmediateContext(&context);
        context->Flush();
    }
    last_frame = now;
    if (moving) request_frame();  // smooth scrolling: draw again at the next refresh
    bool shown = headless || dock_pos.has_value();
    schedule_animation(shown && ANIMATE ? next_due : INFINITY);
}

void App::fill_round(const Box& b, float radius, uint32_t c) {
    brush->SetColor(color(c));
    dc->FillRoundedRectangle(D2D1::RoundedRect(b.d2d(), radius, radius), brush.Get());
}

void App::text(const std::wstring& s, IDWriteTextFormat* format, const Box& b, uint32_t c) {
    if (s.empty()) return;
    // Laying text out (shaping) is the costly part of drawing it, so layouts are kept and
    // reused frame after frame; animated emotes redraw the panel up to 100 times a second.
    std::wstring key = s;
    key += L'\x1f';
    key += std::to_wstring((uintptr_t)format) + L'|' + std::to_wstring(b.w()) + L'|' + std::to_wstring(b.h());
    auto it = text_layouts.find(key);
    if (it == text_layouts.end()) {
        if (text_layouts.size() > 200) text_layouts.clear();  // e.g. many hovered emote names
        ComPtr<IDWriteTextLayout> l;
        if (FAILED(dwrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), format, b.w(), b.h(), &l))) return;
        it = text_layouts.emplace(std::move(key), std::move(l)).first;
    }
    brush->SetColor(color(c));
    dc->DrawTextLayout({b.l, b.t}, it->second.Get(), brush.Get());
}

float App::text_width(const std::wstring& s, IDWriteTextFormat* format) const {
    ComPtr<IDWriteTextLayout> l;
    DWRITE_TEXT_METRICS m{};
    if (FAILED(dwrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), format, 100000, 1000, &l)))
        return 0;
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

// The search box's text, laid out (kept until the text changes).
IDWriteTextLayout* App::box_layout() const {
    if (!layout_cache || layout_text != box.text()) {
        layout_text = box.text();
        layout_cache.Reset();
        dwrite->CreateTextLayout(layout_text.c_str(), (UINT32)layout_text.size(), f_text.Get(),
                                 100000, text_line, &layout_cache);
    }
    return layout_cache.Get();
}

void App::draw_header() {
    float px1 = (float)px(1);
    // Resize grip: the panel hangs from Discord's bottom-right corner, so it grows from its
    // top-left one.
    brush->SetColor(color(hot == Hot::Grip || drag == Drag::Resize ? FAINT : GRIP));
    for (double d : {4.0, 8.0, 12.0})
        dc->DrawLine({(float)px(3), (float)px(3 + d)}, {(float)px(3 + d), (float)px(3)}, brush.Get(),
                     (float)px(1.3));

    // The search box.
    fill_round(field, (float)px(8), FIELD);
    float icon_x = field.l + (float)px(16);
    text(glyph_search, f_icon.Get(), {icon_x - 50, field.t, icon_x + 50, field.b}, FAINT);
    float top = mid - text_line / 2;
    dc->PushAxisAlignedClip({text_x - px1, field.t, text_r + px1, field.b}, D2D1_ANTIALIAS_MODE_ALIASED);
    if (box.text().empty()) {
        text(L"Search 7TV emotes", f_text.Get(),
             {text_x + (float)px(3), top, text_r + 1000, top + text_line}, FAINT);  // caret stays visible
    } else {
        IDWriteTextLayout* l = box_layout();
        UINT32 len = (UINT32)box.text().size();
        l->SetDrawingEffect(nullptr, {0, len});
        if (box.has_selection()) {
            DWRITE_HIT_TEST_METRICS ranges[16];
            UINT32 n = 0;
            UINT32 start = (UINT32)box.sel_start(), count = (UINT32)(box.sel_end() - box.sel_start());
            l->HitTestTextRange(start, count, text_x - text_scroll, top, ranges, 16, &n);
            brush->SetColor(color(TEXT_SELECTION));
            for (UINT32 i = 0; i < std::min(n, 16u); i++)
                dc->FillRectangle({ranges[i].left, top, ranges[i].left + ranges[i].width, top + text_line},
                                  brush.Get());
            l->SetDrawingEffect(white.Get(), {start, count});
        }
        brush->SetColor(color(TEXT));
        dc->DrawTextLayout({text_x - text_scroll, top}, l, brush.Get());
    }
    if (focused && caret_on) {
        float x = std::round(text_x + caret_x(box.caret()) - text_scroll);
        brush->SetColor(color(TEXT));
        dc->FillRectangle({x, top + px1, x + (float)px(1.2), top + text_line - px1}, brush.Get());
    }
    dc->PopAxisAlignedClip();

    // Settings and close buttons light up under the mouse; the gear stays lit while the
    // settings row is open.
    for (auto [b, glyph, which] : {std::tuple{gear, glyph_gear, Hot::Gear},
                                   std::tuple{close, glyph_close, Hot::Close}}) {
        uint32_t bg = hot == which ? HOVER : which == Hot::Gear && settings_open ? SELECTED : BG;
        if (bg != BG) fill_round(b, (float)px(6), bg);
        text(glyph, f_icon.Get(), b, bg == BG ? MUTED : TEXT);
    }
}

// The settings: "Paste as" [link file] [1x 2x 3x 4x] [webp avif gif png], then the emote
// size slider.
void App::draw_settings() {
    if (!settings_open || segments.empty()) return;
    Box row = settings_row;
    text(L"Paste as", f_small.Get(), {row.l, row.t, row.l + 1000, row.b}, MUTED);
    float radius = (float)px(4);
    for (Option group : {Option::Paste, Option::Size, Option::Format}) {
        Box b{1e9f, 1e9f, -1e9f, -1e9f};
        for (auto& s : segments)
            if (s.group == group)
                b = {std::min(b.l, s.box.l), std::min(b.t, s.box.t), std::max(b.r, s.box.r), std::max(b.b, s.box.b)};
        if (b.r > b.l) fill_round(b, radius, FIELD);
    }
    for (int i = 0; i < (int)segments.size(); i++) {
        auto& s = segments[i];
        bool on = option_value(s.group) == s.option;
        if (on) fill_round(s.box, radius, SELECTED);
        bool lit = on || (hot == Hot::Segment && hot_segment == i);
        text(s.option, f_small_center.Get(), s.box, lit ? TEXT : MUTED);
    }
    if (note_box.w() > 0) text(format_note(), f_small.Get(), note_box, FAINT);

    // Emote size slider: blurple up to the value, a mark at 100%, a white knob, the value.
    Box sr = scale_row, tr = scale_track;
    text(L"Emote size", f_small.Get(), {sr.l, sr.t, sr.l + 1000, sr.b}, MUTED);
    auto at = [&](double v) {
        return tr.l + (float)((v - MIN_EMOTE_SCALE) / (MAX_EMOTE_SCALE - MIN_EMOTE_SCALE)) * tr.w();
    };
    float knob_x = at(settings.scale), cy = (tr.t + tr.b) / 2, r = tr.h() / 2;
    fill_round(tr, r, GRIP);
    fill_round({tr.l, tr.t, std::max(tr.l + tr.h(), knob_x), tr.b}, r, TEXT_SELECTION);
    float mark = std::round(at(1.0)), mark_w = (float)px(1);
    brush->SetColor(color(settings.scale >= 1.0 ? SELECTED_TEXT : FAINT));
    dc->FillRectangle({mark - mark_w / 2, tr.t - (float)px(3), mark + mark_w / 2, tr.b + (float)px(3)}, brush.Get());
    bool lit = hot == Hot::Slider || drag == Drag::Slider;
    float knob = (float)px(lit ? 8 : 7);
    brush->SetColor(color(SELECTED_TEXT));
    dc->FillEllipse(D2D1::Ellipse({knob_x, cy}, knob, knob), brush.Get());
    wchar_t value[16];
    swprintf_s(value, L"%d%%", (int)std::lround(settings.scale * 100));
    text(value, f_small.Get(), {scale_value_x, sr.t, scale_value_x + 1000, sr.b}, lit ? TEXT : MUTED);
}

// The emotes in view. Animated ones show the frame due now; next_due gets when the next
// one changes.
void App::draw_grid(double now, double& next_due) {
    dc->PushAxisAlignedClip(grid.d2d(), D2D1_ANTIALIAS_MODE_ALIASED);
    auto [from, to] = on_screen();
    float inset = (float)px(2);
    // Where a w x h preview made at made_h tall goes in the cell at x, y: as is if it was made
    // for the current size, else scaled to it (while the size slider moves, until it's made again).
    auto place = [&](float x, float y, int w, int h, int made_h, bool exact) {
        float dw = (float)w, dh = (float)h;
        if (!exact) {
            float f = made_h > 0 ? (float)emote_h / made_h : 1.f;
            dw *= f, dh *= f;
            float fit = std::min(1.f, std::min(room / dw, room / dh));
            dw *= fit, dh *= fit;
        }
        float dx = std::floor(x + (cell - dw) / 2.f), dy = std::floor(y + (cell - dh) / 2.f);
        return D2D1_RECT_F{dx, dy, dx + dw, dy + dh};
    };
    for (int i = from; i < to; i++) {
        float x = grid.l + x0 + (float)(i % cols * cell), y = grid.t + (float)(i / cols * cell - scroll_px);
        if (i == hover_i) fill_round({x + inset, y + inset, x + cell - inset, y + cell - inset}, (float)px(8), HOVER);
        bool exact = false;
        if (Anim* a = anim_at(i, &exact)) {
            if (a->start < 0) a->start = std::floor(now / 10) * 10;  // GIF timing is in 10ms steps
            double t = std::fmod(now - a->start, (double)a->ends.back());
            int f = (int)(std::upper_bound(a->ends.begin(), a->ends.end(), t) - a->ends.begin());
            f = std::min(f, a->count - 1);
            next_due = std::min(next_due, now + (a->ends[f] - t));
            float sx = (float)(f % a->cols * a->w), sy = (float)(f / a->cols * a->h);
            dc->DrawBitmap(a->atlas.Get(), place(x, y, a->w, a->h, a->height, exact), 1.f,
                           exact ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_LINEAR,
                           {sx, sy, sx + a->w, sy + a->h});
        } else if (const Still* s = still_for(i, exact)) {
            dc->DrawBitmap(s->bitmap.Get(), place(x, y, s->w, s->h, s->height, exact), 1.f,
                           exact ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                 : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }
    }
    if (!empty_note.empty()) {
        float y = grid.t + (float)px(40);
        text(empty_note, f_note.Get(), {grid.l, y - 50, grid.r, y + 50}, FAINT);
    }
    dc->PopAxisAlignedClip();
    if (scroll_max() > 0) {  // slim scrollbar, only when not everything fits
        Box t = thumb_box();
        fill_round(t, t.w() / 2, hot == Hot::Scrollbar || drag == Drag::Thumb ? SCROLL_THUMB_ACTIVE : SCROLL_THUMB);
    }
}

// The status line: a clicked emote's file downloading, the emote under the mouse, a moment's
// "Pasted!", or how the search went.
void App::draw_status() {
    const std::wstring* s = &status_text;
    uint32_t c = MUTED;
    std::wstring getting = pending ? L"Downloading " + pending->name + L"…" : L"";
    if (pending) s = &getting, c = TEXT;
    else if (flashing) s = &flash_text, c = TEXT;
    else if (hover_i >= 0 && hover_i < (int)emotes.size()) s = &emotes[hover_i].name, c = TEXT;
    text(*s, f_status.Get(), {(float)px(12), status.t + (float)px(4), win_w - (float)px(12), status.b - (float)px(7)}, c);
}

// Saves what was last drawn as a PNG (for tests).
bool App::save_png(const std::wstring& path) {
    ComPtr<ID2D1Bitmap1> cpu;
    auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    D2D1_MAPPED_RECT mapped{};
    if (!target || FAILED(dc->CreateBitmap(D2D1::SizeU(win_w, win_h), nullptr, 0, props, &cpu)) ||
        FAILED(cpu->CopyFromBitmap(nullptr, target.Get(), nullptr)) ||
        FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped)))
        return false;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
              SUCCEEDED(wic->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(win_w, win_h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
              SUCCEEDED(frame->WritePixels(win_h, mapped.pitch, mapped.pitch * win_h, mapped.bits)) &&
              SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    cpu->Unmap();
    return ok;
}
