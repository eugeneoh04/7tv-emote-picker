// HTTPS downloads (WinHTTP: connections are kept open and shared, HTTP/2 when the server has
// it) and the 7TV search.
#pragma once

#include <atomic>

#include "common.h"

struct Emote {
    std::string id;
    std::wstring name;
    bool animated = false;
};

void net_init();
// Counts since the start (tests print them).
struct NetStats {
    int requests = 0;              // answered
    int http2 = 0;                 // of those, over HTTP/2
    long long bytes = 0;           // downloaded (response bodies)
    int cancelled = 0;             // downloads stopped part-way: no longer wanted
};
NetStats net_stats();
// Opens the previews' connections to url's server in the background, so the first real
// downloads don't wait for the secure handshakes.
void net_warm_up(const std::wstring& url);
// Opens the connections the next search and its previews will use, unless they're open
// already: 7TV closes idle ones (its CDN after 30 s, its search server after 50 s), and opening
// one takes a second or so from here. Called when you're probably about to search (the panel
// shows, a click in the search box, typing), so it's done by the time you've typed. files: also
// fetch_alone's connection. search false: not the search's (one is starting now and opens it;
// both opening it at once would make two). Cheap when there's nothing to do.
void net_warm_soon(bool files, bool search = true);
// GET (body empty) or POST (JSON body). False and err set on failure. Once cancel is set, the
// download stops part-way (err "cancelled") and the server stops sending it. With the
// environment variable PICKER_NET_LOG set to a file's path, every request is logged there.
bool fetch(const std::wstring& url, const std::string& body, Bytes& out, std::wstring& err,
           const std::atomic<bool>* cancel = nullptr);
// A GET for a file someone is waiting for, on a connection of its own: on the shared ones it
// waits behind the previews' downloads (about 3x slower while those run).
bool fetch_alone(const std::wstring& url, Bytes& out, std::wstring& err);
// Up to PER_PAGE emotes matching text, most used first; recent searches are remembered.
// Safe to call from any thread.
bool search_emotes(const std::wstring& text, std::vector<Emote>& out, std::wstring& err);
