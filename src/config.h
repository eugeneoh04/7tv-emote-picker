// Things you might want to change. Rebuild with build.cmd afterwards.
#pragma once

#include <cstdint>

// The pasted link's size and format are picked with the gear button in the app and saved to
// settings.json next to the program, with the panel size.
inline constexpr const wchar_t* LINK_SIZES[] = {L"1x", L"2x", L"3x", L"4x"};
inline constexpr const wchar_t* LINK_FORMATS[] = {L"webp", L"avif", L"gif", L"png"};
inline constexpr const wchar_t* DEFAULT_SIZE = L"4x";
inline constexpr const wchar_t* DEFAULT_FORMAT = L"webp";
// Panel size at 100% display scaling; drag the corner grip to change it, everything scales.
inline constexpr int DEFAULT_WIDTH = 340, DEFAULT_HEIGHT = 420;
inline constexpr int MIN_WIDTH = 220, MIN_HEIGHT = 140;

// What clicking an emote pastes, also picked with the gear button: its link, or the file the
// link points to. A file is downloaded first and pasted like a file copied in Explorer, so
// Discord uploads it with your message and shows it right away, instead of loading the link
// from 7TV after the message is sent.
inline constexpr const wchar_t* PASTE_MODES[] = {L"link", L"file"};
inline constexpr const wchar_t* DEFAULT_PASTE = L"link";
// Downloaded files are kept (in %LOCALAPPDATA%\7TV Emote Picker\uploads), so an emote you've
// sent before pastes at once. Ones older than this many days are deleted at start.
inline constexpr int FILE_KEEP_DAYS = 30;
// The mouse resting on an emote this long starts downloading its file, so it's usually there
// by the time you click it.
inline constexpr int FILE_PREFETCH_MS = 150;
// Clicking an emote whose file isn't downloaded yet pastes it once it is, unless that takes
// longer than this (then it isn't pasted; click again).
inline constexpr int FILE_WAIT_MS = 10000;

inline constexpr bool ADD_SPACE_AFTER = false;  // append a space after the pasted link
// After clicking an emote while you were typing in the picker, give Discord this long to
// take focus back before pasting. (Clicking while Discord has focus pastes right away.)
inline constexpr int PASTE_DELAY_MS = 250;
// This long after pasting, whatever you had copied before (from Chrome etc.) goes back on
// the clipboard. Raise it if Discord ever pastes that instead of the emote.
inline constexpr int RESTORE_CLIPBOARD_MS = 1000;
inline constexpr int PER_PAGE = 250;          // emotes per search (7TV allows at most 250)
// How big emotes show, at 100% display scaling. The nearest of 7TV's preview sizes is used
// (32/64/96/128px, e.g. 64px at 175%), and each row fits as many emotes as it has room for.
inline constexpr int EMOTE_SIZE = 40;
// The emote size slider (gear button) scales that: 100% shows the 7TV size as is. Bigger or
// smaller sizes come from the sharpest 7TV size at least that big, scaled down.
inline constexpr double MIN_EMOTE_SCALE = 0.5, MAX_EMOTE_SCALE = 3.0, EMOTE_SCALE_STEP = 0.05;
inline constexpr const wchar_t* DISCORD_EXES[] = {L"discord.exe", L"discordptb.exe",
                                                  L"discordcanary.exe"};
// Gap between the panel and Discord's bottom-right corner, at 100% display scaling.
// The bottom gap keeps Discord's message box uncovered.
inline constexpr int DOCK_MARGIN_RIGHT = 16;
inline constexpr int DOCK_MARGIN_BOTTOM = 80;
// Discord's focus and position are followed through Windows events, instantly; this is
// only a safety net in case one is missed.
inline constexpr int FOLLOW_POLL_MS = 200;
inline constexpr bool ANIMATE = true;         // false: animated emotes stay still
// Previews download for the rows on screen and the next screen down, the rest as you scroll.
// Previews downloading at once. They all share one HTTP/2 connection, and each takes ~300ms
// mostly waiting on the network, so many at once fill a screen in about one round trip.
inline constexpr int PREVIEW_DOWNLOADS = 32;
inline constexpr int MAX_SAVED_PREVIEWS = 1000;  // past this many saved previews, forget unshown
inline constexpr int MAX_GIF_MB = 48;          // past this much downloaded GIF, forget unshown
inline constexpr int SEARCH_DELAY_MS = 250;   // wait this long after you stop typing to search
inline constexpr double SCROLL_EASE_MS = 45;  // smooth scrolling: lower is snappier, 0 is instant

inline constexpr const wchar_t* API_URL = L"https://7tv.io/v4/gql";
// {id}, {size} and {format} are filled in.
inline constexpr const wchar_t* LINK_URL = L"https://cdn.7tv.app/emote/{id}/{size}.{format}";
// Previews: a PNG still first (quick), then for animated emotes on screen the GIF.
// {s} is 1-4 (32-128px tall).
inline constexpr const wchar_t* THUMB_ANIMATED = L"https://cdn.7tv.app/emote/{id}/{s}x_static.png";
inline constexpr const wchar_t* THUMB_STATIC = L"https://cdn.7tv.app/emote/{id}/{s}x.png";
inline constexpr const wchar_t* THUMB_GIF = L"https://cdn.7tv.app/emote/{id}/{s}x.gif";

// Look: Discord's dark theme colors, so the panel feels like part of Discord (0xRRGGBB).
inline constexpr uint32_t BG = 0x2b2d31, FIELD = 0x1e1f22, HOVER = 0x393c41, SELECTED = 0x404249;
inline constexpr uint32_t TEXT = 0xdbdee1, MUTED = 0x949ba4, FAINT = 0x6d6f78, BORDER = 0x1e1f22;
inline constexpr uint32_t SCROLL_THUMB = 0x1a1b1e, SCROLL_THUMB_ACTIVE = 0x111214, GRIP = 0x4e5058;
inline constexpr uint32_t TEXT_SELECTION = 0x5865f2, SELECTED_TEXT = 0xffffff;
// Spacing at 100% display scaling, in pixels.
inline constexpr double PAD = 8, FIELD_H = 32, CELL_PAD = 6;
