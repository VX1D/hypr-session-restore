
#pragma once

#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace hsrjson {

inline constexpr int MAX_JSON_DEPTH = 64;

struct Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

struct Value {
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v;

    Value() : v(nullptr) {}
    Value(bool b) : v(b) {}
    Value(int i) : v(static_cast<double>(i)) {}
    Value(double d) : v(d) {}
    Value(const char* s) : v(std::string(s)) {}
    Value(std::string s) : v(std::move(s)) {}
    Value(Array a) : v(std::move(a)) {}
    Value(Object o) : v(std::move(o)) {}

    bool isObject() const {
        return std::holds_alternative<Object>(v);
    }
    bool isArray() const {
        return std::holds_alternative<Array>(v);
    }
    bool isString() const {
        return std::holds_alternative<std::string>(v);
    }
    bool isNumber() const {
        return std::holds_alternative<double>(v);
    }
    bool isBool() const {
        return std::holds_alternative<bool>(v);
    }
    bool isNull() const {
        return std::holds_alternative<std::nullptr_t>(v);
    }

    const Object& obj() const {
        return std::get<Object>(v);
    }
    const Array& arr() const {
        return std::get<Array>(v);
    }
    const std::string& str() const {
        return std::get<std::string>(v);
    }
    double num() const {
        return std::get<double>(v);
    }
    bool boolean() const {
        return std::get<bool>(v);
    }

    Object& obj() {
        return std::get<Object>(v);
    }
    Array& arr() {
        return std::get<Array>(v);
    }
    std::string& str() {
        return std::get<std::string>(v);
    }

    std::string strOr(std::string_view fallback) const {
        return isString() ? str() : std::string(fallback);
    }
    double numOr(double fallback) const {
        return isNumber() ? num() : fallback;
    }
    bool boolOr(bool fallback) const {
        return isBool() ? boolean() : fallback;
    }

    const Value* find(std::string_view key) const {
        if (!isObject()) {
            return nullptr;
        }
        auto it = obj().find(std::string(key));
        return it == obj().end() ? nullptr : &it->second;
    }
};

inline void appendChecked(std::ostringstream& o, const char* buffer, int written, size_t capacity) {
    if (written > 0 && static_cast<size_t>(written) < capacity) {
        o << buffer;
    }
}

inline void writeStr(std::ostringstream& o, const std::string& s) {
    o << '"';
    for (char c : s) {
        switch (c) {
        case '"':
            o << "\\\"";
            break;
        case '\\':
            o << "\\\\";
            break;
        case '\n':
            o << "\\n";
            break;
        case '\r':
            o << "\\r";
            break;
        case '\t':
            o << "\\t";
            break;
        case '\b':
            o << "\\b";
            break;
        case '\f':
            o << "\\f";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                std::array<char, 8> buf{};
                int written =
                    std::snprintf(buf.data(), buf.size(), "\\u%04x", static_cast<unsigned char>(c));
                appendChecked(o, buf.data(), written, buf.size());
            } else {
                o << c;
            }
        }
    }
    o << '"';
}

inline void writeIndent(std::ostringstream& o, int indent, int depth) {
    for (int i = 0; i < depth * indent; ++i) {
        o << ' ';
    }
}

inline void writeNumber(std::ostringstream& o, double value) {
    if (!std::isfinite(value)) {
        o << "null";
        return;
    }
    double intPart = 0.0;
    if (std::modf(value, &intPart) == 0.0 && value > -1e15 && value < 1e15) {
        o << static_cast<int64_t>(value);
        return;
    }
    std::array<char, 64> buf{};
    int written = std::snprintf(buf.data(), buf.size(), "%.6f", value);
    appendChecked(o, buf.data(), written, buf.size());
}

inline void writeValue(std::ostringstream& o, const Value& v, int indent, int depth);

inline void writeArray(std::ostringstream& o, const Array& values, int indent, int depth) {
    if (values.empty()) {
        o << "[]";
        return;
    }
    o << '[';
    for (size_t index = 0; index < values.size(); ++index) {
        if (indent != 0) {
            o << '\n';
            writeIndent(o, indent, depth + 1);
        }
        writeValue(o, values[index], indent, depth + 1);
        if (index + 1 < values.size()) {
            o << ',';
        }
    }
    if (indent != 0) {
        o << '\n';
        writeIndent(o, indent, depth);
    }
    o << ']';
}

inline void writeObject(std::ostringstream& o, const Object& values, int indent, int depth) {
    if (values.empty()) {
        o << "{}";
        return;
    }
    o << '{';
    size_t index = 0;
    for (const auto& [key, value] : values) {
        if (indent != 0) {
            o << '\n';
            writeIndent(o, indent, depth + 1);
        }
        writeStr(o, key);
        o << (indent != 0 ? ": " : ":");
        writeValue(o, value, indent, depth + 1);
        if (++index < values.size()) {
            o << ',';
        }
    }
    if (indent != 0) {
        o << '\n';
        writeIndent(o, indent, depth);
    }
    o << '}';
}

inline void writeValue(std::ostringstream& o, const Value& v, int indent, int depth) {
    if (v.isNull()) {
        o << "null";
        return;
    }
    if (v.isBool()) {
        o << (v.boolean() ? "true" : "false");
        return;
    }
    if (v.isNumber()) {
        writeNumber(o, v.num());
        return;
    }
    if (v.isString()) {
        writeStr(o, v.str());
        return;
    }
    if (v.isArray()) {
        writeArray(o, v.arr(), indent, depth);
        return;
    }
    if (v.isObject()) {
        writeObject(o, v.obj(), indent, depth);
    }
}

