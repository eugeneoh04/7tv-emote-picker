#include "images.h"

#include <objbase.h>  // CreateStreamOnHGlobal
#include <wincodec.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {

IWICImagingFactory* wic() {
    static IWICImagingFactory* factory = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&factory));
    });
    return factory;
}

ComPtr<IWICBitmapDecoder> open(const Bytes& data) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    if (!wic() || data.empty() || FAILED(wic()->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory((BYTE*)data.data(), (DWORD)data.size())) ||
        FAILED(wic()->CreateDecoderFromStream(stream.Get(), nullptr,
                                              WICDecodeMetadataCacheOnDemand, &decoder)))
        return nullptr;
    return decoder;
}

// The size to show a w x h picture at, keeping its shape: height tall (its own height if 0),
// but never past room x room (no limit if 0). by_room: the room decided, not the height.
void fit(int w, int h, int height, int room, int& fw, int& fh, bool& by_room) {
    double k = height > 0 ? (double)height / h : 1.0;
    by_room = room > 0 && (w * k > room || h * k > room);
    if (by_room) k = std::min((double)room / w, (double)room / h);
    fw = std::max(1, (int)std::lround(w * k));
    fh = std::max(1, (int)std::lround(h * k));
}

// Copies source (premultiplied BGRA) into dest at stride bytes per row, scaled to fw x fh.
bool copy_scaled(IWICBitmapSource* source, int w, int h, int fw, int fh, uint32_t* dest,
                 UINT stride) {
    ComPtr<IWICBitmapSource> src = source;
    if (fw != w || fh != h) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic()->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(source, fw, fh, WICBitmapInterpolationModeHighQualityCubic)))
            return false;
        src = scaler;
    }
    WICRect all{0, 0, fw, fh};
    return SUCCEEDED(src->CopyPixels(&all, stride, stride * (fh - 1) + fw * 4, (BYTE*)dest));
}

// The whole W x H picture canvas (premultiplied BGRA), scaled to fw x fh into dest.
bool scale_canvas(const std::vector<uint32_t>& canvas, int W, int H, int fw, int fh,
                  uint32_t* dest, UINT stride) {
    ComPtr<IWICBitmap> bitmap;
    return SUCCEEDED(wic()->CreateBitmapFromMemory(W, H, GUID_WICPixelFormat32bppPBGRA, W * 4,
                                                   (UINT)canvas.size() * 4, (BYTE*)canvas.data(),
                                                   &bitmap)) &&
           copy_scaled(bitmap.Get(), W, H, fw, fh, dest, stride);
}

int meta_int(IWICMetadataQueryReader* reader, const wchar_t* name, int fallback) {
    PROPVARIANT v;
    PropVariantInit(&v);
    int out = fallback;
    if (reader && SUCCEEDED(reader->GetMetadataByName(name, &v))) {
        switch (v.vt) {
            case VT_UI1: out = v.bVal; break;
            case VT_UI2: out = v.uiVal; break;
            case VT_UI4: out = (int)v.ulVal; break;
            case VT_BOOL: out = v.boolVal ? 1 : 0; break;
            default: break;
        }
    }
    PropVariantClear(&v);
    return out;
}

