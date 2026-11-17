// Minimal zero-dependency JSON library (objects, arrays, strings, numbers,
// bools, null) with a parser and two serializers (compact / pretty).
#pragma once

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace json {

class parse_error : public std::runtime_error {
public:
    explicit parse_error(const std::string& msg) : std::runtime_error(msg) {}
};

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    Value() = default;
    Value(Type t) : type(t) {}
    Value(bool b) : type(Type::Bool), boolean(b) {}
    Value(double n) : type(Type::Number), number(n) {}
    Value(float n) : type(Type::Number), number(static_cast<double>(n)) {}
    template <typename T,
              typename = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>>
    Value(T n) : type(Type::Number), number(static_cast<double>(n)) {}
    Value(const char* s) : type(Type::String), str(s) {}
    Value(std::string s) : type(Type::String), str(std::move(s)) {}

    static Value array() { return Value(Type::Array); }
    static Value object() { return Value(Type::Object); }

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    // Object access; auto-converts a Null value into an object.
    Value& operator[](const std::string& key) {
        if (type != Type::Object) {
            type = Type::Object;
            obj.clear();
        }
        return obj[key];
    }
    const Value* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
    const Value& at(const std::string& key) const {
        const Value* v = find(key);
        if (!v) throw std::out_of_range("json: missing key '" + key + "'");
        return *v;
    }
    double as_number(double fallback = 0.0) const {
        return type == Type::Number ? number : fallback;
    }
    bool as_bool(bool fallback = false) const {
        return type == Type::Bool ? boolean : fallback;
    }
    const std::string& as_string() const {
        if (type != Type::String) throw std::runtime_error("json: value is not a string");
        return str;
    }
    void push(Value v) {
        if (type != Type::Array) {
            type = Type::Array;
            arr.clear();
        }
        arr.push_back(std::move(v));
    }
};

namespace detail {

inline void dump_string(const std::string& s, std::string& out) {
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
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

inline void dump_number(double n, std::string& out) {
    // Doubles represent integers exactly up to 2^53 — print those as integers.
    constexpr double MAX_EXACT_INT = 9007199254740992.0;  // 2^53
    if (std::isfinite(n) && std::floor(n) == n && std::fabs(n) <= MAX_EXACT_INT) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n));
        out += buf;
    } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", n);  // full round-trip precision
        out += buf;
    }
}

inline void dump(const Value& v, std::string& out) {
    switch (v.type) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
        case Value::Type::Number: dump_number(v.number, out); break;
        case Value::Type::String: dump_string(v.str, out); break;
        case Value::Type::Array: {
            out += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out += ',';
                dump(v.arr[i], out);
            }
            out += ']';
            break;
        }
        case Value::Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& kv : v.obj) {
                if (!first) out += ',';
                first = false;
                dump_string(kv.first, out);
                out += ':';
                dump(kv.second, out);
            }
            out += '}';
            break;
        }
    }
}

inline void dump_pretty(const Value& v, std::string& out, int indent) {
    auto pad = [&](int n) { out.append(static_cast<size_t>(n) * 2, ' '); };
    switch (v.type) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
        case Value::Type::Number: dump_number(v.number, out); break;
        case Value::Type::String: dump_string(v.str, out); break;
        case Value::Type::Array: {
            if (v.arr.empty()) { out += "[]"; break; }
            out += "[\n";
            for (size_t i = 0; i < v.arr.size(); ++i) {
                pad(indent + 1);
                dump_pretty(v.arr[i], out, indent + 1);
                out += (i + 1 < v.arr.size()) ? ",\n" : "\n";
            }
            pad(indent);
            out += ']';
            break;
        }
        case Value::Type::Object: {
            if (v.obj.empty()) { out += "{}"; break; }
            out += "{\n";
            size_t i = 0;
            for (const auto& kv : v.obj) {
                pad(indent + 1);
                dump_string(kv.first, out);
                out += ": ";
                dump_pretty(kv.second, out, indent + 1);
                ++i;
                out += (i < v.obj.size()) ? ",\n" : "\n";
            }
            pad(indent);
            out += '}';
            break;
        }
    }
}

struct Parser {
    const std::string& s;
    size_t pos = 0;
    explicit Parser(const std::string& text) : s(text) {}

    [[noreturn]] void fail(const std::string& msg) {
        throw parse_error(msg + " at offset " + std::to_string(pos));
    }
    char peek() const { return pos < s.size() ? s[pos] : '\0'; }
    char get() { return pos < s.size() ? s[pos++] : '\0'; }
    void skip_ws() {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    }
    void expect(char c) {
        if (get() != c) fail(std::string("expected '") + c + "'");
    }
    void expect_lit(const char* lit) {
        for (const char* p = lit; *p; ++p) {
            if (get() != *p) fail(std::string("invalid literal, expected ") + lit);
        }
    }

    Value parse_value() {
        skip_ws();
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return Value(parse_string());
        if (c == 't') { expect_lit("true"); return Value(true); }
        if (c == 'f') { expect_lit("false"); return Value(false); }
        if (c == 'n') { expect_lit("null"); return Value(); }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number();
        fail("unexpected character");
    }

    Value parse_number() {
        size_t start = pos;
        if (peek() == '-') ++pos;
        bool any = false;
        while (std::isdigit(static_cast<unsigned char>(peek()))) { ++pos; any = true; }
        if (peek() == '.') {
            ++pos;
            while (std::isdigit(static_cast<unsigned char>(peek()))) { ++pos; any = true; }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos;
            if (peek() == '+' || peek() == '-') ++pos;
            while (std::isdigit(static_cast<unsigned char>(peek()))) { ++pos; any = true; }
        }
        if (!any) fail("invalid number");
        return Value(std::strtod(s.c_str() + start, nullptr));
    }

    static void append_utf8(std::string& out, unsigned int cp) {
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

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            char c = get();
            if (c == '\0') fail("unterminated string");
            if (c == '"') break;
            if (c == '\\') {
                char e = get();
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        unsigned int cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = get();
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned int>(h - 'A' + 10);
                            else fail("bad \\u escape");
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: fail("bad escape");
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    Value parse_array() {
        expect('[');
        Value v = Value::array();
        skip_ws();
        if (peek() == ']') { ++pos; return v; }
        while (true) {
            v.push(parse_value());
            skip_ws();
            char c = get();
            if (c == ']') break;
            if (c != ',') fail("expected ',' or ']'");
        }
        return v;
    }

    Value parse_object() {
        expect('{');
        Value v = Value::object();
        skip_ws();
        if (peek() == '}') { ++pos; return v; }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            v[key] = parse_value();
            skip_ws();
            char c = get();
            if (c == '}') break;
            if (c != ',') fail("expected ',' or '}'");
        }
        return v;
    }
};

} // namespace detail

inline std::string dump(const Value& v) {
    std::string out;
    detail::dump(v, out);
    return out;
}

inline std::string dump_pretty(const Value& v) {
    std::string out;
    detail::dump_pretty(v, out, 0);
    return out;
}

inline Value parse(const std::string& text) {
    detail::Parser p(text);
    Value v = p.parse_value();
    p.skip_ws();
    if (p.pos != text.size()) p.fail("trailing characters");
    return v;
}

} // namespace json
