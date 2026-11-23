#include "images.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <mutex>

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

    // The picture so far; 0 is "nothing drawn here" (transparent).
    std::vector<uint32_t> canvas((size_t)W * H, 0), saved;
    std::vector<BYTE> indexes;
    int elapsed = 0;
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

        // Into the atlas, shrunk if it has to be.
        uint32_t* cell = &out.atlas.px[(size_t)(f / out.cols) * out.h * out.atlas.w +
                                       (size_t)(f % out.cols) * out.w];
        if (out.w == W && out.h == H) {
            for (int y = 0; y < H; y++)
                std::copy_n(&canvas[(size_t)y * W], W, cell + (size_t)y * out.atlas.w);
        } else {
            ComPtr<IWICBitmap> bitmap;
            if (FAILED(wic()->CreateBitmapFromMemory(W, H, GUID_WICPixelFormat32bppPBGRA, W * 4,
                                                     (UINT)canvas.size() * 4, (BYTE*)canvas.data(),
                                                     &bitmap)) ||
                !copy_scaled(bitmap.Get(), W, H, out.w, out.h, cell, out.atlas.w * 4))
                return false;
        }
        // Like browsers, frames with no real delay show for 100ms.
        elapsed += delay > 1 ? delay * 10 : 100;
        out.ends.push_back(elapsed);

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