// Goes through a GIF's frames in turn, layering each on the ones before it the way the GIF
// says. frame_ready(f, canvas, delay) gets the whole W x H picture as it shows then
// (premultiplied BGRA, 0 where nothing is drawn) and the frame's delay in hundredths of a
// second, as written; it returns false to stop.
template <class Fn>
bool each_frame(IWICBitmapDecoder* decoder, UINT count, int W, int H, Fn&& frame_ready) {
    // The picture so far; 0 is "nothing drawn here" (transparent).
    std::vector<uint32_t> canvas((size_t)W * H, 0), saved;
    std::vector<BYTE> indexes;
    for (UINT f = 0; f < count; f++) {
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICMetadataQueryReader> meta;
        if (FAILED(decoder->GetFrame(f, &frame))) return false;
        frame->GetMetadataQueryReader(&meta);
        int left = meta_int(meta.Get(), L"/imgdesc/Left", 0);
        int top = meta_int(meta.Get(), L"/imgdesc/Top", 0);
        int delay = meta_int(meta.Get(), L"/grctlext/Delay", 0);       // hundredths of a second
        int disposal = meta_int(meta.Get(), L"/grctlext/Disposal", 0);
        bool has_transparent = meta_int(meta.Get(), L"/grctlext/TransparencyFlag", 0) != 0;
        int transparent = meta_int(meta.Get(), L"/grctlext/TransparentColorIndex", -1);
        UINT fw = 0, fh = 0;
        frame->GetSize(&fw, &fh);
        if (disposal == 3) saved = canvas;

        // Its pixels go on top, except transparent ones.
        WICPixelFormatGUID format{};
        frame->GetPixelFormat(&format);
        if (format == GUID_WICPixelFormat8bppIndexed) {
            ComPtr<IWICPalette> palette;
            WICColor colors[256] = {};
            UINT ncolors = 0;
            indexes.resize((size_t)fw * fh);
            if (FAILED(wic()->CreatePalette(&palette)) || FAILED(frame->CopyPalette(palette.Get())) ||
                FAILED(palette->GetColors(256, colors, &ncolors)) ||
                FAILED(frame->CopyPixels(nullptr, fw, (UINT)indexes.size(), indexes.data())))
                return false;
            for (int y = 0; y < (int)fh && top + y < H; y++) {
                const BYTE* row = &indexes[(size_t)y * fw];
                uint32_t* dst = &canvas[(size_t)(top + y) * W];
                for (int x = 0; x < (int)fw && left + x < W; x++) {
                    BYTE i = row[x];
                    if ((has_transparent && i == transparent) || i >= ncolors || !(colors[i] >> 24))
                        continue;
                    dst[left + x] = 0xFF000000u | (colors[i] & 0xFFFFFFu);
                }
            }
        } else {  // not expected from a GIF, but any format WIC can turn into BGRA works
            ComPtr<IWICFormatConverter> converter;
            std::vector<uint32_t> px((size_t)fw * fh);
            if (FAILED(wic()->CreateFormatConverter(&converter)) ||
                FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                             WICBitmapDitherTypeNone, nullptr, 0,
                                             WICBitmapPaletteTypeCustom)) ||
                FAILED(converter->CopyPixels(nullptr, fw * 4, (UINT)px.size() * 4, (BYTE*)px.data())))
                return false;
            for (int y = 0; y < (int)fh && top + y < H; y++)
                for (int x = 0; x < (int)fw && left + x < W; x++)
                    if (uint32_t p = px[(size_t)y * fw + x]; p >> 24)
                        canvas[(size_t)(top + y) * W + left + x] = p;
        }

        if (!frame_ready(f, canvas, delay)) return false;

        if (disposal == 2) {         // its area is cleared before the next frame
            int x0 = std::max(0, left), x1 = std::min(W, left + (int)fw);
            for (int y = std::max(0, top); x0 < x1 && y < std::min(H, top + (int)fh); y++)
                std::fill(canvas.begin() + (size_t)y * W + x0, canvas.begin() + (size_t)y * W + x1, 0u);
        } else if (disposal == 3) {  // what was there before it comes back
            canvas = saved;
        }
    }
    return true;
}

// ---- writing GIFs and PNGs (file mode's "auto" size) --------------------------------------

// A premultiplied BGRA pixel as plain BGRA: its color no longer scaled down by its alpha.
uint32_t unpremultiply(uint32_t p) {
    uint32_t a = p >> 24;
    if (!a) return 0;
    auto c = [&](int shift) { return std::min(255u, ((p >> shift & 0xFF) * 255 + a / 2) / a) << shift; };
    return a << 24 | c(16) | c(8) | c(0);
}

int channel(uint32_t rgb, int c) { return rgb >> (16 - 8 * c) & 0xFF; }  // c: 0 red, 1 green, 2 blue
constexpr int WEIGHT[3] = {3, 4, 2};  // how much a difference in red, green, blue shows

struct Swatch {
    uint32_t rgb, n;  // a color (0xRRGGBB), and how many pixels have it
};

