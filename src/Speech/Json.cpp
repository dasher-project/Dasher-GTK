#include "Json.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace Dasher {

// Parser state: recursive descent over the document with a hard depth
// cap so hostile input cannot blow the stack (the registry nests 4
// deep; the cap leaves generous headroom).
class JsonParser {
  public:
    JsonParser(const std::string& text, std::string* error) : m_text(text), m_error(error) {}

    JsonValue run() {
        skip_ws();
        JsonValue v = parse_value(0);
        if (!failed()) {
            skip_ws();
            if (m_pos != m_text.size()) fail("trailing content after document");
        }
        return v;
    }

  private:
    const std::string& m_text;
    std::string* m_error;
    size_t m_pos = 0;
    bool m_failed = false;
    static constexpr int kMaxDepth = 64;

    bool failed() const { return m_failed; }
    void fail(const char* message) {
        if (!m_failed) {
            m_failed = true;
            if (m_error) *m_error = message;
        }
    }

    void skip_ws() {
        while (m_pos < m_text.size()) {
            char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                m_pos++;
            else
                break;
        }
    }

    bool peek(char c) const { return m_pos < m_text.size() && m_text[m_pos] == c; }

    bool consume(char c) {
        if (peek(c)) {
            m_pos++;
            return true;
        }
        return false;
    }

    JsonValue parse_value(int depth) {
        if (depth > kMaxDepth) {
            fail("nesting too deep");
            return JsonValue();
        }
        skip_ws();
        if (m_pos >= m_text.size()) {
            fail("unexpected end of input");
            return JsonValue();
        }
        char c = m_text[m_pos];
        switch (c) {
        case '{':
            return parse_object(depth);
        case '[':
            return parse_array(depth);
        case '"': {
            JsonValue v;
            v.m_type = JsonValue::Type::String;
            v.m_string = parse_string();
            return v;
        }
        case 't':
            if (m_text.compare(m_pos, 4, "true") == 0) {
                m_pos += 4;
                JsonValue v;
                v.m_type = JsonValue::Type::Bool;
                v.m_bool = true;
                return v;
            }
            fail("invalid literal");
            return JsonValue();
        case 'f':
            if (m_text.compare(m_pos, 5, "false") == 0) {
                m_pos += 5;
                JsonValue v;
                v.m_type = JsonValue::Type::Bool;
                v.m_bool = false;
                return v;
            }
            fail("invalid literal");
            return JsonValue();
        case 'n':
            if (m_text.compare(m_pos, 4, "null") == 0) {
                m_pos += 4;
                return JsonValue();
            }
            fail("invalid literal");
            return JsonValue();
        default:
            return parse_number();
        }
    }

    JsonValue parse_object(int depth) {
        JsonValue v;
        v.m_type = JsonValue::Type::Object;
        m_pos++; // '{'
        skip_ws();
        if (consume('}')) return v;
        while (!failed()) {
            skip_ws();
            if (!peek('"')) {
                fail("expected object key");
                break;
            }
            std::string key = parse_string();
            if (failed()) break;
            skip_ws();
            if (!consume(':')) {
                fail("expected ':'");
                break;
            }
            JsonValue value = parse_value(depth + 1);
            if (failed()) break;
            v.m_fields[std::move(key)] = std::move(value);
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            fail("expected ',' or '}'");
        }
        return v;
    }

    JsonValue parse_array(int depth) {
        JsonValue v;
        v.m_type = JsonValue::Type::Array;
        m_pos++; // '['
        skip_ws();
        if (consume(']')) return v;
        while (!failed()) {
            v.m_items.push_back(parse_value(depth + 1));
            if (failed()) break;
            skip_ws();
            if (consume(',')) continue;
            if (consume(']')) break;
            fail("expected ',' or ']'");
        }
        return v;
    }

    static void append_utf8(std::string& out, unsigned int cp) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
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

    unsigned int parse_hex4() {
        unsigned int value = 0;
        for (int i = 0; i < 4; i++) {
            if (m_pos >= m_text.size()) {
                fail("truncated \\u escape");
                return 0;
            }
            char c = m_text[m_pos++];
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<unsigned int>(c - 'A' + 10);
            else {
                fail("invalid \\u escape");
                return 0;
            }
        }
        return value;
    }

    std::string parse_string() {
        std::string out;
        m_pos++; // opening quote
        while (m_pos < m_text.size()) {
            char c = m_text[m_pos++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("control character in string");
                return out;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_pos >= m_text.size()) {
                fail("truncated escape");
                return out;
            }
            char e = m_text[m_pos++];
            switch (e) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
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
            case 'u': {
                unsigned int cp = parse_hex4();
                if (failed()) return out;
                // Surrogate pair: high surrogate must be followed by
                // \u-escaped low surrogate.
                if (cp >= 0xD800 && cp <= 0xDBFF && m_pos + 1 < m_text.size() && m_text[m_pos] == '\\' &&
                    m_text[m_pos + 1] == 'u') {
                    m_pos += 2;
                    unsigned int lo = parse_hex4();
                    if (failed()) return out;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        fail("invalid surrogate pair");
                        return out;
                    }
                }
                append_utf8(out, cp);
                break;
            }
            default:
                fail("invalid escape");
                return out;
            }
        }
        fail("unterminated string");
        return out;
    }

    JsonValue parse_number() {
        const char* start = m_text.c_str() + m_pos;
        char* end = nullptr;
        double value = std::strtod(start, &end);
        if (end == start) {
            fail("invalid number");
            return JsonValue();
        }
        JsonValue v;
        v.m_type = JsonValue::Type::Number;
        v.m_number = value;
        m_pos += static_cast<size_t>(end - start);
        return v;
    }
};

JsonValue JsonValue::parse(const std::string& text, std::string* error) {
    if (error) error->clear();
    return JsonParser(text, error).run();
}

bool JsonValue::as_bool(bool def) const {
    return m_type == Type::Bool ? m_bool : def;
}
double JsonValue::as_number(double def) const {
    return m_type == Type::Number ? m_number : def;
}
const std::string& JsonValue::as_string() const {
    static const std::string empty;
    return m_type == Type::String ? m_string : empty;
}

const std::vector<JsonValue>& JsonValue::items() const {
    static const std::vector<JsonValue> empty;
    return m_type == Type::Array ? m_items : empty;
}

const std::map<std::string, JsonValue>& JsonValue::fields() const {
    static const std::map<std::string, JsonValue> empty;
    return m_type == Type::Object ? m_fields : empty;
}

const JsonValue* JsonValue::find(const std::string& key) const {
    if (m_type != Type::Object) return nullptr;
    auto it = m_fields.find(key);
    return it == m_fields.end() ? nullptr : &it->second;
}

std::string JsonValue::get_string(const std::string& key, const std::string& def) const {
    const JsonValue* v = find(key);
    return (v && v->is_string()) ? v->as_string() : def;
}

double JsonValue::get_number(const std::string& key, double def) const {
    const JsonValue* v = find(key);
    return (v && v->is_number()) ? v->as_number() : def;
}

bool JsonValue::get_bool(const std::string& key, bool def) const {
    const JsonValue* v = find(key);
    return (v && v->is_bool()) ? v->as_bool() : def;
}

} // namespace Dasher
