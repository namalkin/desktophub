#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mj {

const Value* Value::Find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : obj) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;

    explicit Parser(const std::string& text) : s(text) {}

    void SkipWs() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    }

    bool Lit(const char* lit) {
        size_t n = 0;
        while (lit[n]) n++;
        if (s.compare(i, n, lit) != 0) return false;
        i += n;
        return true;
    }

    static void AppendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(unsigned& out) {
        if (i + 4 > s.size()) return false;
        out = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i + k];
            unsigned d;
            if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
            else return false;
            out = out * 16 + d;
        }
        i += 4;
        return true;
    }

    bool String(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        out.clear();
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
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        size_t save = i;
                        i += 2;
                        unsigned lo;
                        if (Hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            i = save;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool ParseValue(Value& v) {
        SkipWs();
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '{') {
            i++;
            v = Value::Object();
            SkipWs();
            if (i < s.size() && s[i] == '}') { i++; return true; }
            while (true) {
                SkipWs();
                std::string key;
                if (!String(key)) return false;
                SkipWs();
                if (i >= s.size() || s[i] != ':') return false;
                i++;
                Value child;
                if (!ParseValue(child)) return false;
                v.obj.emplace_back(std::move(key), std::move(child));
                SkipWs();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; return true; }
                return false;
            }
        }
        if (c == '[') {
            i++;
            v = Value::Array();
            SkipWs();
            if (i < s.size() && s[i] == ']') { i++; return true; }
            while (true) {
                Value child;
                if (!ParseValue(child)) return false;
                v.arr.push_back(std::move(child));
                SkipWs();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; return true; }
                return false;
            }
        }
        if (c == '"') {
            std::string out;
            if (!String(out)) return false;
            v = Value::Str(std::move(out));
            return true;
        }
        if (Lit("true")) { v = Value::Bool(true); return true; }
        if (Lit("false")) { v = Value::Bool(false); return true; }
        if (Lit("null")) { v = Value(); return true; }

        const char* begin = s.c_str() + i;
        char* end = nullptr;
        double d = std::strtod(begin, &end);
        if (end == begin) return false;
        i += static_cast<size_t>(end - begin);
        v = Value::Num(d);
        return true;
    }
};

void EscapeTo(std::string& out, const std::string& in) {
    out += '"';
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);  // UTF-8 проходит как есть
                }
        }
    }
    out += '"';
}

void SerializeTo(std::string& out, const Value& v, int depth) {
    const std::string pad(static_cast<size_t>(depth + 1) * 2, ' ');
    const std::string padEnd(static_cast<size_t>(depth) * 2, ' ');
    switch (v.type) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.boolean ? "true" : "false"; break;
        case Type::Number: {
            char buf[64];
            if (v.number == std::floor(v.number) && std::fabs(v.number) < 1e15) {
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.number));
            } else {
                std::snprintf(buf, sizeof(buf), "%.2f", v.number);
            }
            out += buf;
            break;
        }
        case Type::String: EscapeTo(out, v.str); break;
        case Type::Array:
            if (v.arr.empty()) { out += "[]"; break; }
            out += "[\n";
            for (size_t k = 0; k < v.arr.size(); k++) {
                out += pad;
                SerializeTo(out, v.arr[k], depth + 1);
                if (k + 1 < v.arr.size()) out += ',';
                out += '\n';
            }
            out += padEnd + "]";
            break;
        case Type::Object:
            if (v.obj.empty()) { out += "{}"; break; }
            out += "{\n";
            for (size_t k = 0; k < v.obj.size(); k++) {
                out += pad;
                EscapeTo(out, v.obj[k].first);
                out += ": ";
                SerializeTo(out, v.obj[k].second, depth + 1);
                if (k + 1 < v.obj.size()) out += ',';
                out += '\n';
            }
            out += padEnd + "}";
            break;
    }
}

}  // namespace

bool Parse(const std::string& text, Value& out) {
    Parser p(text);
    if (!p.ParseValue(out)) return false;
    p.SkipWs();
    return true;
}

std::string Serialize(const Value& v) {
    std::string out;
    SerializeTo(out, v, 0);
    out += '\n';
    return out;
}

}  // namespace mj
