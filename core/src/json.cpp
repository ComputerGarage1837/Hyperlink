#include "hyperlink/json.h"

#include <cctype>
#include <cstdlib>

namespace hl::json {

static const Value kNull;

const Value& Value::operator[](const std::string& key) const {
    if (type != Object) return kNull;
    auto it = obj.find(key);
    return it == obj.end() ? kNull : it->second;
}

const Value& Value::operator[](size_t i) const {
    return type == Array && i < arr.size() ? arr[i] : kNull;
}

namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;

    void ws() {
        while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    }

    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& v) {
        if (i + 4 > s.size()) return false;
        v = (unsigned)std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
        i += 4;
        return true;
    }

    bool string(std::string& out) {
        if (s[i] != '"') return false;
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
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        unsigned lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: out += e;
            }
        }
        return false;
    }

    bool value(Value& v) {
        if (++depth > 64) return false;
        ws();
        if (i >= s.size()) return false;
        char c = s[i];
        bool ok = true;
        if (c == '{') {
            v.type = Value::Object;
            i++;
            ws();
            if (i < s.size() && s[i] == '}') i++;
            else
                for (;;) {
                    ws();
                    std::string key;
                    if (i >= s.size() || !string(key)) { ok = false; break; }
                    ws();
                    if (i >= s.size() || s[i++] != ':') { ok = false; break; }
                    if (!value(v.obj[key])) { ok = false; break; }
                    ws();
                    if (i < s.size() && s[i] == ',') { i++; continue; }
                    if (i < s.size() && s[i] == '}') { i++; break; }
                    ok = false;
                    break;
                }
        } else if (c == '[') {
            v.type = Value::Array;
            i++;
            ws();
            if (i < s.size() && s[i] == ']') i++;
            else
                for (;;) {
                    v.arr.emplace_back();
                    if (!value(v.arr.back())) { ok = false; break; }
                    ws();
                    if (i < s.size() && s[i] == ',') { i++; continue; }
                    if (i < s.size() && s[i] == ']') { i++; break; }
                    ok = false;
                    break;
                }
        } else if (c == '"') {
            v.type = Value::String;
            ok = string(v.str);
        } else if (s.compare(i, 4, "true") == 0) {
            v.type = Value::Bool; v.b = true; i += 4;
        } else if (s.compare(i, 5, "false") == 0) {
            v.type = Value::Bool; i += 5;
        } else if (s.compare(i, 4, "null") == 0) {
            i += 4;
        } else {
            char* end = nullptr;
            v.type = Value::Number;
            v.num = std::strtod(s.c_str() + i, &end);
            size_t used = end - (s.c_str() + i);
            if (!used) ok = false;
            i += used;
        }
        depth--;
        return ok;
    }
};

}  // namespace

bool parse(const std::string& text, Value& out) {
    out = Value();
    Parser p{text};
    return p.value(out);
}

long long versionCode(const std::string& v) {
    long long parts[3] = {0, 0, 0};
    size_t i = 0;
    while (i < v.size() && !std::isdigit((unsigned char)v[i])) i++;
    for (int k = 0; k < 3 && i < v.size(); k++) {
        long long n = 0;
        while (i < v.size() && std::isdigit((unsigned char)v[i])) n = n * 10 + (v[i++] - '0');
        parts[k] = n;
        if (i < v.size() && v[i] == '.') i++;
        else break;
    }
    return parts[0] * 1000000 + parts[1] * 1000 + parts[2];
}

}  // namespace hl::json