inline std::string serialize(const Value& v, int indent = 2) {
    std::ostringstream o;
    writeValue(o, v, indent, 0);
    return o.str();
}

class Parser {
  public:
    explicit Parser(std::string_view src) : m_src(src) {}
    std::optional<Value> parse() {
        skipWs();
        auto v = parseValue(0);
        if (!v) {
            return std::nullopt;
        }
        skipWs();
        if (m_pos != m_src.size()) {
            return std::nullopt;
        }
        return v;
    }

  private:
    std::string_view m_src;
    size_t m_pos = 0;

    void skipWs() {
        while (m_pos < m_src.size() && (m_src[m_pos] == ' ' || m_src[m_pos] == '\n' ||
                                        m_src[m_pos] == '\r' || m_src[m_pos] == '\t')) {
            ++m_pos;
        }
    }
    bool consume(char c) {
        skipWs();
        if (m_pos < m_src.size() && m_src[m_pos] == c) {
            ++m_pos;
            return true;
        }
        return false;
    }
    bool consume(std::string_view s) {
        skipWs();
        if (m_pos + s.size() <= m_src.size() && m_src.compare(m_pos, s.size(), s) == 0) {
            m_pos += s.size();
            return true;
        }
        return false;
    }

    std::optional<Value> parseValue(int depth) {
        if (depth > MAX_JSON_DEPTH) {
            return std::nullopt;
        }
        skipWs();
        if (m_pos >= m_src.size()) {
            return std::nullopt;
        }
        char c = m_src[m_pos];
        if (c == '{') {
            return parseObject(depth + 1);
        }
        if (c == '[') {
            return parseArray(depth + 1);
        }
        if (c == '"') {
            return parseString();
        }
        if (c == 't' || c == 'f') {
            return parseBool();
        }
        if (c == 'n') {
            if (consume("null")) {
                return Value();
            }
            return std::nullopt;
        }
        return parseNumber();
    }

    std::optional<Value> parseString() {
        if (!consume('"')) {
            return std::nullopt;
        }
        std::string out;
        while (m_pos < m_src.size()) {
            char c = m_src[m_pos++];
            if (c == '"') {
                return Value(std::move(out));
            }
            if (c == '\\') {
                if (m_pos >= m_src.size()) {
                    return std::nullopt;
                }
                char esc = m_src[m_pos++];
                switch (esc) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'u': {
                    if (m_pos + 4 > m_src.size()) {
                        return std::nullopt;
                    }
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = m_src[m_pos++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') {
                            cp |= h - '0';
                        } else if (h >= 'a' && h <= 'f') {
                            cp |= h - 'a' + 10;
                        } else if (h >= 'A' && h <= 'F') {
                            cp |= h - 'A' + 10;
                        } else {
                            return std::nullopt;
                        }
                    }
                    if (cp < 0x80) {
                        out += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default:
                    return std::nullopt;
                }
            } else {
                out += c;
            }
        }
        return std::nullopt;
    }

    std::optional<Value> parseNumber() {
        size_t start = m_pos;
        if (m_pos < m_src.size() && m_src[m_pos] == '-') {
            ++m_pos;
        }
        while (m_pos < m_src.size() &&
               (std::isdigit(static_cast<unsigned char>(m_src[m_pos])) || m_src[m_pos] == '.' ||
                m_src[m_pos] == 'e' || m_src[m_pos] == 'E' || m_src[m_pos] == '-' ||
                m_src[m_pos] == '+')) {
            ++m_pos;
        }
        if (start == m_pos) {
            return std::nullopt;
        }
        std::string num(m_src.substr(start, m_pos - start));
        char* end = nullptr;
        double d = std::strtod(num.c_str(), &end);
        if (end != num.c_str() + num.size()) {
            return std::nullopt;
        }
        return Value(d);
    }

    std::optional<Value> parseBool() {
        if (consume("true")) {
            return Value(true);
        }
        if (consume("false")) {
            return Value(false);
        }
        return std::nullopt;
    }

    std::optional<Value> parseArray(int depth) {
        if (!consume('[')) {
            return std::nullopt;
        }
        Array out;
        skipWs();
        if (consume(']')) {
            return Value(std::move(out));
        }
        while (true) {
            auto v = parseValue(depth);
            if (!v) {
                return std::nullopt;
            }
            out.push_back(std::move(*v));
            skipWs();
            if (consume(']')) {
                return Value(std::move(out));
            }
            if (!consume(',')) {
                return std::nullopt;
            }
        }
    }

    std::optional<Value> parseObject(int depth) {
        if (!consume('{')) {
            return std::nullopt;
        }
        Object out;
        skipWs();
        if (consume('}')) {
            return Value(std::move(out));
        }
        while (true) {
            skipWs();
            auto key = parseString();
            if (!key) {
                return std::nullopt;
            }
            if (!consume(':')) {
                return std::nullopt;
            }
            auto val = parseValue(depth);
            if (!val) {
                return std::nullopt;
            }
            out.emplace(std::move(key->str()), std::move(*val));
            skipWs();
            if (consume('}')) {
                return Value(std::move(out));
            }
            if (!consume(',')) {
                return std::nullopt;
            }
        }
    }
};

inline std::optional<Value> parse(std::string_view src) {
    return Parser(src).parse();
}

} // namespace hsrjson