// At most `most` colors to show the swatches with: all of them if there are no more than that,
// otherwise by median cut. The colors start out in one box; the box with the most (squared)
// error is split in two at the middle pixel of its widest channel, until there are `most`
// boxes, and each box gives the average of its pixels.
std::vector<uint32_t> make_palette(std::vector<Swatch> colors, int most) {
    std::vector<uint32_t> palette;
    if ((int)colors.size() <= most) {
        for (auto& s : colors) palette.push_back(s.rgb);
        return palette;
    }
    struct Box {
        size_t from, to;
        double n = 0, sum[3] = {}, err = 0;
        int axis = 0;
    };
    auto measure = [&](size_t from, size_t to) {
        Box b{from, to};
        double sq[3] = {};
        for (size_t i = from; i < to; i++) {
            double w = colors[i].n;
            b.n += w;
            for (int c = 0; c < 3; c++) {
                double v = channel(colors[i].rgb, c);
                b.sum[c] += w * v;
                sq[c] += w * v * v;
            }
        }
        double widest = -1;
        for (int c = 0; c < 3; c++) {
            double e = WEIGHT[c] * (sq[c] - b.sum[c] * b.sum[c] / b.n);
            b.err += e;
            if (e > widest) widest = e, b.axis = c;
        }
        if (to - from < 2) b.err = 0;  // one color: nothing to split
        return b;
    };
    std::vector<Box> boxes{measure(0, colors.size())};
    while ((int)boxes.size() < most) {
        auto worst = std::max_element(boxes.begin(), boxes.end(),
                                      [](const Box& a, const Box& b) { return a.err < b.err; });
        if (worst->err <= 0) break;
        Box b = *worst;
        std::sort(colors.begin() + b.from, colors.begin() + b.to, [&](const Swatch& x, const Swatch& y) {
            return channel(x.rgb, b.axis) < channel(y.rgb, b.axis);
        });
        // About half its pixels on each side, and at least one color.
        double below = colors[b.from].n;
        size_t split = b.from + 1;
        while (split + 1 < b.to && below + colors[split].n <= b.n / 2) below += colors[split++].n;
        *worst = measure(b.from, split);
        boxes.push_back(measure(split, b.to));
    }
    for (auto& b : boxes) {
        auto avg = [&](int c) { return (uint32_t)std::clamp(std::lround(b.sum[c] / b.n), 0L, 255L); };
        palette.push_back(avg(0) << 16 | avg(1) << 8 | avg(2));
    }
    return palette;
}

// Where in palette the color nearest to rgb is.
int nearest(const std::vector<uint32_t>& palette, uint32_t rgb) {
    int r = channel(rgb, 0), g = channel(rgb, 1), b = channel(rgb, 2), best = 0, best_d = INT_MAX;
    for (int i = 0; i < (int)palette.size(); i++) {
        int dr = channel(palette[i], 0) - r, dg = channel(palette[i], 1) - g, db = channel(palette[i], 2) - b;
        int d = WEIGHT[0] * dr * dr + WEIGHT[1] * dg * dg + WEIGHT[2] * db * db;
        if (d < best_d) best_d = d, best = i;
    }
    return best;
}

void put16(Bytes& out, int v) {  // little-endian, as GIF numbers are
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)(v >> 8 & 0xFF));
}

