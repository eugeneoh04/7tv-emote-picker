// HTTPS downloads (WinHTTP: connections are kept open and shared, HTTP/2 when the server
// has it) and the 7TV search.
#pragma once

#include "common.h"

struct Emote {
    std::string id;
    std::wstring name;
    bool animated = false;
};

void net_init();
void net_stats(int& requests, int& http2);  // how many requests so far, how many over HTTP/2
// Opens the connections to url's server in the background, so the first real downloads
// don't wait for the secure handshakes (~1s).
void net_warm_up(const std::wstring& url);
// GET (body empty) or POST (JSON body). False and err set on failure.
bool fetch(const std::wstring& url, const std::string& body, Bytes& out, std::wstring& err);
// A GET for a file someone is waiting for, on a connection of its own: on the shared ones it
// waits behind the previews' downloads (about 3x slower while those run).
bool fetch_alone(const std::wstring& url, Bytes& out, std::wstring& err);
// Opens fetch_alone's connection to url's server ahead of time (skipped if it was just used).
void net_warm_alone(const std::wstring& url);
// Up to PER_PAGE emotes matching text, most used first; recent searches are remembered.
// Safe to call from any thread.
bool search_emotes(const std::wstring& text, std::vector<Emote>& out, std::wstring& err);
