#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace qj {

static void dumpString(std::string &out, const std::string &s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

std::string Value::dump() const {
    std::string out;
    switch (type) {
        case Null: out += "null"; break;
        case Bool: out += b ? "true" : "false"; break;
        case Number: {
            char buf[32];
            if (num == static_cast<int64_t>(num)) {
                std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(num));
            } else {
                std::snprintf(buf, sizeof buf, "%.17g", num);
            }
            out += buf;
            break;
        }
        case String: dumpString(out, str); break;
        case Array: {
            out += '[';
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i) out += ',';
                out += arr[i].dump();
            }
            out += ']';
            break;
        }
        case Object: {
            out += '{';
            for (size_t i = 0; i < obj.size(); ++i) {
                if (i) out += ',';
                dumpString(out, obj[i].first);
                out += ':';
                out += obj[i].second.dump();
            }
            out += '}';
            break;
        }
    }
    return out;
}

namespace {

struct Parser {
    const std::string &s;
    size_t i = 0;
    std::string error;

    explicit Parser(const std::string &text) : s(text) {}

    bool fail(const std::string &msg) {
        if (error.empty()) error = msg + " (位置 " + std::to_string(i) + ")";
        return false;
    }

    void skipWs() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }

    bool parseValue(Value &out) {
        skipWs();
        if (i >= s.size()) return fail("意外的结尾");
        char c = s[i];
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') return parseString(out);
        if (c == 't' || c == 'f') return parseBool(out);
        if (c == 'n') return parseNull(out);
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
        return fail(std::string("无法识别的字符 '") + c + "'");
    }

    bool parseObject(Value &out) {
        ++i;  // {
        out = Value::makeObject();
        skipWs();
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            skipWs();
            Value key;
            if (!parseString(key)) return false;
            skipWs();
            if (i >= s.size() || s[i] != ':') return fail("对象成员缺冒号");
            ++i;
            Value val;
            if (!parseValue(val)) return false;
            out.set(key.str, std::move(val));
            skipWs();
            if (i >= s.size()) return fail("对象未闭合");
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == '}') { ++i; return true; }
            return fail("对象成员分隔符错误");
        }
    }

    bool parseArray(Value &out) {
        ++i;  // [
        out = Value::makeArray();
        skipWs();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        while (true) {
            Value val;
            if (!parseValue(val)) return false;
            out.push(std::move(val));
            skipWs();
            if (i >= s.size()) return fail("数组未闭合");
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == ']') { ++i; return true; }
            return fail("数组分隔符错误");
        }
    }

    bool parseString(Value &out) {
        ++i;  // "
        std::string text;
        while (true) {
            if (i >= s.size()) return fail("字符串未闭合");
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == '"') { ++i; out = Value::makeString(std::move(text)); return true; }
            if (c == '\\') {
                ++i;
                if (i >= s.size()) return fail("转义未闭合");
                char e = s[i++];
                switch (e) {
                    case '"': text += '"'; break;
                    case '\\': text += '\\'; break;
                    case '/': text += '/'; break;
                    case 'n': text += '\n'; break;
                    case 'r': text += '\r'; break;
                    case 't': text += '\t'; break;
                    case 'b': text += '\b'; break;
                    case 'f': text += '\f'; break;
                    case 'u': {
                        if (i + 4 > s.size()) return fail("\\u 转义不完整");
                        unsigned code = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s[i++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                            else return fail("\\u 转义非法");
                        }
                        // 只处理 BMP；代理对按两个 \\u 分别转 UTF-8（协议中文均为 BMP）。
                        if (code >= 0xD800 && code <= 0xDBFF && i + 6 <= s.size() &&
                            s[i] == '\\' && s[i + 1] == 'u') {
                            unsigned low = 0;
                            bool ok = true;
                            for (int k = 0; k < 4; ++k) {
                                char h = s[i + 2 + k];
                                low <<= 4;
                                if (h >= '0' && h <= '9') low |= static_cast<unsigned>(h - '0');
                                else if (h >= 'a' && h <= 'f') low |= static_cast<unsigned>(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') low |= static_cast<unsigned>(h - 'A' + 10);
                                else { ok = false; break; }
                            }
                            if (ok && low >= 0xDC00 && low <= 0xDFFF) {
                                i += 6;
                                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                            }
                        }
                        appendUtf8(text, code);
                        break;
                    }
                    default: return fail("非法转义");
                }
                continue;
            }
            if (c < 0x20) return fail("字符串含控制字符");
            text += static_cast<char>(c);
            ++i;
        }
    }

    bool parseBool(Value &out) {
        if (s.compare(i, 4, "true") == 0) { i += 4; out = Value::makeBool(true); return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; out = Value::makeBool(false); return true; }
        return fail("布尔值无法识别");
    }

    bool parseNull(Value &out) {
        if (s.compare(i, 4, "null") == 0) { i += 4; out = Value(); return true; }
        return fail("null 无法识别");
    }

    bool parseNumber(Value &out) {
        size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        }
        out = Value::makeNumber(std::strtod(s.c_str() + start, nullptr));
        return true;
    }

    static void appendUtf8(std::string &text, unsigned code) {
        if (code < 0x80) {
            text += static_cast<char>(code);
        } else if (code < 0x800) {
            text += static_cast<char>(0xC0 | (code >> 6));
            text += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            text += static_cast<char>(0xE0 | (code >> 12));
            text += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            text += static_cast<char>(0xF0 | (code >> 18));
            text += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            text += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
};

}  // namespace

bool Value::parse(const std::string &text, Value &out, std::string &error) {
    Parser p(text);
    if (!p.parseValue(out)) {
        error = p.error;
        return false;
    }
    p.skipWs();
    if (p.i != text.size()) {
        error = "JSON 尾部有多余内容";
        return false;
    }
    return true;
}

}  // namespace qj
