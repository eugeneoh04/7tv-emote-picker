#include "clipboard.h"

#include <shlobj.h>  // DROPFILES

#include <algorithm>
#include <cstring>
#include <set>

namespace {

bool open_clipboard(HWND owner) {
    for (int i = 0; i < 20; i++) {  // another app may have it open for a moment
        if (OpenClipboard(owner)) return true;
        Sleep(5);
    }
    return false;
}

// Formats that aren't a plain block of memory (bitmaps, metafiles, palettes, private
// handles) can't be saved byte for byte; Windows rebuilds bitmaps from CF_DIB anyway. OLE's
// "DataObject" and "Ole Private Data" point at the copying app's live object, so skip them too.
bool skip_format(UINT f) {
    static const std::set<UINT> skip = [] {
        std::set<UINT> s{CF_BITMAP, CF_METAFILEPICT, CF_PALETTE, CF_ENHMETAFILE, CF_OWNERDISPLAY,
                         CF_DSPBITMAP, CF_DSPMETAFILEPICT, CF_DSPENHMETAFILE};
        s.insert(RegisterClipboardFormatW(L"DataObject"));
        s.insert(RegisterClipboardFormatW(L"Ole Private Data"));
        return s;
    }();
    return skip.count(f) || (f >= 0x200 && f < 0x400);  // private and GDI-object formats
}

// Tells clipboard history (Win+V), cloud sync and clipboard managers to ignore what's on it.
const ClipItems& hide_from_history() {
    static const ClipItems marks = [] {
        ClipItems m;
        for (auto name : {L"ExcludeClipboardContentFromMonitorProcessing",
                          L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard"})
            m.push_back({RegisterClipboardFormatW(name), Bytes(4, 0)});
        return m;
    }();
    return marks;
}

}  // namespace

std::optional<ClipItems> clipboard_snapshot(HWND owner) {
    if (!open_clipboard(owner)) return std::nullopt;
    ClipItems saved;
    for (UINT f = EnumClipboardFormats(0); f; f = EnumClipboardFormats(f)) {
        if (skip_format(f)) continue;
        HANDLE h = GetClipboardData(f);
        void* p = h ? GlobalLock(h) : nullptr;
        if (p) {
            auto* b = (const uint8_t*)p;
            saved.push_back({f, Bytes(b, b + GlobalSize(h))});
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return saved;
}

std::optional<DWORD> clipboard_set(HWND owner, const ClipItems& items, bool hidden) {
    if (!open_clipboard(owner)) return std::nullopt;
    EmptyClipboard();
    ClipItems all = items;
    if (hidden && !items.empty())
        for (auto& mark : hide_from_history()) {
            bool have = false;
            for (auto& [f, d] : items) have |= f == mark.first;
            if (!have) all.push_back(mark);
        }
    for (auto& [f, data] : all) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, std::max<size_t>(1, data.size()));
        if (!h) continue;
        if (void* p = GlobalLock(h)) {
            if (!data.empty()) memcpy(p, data.data(), data.size());
            GlobalUnlock(h);
        }
        if (!SetClipboardData(f, h)) GlobalFree(h);
    }
    CloseClipboard();
    return GetClipboardSequenceNumber();
}

ClipItems clipboard_text(std::wstring_view text) {
    Bytes data((text.size() + 1) * sizeof(wchar_t), 0);
    memcpy(data.data(), text.data(), text.size() * sizeof(wchar_t));
    return {{CF_UNICODETEXT, std::move(data)}};
}

ClipItems clipboard_files(const std::vector<std::wstring>& paths) {
    // CF_HDROP: a DROPFILES header, then each path ending in a 0, then one more 0.
    size_t chars = 1;
    for (auto& p : paths) chars += p.size() + 1;
    Bytes drop(sizeof(DROPFILES) + chars * sizeof(wchar_t), 0);
    DROPFILES header{};
    header.pFiles = sizeof(DROPFILES);
    header.fWide = TRUE;
    memcpy(drop.data(), &header, sizeof(header));
    size_t at = sizeof(DROPFILES);
    for (auto& p : paths) {
        memcpy(drop.data() + at, p.data(), p.size() * sizeof(wchar_t));
        at += (p.size() + 1) * sizeof(wchar_t);
    }
    // Copied, not cut: pasting it in Explorer copies the file rather than moving it.
    Bytes effect(4, 0);
    effect[0] = DROPEFFECT_COPY;
    return {{CF_HDROP, std::move(drop)},
            {RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), std::move(effect)}};
}

std::wstring clipboard_get_text(HWND owner) {
    std::wstring text;
    if (!open_clipboard(owner)) return text;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = (const wchar_t*)GlobalLock(h)) {
            text.assign(p, wcsnlen(p, GlobalSize(h) / sizeof(wchar_t)));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return text;
}