// GIF's LZW compression of palette indexes (each below 1 << min_bits), as sub-blocks of up to
// 255 bytes, then an empty one. Codes start min_bits + 1 bits wide and grow as the dictionary
// does; a full dictionary (4096 codes) starts over after a clear code.
void lzw(const std::vector<uint8_t>& px, int min_bits, Bytes& out) {
    const int clear = 1 << min_bits, stop = clear + 1;
    int bits = min_bits + 1, last = stop;  // code width, and the newest code in the dictionary
    // The dictionary: a code followed by one more index -> its code. Open addressing.
    constexpr uint32_t SLOTS = 1 << 13, EMPTY = ~0u;
    std::vector<uint32_t> keys(SLOTS, EMPTY);
    std::vector<uint16_t> codes(SLOTS);
    Bytes block;  // the sub-block being filled
    uint32_t acc = 0;
    int held = 0;  // bits in acc
    auto put = [&](int code) {
        acc |= (uint32_t)code << held;
        for (held += bits; held >= 8; held -= 8, acc >>= 8) {
            block.push_back((uint8_t)acc);
            if (block.size() == 255) {
                out.push_back(255);
                out.insert(out.end(), block.begin(), block.end());
                block.clear();
            }
        }
    };
    out.push_back((uint8_t)min_bits);
    put(clear);
    int run = -1;  // the code for the indexes since the last code written
    for (uint8_t c : px) {
        if (run < 0) {
            run = c;
            continue;
        }
        uint32_t key = (uint32_t)run << 8 | c, slot = (key * 2654435761u) >> 19;
        while (keys[slot] != EMPTY && keys[slot] != key) slot = (slot + 1) & (SLOTS - 1);
        if (keys[slot] == key) {
            run = codes[slot];
            continue;
        }
        put(run);
        keys[slot] = key;
        codes[slot] = (uint16_t)++last;
        if (last >= (1 << bits)) bits++;
        if (last == 4095) {
            put(clear);
            std::fill(keys.begin(), keys.end(), EMPTY);
            bits = min_bits + 1;
            last = stop;
        }
        run = c;
    }
    if (run >= 0) put(run);
    // A reader widens its codes as if this last code had made a new one, too.
    if (last + 1 >= (1 << bits) && bits < 12) bits++;
    put(stop);
    if (held > 0) block.push_back((uint8_t)acc);
    if (!block.empty()) {
        out.push_back((uint8_t)block.size());
        out.insert(out.end(), block.begin(), block.end());
    }
    out.push_back(0);
}

// How many more times a GIF plays after the first (0: forever), from its NETSCAPE2.0 block; -1
// if it has none (it plays once).
int loop_count(IWICMetadataQueryReader* global) {
    PROPVARIANT app, data;
    PropVariantInit(&app);
    PropVariantInit(&data);
    int loops = -1;
    if (global && SUCCEEDED(global->GetMetadataByName(L"/appext/Application", &app)) &&
        app.vt == (VT_UI1 | VT_VECTOR) && app.caub.cElems == 11 &&
        (!memcmp(app.caub.pElems, "NETSCAPE2.0", 11) || !memcmp(app.caub.pElems, "ANIMEXTS1.0", 11))) {
        loops = 0;
        // Its data: a 3-byte sub-block, 1, then the count (little-endian).
        if (SUCCEEDED(global->GetMetadataByName(L"/appext/Data", &data)) && data.vt == (VT_UI1 | VT_VECTOR) &&
            data.caub.cElems >= 4 && data.caub.pElems[0] >= 3 && data.caub.pElems[1] == 1)
            loops = data.caub.pElems[2] | data.caub.pElems[3] << 8;
    }
    PropVariantClear(&app);
    PropVariantClear(&data);
    return loops;
}

// The color table of a GIF image: palette after index 0 (transparent), in 1 << bits entries.
// Returns bits.
int put_color_table(Bytes& out, const std::vector<uint32_t>& palette) {
    int bits = 1;
    while ((1 << bits) < (int)palette.size() + 1) bits++;
    for (int i = 0; i < (1 << bits); i++) {
        uint32_t c = i >= 1 && i <= (int)palette.size() ? palette[i - 1] : 0;
        out.insert(out.end(), {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c});
    }
    return bits;
}

// Each color's nearest palette entry: index[color] is its place + 1 (0 is transparent).
void map_colors(const std::unordered_map<uint32_t, uint32_t>& counts, const std::vector<uint32_t>& palette,
                std::unordered_map<uint32_t, uint8_t>& index) {
    for (auto& [c, n] : counts) index[c] = (uint8_t)(nearest(palette, c) + 1);
}

// How far colors are from the palette entries index gives them: the average difference per
// channel, over all their pixels.
double how_far(const std::unordered_map<uint32_t, uint32_t>& counts, const std::vector<uint32_t>& palette,
               const std::unordered_map<uint32_t, uint8_t>& index) {
    double off = 0, pixels = 0;
    for (auto& [c, n] : counts) {
        uint32_t p = palette[index.at(c) - 1];
        for (int ch = 0; ch < 3; ch++) off += (double)n * std::abs(channel(c, ch) - channel(p, ch));
        pixels += n;
    }
    return pixels ? off / (3 * pixels) : 0;
}

std::vector<Swatch> swatches_of(const std::unordered_map<uint32_t, uint32_t>& counts) {
    std::vector<Swatch> s;
    s.reserve(counts.size());
    for (auto& [c, n] : counts) s.push_back({c, n});
    return s;
}

