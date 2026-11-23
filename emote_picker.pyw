"""7TV emote picker.

A tiny borderless panel docked to the bottom-right of Discord, just above the
message box. It only shows while Discord (or the picker itself) is focused.
Type to search 7TV, click an emote and its link is pasted into Discord. The link only
borrows the clipboard: whatever you had copied before is put back right after.
Drag the corner grip to resize, click the X to quit. Starting it again (say, after
changing this file) replaces the copy that's running.
Run with:  pythonw emote_picker.pyw   (or double-click the file)
"""

import bisect
import ctypes
from ctypes import wintypes
import json
import os
import queue
import struct
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
from tkinter import ttk
import http.client
from concurrent.futures import ThreadPoolExecutor
from itertools import accumulate
from urllib.parse import urlsplit

# ---- settings -------------------------------------------------------------
# The pasted link's size and format are picked with the gear button in the app
# and saved to settings.json next to this file.
LINK_URL = "https://cdn.7tv.app/emote/{id}/{size}.{format}"
LINK_SIZES = ["1x", "2x", "3x", "4x"]
LINK_FORMATS = ["webp", "avif", "gif", "png"]
# width/height are the panel size at 100% display scaling; drag the corner grip to
# change them, everything scales.
DEFAULT_SETTINGS = {"size": "4x", "format": "webp", "width": 340, "height": 420}
ADD_SPACE_AFTER = False    # append a space after the pasted link
PASTE_DELAY_MS = 250       # after clicking an emote, give Discord this long to take focus
# This long after pasting, whatever you had copied before (from Chrome etc.) goes back on
# the clipboard. Raise it if Discord ever pastes that instead of the emote.
RESTORE_CLIPBOARD_MS = 1000
PER_PAGE = 250             # emotes per search (7TV allows at most 250)
# How big emotes show, at 100% display scaling. The nearest of 7TV's preview sizes is used
# (32/64/96/128px, e.g. 64px at 175%), and each row fits as many emotes as it has room for.
EMOTE_SIZE = 40
MIN_SIZE = (220, 140)
DISCORD_EXES = {"discord.exe", "discordptb.exe", "discordcanary.exe"}
# Gap between the panel and Discord's bottom-right corner, at 100% display scaling.
# The bottom gap keeps Discord's message box uncovered.
DOCK_MARGIN_RIGHT = 16
DOCK_MARGIN_BOTTOM = 80
FOLLOW_MS = 50             # how often to check Discord's position and focus
# Preview images: PNG, since tkinter can't show webp. {s} is 1-4 (32-128px tall).
THUMB_ANIMATED = "https://cdn.7tv.app/emote/{id}/{s}x_static.png"
THUMB_STATIC = "https://cdn.7tv.app/emote/{id}/{s}x.png"
# Animated emotes on screen then also download their GIF (the moving format tkinter can
# show, ~60KB each) and start moving once it's decoded.
ANIMATE = True             # False: animated emotes stay still
THUMB_GIF = "https://cdn.7tv.app/emote/{id}/{s}x.gif"
# Previews download for the rows on screen and the next screen down, the rest as you scroll.
PREVIEW_DOWNLOADS = 12     # previews downloading at once
MAX_SAVED_PREVIEWS = 1000  # past this many downloaded previews, forget the ones not shown
MAX_GIF_MB = 48            # past this much downloaded GIF, forget the ones not on screen
API_URL = "https://7tv.io/v4/gql"
SEARCH_DELAY_MS = 250      # wait this long after you stop typing before searching
# ---------------------------------------------------------------------------

# Look: Discord's dark theme colors, so the panel feels like part of Discord.
BG, FIELD, HOVER, SELECTED = "#2b2d31", "#1e1f22", "#393c41", "#404249"
TEXT, MUTED, FAINT, BORDER = "#dbdee1", "#949ba4", "#6d6f78", "#1e1f22"
SCROLL_THUMB, SCROLL_THUMB_ACTIVE, GRIP = "#1a1b1e", "#111214", "#4e5058"
PAD, FIELD_H, CELL_PAD = 8, 32, 6   # spacing at 100% display scaling, in pixels
FRAME_MS = 30              # animated emotes move on this often (Windows makes it ~31ms)
DECODE_MS = 6              # at most this much GIF decoding each time, so nothing stutters

