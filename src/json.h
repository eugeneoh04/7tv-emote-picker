// A small JSON reader (enough for 7TV's answers and settings.json) and string escaping.
#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool boolean = false;
    double number = 0;
    std::string str;                                    // UTF-8
    std::vector<Json> items;                            // Array
    std::vector<std::pair<std::string, Json>> members;  // Object

    const Json& operator[](std::string_view key) const {
        if (type == Object)
            for (auto& [k, v] : members)
                if (k == key) return v;
        return none();
    }
    static const Json& none() {
        static const Json n;
        return n;
    }
    bool is(Type t) const { return type == t; }
};

namespace json_detail {

struct Parser {
    std::string_view s;
    size_t i = 0;
    int depth = 0;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }
    bool lit(std::string_view word) {
        if (s.substr(i, word.size()) != word) return false;
        i += word.size();
        return true;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | cp >> 6);
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | cp >> 12);
            out += (char)(0x80 | (cp >> 6 & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | cp >> 18);
            out += (char)(0x80 | (cp >> 12 & 0x3F));
            out += (char)(0x80 | (cp >> 6 & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && s.substr(i, 2) == "\\u") {
                        size_t save = i;
                        i += 2;
                        unsigned lo;
                        if (hex4(lo) && lo >= 0xDC00 && lo < 0xE000)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            i = save;
                    }
                    utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(Json& v) {
        if (++depth > 64) return false;
        ws();
        if (i >= s.size()) return false;
        bool ok = true;
        char c = s[i];
        if (c == '{') {
            v.type = Json::Object;
            i++;
            ws();
            if (i < s.size() && s[i] == '}') {
                i++;
            } else {
                for (;;) {
                    ws();
                    std::pair<std::string, Json> m;
                    if (!string(m.first)) return false;
                    ws();
                    if (i >= s.size() || s[i++] != ':') return false;
                    if (!value(m.second)) return false;
                    v.members.push_back(std::move(m));
                    ws();
                    if (i < s.size() && s[i] == ',') { i++; continue; }
                    if (i < s.size() && s[i] == '}') { i++; break; }
                    return false;
                }
            }
        } else if (c == '[') {
            v.type = Json::Array;
            i++;
            ws();
            if (i < s.size() && s[i] == ']') {
                i++;
            } else {
                for (;;) {
                    Json item;
                    if (!value(item)) return false;
                    v.items.push_back(std::move(item));
                    ws();
                    if (i < s.size() && s[i] == ',') { i++; continue; }
                    if (i < s.size() && s[i] == ']') { i++; break; }
                    return false;
                }
            }
        } else if (c == '"') {
            v.type = Json::String;
            ok = string(v.str);
        } else if (lit("true")) {
            v.type = Json::Bool;
            v.boolean = true;
        } else if (lit("false")) {
            v.type = Json::Bool;
        } else if (lit("null")) {
            v.type = Json::Null;
        } else {
            std::string num;
            while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' ||
                                    s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
                num += s[i++];
            if (num.empty()) return false;
            char* end = nullptr;
            v.type = Json::Number;
            v.number = strtod(num.c_str(), &end);
            ok = end && *end == '\0';
        }
        depth--;
        return ok;
    }
};

}  // namespace json_detail

// Parses text; false if it isn't valid JSON.
inline bool parse_json(std::string_view text, Json& out) {
    json_detail::Parser p{text};
    out = Json{};
    if (!p.value(out)) return false;
    p.ws();
    return p.i == text.size();
}

// s (UTF-8) as a JSON string literal, quotes included.
inline std::string json_quote(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out + "\"";
}