// An animated GIF, written once all its frames are in. Measured on 24 popular 7TV emotes (2x
// made 48px tall): 22% smaller than the 2x files, colors 0-2.5 levels off on average.
// - A frame draws only what changed since the one before; the rest of it is transparent,
//   showing what's already there. A pixel within SAME_WITHIN of what's on screen in every
//   channel counts as unchanged: emotes are full of changes too small to see, and drawing them
//   made files a third bigger.
// - Pixels can't turn transparent that way, so when the next frame has transparent pixels where
//   this one shows something, this one covers the whole picture and is cleared once shown
//   (disposal 2), and the next is drawn whole; otherwise it stays (disposal 1).
// - One palette made from every frame's colors serves them all (like 7TV's own GIFs). A frame
//   whose colors it would show more than OWN_PALETTE_PAST levels off on average (very colorful
//   ones) gets a palette of its own, if that's at least half a level closer.
// - Index 0 is transparent everywhere.
struct GifWriter {
    static constexpr uint32_t CLEAR = 0xFF000000;  // not a color (0xRRGGBB): a transparent pixel
    static constexpr int SAME_WITHIN = 2;
    static constexpr double OWN_PALETTE_PAST = 3;
    int w, h, loops;
    struct Frame {
        std::vector<uint32_t> px;  // colors, or CLEAR
        int delay;
        std::vector<uint8_t> drawn;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the part it covers: [x0, x1) x [y0, y1)
        bool clear_after = false;
    };
    std::vector<Frame> frames;

    // px: w x h premultiplied BGRA. Pixels at least half opaque keep their color, made fully
    // opaque (GIF has nothing in between); the rest become transparent.
    void add(const uint32_t* px, int delay) {
        Frame f{std::vector<uint32_t>((size_t)w * h), delay};
        for (size_t i = 0; i < f.px.size(); i++) f.px[i] = px[i] >> 24 < 128 ? CLEAR : unpremultiply(px[i]) & 0xFFFFFF;
        frames.push_back(std::move(f));
    }

    void write(Bytes& out) {
        size_t n = (size_t)w * h;
        // What each frame draws: on a cleared screen whatever isn't transparent, otherwise what
        // changed (by more than SAME_WITHIN, from what's on screen).
        bool cleared = true;
        std::vector<uint32_t> shown(n, CLEAR);       // the colors on screen, as they were asked for
        std::unordered_map<uint32_t, uint32_t> all;  // every color drawn, and how often
        for (size_t k = 0; k < frames.size(); k++) {
            Frame& f = frames[k];
            f.drawn.assign(n, 0);
            f.x0 = w, f.y0 = h;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    size_t i = (size_t)y * w + x;
                    if (f.px[i] == CLEAR) continue;
                    if (!cleared && shown[i] != CLEAR) {
                        int off = 0;
                        for (int ch = 0; ch < 3; ch++)
                            off = std::max(off, std::abs(channel(f.px[i], ch) - channel(shown[i], ch)));
                        if (off <= SAME_WITHIN) continue;
                    }
                    shown[i] = f.px[i];
                    f.drawn[i] = 1;
                    all[f.px[i]]++;
                    f.x0 = std::min(f.x0, x), f.y0 = std::min(f.y0, y);
                    f.x1 = std::max(f.x1, x + 1), f.y1 = std::max(f.y1, y + 1);
                }
            for (size_t i = 0; k + 1 < frames.size() && i < n && !f.clear_after; i++)
                f.clear_after = frames[k + 1].px[i] == CLEAR && f.px[i] != CLEAR;
            if (f.clear_after) f.x0 = 0, f.y0 = 0, f.x1 = w, f.y1 = h;  // so all of it is cleared
            if (f.x1 <= f.x0) f.x0 = 0, f.y0 = 0, f.x1 = 1, f.y1 = 1;    // nothing changed
            cleared = f.clear_after;
            if (cleared) std::fill(shown.begin(), shown.end(), CLEAR);
        }

