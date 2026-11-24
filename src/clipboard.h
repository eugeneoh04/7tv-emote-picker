// Borrowing the clipboard: save what's on it, put an emote link on it (kept out of
// clipboard history), and put the saved copy back afterwards.
#pragma once

#include <optional>
#include <utility>

#include "common.h"

using ClipItems = std::vector<std::pair<UINT, Bytes>>;  // (format, data)

// Everything on the clipboard that can be saved byte for byte, or nothing if it's busy.
std::optional<ClipItems> clipboard_snapshot(HWND owner);
// Replaces the clipboard with items; hidden keeps it out of clipboard history (Win+V), cloud
// sync and clipboard managers. Returns the clipboard's new sequence number, or nothing if busy.
std::optional<DWORD> clipboard_set(HWND owner, const ClipItems& items, bool hidden);
// Like clipboard_set(hidden), but the items are only promised ("delayed rendering"): when a
// program asks for one, Windows sends owner WM_RENDERFORMAT, which hands it over with
// clipboard_render. That way owner knows when the paste was actually read.
std::optional<DWORD> clipboard_lend(HWND owner, const ClipItems& items);
// For WM_RENDERFORMAT: puts format's data from items on the clipboard. False if items has none.
bool clipboard_render(UINT format, const ClipItems& items);
ClipItems clipboard_text(std::wstring_view text);  // text as clipboard items
// Files as clipboard items, the way Explorer copies them (Ctrl+V in Discord attaches them).
ClipItems clipboard_files(const std::vector<std::wstring>& paths);
// Plain text on the clipboard (for pasting into the search box).
std::wstring clipboard_get_text(HWND owner);
