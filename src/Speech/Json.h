// Minimal JSON parser for the Sherpa-ONNX model registry and the
// Hugging Face tree API responses. Deliberately small: the app only
// ever reads trusted, machine-generated JSON from the wrapper's
// embedded registry or the HF API, so a compact recursive-descent
// parser (rather than a vendored library) keeps the dependency
// surface at zero. Grammar follows RFC 8259: objects, arrays, strings
// with full escape handling (including \uXXXX surrogate pairs),
// numbers, booleans and null, with a depth cap and trailing-content
// rejection.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace Dasher {

class JsonValue {
  public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    // Parse a complete JSON document. Returns a Null value and fills
    // `error` (when non-null) on malformed input.
    static JsonValue parse(const std::string& text, std::string* error = nullptr);

    Type type() const { return m_type; }
    bool is_null() const { return m_type == Type::Null; }
    bool is_bool() const { return m_type == Type::Bool; }
    bool is_number() const { return m_type == Type::Number; }
    bool is_string() const { return m_type == Type::String; }
    bool is_array() const { return m_type == Type::Array; }
    bool is_object() const { return m_type == Type::Object; }

    // Accessors with defaults — reading the wrong type yields the
    // default rather than throwing, so a schema drift in the registry
    // degrades to "field missing" instead of a crash.
    bool as_bool(bool def = false) const;
    double as_number(double def = 0.0) const;
    const std::string& as_string() const; // empty when not a string

    const std::vector<JsonValue>& items() const;            // array
    const std::map<std::string, JsonValue>& fields() const; // object

    // Object field lookup; null when absent.
    const JsonValue* find(const std::string& key) const;

    // Convenience: object field as string / number with default.
    std::string get_string(const std::string& key, const std::string& def = "") const;
    double get_number(const std::string& key, double def = 0.0) const;
    bool get_bool(const std::string& key, bool def = false) const;

  private:
    // The parser (JsonParser, defined in Json.cpp) fills the members
    // directly; declared as a named friend because anonymous-namespace
    // classes cannot be named in a friend declaration.
    friend class JsonParser;

    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0.0;
    std::string m_string;
    std::vector<JsonValue> m_items;
    std::map<std::string, JsonValue> m_fields;
};

} // namespace Dasher