        const char* signature = "GIF89a";
        out.insert(out.end(), signature, signature + 6);
        put16(out, w);
        put16(out, h);
        std::vector<uint32_t> global = make_palette(swatches_of(all), 255);
        std::unordered_map<uint32_t, uint8_t> in_global;  // found once for all frames
        map_colors(all, global, in_global);
        size_t packed_at = out.size();
        out.insert(out.end(), {0, 0, 0});
        int global_bits = put_color_table(out, global);
        out[packed_at] = (uint8_t)(0x80 | 0x70 | (global_bits - 1));
        if (loops >= 0) {
            const char* looping = "\x21\xFF\x0BNETSCAPE2.0\x03\x01";
            out.insert(out.end(), looping, looping + 16);
            put16(out, loops);
            out.push_back(0);
        }

        for (Frame& f : frames) {
            std::unordered_map<uint32_t, uint32_t> counts;
            for (size_t i = 0; i < n; i++)
                if (f.drawn[i]) counts[f.px[i]]++;
            std::unordered_map<uint32_t, uint8_t> in_own;
            std::vector<uint32_t> own;
            if (double off = how_far(counts, global, in_global); off > OWN_PALETTE_PAST) {
                own = make_palette(swatches_of(counts), 255);
                map_colors(counts, own, in_own);
                if (how_far(counts, own, in_own) >= off - 0.5) own.clear();
            }
            const auto& index = own.empty() ? in_global : in_own;
            // Graphic control: what happens to it once shown, index 0 transparent, its delay.
            out.insert(out.end(), {0x21, 0xF9, 0x04, (uint8_t)((f.clear_after ? 2 : 1) << 2 | 1)});
            put16(out, f.delay);
            out.insert(out.end(), {0, 0});
            // Where it goes, its own color table if it has one, then its pixels.
            int rw = f.x1 - f.x0, rh = f.y1 - f.y0;
            out.push_back(0x2C);
            put16(out, f.x0);
            put16(out, f.y0);
            put16(out, rw);
            put16(out, rh);
            size_t flags_at = out.size();
            out.push_back(0);
            int bits = global_bits;
            if (!own.empty()) {
                bits = put_color_table(out, own);
                out[flags_at] = (uint8_t)(0x80 | (bits - 1));
            }
            std::vector<uint8_t> indexes((size_t)rw * rh);
            for (int y = 0; y < rh; y++)
                for (int x = 0; x < rw; x++) {
                    size_t i = (size_t)(f.y0 + y) * w + f.x0 + x;
                    indexes[(size_t)y * rw + x] = f.drawn[i] ? index.at(f.px[i]) : 0;
                }
            lzw(indexes, std::max(2, bits), out);
        }
        out.push_back(0x3B);
    }
};

bool resize_gif(IWICBitmapDecoder* decoder, int height, Bytes& out) {
    UINT count = 0;
    ComPtr<IWICMetadataQueryReader> global;
    if (FAILED(decoder->GetFrameCount(&count)) || !count || FAILED(decoder->GetMetadataQueryReader(&global)))
        return false;
    int W = meta_int(global.Get(), L"/logscrdesc/Width", 0);
    int H = meta_int(global.Get(), L"/logscrdesc/Height", 0);
    // The frames are kept until the end (~5 bytes a pixel once resized), so not huge GIFs: 7TV's
    // 2x ones are under 1M pixels in all.
    if (W <= 0 || H <= height || W > 4096 || H > 4096 || (size_t)W * H * count > 16u << 20) return false;
    int w, h;
    bool by_room;
    fit(W, H, height, 0, w, h, by_room);
    GifWriter gif{w, h, loop_count(global.Get())};
    std::vector<uint32_t> resized((size_t)w * h);
    if (!each_frame(decoder, count, W, H, [&](UINT, const std::vector<uint32_t>& canvas, int delay) {
            if (!scale_canvas(canvas, W, H, w, h, resized.data(), w * 4)) return false;
            gif.add(resized.data(), delay);
            return true;
        }))
        return false;
    out.clear();
    gif.write(out);
    return true;
}