SEARCH_QUERY = """
query($q: String!, $perPage: Int!) {
  emotes {
    search(query: $q, page: 1, perPage: $perPage,
           sort: {sortBy: TOP_ALL_TIME, order: DESCENDING}) {
      items { id defaultName flags { animated } }
    }
  }
}
"""

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
VK_CONTROL, VK_F24, VK_V, KEYUP = 0x11, 0x87, 0x56, 0x0002
HWND_TOPMOST = -1
SWP_NOSIZE, SWP_NOMOVE, SWP_NOZORDER, SWP_NOACTIVATE = 0x1, 0x2, 0x4, 0x10
SWP_SHOWWINDOW, SWP_HIDEWINDOW = 0x40, 0x80
user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, ctypes.c_uint]
CF_UNICODETEXT, GMEM_MOVEABLE = 13, 0x0002
for func, args, res in (
        (user32.OpenClipboard, [wintypes.HWND], wintypes.BOOL),
        (user32.EnumClipboardFormats, [wintypes.UINT], wintypes.UINT),
        (user32.GetClipboardData, [wintypes.UINT], wintypes.HANDLE),
        (user32.SetClipboardData, [wintypes.UINT, wintypes.HANDLE], wintypes.HANDLE),
        (user32.RegisterClipboardFormatW, [wintypes.LPCWSTR], wintypes.UINT),
        (user32.GetClipboardSequenceNumber, [], wintypes.DWORD),
        (kernel32.GlobalAlloc, [wintypes.UINT, ctypes.c_size_t], wintypes.HGLOBAL),
        (kernel32.GlobalLock, [wintypes.HGLOBAL], ctypes.c_void_p),
        (kernel32.GlobalUnlock, [wintypes.HGLOBAL], wintypes.BOOL),
        (kernel32.GlobalSize, [wintypes.HGLOBAL], ctypes.c_size_t),
        (kernel32.GlobalFree, [wintypes.HGLOBAL], wintypes.HGLOBAL),
        (user32.FindWindowExW, [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR],
         wintypes.HWND),
        (user32.PostMessageW, [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM],
         wintypes.BOOL),
        (kernel32.OpenProcess, [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
        (kernel32.WaitForSingleObject, [wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
        (kernel32.TerminateProcess, [wintypes.HANDLE, wintypes.UINT], wintypes.BOOL),
        (kernel32.CloseHandle, [wintypes.HANDLE], wintypes.BOOL)):
    func.argtypes, func.restype = args, res
# Hook functions come from a private copy of user32, so their declarations can't clash
# with anyone else's in the process.
hooks = ctypes.WinDLL("user32")
HOOKPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM)
hooks.SetWindowsHookExW.argtypes = [ctypes.c_int, HOOKPROC, wintypes.HINSTANCE, wintypes.DWORD]
hooks.SetWindowsHookExW.restype = ctypes.c_void_p
hooks.CallNextHookEx.argtypes = [ctypes.c_void_p, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM]
hooks.CallNextHookEx.restype = ctypes.c_ssize_t
hooks.UnhookWindowsHookEx.argtypes = [ctypes.c_void_p]
# Clipboard formats that aren't a plain block of memory (bitmaps, metafiles, palettes, private
# handles) can't be saved byte for byte; Windows rebuilds bitmaps from CF_DIB anyway. OLE's
# "DataObject" and "Ole Private Data" point at the copying app's live object, so skip them too.
SKIP_FORMATS = {2, 3, 9, 14, 0x80, 0x82, 0x83, 0x8E, *range(0x200, 0x400),
                *map(user32.RegisterClipboardFormatW, ("DataObject", "Ole Private Data"))}
# Tells clipboard history (Win+V), cloud sync and clipboard managers to ignore what's on the
# clipboard, so the borrowed link and the put-back copy don't pile up there.
HIDE_FROM_HISTORY = [(user32.RegisterClipboardFormatW(name), bytes(4)) for name in (
    "ExcludeClipboardContentFromMonitorProcessing", "CanIncludeInClipboardHistory",
    "CanUploadToCloudClipboard")]
MY_PID = os.getpid()
WINDOW_TITLE = "7TV"         # a newly started picker finds the running one by it
SETTINGS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "settings.json")

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(1)
except Exception:
    pass


# Each thread keeps its HTTPS connections open between requests. Opening a new
# secure connection costs ~1s, reusing one makes requests about 3x faster.
_conns = threading.local()


def fetch(url, data=None):
    u = urlsplit(url)
    pool = _conns.__dict__.setdefault("pool", {})
    for attempt in (1, 2):
        conn = pool.get(u.netloc)
        if conn is None:
            conn = pool[u.netloc] = http.client.HTTPSConnection(u.netloc, timeout=10)
        try:
            conn.request("POST" if data else "GET", u.path, body=data, headers={
                "User-Agent": "emote-picker/1.0", "Content-Type": "application/json"})
            r = conn.getresponse()
            body = r.read()
        except (http.client.HTTPException, OSError):
            conn.close()
            del pool[u.netloc]     # server dropped the idle connection; retry once
            if attempt == 2:
                raise
            continue
        if r.status != 200:
            raise OSError(f"HTTP {r.status}")
        return body


_search_cache = {}


def search_emotes(text):
    key = text.lower()
    emotes = _search_cache.get(key)
    if emotes is None:
        body = json.dumps({"query": SEARCH_QUERY,
                           "variables": {"q": text, "perPage": PER_PAGE}}).encode()
        data = json.loads(fetch(API_URL, body))
        items = data["data"]["emotes"]["search"]["items"]
        emotes = [(e["id"], e["defaultName"], e["flags"]["animated"]) for e in items]
        if len(_search_cache) >= 200:
            _search_cache.clear()    # the picker runs for days; don't keep every search
        _search_cache[key] = emotes
    return emotes


def load_settings():
    settings = dict(DEFAULT_SETTINGS)
    try:
        with open(SETTINGS_FILE, encoding="utf-8") as f:
            saved = json.load(f)
        if saved.get("size") in LINK_SIZES:
            settings["size"] = saved["size"]
        if saved.get("format") in LINK_FORMATS:
            settings["format"] = saved["format"]
        for key, low in zip(("width", "height"), MIN_SIZE):
            if isinstance(saved.get(key), int) and saved[key] >= low:
                settings[key] = saved[key]
    except (OSError, ValueError, AttributeError):
        pass  # missing or broken file: use defaults
    return settings


def save_settings(settings):
    try:
        with open(SETTINGS_FILE, "w", encoding="utf-8") as f:
            json.dump(settings, f, indent=2)
    except OSError:
        pass


def emote_link(eid, animated, settings):
    fmt = settings["format"]
    # 7TV only has gif for animated emotes and png for still ones; swap so links work.
    if fmt == "gif" and not animated:
        fmt = "png"
    elif fmt == "png" and animated:
        fmt = "gif"
    return LINK_URL.format(id=eid, size=settings["size"], format=fmt)


def window_pid(hwnd):
    pid = ctypes.c_ulong()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


_exe_names = {}


def exe_name(pid):
    """The program a process runs, lowercase, e.g. "discord.exe" ("" if unknown)."""
    if pid not in _exe_names:
        name = ""
        h = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if h:
            buf, size = ctypes.create_unicode_buffer(260), ctypes.c_ulong(260)
            if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
                name = os.path.basename(buf.value).lower()
            kernel32.CloseHandle(h)
        _exe_names[pid] = name
    return _exe_names[pid]


def is_discord(pid):
    return exe_name(pid) in DISCORD_EXES


# The window's visible edges. GetWindowRect would include Windows 10/11's
# invisible resize borders, which would leave a gap.
def window_rect(hwnd):
    r = wintypes.RECT()
    if ctypes.windll.dwmapi.DwmGetWindowAttribute(
            wintypes.HWND(hwnd), 9, ctypes.byref(r), ctypes.sizeof(r)):  # extended frame bounds
        user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def rounded(x1, y1, x2, y2, r):
    """Outline of a rounded rectangle, for create_polygon(..., smooth=True)."""
    return (x1 + r, y1, x1 + r, y1, x2 - r, y1, x2 - r, y1, x2, y1, x2, y1 + r, x2, y1 + r,
            x2, y2 - r, x2, y2 - r, x2, y2, x2 - r, y2, x2 - r, y2, x1 + r, y2, x1 + r, y2,
            x1, y2, x1, y2 - r, x1, y2 - r, x1, y1 + r, x1, y1 + r, x1, y1)


def colorref(color):
    """'#rrggbb' as a Windows COLORREF (0x00bbggrr)."""
    return int(color[5:7] + color[3:5] + color[1:3], 16)


# Tk activates a window when it first shows it, which can take the keyboard focus from
# whatever you're using. While this hook is on (on this thread only, see App), it says no.
@HOOKPROC
def refuse_activation(code, wparam, lparam):
    if code == 5:                    # HCBT_ACTIVATE: nonzero stops it
        return 1
    return hooks.CallNextHookEx(None, code, wparam, lparam)


def open_clipboard(hwnd):
    for _ in range(20):          # another app may have it open for a moment
        if user32.OpenClipboard(hwnd):
            return True
        time.sleep(0.005)
    return False


def clipboard_snapshot(hwnd):
    """Everything on the clipboard as [(format, bytes)], or None if it's busy."""
    if not open_clipboard(hwnd):
        return None
    saved, fmt = [], 0
    try:
        while fmt := user32.EnumClipboardFormats(fmt):
            if fmt in SKIP_FORMATS:
                continue
            h = user32.GetClipboardData(fmt)
            p = h and kernel32.GlobalLock(h)
            if p:
                saved.append((fmt, ctypes.string_at(p, kernel32.GlobalSize(h))))
                kernel32.GlobalUnlock(h)
    finally:
        user32.CloseClipboard()
    return saved


def clipboard_set(hwnd, items, hidden):
    """Replace the clipboard with items [(format, bytes)]; hidden keeps it out of clipboard
    history. Returns the clipboard's new sequence number, or None if it's busy."""
    if not open_clipboard(hwnd):
        return None
    try:
        user32.EmptyClipboard()
        if hidden and items:
            have = {fmt for fmt, _ in items}
            items = items + [m for m in HIDE_FROM_HISTORY if m[0] not in have]
        for fmt, data in items:
            h = kernel32.GlobalAlloc(GMEM_MOVEABLE, max(1, len(data)))
            if h:
                ctypes.memmove(kernel32.GlobalLock(h), data, len(data))
                kernel32.GlobalUnlock(h)
                if not user32.SetClipboardData(fmt, h):
                    kernel32.GlobalFree(h)
    finally:
        user32.CloseClipboard()
    return user32.GetClipboardSequenceNumber()


# ---- animated previews --------------------------------------------------------
# tkinter decodes GIF frames quickly only if they have no transparency (with it, ~40x
# slower), and it can't layer a frame onto the one before. So each frame is decoded with a
# stand-in color table where color i is gray level i, read back as color indexes, layered
# here the way the GIF says, then colored for display: transparent pixels get the color
# behind the emote.

def gif_frames(data):
    """Finds a GIF's frames. Returns (width, height, color table, frames); each frame is
    (start, end, x, y, w, h, delay ms, disposal, transparent index or None), where
    data[start:end] is its image descriptor and pixel data. Raises ValueError (IndexError
    if cut short) for GIFs this can't play: all frames must share one color table, as 7TV's
    do."""
    if data[:6] not in (b"GIF87a", b"GIF89a") or not data[10] & 0x80:
        raise ValueError("not a GIF with a shared color table")
    width, height = struct.unpack_from("<HH", data, 6)
    pos = 13 + 3 * (2 << (data[10] & 7))
    colors, frames, control = data[13:pos], [], None
    while data[pos] != 0x3B:                 # 0x3B ends the GIF
        if data[pos] == 0x21:                # an extension: skip its blocks
            start = pos
            pos += 2
            while data[pos]:
                pos += data[pos] + 1
            pos += 1
            if data[start + 1] == 0xF9 and pos - start >= 8:  # timing, disposal, transparency
                control = data[start + 3:start + 7]
        elif data[pos] == 0x2C:              # a frame
            x, y, w, h, flags = struct.unpack_from("<HHHHB", data, pos + 1)
            if flags & 0x80:
                raise ValueError("a frame with its own color table")
            start = pos
            pos += 11                        # its descriptor and LZW code size
            while data[pos]:
                pos += data[pos] + 1
            pos += 1
            packed, low, high, transparent = control or bytes(4)
            delay = low | high << 8          # hundredths of a second
            # Like browsers, show frames with no real delay for 100ms.
            frames.append((start, pos, x, y, w, h, delay * 10 if delay > 1 else 100,
                           packed >> 2 & 7, transparent if packed & 1 else None))
            control = None
        else:
            raise ValueError("broken GIF")
    return width, height, colors, frames


_gray_colors = {}
_opaque_tables = {}


def opaque_mask(pixels, transparent):
    """255 for each pixel that isn't the transparent color index, 0 for each that is."""
    if transparent not in _opaque_tables:
        _opaque_tables[transparent] = bytes(0 if i == transparent else 255 for i in range(256))
    return pixels.translate(_opaque_tables[transparent])


class AnimatedPreview:
    """An animated emote's GIF: its frames, decoded a few at a time, and its playback.
    Made in a download thread; everything that uses tkinter runs in the main one."""

    def __init__(self, data):
        self.data = data
        self.width, self.height, colors, self.frames = gif_frames(data)
        if len(self.frames) < 2:
            raise ValueError("not animated")
        colors = colors.ljust(768, b"\0")
        self.channels = colors[0::3], colors[1::3], colors[2::3]
        self.ends = list(accumulate(f[6] for f in self.frames))  # when each frame ends, ms
        self.tables = {}             # (background, marker) -> red, green, blue lookup tables
        self.shrink = 1
        self.drop()

    def drop(self):
        """Forgets the decoded frames (they take memory); decoding starts over when needed."""
        self.planes = []             # decoded frames, shrunk to fit: see snapshot()
        self.layers = self.drawn = None  # the frames so far, layered, and where they've drawn
        self.photo = None            # the image in the grid, once every frame is decoded
        self.shown = None            # (frame, background color) in the photo
        self.start = None            # when playback started (ms)

    def decode(self, scratch, shrink, deadline):
        """Decodes frames until all are done or time.perf_counter() passes deadline.
        scratch is a PhotoImage to decode in, reused to save making one per frame."""
        if shrink != self.shrink:
            self.drop()
            self.shrink = shrink
        d, W, H = self.data, self.width, self.height
        bits = d[10] & 7
        if bits not in _gray_colors:
            _gray_colors[bits] = bytes(v for i in range(2 << bits) for v in (i, i, i))
        if self.layers is None:
            self.layers, self.drawn = bytearray(W * H), bytearray(W * H)
        layers, drawn = self.layers, self.drawn   # color index; 255 where something's drawn
        while len(self.planes) < len(self.frames):
            start, end, x, y, w, h, delay, disposal, transparent = self.frames[len(self.planes)]
            # The frame alone, at the top-left, with the gray colors and no transparency.
            gif = (b"GIF89a" + struct.pack("<HHBBB", w, h, 0x80 | bits, 0, 0) + _gray_colors[bits]
                   + struct.pack("<BHHHHB", 0x2C, 0, 0, w, h, d[start + 9] & 0x40)
                   + d[start + 10:end] + b";")
            scratch.configure(data=gif, format="gif")
            pixels = scratch.data(format="ppm")[-3 * w * h::3]
            cw, ch = min(w, W - x), max(0, min(h, H - y))  # the part inside the GIF's area
            if cw == W:                       # whole rows: one stretch of memory
                spans, width = [slice(y * W, (y + ch) * W)], W * ch
            else:
                spans, width = [slice(r * W + x, r * W + x + cw) for r in range(y, y + ch)], cw
            if cw <= 0 or ch <= 0:
                spans = []
            saved = (bytes(layers), bytes(drawn)) if disposal == 3 else None
            if spans:
                # Its pixels go on top, except transparent ones. Bit operations on big
                # integers do a whole frame at once.
                new = (pixels[:ch * w] if cw == w else
                       b"".join(pixels[i * w:i * w + cw] for i in range(ch)))
                mask = b"\xff" * len(new) if transparent is None else opaque_mask(new, transparent)
                m = int.from_bytes(mask, "little")
                old = int.from_bytes(b"".join(map(layers.__getitem__, spans)), "little")
                was = int.from_bytes(b"".join(map(drawn.__getitem__, spans)), "little")
                new = ((int.from_bytes(new, "little") & m) | (old & ~m)).to_bytes(len(mask), "little")
                cover = (m | was).to_bytes(len(mask), "little")
                for i, s in enumerate(spans):
                    layers[s], drawn[s] = new[i * width:(i + 1) * width], cover[i * width:(i + 1) * width]
            self.planes.append(self.snapshot(transparent, shrink))
            if disposal == 2:                 # its area is cleared before the next frame
                for s in spans:
                    drawn[s] = bytes(width)
            elif disposal == 3:               # what was there before it comes back
                layers[:], drawn[:] = saved
            if time.perf_counter() > deadline:
                break
        if len(self.planes) == len(self.frames):
            self.layers = self.drawn = None
            self.size = (-(-W // shrink), -(-H // shrink))
            self.photo = tk.PhotoImage(width=self.size[0], height=self.size[1])

    def snapshot(self, hint, shrink):
        """The picture so far, as (color indexes, marker): pixels nothing is drawn on get the
        marker, an index no drawn pixel uses (None if everything is drawn). Each frame picks
        its own, since a GIF's frames can use any of its colors; the last frame's marker and
        then hint are tried first."""
        layers, drawn = self.layers, self.drawn
        blank = drawn.count(0)
        plane, marker = bytes(layers), None
        if blank:
            n, cover = len(layers), int.from_bytes(drawn, "little")
            colored = int.from_bytes(layers, "little") & cover
            last = self.planes[-1][1] if self.planes else None
            for marker in (*(m for m in (last, hint) if m is not None), *range(256)):
                fill = int.from_bytes(bytes([marker]) * n, "little") & ~cover
                plane = (colored | fill).to_bytes(n, "little")
                if plane.count(marker) == blank:
                    break
        if shrink > 1:
            W = self.width
            plane = b"".join(plane[r * W:(r + 1) * W:shrink] for r in range(0, self.height, shrink))
        return plane, marker

    def show(self, now, background):
        """Shows the frame due at time now (ms), with background ('#rrggbb') behind it."""
        if self.start is None:
            self.start = now
        self.paint(bisect.bisect_right(self.ends, (now - self.start) % self.ends[-1]),
                   background)

    def paint(self, frame, background):
        if (frame, background) == self.shown:
            return
        plane, marker = self.planes[frame]
        if (background, marker) not in self.tables:
            r, g, b = (bytearray(c) for c in self.channels)
            if marker is not None:
                r[marker], g[marker], b[marker] = bytes.fromhex(background[1:])
            self.tables[(background, marker)] = bytes(r), bytes(g), bytes(b)
        red, green, blue = self.tables[(background, marker)]
        rgb = bytearray(3 * len(plane))
        rgb[0::3], rgb[1::3], rgb[2::3] = (plane.translate(red), plane.translate(green),
                                         plane.translate(blue))
        self.photo.tk.call(self.photo.name, "put", b"P6 %d %d 255\n" % self.size + rgb,
                           "-format", "ppm")
        self.shown = (frame, background)


class App:
    def __init__(self, root):
        self.root = root
        self.target_hwnd = None      # the window the user was typing in
        self.discord_hwnd = None     # Discord's window while it's focused, else None
        self.dock_pos = (0, 0, 0, 0)  # (x, y, w, h) last placed at, or None while hidden
        self.drag = None
        self.dpi_scale = 1.0         # Discord's display scaling, e.g. 2.0 at 200%
        self.raw = {}                # (emote id, size) -> downloaded png bytes
        self.photos = {}             # (emote id, size, shrink) -> PhotoImage
        self.emotes = []             # current results: (id, name, animated)
        self.index = {}              # emote id -> its place in self.emotes
        self.items = []              # canvas image item of each emote, same order
        self.cols, self.cell, self.x0 = 1, 44, 0  # grid columns, cell size, left margin (px)
        self.hover_i = None          # index of the emote under the mouse
        self.view = None             # (top, bottom) of the grid in view, as fractions
        self.applied_k = None        # display scaling the fonts and spacing are sized for
        self.thumb_s = 1             # which 7TV size (1x-4x) previews use right now
        self.started = set()         # emotes whose preview is showing or on its way
        self.fetching = set()        # (emote id, size) previews downloading right now
        self.anims = {}              # (emote id, size) -> AnimatedPreview; None if it can't play
        self.anim_job = None         # pending animate() call
        self.scratch = tk.PhotoImage(master=root)  # GIF frames are decoded in it
        self.layout_job = None
        self.results_q = queue.Queue()
        self.pool = ThreadPoolExecutor(max_workers=PREVIEW_DOWNLOADS)
        self.search_gen = 0
        self.search_text = ""        # what the latest search was for
        self.debounce = None
        self.status_text = ""        # status line while the mouse isn't over an emote
        self.empty_note = ""         # shown in the grid when a search finds nothing
        self.flash_job = None
        self.settings = load_settings()
        self.clip_saved = None       # what you had copied, while an emote link borrows the clipboard
        self.clip_seq = 0            # clipboard sequence number right after the link went on it
        self.restore_job = None      # pending restore_clipboard() call

        root.title(WINDOW_TITLE)
        root.overrideredirect(True)  # no title bar or border; we place it ourselves
        # Starts offscreen, so it doesn't flash in the corner: dock() moves it over Discord.
        root.geometry(f"{self.settings['width']}x{self.settings['height']}+-32000+-32000")
        root.attributes("-topmost", True)
        root.configure(bg=BG)
        families = set(tkfont.families())
        text_font = next((f for f in ("Segoe UI Variable Text", "Segoe UI") if f in families),
                         "TkDefaultFont")
        icon_font = next((f for f in ("Segoe Fluent Icons", "Segoe MDL2 Assets")
                          if f in families), None)
        # Shared fonts, sized in pixels by apply_scale() for Discord's display scaling.
        self.font_px = {"text": 14, "small": 12, "icon": 14}
        self.fonts = {name: tkfont.Font(family=icon_font if name == "icon" and icon_font
                                        else text_font, size=-size)
                      for name, size in self.font_px.items()}
        self.glyphs = ({"search": "\ue721", "gear": "\ue713", "close": "\ue711"} if icon_font
                       else {"search": "⌕", "gear": "⚙", "close": "✕"})

        # Top row, drawn on a canvas by draw_header(): resize grip in the top-left corner,
        # rounded search box, settings and close buttons.
        self.header = h = tk.Canvas(root, bg=BG, highlightthickness=0, height=1)
        h.pack(fill="x")
        self.query = tk.StringVar()
        self.entry = tk.Entry(h, textvariable=self.query, bg=FIELD, fg=TEXT,
                              insertbackground=TEXT, selectbackground="#5865f2",
                              selectforeground="white", relief="flat", bd=0,
                              highlightthickness=0, font=self.fonts["text"], width=1)
        self.placeholder = tk.Label(h, text="Search 7TV emotes", bg=FIELD, fg=FAINT, bd=0,
                                    padx=0, pady=0, font=self.fonts["text"], cursor="xterm")
        self.entry_item = h.create_window(0, 0, window=self.entry, anchor="w")
        self.placeholder_item = h.create_window(0, 0, window=self.placeholder, anchor="w")
        self.entry.focus_set()
        self.query.trace_add("write", lambda *_: self.query_changed())
        self.entry.bind("<Return>", lambda e: self.search_now())
        self.placeholder.bind("<Button-1>", lambda e: self.entry.focus_set())
        h.tag_bind("field", "<Button-1>", lambda e: self.entry.focus_set())
        for name, action in (("gear", self.toggle_settings), ("close", root.destroy)):
            h.tag_bind(name, "<Enter>", lambda e, n=name: self.paint_buttons(n))
            h.tag_bind(name, "<Leave>", lambda e: self.paint_buttons())
            h.tag_bind(name, "<Button-1>", lambda e, a=action: a())
        h.bind("<Motion>", self.header_cursor)
        h.bind("<Leave>", lambda e: h.itemconfigure("grip", fill=GRIP))
        h.bind("<Button-1>", lambda e: self.start_resize(e) if self.on_grip(e) else None)
        h.bind("<B1-Motion>", lambda e: self.do_resize(e) if self.drag else None)
        h.bind("<ButtonRelease-1>", lambda e: self.end_resize())
        h.bind("<Configure>", lambda e: self.draw_header())
        self.build_settings_panel()

        self.status = tk.Label(root, bg=BG, fg=MUTED, font=self.fonts["small"], anchor="w")
        self.status.pack(side="bottom", fill="x")

        # Emotes are drawn straight onto a canvas: quick to redraw, and hover can be rounded.
        self.wrap = tk.Frame(root, bg=BG)
        self.wrap.pack(fill="both", expand=True)
        style = ttk.Style(root)
        style.theme_use("clam")      # the built-in theme whose scrollbar can be recolored
        # A slim scrollbar with no arrows; "Hidden" is the same one, invisible, for when
        # everything fits.
        for name, thumb, active in (("Slim", SCROLL_THUMB, SCROLL_THUMB_ACTIVE),
                                    ("Hidden", BG, BG)):
            style.layout(f"{name}.Vertical.TScrollbar", [("Vertical.Scrollbar.trough", {
                "sticky": "ns", "children": [("Vertical.Scrollbar.thumb",
                                              {"expand": "1", "sticky": "nswe"})]})])
            style.configure(f"{name}.Vertical.TScrollbar", troughcolor=BG, bordercolor=BG,
                            background=thumb, lightcolor=thumb, darkcolor=thumb,
                            gripcount=0, relief="flat")
            style.map(f"{name}.Vertical.TScrollbar", **{
                option: [("pressed", active), ("active", active)]
                for option in ("background", "lightcolor", "darkcolor")})
        self.canvas = tk.Canvas(self.wrap, bg=BG, highlightthickness=0)
        self.bar = ttk.Scrollbar(self.wrap, orient="vertical", style="Hidden.Vertical.TScrollbar",
                                 command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.scrolled)
        self.bar.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)
        self.highlight = self.canvas.create_polygon(rounded(0, 0, 1, 1, 0), smooth=True,
                                                    fill=HOVER, state="hidden")
        self.canvas.bind("<Motion>", lambda e: self.hover_at(e.x, e.y))
        self.canvas.bind("<Leave>", lambda e: self.hover_at(None, None))
        self.canvas.bind("<Button-1>", self.clicked)
        root.bind_all("<MouseWheel>",
                      lambda e: self.canvas.yview_scroll(-e.delta // 120, "units"))
        self.canvas.bind("<Configure>", lambda e: self.schedule_layout())

        # Map the window so its outer handle exists, without it taking the focus: Tk
        # activates windows it shows, and sometimes Windows lets a new program take the focus
        # (after a double-click, or when you haven't touched anything for a while). A hook on
        # this thread (WH_CBT) refuses, and whatever had the focus gets it back.
        previous = user32.GetForegroundWindow()
        hook = hooks.SetWindowsHookExW(5, refuse_activation, None, kernel32.GetCurrentThreadId())
        try:
            root.update_idletasks()
        finally:
            if hook:
                hooks.UnhookWindowsHookEx(hook)
        now = user32.GetForegroundWindow()
        if previous and now != previous and (not now or window_pid(now) == MY_PID):
            user32.SetForegroundWindow(previous)
        self.hwnd = user32.GetParent(root.winfo_id())
        # Windows 11: rounded corners and a thin dark border, like Discord's own popups.
        for attr, value in ((33, ctypes.c_int(2)), (34, wintypes.DWORD(colorref(BORDER)))):
            try:
                ctypes.windll.dwmapi.DwmSetWindowAttribute(
                    wintypes.HWND(self.hwnd), attr, ctypes.byref(value), ctypes.sizeof(value))
            except (OSError, AttributeError):
                pass
        self.apply_scale()
        self.track_foreground()
        self.drain_queue()
        self.search_now()  # show popular emotes on start

    def px(self, v):
        """v pixels at 100% display scaling, in pixels at Discord's scaling."""
        return max(1, round(v * self.dpi_scale))

    # Size fonts, spacing and the scrollbar for Discord's display scaling.
    def apply_scale(self):
        self.applied_k = self.dpi_scale
        px = self.px
        for name, font in self.fonts.items():
            font.configure(size=-px(self.font_px[name]))
        self.header.configure(height=px(PAD + FIELD_H))
        self.wrap.pack_configure(padx=(px(PAD), px(3)), pady=(px(6), 0))
        self.status.pack_configure(padx=px(PAD + 4), pady=(px(4), px(7)))
        for name in ("Slim", "Hidden"):
            ttk.Style(self.root).configure(f"{name}.Vertical.TScrollbar", arrowsize=px(8))
        for var, option, label in self.segments:
            label.configure(padx=px(9), pady=px(3))
        for widget in self.panel.winfo_children()[1:]:
            widget.pack_configure(padx=(px(8), 0))
        if self.panel.winfo_manager():
            self.panel.pack_configure(padx=px(PAD + 4), pady=(px(6), 0))
        self.draw_header()

    def draw_header(self):
        h, px = self.header, self.px
        h.delete("deco")
        pad, fh = px(PAD), px(FIELD_H)
        mid = pad + fh / 2
        # Resize grip: the panel hangs from Discord's bottom-right corner, so it grows from
        # its top-left one.
        for d in (4, 8, 12):
            h.create_line(px(3), px(3 + d), px(3 + d), px(3), fill=GRIP, width=px(1.3),
                          tags=("deco", "grip"))
        close_x = h.winfo_width() - pad - fh / 2
        gear_x = close_x - fh - px(4)
        left, right = px(24), gear_x - fh / 2 - px(6)
        h.create_polygon(rounded(left, pad, right, pad + fh, px(8)), smooth=True, fill=FIELD,
                         tags=("deco", "field"))
        h.create_text(left + px(16), mid, text=self.glyphs["search"], font=self.fonts["icon"],
                      fill=FAINT, tags=("deco", "field"))
        x = left + px(32)
        h.coords(self.entry_item, x, mid)
        h.itemconfigure(self.entry_item, width=max(1, right - px(10) - x), height=fh - px(8))
        h.coords(self.placeholder_item, x + px(3), mid)   # leaves the caret visible
        for name, cx in (("gear", gear_x), ("close", close_x)):
            h.create_polygon(rounded(cx - fh / 2, pad, cx + fh / 2, pad + fh, px(6)),
                             smooth=True, fill=BG, tags=("deco", name, name + "-bg"))
            h.create_text(cx, mid, text=self.glyphs[name], font=self.fonts["icon"],
                          fill=MUTED, tags=("deco", name, name + "-fg"))
        self.paint_buttons()
        self.update_placeholder()

    # Buttons light up under the mouse; the gear stays lit while the settings row is open.
    def paint_buttons(self, hovered=None):
        settings_open = bool(self.panel.winfo_manager())
        for name in ("gear", "close"):
            bg = (HOVER if name == hovered else
                  SELECTED if name == "gear" and settings_open else BG)
            self.header.itemconfigure(name + "-bg", fill=bg)
            self.header.itemconfigure(name + "-fg", fill=MUTED if bg == BG else TEXT)

    def header_cursor(self, e):
        tags, grip = self.header.gettags("current"), self.on_grip(e)
        cursor = ("size_nw_se" if grip else
                  "hand2" if "gear" in tags or "close" in tags else "")
        if self.header.cget("cursor") != cursor:
            self.header.configure(cursor=cursor)
            self.header.itemconfigure("grip", fill=FAINT if grip else GRIP)

    def on_grip(self, e):
        return e.x < self.px(22) and e.y < self.px(22)

    def query_changed(self):
        self.update_placeholder()
        self.schedule_search()

    def update_placeholder(self):
        self.header.itemconfigure(self.placeholder_item,
                                  state="hidden" if self.query.get() else "normal")

    def say(self, text):
        """Set the status line, shown whenever the mouse isn't over an emote."""
        self.status_text = text
        self.show_status()

    def flash(self, text):
        """Show text on the status line for a moment."""
        self.status.configure(text=text, fg=TEXT)
        if self.flash_job:
            self.root.after_cancel(self.flash_job)
        self.flash_job = self.root.after(1500, self.show_status)

    def show_status(self):
        if self.hover_i is not None:
            self.status.configure(text=self.emotes[self.hover_i][1], fg=TEXT)
        else:
            self.status.configure(text=self.status_text, fg=MUTED)

    # Hidden row under the search box for choosing the pasted link's size and format.
    def build_settings_panel(self):
        self.panel = tk.Frame(self.root, bg=BG)
        tk.Label(self.panel, text="Paste as", bg=BG, fg=MUTED,
                 font=self.fonts["small"]).pack(side="left")
        self.size_var = tk.StringVar(value=self.settings["size"])
        self.format_var = tk.StringVar(value=self.settings["format"])
        self.segments = []           # (variable, option, label) for each toggle button
        for var, options in ((self.size_var, LINK_SIZES), (self.format_var, LINK_FORMATS)):
            group = tk.Frame(self.panel, bg=FIELD)
            group.pack(side="left")
            for option in options:
                label = tk.Label(group, text=option, font=self.fonts["small"], bd=0,
                                 cursor="hand2")
                label.pack(side="left")
                label.bind("<Button-1>", lambda e, v=var, o=option: v.set(o))
                label.bind("<Enter>", lambda e, w=label: w.configure(fg=TEXT))
                label.bind("<Leave>", lambda e: self.paint_segments())
                self.segments.append((var, option, label))
            var.trace_add("write", lambda *_: self.settings_changed())
        self.format_note = tk.Label(self.panel, bg=BG, fg=FAINT, font=self.fonts["small"])
        self.format_note.pack(side="left")
        self.update_format_note()
        self.paint_segments()

    def paint_segments(self):
        for var, option, label in self.segments:
            on = var.get() == option
            label.configure(bg=SELECTED if on else FIELD, fg=TEXT if on else MUTED)

    def toggle_settings(self):
        if self.panel.winfo_manager():
            self.panel.pack_forget()
        else:
            self.panel.pack(fill="x", padx=self.px(PAD + 4), pady=(self.px(6), 0),
                            after=self.header)
        self.paint_buttons("gear")

    def settings_changed(self):
        self.settings.update(size=self.size_var.get(), format=self.format_var.get())
        save_settings(self.settings)
        self.update_format_note()
        self.paint_segments()

    def update_format_note(self):
        note = {"gif": "png for still emotes",
                "png": "gif for animated ones"}.get(self.settings["format"], "")
        self.format_note.configure(text=note)

    # Remember the last window that had focus that isn't us, and follow Discord.
    def track_foreground(self):
        hwnd = user32.GetForegroundWindow()
        pid = window_pid(hwnd) if hwnd else 0
        if pid != MY_PID:            # while you're typing in the picker, keep the old state
            if hwnd:
                self.target_hwnd = hwnd
            self.discord_hwnd = (hwnd if hwnd and is_discord(pid)
                                 and not user32.IsIconic(hwnd) else None)
        self.dock()
        self.root.after(FOLLOW_MS, self.track_foreground)

    # Pin the panel to Discord's bottom-right corner, or hide it when Discord isn't focused.
    # Uses SetWindowPos with NOACTIVATE so showing it never takes focus away from Discord.
    def dock(self):
        if not self.discord_hwnd:
            if self.dock_pos:
                user32.SetWindowPos(self.hwnd, None, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE
                                    | SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW)
                self.dock_pos = None
            return
        left, top, right, bottom = window_rect(self.discord_hwnd)
        k = self.dpi_scale = (user32.GetDpiForWindow(self.discord_hwnd) or 96) / 96
        if k != self.applied_k and not self.layout_job:
            self.schedule_layout()   # different display scaling: resize fonts and spacing
        mr, mb = round(DOCK_MARGIN_RIGHT * k), round(DOCK_MARGIN_BOTTOM * k)
        w = max(1, min(round(self.settings["width"] * k), right - left - mr))
        h = max(1, min(round(self.settings["height"] * k), bottom - top - mb))
        pos = (right - mr - w, bottom - mb - h, w, h)
        if pos != self.dock_pos:
            if self.dock_pos is None:
                self.start_animation()   # showing again: animated emotes move again
            user32.SetWindowPos(self.hwnd, HWND_TOPMOST, *pos,
                                SWP_NOACTIVATE | SWP_SHOWWINDOW)
            self.dock_pos = pos

    def start_resize(self, e):
        self.drag = (e.x_root, e.y_root, self.root.winfo_width(), self.root.winfo_height())

    def do_resize(self, e):
        x0, y0, w0, h0 = self.drag
        k = self.dpi_scale
        self.settings["width"] = max(MIN_SIZE[0], round((w0 - (e.x_root - x0)) / k))
        self.settings["height"] = max(MIN_SIZE[1], round((h0 - (e.y_root - y0)) / k))
        self.dock()

    def end_resize(self):
        if self.drag:
            self.drag = None
            save_settings(self.settings)

    def schedule_search(self):
        if self.debounce:
            self.root.after_cancel(self.debounce)
        self.debounce = self.root.after(SEARCH_DELAY_MS, self.search_now)

    def search_now(self):
        self.search_gen += 1
        gen, text = self.search_gen, self.query.get().strip()
        self.search_text = text
        self.say("Searching…")

        def work():
            try:
                self.results_q.put(("results", gen, search_emotes(text)))
            except Exception as ex:
                self.results_q.put(("error", gen, str(ex)))
        threading.Thread(target=work, daemon=True).start()

    # Tk isn't thread safe: worker threads put results in a queue, we read it here.
    def drain_queue(self):
        try:
            while True:
                kind, gen, payload = self.results_q.get_nowait()
                if kind == "thumb":
                    self.got_thumb(*payload)
                elif kind == "gif":
                    self.got_gif(*payload)
                elif gen != self.search_gen:
                    continue  # stale search
                elif kind == "results":
                    self.show_results(payload)
                else:
                    self.say(f"Search failed: {payload}")
        except queue.Empty:
            pass
        self.root.after(30, self.drain_queue)

    def show_results(self, emotes):
        # Only keep preview images for these results; the picker runs for days and old ones
        # would pile up. Downloads are kept longer, so going back to a search is quick.
        shown = {eid for eid, name, animated in emotes}
        self.photos = {k: p for k, p in self.photos.items() if k[0] in shown}
        self.anims = {k: a for k, a in self.anims.items() if k[0] in shown}
        if len(self.raw) > MAX_SAVED_PREVIEWS:
            self.raw = {k: d for k, d in self.raw.items() if k[0] in shown}
        self.emotes, self.hover_i = emotes, None
        self.index = {eid: i for i, (eid, name, animated) in enumerate(emotes)}
        q = self.search_text
        self.empty_note = ("" if emotes else
                           f"No emotes found for “{q}”" if q else "No emotes found")
        self.say("" if not emotes else
                 f"{len(emotes)} results for “{q}”" if q else "Popular on 7TV")
        self.canvas.yview_moveto(0)
        self.draw_grid()

    def schedule_layout(self):
        if self.layout_job:
            self.root.after_cancel(self.layout_job)
        self.layout_job = self.root.after(100, self.layout)

    # Emotes keep one size; a wider panel fits more of them in each row.
    def layout(self):
        self.layout_job = None
        if self.applied_k != self.dpi_scale:
            self.apply_scale()
        # The 7TV preview size (1x-4x = 32-128px tall) closest to EMOTE_SIZE, shown unshrunk.
        want = EMOTE_SIZE * self.dpi_scale
        self.thumb_s = min(range(1, 5), key=lambda s: (abs(s * 32 - want), -s))
        width = self.canvas.winfo_width()
        self.cols = max(1, width // (self.thumb_s * 32 + 2 * self.px(CELL_PAD)))
        self.cell = max(1, width // self.cols)
        self.x0 = (width - self.cols * self.cell) // 2
        self.canvas.configure(yscrollincrement=self.cell)   # mouse wheel: a row per notch
        self.draw_grid()

    # A cell for every result; previews fill in as they load (load_previews).
    def draw_grid(self):
        c, cell, cols = self.canvas, self.cell, self.cols
        c.delete("cell")
        self.items = [c.create_image(self.x0 + (i % cols + 0.5) * cell,
                                     (i // cols + 0.5) * cell, tags="cell")
                      for i in range(len(self.emotes))]
        if self.empty_note:
            c.create_text(c.winfo_width() / 2, self.px(40), text=self.empty_note, fill=FAINT,
                          font=self.fonts["text"], tags="cell")
        c.configure(scrollregion=(0, 0, c.winfo_width(),
                                  -(-len(self.emotes) // cols) * cell))
        self.hover_i = None
        c.itemconfigure(self.highlight, state="hidden")
        self.show_status()
        self.started = set()         # redo the previews in view, at the current size
        self.load_previews()
        self.start_animation()

    # The canvas calls this whenever what's in view changes (scrolling, new results).
    def scrolled(self, first, last):
        self.bar.set(first, last)
        fits = float(first) <= 0 and float(last) >= 1
        style = ("Hidden" if fits else "Slim") + ".Vertical.TScrollbar"
        if self.bar.cget("style") != style:
            self.bar.configure(style=style)
        if (first, last) != self.view:
            self.view = (first, last)
            if self.hover_i is not None:  # the grid moved: a different emote is under the mouse
                self.hover_at(self.root.winfo_pointerx() - self.canvas.winfo_rootx(),
                              self.root.winfo_pointery() - self.canvas.winfo_rooty())
            self.forget_far_animations()
            self.start_animation()
        self.load_previews()

    def on_screen(self):
        """Indexes of the emotes in view."""
        top = int(self.canvas.canvasy(0))
        bottom = top + self.canvas.winfo_height()
        return range(min(len(self.emotes), top // self.cell * self.cols),
                     min(len(self.emotes), ((bottom - 1) // self.cell + 1) * self.cols))

    def cell_at(self, x, y):
        """Index of the emote at x, y (pixels in the canvas window), or None."""
        c = self.canvas
        if x is None or not (0 <= x < c.winfo_width() and 0 <= y < c.winfo_height()):
            return None
        x, y = c.canvasx(x) - self.x0, c.canvasy(y)
        col, row = int(x // self.cell), int(y // self.cell)
        i = row * self.cols + col
        return i if x >= 0 and col < self.cols and i < len(self.emotes) else None

    # Rounded highlight behind the emote under the mouse, and its name on the status line.
    def hover_at(self, x, y):
        i = self.cell_at(x, y)
        if i == self.hover_i:
            return
        old, self.hover_i = self.hover_i, i
        c = self.canvas
        if i is None:
            c.itemconfigure(self.highlight, state="hidden")
        else:
            x1, y1 = self.x0 + i % self.cols * self.cell, i // self.cols * self.cell
            inset = self.px(2)
            c.coords(self.highlight, *rounded(x1 + inset, y1 + inset, x1 + self.cell - inset,
                                              y1 + self.cell - inset, self.px(8)))
            c.itemconfigure(self.highlight, state="normal")
        # A moving emote's frames have the color behind it painted in: repaint these two.
        for j, color in ((old, BG), (i, HOVER)):
            anim = self.anim_at(j)
            if anim and anim.shown:
                anim.paint(anim.shown[0], color)
        cursor = "" if i is None else "hand2"
        if c.cget("cursor") != cursor:
            c.configure(cursor=cursor)
        self.show_status()

    def clicked(self, e):
        i = self.cell_at(e.x, e.y)
        if i is not None:
            eid, name, animated = self.emotes[i]
            self.paste(eid, animated)

    # Previews download for the rows on screen, then the GIFs of the animated ones there,
    # then the next screen down; the rest wait until you scroll to them. Only
    # PREVIEW_DOWNLOADS run at once, and whenever one finishes this runs again, so whatever
    # is in view by then goes next.
    def load_previews(self):
        screen = self.on_screen()
        bottom = int(self.canvas.canvasy(0)) + 2 * self.canvas.winfo_height()
        self.load_stills(screen)
        for i in screen if ANIMATE else ():
            eid, name, animated = self.emotes[i]
            key = (eid, self.thumb_s, "gif")
            if (animated and (eid, self.thumb_s) not in self.anims and key not in self.fetching
                    and len(self.fetching) < PREVIEW_DOWNLOADS):
                self.fetching.add(key)
                self.pool.submit(self.fetch_gif, eid, self.thumb_s)
        self.load_stills(range(screen.stop,
                               min(len(self.emotes), (bottom // self.cell + 1) * self.cols)))

    def load_stills(self, indexes):
        for i in indexes:
            eid, name, animated = self.emotes[i]
            key = (eid, self.thumb_s)
            if eid in self.started:
                continue
            if key in self.raw:
                self.set_thumb(eid, self.thumb_s)
            elif key not in self.fetching:
                if len(self.fetching) >= PREVIEW_DOWNLOADS:
                    continue         # its turn comes when a download finishes
                self.fetching.add(key)
                self.pool.submit(self.fetch_thumb, eid, animated, self.thumb_s)
            self.started.add(eid)

    # Runs in a download thread. Always reports back, even on failure, so the slot frees up.
    def fetch_thumb(self, eid, animated, size):
        url = (THUMB_ANIMATED if animated else THUMB_STATIC).format(id=eid, s=size)
        try:
            data = fetch(url)
        except Exception:
            data = None              # that cell stays blank
        self.results_q.put(("thumb", 0, (eid, size, data)))

    def got_thumb(self, eid, size, data):
        self.fetching.discard((eid, size))
        if data is not None:
            self.raw[(eid, size)] = data
            if size == self.thumb_s:
                self.set_thumb(eid, size)
        self.load_previews()         # a download slot is free

    def set_thumb(self, eid, size):
        i = self.index.get(eid)
        if i is None or i >= len(self.items):
            return
        anim = self.anims.get((eid, size))
        if anim and anim.photo is not None:   # it's moving: keep that
            self.canvas.itemconfigure(self.items[i], image=anim.photo)
            return
        if (eid, size, 1) not in self.photos:
            try:
                self.photos[(eid, size, 1)] = tk.PhotoImage(data=self.raw[(eid, size)])
            except tk.TclError:
                return
        full = self.photos[(eid, size, 1)]
        shrink = self.shrink_for(full.width(), full.height())
        if (eid, size, shrink) not in self.photos:
            self.photos[(eid, size, shrink)] = full.subsample(shrink)
        self.canvas.itemconfigure(self.items[i], image=self.photos[(eid, size, shrink)])

    def shrink_for(self, width, height):
        """How much a preview has to shrink to fit its cell. tkinter can only shrink by
        whole numbers; only extra-wide emotes need it, as the size was picked to fit."""
        room = max(1, self.cell - 2 * self.px(3))
        return max(1, -(-height // room), -(-width // room))

    # Runs in a download thread. Always reports back, even on failure (None), so the slot
    # frees up and that emote isn't tried again.
    def fetch_gif(self, eid, size):
        try:
            anim = AnimatedPreview(fetch(THUMB_GIF.format(id=eid, s=size)))
        except Exception:
            anim = None              # it stays still
        self.results_q.put(("gif", 0, (eid, size, anim)))

    def got_gif(self, eid, size, anim):
        self.fetching.discard((eid, size, "gif"))
        if eid in self.index:        # still in the results
            self.anims[(eid, size)] = anim
        # GIFs are big: past MAX_GIF_MB, forget the ones off screen (they download again if
        # you scroll back to them).
        if sum(len(a.data) for a in self.anims.values() if a) > MAX_GIF_MB << 20:
            keep = {(self.emotes[i][0], self.thumb_s) for i in self.on_screen()}
            for key in [k for k in self.anims if k not in keep]:
                self.stop_playing(*key)
                del self.anims[key]
        self.start_animation()
        self.load_previews()         # a download slot is free

    def anim_at(self, i):
        """Emote i's animated preview, if it's moving."""
        if i is None or i >= len(self.emotes):
            return None
        anim = self.anims.get((self.emotes[i][0], self.thumb_s))
        return anim if anim and anim.photo is not None else None

    def start_animation(self):
        if ANIMATE and not self.anim_job:
            self.anim_job = self.root.after(FRAME_MS, self.animate)

    # Moves the animated emotes on screen along, and decodes more of their frames, for at
    # most DECODE_MS each time. Keeps going while any are on screen and the panel shows.
    def animate(self):
        self.anim_job = None
        if self.dock_pos is None:
            return                   # hidden: dock() starts it again
        now = time.perf_counter()
        deadline = now + DECODE_MS / 1000
        playing = False
        for i in self.on_screen():
            eid = self.emotes[i][0]
            anim = self.anims.get((eid, self.thumb_s))
            if not anim:
                continue
            playing = True
            shrink = self.shrink_for(anim.width, anim.height)
            if anim.photo is not None and anim.shrink != shrink:
                self.stop_playing(eid, self.thumb_s)  # the cells changed size: decode again
            if anim.photo is None:
                if time.perf_counter() > deadline:
                    continue         # next time
                try:
                    anim.decode(self.scratch, shrink, deadline)
                except (tk.TclError, ValueError, IndexError):
                    self.anims[(eid, self.thumb_s)] = None   # can't play it: it stays still
                    continue
                if anim.photo is None:
                    continue
                self.canvas.itemconfigure(self.items[i], image=anim.photo)
            anim.show(now * 1000, HOVER if i == self.hover_i else BG)
        if playing:
            self.anim_job = self.root.after(FRAME_MS, self.animate)

    def stop_playing(self, eid, size):
        """Frees an animated emote's decoded frames and puts its still preview back."""
        anim = self.anims.get((eid, size))
        if not anim or not (anim.planes or anim.layers):
            return
        i = self.index.get(eid)
        mine = size == self.thumb_s and i is not None and i < len(self.items)
        if mine and anim.photo is not None:
            self.canvas.itemconfigure(self.items[i], image="")
        anim.drop()
        if mine and (eid, size) in self.raw:
            self.set_thumb(eid, size)

    def forget_far_animations(self):
        """Frees the decoded frames of animated emotes more than a screen away."""
        top, height = int(self.canvas.canvasy(0)), self.canvas.winfo_height()
        near = range((top - height) // self.cell * self.cols,
                     ((top + 2 * height) // self.cell + 1) * self.cols)
        for eid, size in list(self.anims):
            i = self.index.get(eid)
            if size != self.thumb_s or i is None or i not in near:
                self.stop_playing(eid, size)

    def paste(self, eid, animated):
        link = emote_link(eid, animated, self.settings) + (" " if ADD_SPACE_AFTER else "")
        link_data = [(CF_UNICODETEXT, (link + "\0").encode("utf-16-le"))]
        if not self.target_hwnd:
            clipboard_set(self.hwnd, link_data, hidden=False)
            self.flash("Copied (no target window yet)")
            return
        # Save what you had copied, then lend the clipboard to the link. If an earlier paste
        # is still borrowing it, keep that one's saved copy: it's the one to put back.
        if not self.restore_job:
            self.clip_saved = clipboard_snapshot(self.hwnd)
        seq = clipboard_set(self.hwnd, link_data, hidden=True)
        if seq is None:
            self.flash("Clipboard busy, click again")
            return
        self.clip_seq = seq
        if self.restore_job:
            self.root.after_cancel(self.restore_job)
        if not user32.SetForegroundWindow(self.target_hwnd):
            # Windows sometimes refuses focus switches; any key we send unlocks it.
            # (F24 rather than Alt: a lone Alt tap opens menu mode and eats Ctrl+V.)
            user32.keybd_event(VK_F24, 0, 0, 0)
            user32.keybd_event(VK_F24, 0, KEYUP, 0)
            user32.SetForegroundWindow(self.target_hwnd)
        self.flash("Pasted!")
        self.root.after(PASTE_DELAY_MS, self.send_ctrl_v)
        self.restore_job = self.root.after(PASTE_DELAY_MS + RESTORE_CLIPBOARD_MS,
                                           self.restore_clipboard)

    # Put back what you had copied before the paste, unless you've copied something new since.
    def restore_clipboard(self):
        self.restore_job = None
        if self.clip_saved is not None and user32.GetClipboardSequenceNumber() == self.clip_seq:
            # Hidden: it's already in clipboard history from when you copied it.
            if clipboard_set(self.hwnd, self.clip_saved, hidden=True) is None:
                self.restore_job = self.root.after(100, self.restore_clipboard)  # busy: retry
                return
        self.clip_saved = None

    @staticmethod
    def send_ctrl_v():
        user32.keybd_event(VK_CONTROL, 0, 0, 0)
        user32.keybd_event(VK_V, 0, 0, 0)
        user32.keybd_event(VK_V, 0, KEYUP, 0)
        user32.keybd_event(VK_CONTROL, 0, KEYUP, 0)


def close_running_picker():
    """Close any picker that's already running, as if its X was clicked, and wait for it
    to exit. One that's stuck gets ended after 3 seconds."""
    running, hwnd = [], None
    while hwnd := user32.FindWindowExW(None, hwnd, "TkTopLevel", WINDOW_TITLE):
        pid = window_pid(hwnd)
        if pid != MY_PID and exe_name(pid).startswith("python"):
            running.append((hwnd, pid))
    for hwnd, pid in running:
        process = kernel32.OpenProcess(0x00100001, False, pid)  # SYNCHRONIZE | PROCESS_TERMINATE
        user32.PostMessageW(hwnd, 0x0010, 0, 0)                  # WM_CLOSE
        if process:
            if kernel32.WaitForSingleObject(process, 3000):      # 0 means it exited
                kernel32.TerminateProcess(process, 1)
                kernel32.WaitForSingleObject(process, 2000)
            kernel32.CloseHandle(process)


if __name__ == "__main__":
    # One picker at a time: it also starts with Windows, and starting it again (say, after
    # changing this file) should give you the new version, not a second panel or nothing.
    close_running_picker()
    root = tk.Tk()
    App(root)
    root.mainloop()
