#pragma once

// Canonical JSON: the single encoding used for durable state, plans, reports, and
// every digest the library publishes.
//
// The accepted subset is deliberately narrow so that one value has exactly one byte
// sequence: integers only (no floating point, no exponents), object keys sorted by
// byte order, no duplicate keys, no insignificant whitespace, and UTF-8 validated on
// both encode and decode. Two equal values therefore always produce equal bytes and
// equal digests, independent of map order, hash order, or thread timing.

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "black_start_manager/error.hpp"
#include "black_start_manager/limits.hpp"

namespace black_start_manager {

class JsonValue {
 public:
  enum class Kind : std::uint8_t { Null = 0, Bool, Integer, String, Array, Object };

  using Array = std::vector<JsonValue>;
  using Field = std::pair<std::string, JsonValue>;
  using Object = std::vector<Field>;  // sorted by key, keys unique

  JsonValue() noexcept = default;

  [[nodiscard]] static JsonValue null() noexcept { return JsonValue(); }
  [[nodiscard]] static JsonValue boolean(bool value) noexcept;
  [[nodiscard]] static JsonValue integer(std::int64_t value) noexcept;
  [[nodiscard]] static Result<JsonValue> unsigned_integer(std::uint64_t value);
  [[nodiscard]] static JsonValue string(std::string value);
  [[nodiscard]] static Result<JsonValue> array(Array values);
  [[nodiscard]] static Result<JsonValue> object(Object fields);
  [[nodiscard]] static Result<JsonValue> object(std::initializer_list<Field> fields);

  [[nodiscard]] Kind kind() const noexcept { return static_cast<Kind>(storage_.index()); }
  [[nodiscard]] bool is_null() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] bool is_bool() const noexcept { return storage_.index() == 1; }
  [[nodiscard]] bool is_integer() const noexcept { return storage_.index() == 2; }
  [[nodiscard]] bool is_string() const noexcept { return storage_.index() == 3; }
  [[nodiscard]] bool is_array() const noexcept { return storage_.index() == 4; }
  [[nodiscard]] bool is_object() const noexcept { return storage_.index() == 5; }

  // Accessors are preconditions: callers check kind() first, as every decoder in
  // this library does. A violated precondition terminates deterministically.
  [[nodiscard]] bool as_bool() const;
  [[nodiscard]] std::int64_t as_integer() const;
  [[nodiscard]] const std::string& as_string() const;
  [[nodiscard]] const Array& as_array() const;
  [[nodiscard]] const Object& as_object() const;

  // Binary search over the sorted object storage. Returns nullptr when absent or
  // when the value is not an object.
  [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept;

  friend bool operator==(const JsonValue& left, const JsonValue& right) noexcept;
  friend bool operator!=(const JsonValue& left, const JsonValue& right) noexcept {
    return !(left == right);
  }

 private:
  using Storage =
      std::variant<std::nullptr_t, bool, std::int64_t, std::string, Array, Object>;
  Storage storage_{};
};

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

void append_canonical_json(const JsonValue& value, std::string& out);
[[nodiscard]] std::string canonical_json(const JsonValue& value);

struct JsonParseOptions {
  std::size_t max_depth = limits::kMaxJsonDepth;
  std::size_t max_bytes = limits::kMaxJsonBytes;
  // Strict decoding rejects any field the caller did not declare, which is what
  // makes silent schema drift impossible. Decoders call json::reject_unknown_fields
  // explicitly; this switch only controls duplicate-key rejection, which is always
  // on because a duplicate key is ambiguous input.
};

// Strict decoder for the canonical subset. Trailing data, duplicate keys, control
// characters in strings, invalid UTF-8, fractional or exponent numbers, values that
// do not fit a signed 64-bit integer, and inputs beyond the configured depth or size
// are all refused with a specific ErrorCode.
[[nodiscard]] Result<JsonValue> parse_json(std::string_view text,
                                           JsonParseOptions options = {});

// Structured field access for strict decoders.
namespace json {

[[nodiscard]] bool has_key(const JsonValue& object, std::string_view key) noexcept;
[[nodiscard]] Result<const JsonValue*> require_field(const JsonValue& object,
                                                     std::string_view key);
[[nodiscard]] const JsonValue* optional_field(const JsonValue& object,
                                              std::string_view key) noexcept;
[[nodiscard]] Result<std::int64_t> require_integer(const JsonValue& object,
                                                   std::string_view key);
[[nodiscard]] Result<std::uint64_t> require_unsigned(const JsonValue& object,
                                                     std::string_view key);
[[nodiscard]] Result<bool> require_bool(const JsonValue& object, std::string_view key);
[[nodiscard]] Result<std::string> require_string(const JsonValue& object,
                                                 std::string_view key);
[[nodiscard]] Result<const JsonValue::Array*> require_array(const JsonValue& object,
                                                            std::string_view key);
[[nodiscard]] Result<const JsonValue::Object*> require_object(const JsonValue& object,
                                                              std::string_view key);
[[nodiscard]] Result<const JsonValue*> require_object_member(const JsonValue& object,
                                                             std::string_view key);
[[nodiscard]] Result<Unit> reject_unknown_fields(
    const JsonValue& object, std::initializer_list<std::string_view> known);
[[nodiscard]] Result<std::string> require_text(const JsonValue& value,
                                               std::string_view what,
                                               std::size_t max_length);

}  // namespace json

// Deterministic object/array construction. A builder records the first error and
// reports it from build() instead of throwing, so construction stays total.
class JsonObjectBuilder {
 public:
  JsonObjectBuilder& set(std::string_view key, JsonValue value);
  JsonObjectBuilder& set_text(std::string_view key, std::string_view value);
  JsonObjectBuilder& set_bool(std::string_view key, bool value);
  JsonObjectBuilder& set_int(std::string_view key, std::int64_t value);
  JsonObjectBuilder& set_uint(std::string_view key, std::uint64_t value);
  JsonObjectBuilder& set_null(std::string_view key);

  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] Result<JsonValue> build() const;

 private:
  JsonValue::Object fields_;
  bool failed_ = false;
  Error error_;
};

class JsonArrayBuilder {
 public:
  JsonArrayBuilder& push(JsonValue value);
  JsonArrayBuilder& push_text(std::string_view value);
  JsonArrayBuilder& push_int(std::int64_t value);

  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] Result<JsonValue> build() const;

 private:
  JsonValue::Array values_;
  bool failed_ = false;
  Error error_;
};

}  // namespace black_start_manager