// px (plain BGRA, w x h) as a PNG file.
bool write_png(std::vector<uint32_t>& px, int w, int h, Bytes& out) {
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    STATSTG stat{};
    LARGE_INTEGER start{};
    ULONG got = 0;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&format)) ||
        format != GUID_WICPixelFormat32bppBGRA ||
        FAILED(frame->WritePixels(h, w * 4, w * h * 4, (BYTE*)px.data())) || FAILED(frame->Commit()) ||
        FAILED(encoder->Commit()) || FAILED(stream->Stat(&stat, STATFLAG_NONAME)))
        return false;
    out.resize((size_t)stat.cbSize.QuadPart);
    return SUCCEEDED(stream->Seek(start, STREAM_SEEK_SET, nullptr)) &&
           SUCCEEDED(stream->Read(out.data(), (ULONG)out.size(), &got)) && got == out.size();
}

bool resize_png(IWICBitmapDecoder* decoder, int height, Bytes& out) {
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT W = 0, H = 0;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(wic()->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&W, &H)) || !W || (int)H <= height || W > 4096 || H > 4096)
        return false;
    int w, h;
    bool by_room;
    fit((int)W, (int)H, height, 0, w, h, by_room);
    std::vector<uint32_t> px((size_t)w * h);
    if (!copy_scaled(converter.Get(), W, H, w, h, px.data(), w * 4)) return false;
    for (auto& p : px) p = unpremultiply(p);  // PNG keeps colors apart from their alpha
    return write_png(px, w, h, out);
}

}  // namespace

bool decode_still(const Bytes& data, int height, int room, Image& out) {
    auto decoder = open(data);
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT w = 0, h = 0;
    if (!decoder || FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(wic()->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&w, &h)) || !w || !h || w > 4096 || h > 4096)
        return false;
    fit((int)w, (int)h, height, room, out.w, out.h, out.by_room);
    out.px.assign((size_t)out.w * out.h, 0);
    return copy_scaled(converter.Get(), w, h, out.w, out.h, out.px.data(), out.w * 4);
}

bool decode_gif(const Bytes& data, int height, int room, int max_texture, Animation& out) {
    auto decoder = open(data);
    UINT count = 0;
    GUID container{};
    if (!decoder || FAILED(decoder->GetContainerFormat(&container)) ||
        container != GUID_ContainerFormatGif || FAILED(decoder->GetFrameCount(&count)) || count < 2)
        return false;
    ComPtr<IWICMetadataQueryReader> global;
    decoder->GetMetadataQueryReader(&global);
    int W = meta_int(global.Get(), L"/logscrdesc/Width", 0);
    int H = meta_int(global.Get(), L"/logscrdesc/Height", 0);
    if (W <= 0 || H <= 0 || (size_t)W * H * count > 32u << 20) return false;  // ~128MB of frames

    fit(W, H, height, room, out.w, out.h, out.by_room);
    out.count = (int)count;
    out.cols = std::max(1, std::min((int)count, max_texture / out.w));
    int rows = ((int)count + out.cols - 1) / out.cols;
    if (rows * out.h > max_texture) return false;
    out.atlas.w = out.cols * out.w;
    out.atlas.h = rows * out.h;
    out.atlas.px.assign((size_t)out.atlas.w * out.atlas.h, 0);
    out.ends.clear();

    int elapsed = 0;
    return each_frame(decoder.Get(), count, W, H, [&](UINT f, const std::vector<uint32_t>& canvas, int delay) {
        // Into the atlas, shrunk if it has to be.
        uint32_t* cell = &out.atlas.px[(size_t)(f / out.cols) * out.h * out.atlas.w +
                                       (size_t)(f % out.cols) * out.w];
        if (out.w == W && out.h == H) {
            for (int y = 0; y < H; y++)
                std::copy_n(&canvas[(size_t)y * W], W, cell + (size_t)y * out.atlas.w);
        } else if (!scale_canvas(canvas, W, H, out.w, out.h, cell, out.atlas.w * 4)) {
            return false;
        }
        // Like browsers, frames with no real delay show for 100ms.
        elapsed += delay > 1 ? delay * 10 : 100;
        out.ends.push_back(elapsed);
        return true;
    });
}

bool resize_image(const Bytes& data, int height, Bytes& out) {
    auto decoder = open(data);
    GUID container{};
    if (height <= 0 || !decoder || FAILED(decoder->GetContainerFormat(&container))) return false;
    if (container == GUID_ContainerFormatGif) return resize_gif(decoder.Get(), height, out);
    if (container == GUID_ContainerFormatPng) return resize_png(decoder.Get(), height, out);
    return false;
}
