#include "net.h"

#include <winhttp.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include "config.h"
#include "json.h"

namespace {

// Downloads are spread over a few HTTP/2 connections (one per session), each carrying several
// at once: one connection alone stalls everything when its network path has a bad moment.
// One more session is fetch_alone's, so a file someone is waiting for doesn't share a
// connection with the previews' downloads.
constexpr int SESSIONS = 4, ALONE = SESSIONS;
HINTERNET g_sessions[SESSIONS + 1] = {};
std::atomic<unsigned> g_next_session{0};
std::atomic<int> g_requests{0}, g_http2{0};
std::atomic<double> g_alone_used{-1e9};  // when fetch_alone's connection was last used
std::mutex g_conn_mutex;
std::map<std::wstring, HINTERNET> g_connections;  // session + host -> connection, shared

HINTERNET connection(const std::wstring& host, INTERNET_PORT port, bool alone) {
    int s = alone ? ALONE : (int)(g_next_session++ % SESSIONS);
    std::lock_guard lock(g_conn_mutex);
    auto key = std::to_wstring(s) + L"|" + host + L":" + std::to_wstring(port);
    auto it = g_connections.find(key);
    if (it != g_connections.end()) return it->second;
    HINTERNET c = WinHttpConnect(g_sessions[s], host.c_str(), port, 0);
    if (c) g_connections[key] = c;
    return c;
}

std::wstring error_text(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT: return L"timed out";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return L"no internet?";
        case ERROR_WINHTTP_CANNOT_CONNECT: return L"can't connect";
        case ERROR_WINHTTP_CONNECTION_ERROR: return L"connection dropped";
        default: return L"error " + std::to_wstring(code);
    }
}

bool fetch_once(const std::wstring& url, const std::string& body, Bytes& out, std::wstring& err,
                bool& retry, bool alone = false) {
    retry = false;
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256], path[2048];
    parts.lpszHostName = host;
    parts.dwHostNameLength = (DWORD)std::size(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = (DWORD)std::size(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        err = L"bad URL";
        return false;
    }
    HINTERNET conn = connection(host, parts.nPort, alone);
    if (!conn) {
        err = error_text(GetLastError());
        return false;
    }
    HINTERNET req = WinHttpOpenRequest(conn, body.empty() ? L"GET" : L"POST", path, nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        err = error_text(GetLastError());
        return false;
    }
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(req, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
    const wchar_t* headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                          : L"Content-Type: application/json\r\n";
    bool ok = WinHttpSendRequest(req, headers, (DWORD)-1L, (void*)body.data(), (DWORD)body.size(),
                                 (DWORD)body.size(), 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        DWORD code = GetLastError();
        err = error_text(code);
        // An idle connection the server dropped: try once more on a fresh one.
        retry = code == ERROR_WINHTTP_CONNECTION_ERROR || code == ERROR_WINHTTP_SECURE_FAILURE;
        WinHttpCloseHandle(req);
        return false;
    }
    DWORD protocol = 0, size = sizeof(protocol);
    WinHttpQueryOption(req, WINHTTP_OPTION_HTTP_PROTOCOL_USED, &protocol, &size);
    g_requests++;
    if (protocol & WINHTTP_PROTOCOL_FLAG_HTTP2) g_http2++;
    DWORD status = 0;
    size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    out.clear();
    uint8_t buf[16384];
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req, buf, sizeof(buf), &got)) {
            err = error_text(GetLastError());
            ok = false;
            break;
        }
        if (!got) break;
        out.insert(out.end(), buf, buf + got);
    }
    WinHttpCloseHandle(req);
    if (ok && status != 200) {
        err = L"HTTP " + std::to_wstring(status);
        ok = false;
    }
    return ok;
}

std::mutex g_cache_mutex;
std::map<std::wstring, std::vector<Emote>> g_search_cache;

const char* SEARCH_QUERY = R"(
query($q: String!, $perPage: Int!) {
  emotes {
    search(query: $q, page: 1, perPage: $perPage,
           sort: {sortBy: TOP_ALL_TIME, order: DESCENDING}) {
      items { id defaultName flags { animated } }
    }
  }
})";

}  // namespace

void net_init() {
    for (HINTERNET& s : g_sessions) {
        s = WinHttpOpen(L"emote-picker/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s)  // older Windows without automatic proxy
            s = WinHttpOpen(L"emote-picker/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        WinHttpSetTimeouts(s, 10000, 10000, 10000, 10000);
        DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
        WinHttpSetOption(s, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols));
        DWORD conns = 32;  // if a server only speaks HTTP/1.1
        WinHttpSetOption(s, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &conns, sizeof(conns));
    }
}

void net_warm_up(const std::wstring& url) {
    for (int i = 0; i < SESSIONS; i++)
        std::thread([url] {
            Bytes ignored;
            std::wstring err;
            fetch(url, "", ignored, err);  // the answer doesn't matter, the open connection does
        }).detach();
}

void net_stats(int& requests, int& http2) {
    requests = g_requests;
    http2 = g_http2;
}

bool fetch(const std::wstring& url, const std::string& body, Bytes& out, std::wstring& err) {
    bool retry = false;
    if (fetch_once(url, body, out, err, retry)) return true;
    return retry && fetch_once(url, body, out, err, retry);
}

bool fetch_alone(const std::wstring& url, Bytes& out, std::wstring& err) {
    g_alone_used = now_ms();
    bool retry = false;
    if (fetch_once(url, "", out, err, retry, true)) return true;
    return retry && fetch_once(url, "", out, err, retry, true);
}

void net_warm_alone(const std::wstring& url) {
    if (now_ms() - g_alone_used < 15000) return;  // used just now: still open
    g_alone_used = now_ms();
    std::thread([url] {
        Bytes ignored;
        std::wstring err;
        bool retry;
        fetch_once(url, "", ignored, err, retry, true);
    }).detach();
}

bool search_emotes(const std::wstring& text, std::vector<Emote>& out, std::wstring& err) {
    std::wstring key = lowercase(text);
    {
        std::lock_guard lock(g_cache_mutex);
        auto it = g_search_cache.find(key);
        if (it != g_search_cache.end()) {
            out = it->second;
            return true;
        }
    }
    std::string body = std::string("{\"query\":") + json_quote(SEARCH_QUERY) +
                       ",\"variables\":{\"q\":" + json_quote(narrow(text)) +
                       ",\"perPage\":" + std::to_string(PER_PAGE) + "}}";
    Bytes answer;
    if (!fetch(API_URL, body, answer, err)) return false;
    Json data;
    if (!parse_json(std::string_view((const char*)answer.data(), answer.size()), data)) {
        err = L"bad answer from 7TV";
        return false;
    }
    const Json& items = data["data"]["emotes"]["search"]["items"];
    if (!items.is(Json::Array)) {
        const Json& message = data["errors"].items.empty() ? Json::none()
                                                           : data["errors"].items[0]["message"];
        err = message.is(Json::String) ? widen(message.str) : L"bad answer from 7TV";
        return false;
    }
    out.clear();
    for (const Json& e : items.items) {
        if (!e["id"].is(Json::String)) continue;
        out.push_back({e["id"].str, widen(e["defaultName"].str), e["flags"]["animated"].boolean});
    }
    std::lock_guard lock(g_cache_mutex);
    if (g_search_cache.size() >= 200) g_search_cache.clear();  // runs for days; don't keep all
    g_search_cache[key] = out;
    return true;
}
