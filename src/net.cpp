#include "net.h"

#include <winhttp.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

#include "config.h"
#include "json.h"

namespace {

// Previews download over a few connections (one per session), each carrying several at once:
// one connection alone stalls everything when its network path has a bad moment. One more
// session is fetch_alone's, so a file someone is waiting for doesn't share a connection with
// the previews' downloads, and one is the search's, so every search finds the same connection
// (each session has connections of its own).
// Not HTTP/3: 7TV's CDN offers it, and it opens a connection a round trip sooner, but through
// WinHTTP from here about 1 request in 4 failed with it (most of a burst on a new connection),
// and in a timed test run WinHTTP never used it anyway (measured 2026-11-24).
constexpr int SESSIONS = 4, ALONE = SESSIONS, SEARCH = SESSIONS + 1, ALL = SESSIONS + 2;
// 7TV closes connections idle this long, less the time typing a search may take (measured
// 2026-11-24: its CDN after 30 s, its search server after 50 s; HTTP/2 pings don't keep either
// open, nor does WinHTTP's HTTP/2 keep-alive).
constexpr double CDN_IDLE_MS = 22000, SEARCH_IDLE_MS = 40000;

HINTERNET g_sessions[ALL] = {};
std::atomic<double> g_used[ALL];  // when each session last started or finished a request (ms)
std::atomic<unsigned> g_next_session{0};
std::atomic<int> g_requests{0}, g_http2{0}, g_cancelled{0};
std::atomic<long long> g_bytes{0};
std::mutex g_conn_mutex;
std::map<std::wstring, HINTERNET> g_connections;  // session + host -> connection, shared

HINTERNET connection(int s, const std::wstring& host, INTERNET_PORT port) {
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

// Diagnostics: with the environment variable PICKER_NET_LOG set to a file's path, each request
// is written there: when it started (ms from the first), how long it took, bytes, session, how
// it ended, the URL. (Emote ids only; nothing about you.)
void log_request(int s, double start, const std::wstring& url, size_t bytes, const std::wstring& how) {
    static FILE* file = [] {
        wchar_t path[MAX_PATH];
        FILE* f = nullptr;
        DWORD n = GetEnvironmentVariableW(L"PICKER_NET_LOG", path, MAX_PATH);
        if (n && n < MAX_PATH) _wfopen_s(&f, path, L"a");
        return f;
    }();
    if (!file) return;
    static const double t0 = start;
    static std::mutex m;
    std::lock_guard lock(m);
    fwprintf(file, L"%9.0f %6.0f %8zu s%d %-12ls %ls\n", start - t0, now_ms() - start, bytes, s, how.c_str(),
             url.c_str());
    fflush(file);
}

// One try at url on session s. On failure, code is WinHTTP's error (0 if the server answered,
// just not with 200, or the download was cancelled).
bool fetch_once(int s, const std::wstring& url, const std::string& body, Bytes& out,
                std::wstring& err, const std::atomic<bool>* cancel, DWORD& code) {
    code = 0;
    auto cancelled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
    if (cancelled()) {
        err = L"cancelled";
        return false;
    }
    double start = now_ms();
    g_used[s] = start;
    auto failed = [&](DWORD c) {
        code = c;
        err = error_text(c);
        g_used[s] = now_ms();
        log_request(s, start, url, 0, L"error " + std::to_wstring(c));
        return false;
    };
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
    HINTERNET conn = connection(s, host, parts.nPort);
    if (!conn) return failed(GetLastError());
    HINTERNET req = WinHttpOpenRequest(conn, body.empty() ? L"GET" : L"POST", path, nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req) return failed(GetLastError());
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(req, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
    const wchar_t* headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                          : L"Content-Type: application/json\r\n";
    if (!WinHttpSendRequest(req, headers, (DWORD)-1L, (void*)body.data(), (DWORD)body.size(),
                            (DWORD)body.size(), 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        DWORD c = GetLastError();
        WinHttpCloseHandle(req);
        return failed(c);
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
    bool ok = true;
    std::wstring how = std::to_wstring(status);
    uint8_t buf[16384];
    for (;;) {
        // No longer wanted: closing the request stops the download (HTTP/2 tells the server to
        // stop sending it), leaving the connection to the others.
        if (cancelled()) {
            err = how = L"cancelled";
            ok = false;
            g_cancelled++;
            break;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, buf, sizeof(buf), &got)) {
            code = GetLastError();
            err = error_text(code);
            how = L"error " + std::to_wstring(code);
            ok = false;
            break;
        }
        if (!got) break;
        out.insert(out.end(), buf, buf + got);
        g_bytes += got;
    }
    WinHttpCloseHandle(req);
    g_used[s] = now_ms();
    log_request(s, start, url, out.size(), how);
    if (ok && status != 200) {
        err = L"HTTP " + std::to_wstring(status);
        ok = false;
    }
    return ok;
}

// url on session s, once more on a fresh connection if the one used was dropped.
bool fetch_on(int s, const std::wstring& url, const std::string& body, Bytes& out,
              std::wstring& err, const std::atomic<bool>* cancel) {
    DWORD code = 0;
    if (fetch_once(s, url, body, out, err, cancel, code)) return true;
    // An idle connection the server dropped: once more on a fresh one.
    bool dropped = code == ERROR_WINHTTP_CONNECTION_ERROR || code == ERROR_WINHTTP_SECURE_FAILURE;
    return dropped && fetch_once(s, url, body, out, err, cancel, code);
}

// Opens session s's connection to url's server in the background, unless the session was used
// in the last idle_ms.
void warm(int s, const std::wstring& url, double idle_ms) {
    double now = now_ms(), last = g_used[s];
    if (now - last < idle_ms || !g_used[s].compare_exchange_strong(last, now)) return;
    std::thread([s, url] {
        Bytes ignored;
        std::wstring err;
        fetch_on(s, url, "", ignored, err, nullptr);  // the answer doesn't matter, the open connection does
    }).detach();
}

// "https://host/" of a URL.
std::wstring server_of(const std::wstring& url) {
    size_t host = url.find(L"://");
    return url.substr(0, url.find(L'/', host == std::wstring::npos ? 0 : host + 3)) + L"/";
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
    for (int s = 0; s < SESSIONS; s++) warm(s, url, 0);
}

void net_warm_soon(bool files, bool search) {
    static const std::wstring search_server = server_of(API_URL), cdn = server_of(LINK_URL);
    if (search) warm(SEARCH, search_server, SEARCH_IDLE_MS);
    for (int s = 0; s < SESSIONS; s++) warm(s, cdn, CDN_IDLE_MS);
    if (files) warm(ALONE, cdn, CDN_IDLE_MS);
}

NetStats net_stats() { return {g_requests, g_http2, g_bytes, g_cancelled}; }

bool fetch(const std::wstring& url, const std::string& body, Bytes& out, std::wstring& err,
           const std::atomic<bool>* cancel) {
    return fetch_on((int)(g_next_session++ % SESSIONS), url, body, out, err, cancel);
}

bool fetch_alone(const std::wstring& url, Bytes& out, std::wstring& err) {
    return fetch_on(ALONE, url, "", out, err, nullptr);
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
    if (!fetch_on(SEARCH, API_URL, body, answer, err, nullptr)) return false;
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
