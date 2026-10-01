// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "black_start_manager/canonical.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace black_start_manager {
namespace {

[[nodiscard]] bool is_hex_digit(char character) noexcept {
  return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
         (character >= 'A' && character <= 'F');
}

[[nodiscard]] std::uint32_t hex_value(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return static_cast<std::uint32_t>(character - '0');
  }
  if (character >= 'a' && character <= 'f') {
    return static_cast<std::uint32_t>(character - 'a' + 10);
  }
  return static_cast<std::uint32_t>(character - 'A' + 10);
}

// Decodes one UTF-8 sequence at text[index]. Refuses overlong encodings, surrogate
// code points, values above U+10FFFF, and truncated sequences.
[[nodiscard]] bool decode_utf8(std::string_view text, std::size_t& index,
                               std::uint32_t& code_point) noexcept {
  if (index >= text.size()) {
    return false;
  }
  const auto byte_at = [&text](std::size_t position) {
    return static_cast<std::uint8_t>(text[position]);
  };
  const std::uint8_t first = byte_at(index);
  if (first < 0x80u) {
    code_point = first;
    index += 1;
    return true;
  }

  std::size_t length = 0;
  std::uint32_t value = 0;
  std::uint32_t minimum = 0;
  if ((first & 0xe0u) == 0xc0u) {
    length = 2;
    value = first & 0x1fu;
    minimum = 0x80u;
  } else if ((first & 0xf0u) == 0xe0u) {
    length = 3;
    value = first & 0x0fu;
    minimum = 0x800u;
  } else if ((first & 0xf8u) == 0xf0u) {
    length = 4;
    value = first & 0x07u;
    minimum = 0x10000u;
  } else {
    return false;
  }
  if (index + length > text.size()) {
    return false;
  }
  for (std::size_t offset = 1; offset < length; ++offset) {
    const std::uint8_t continuation = byte_at(index + offset);
    if ((continuation & 0xc0u) != 0x80u) {
      return false;
    }
    value = (value << 6u) | (continuation & 0x3fu);
  }
  if (value < minimum || value > 0x10ffffu) {
    return false;
  }
  if (value >= 0xd800u && value <= 0xdfffu) {
    return false;
  }
  code_point = value;
  index += length;
  return true;
}

void append_utf8(std::uint32_t code_point, std::string& out) {
  if (code_point < 0x80u) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800u) {
    out.push_back(static_cast<char>(0xc0u | (code_point >> 6u)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
  } else if (code_point < 0x10000u) {
    out.push_back(static_cast<char>(0xe0u | (code_point >> 12u)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6u) & 0x3fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
  } else {
    out.push_back(static_cast<char>(0xf0u | (code_point >> 18u)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 12u) & 0x3fu)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6u) & 0x3fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
  }
}

[[nodiscard]] Error parse_error(ErrorCode code, std::string message, std::size_t offset) {
  JsonObjectBuilder detail;
  detail.set_uint("byte_offset", static_cast<std::uint64_t>(offset));
  const Result<JsonValue> value = detail.build();
  Error error(code, std::move(message));
  if (value.ok()) {
    return error.with_detail(value.value());
  }
  return error;
}

class Parser {
 public:
  Parser(std::string_view text, JsonParseOptions options)
      : text_(text), max_depth_(options.max_depth) {}

  [[nodiscard]] Result<JsonValue> parse_document() {
    if (text_.empty()) {
      return parse_error(ErrorCode::TruncatedInput, "input is empty", 0);
    }
    if (text_.size() > limits::kMaxJsonBytes) {
      return parse_error(ErrorCode::LimitExceeded, "input exceeds the supported size", 0);
    }
    skip_whitespace();
    BSM_TRY_ASSIGN(value, parse_value());
    skip_whitespace();
    if (pos_ != text_.size()) {
      return parse_error(ErrorCode::TrailingData,
                         "input contains data after the first value", pos_);
    }
    return value;
  }

 private:
  void skip_whitespace() noexcept {
    while (pos_ < text_.size()) {
      const char character = text_[pos_];
      if (character == ' ' || character == '\t' || character == '\n' ||
          character == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  [[nodiscard]] char peek() const noexcept {
    return pos_ < text_.size() ? text_[pos_] : '\0';
  }

  [[nodiscard]] Result<JsonValue> parse_value() {
    if (pos_ >= text_.size()) {
      return parse_error(ErrorCode::TruncatedInput, "value is missing", pos_);
    }
    switch (peek()) {
      case '{':
        return parse_object();
      case '[':
        return parse_array();
      case '"': {
        BSM_TRY_ASSIGN(text, parse_string());
        return JsonValue::string(std::move(text));
      }
      case 't':
        return parse_literal("true", JsonValue::boolean(true));
      case 'f':
        return parse_literal("false", JsonValue::boolean(false));
      case 'n':
        return parse_literal("null", JsonValue::null());
      default:
        break;
    }
    if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
      return parse_number();
    }
    return parse_error(ErrorCode::InvalidEncoding, "value does not start a valid token",
                       pos_);
  }

  [[nodiscard]] Result<JsonValue> parse_literal(std::string_view literal,
                                                JsonValue value) {
    if (text_.compare(pos_, literal.size(), literal) != 0) {
      return parse_error(ErrorCode::InvalidEncoding, "invalid literal", pos_);
    }
    pos_ += literal.size();
    return value;
  }

  [[nodiscard]] Result<JsonValue> parse_object() {
    if (depth_ >= max_depth_) {
      return parse_error(ErrorCode::LimitExceeded, "object nesting is too deep", pos_);
    }
    ++depth_;
    ++pos_;  // consume '{'
    JsonValue::Object fields;
    skip_whitespace();
    if (peek() == '}') {
      ++pos_;
      --depth_;
      return JsonValue::object(std::move(fields));
    }
    while (true) {
      skip_whitespace();
      if (peek() != '"') {
        return parse_error(ErrorCode::InvalidEncoding, "object key must be a string",
                           pos_);
      }
      BSM_TRY_ASSIGN(key, parse_string());
      skip_whitespace();
      if (peek() != ':') {
        return parse_error(ErrorCode::InvalidEncoding,
                           "object key must be followed by ':'", pos_);
      }
      ++pos_;
      skip_whitespace();
      BSM_TRY_ASSIGN(value, parse_value());
      if (fields.size() >= limits::kMaxObjectFields) {
        return parse_error(ErrorCode::LimitExceeded, "object has too many fields", pos_);
      }
      fields.emplace_back(std::move(key), std::move(value));
      skip_whitespace();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == '}') {
        ++pos_;
        break;
      }
      return parse_error(ErrorCode::InvalidEncoding,
                         "object must continue with ',' or '}'", pos_);
    }
    --depth_;
    BSM_TRY_ASSIGN(result, JsonValue::object(std::move(fields)));
    return result;
  }

  [[nodiscard]] Result<JsonValue> parse_array() {
    if (depth_ >= max_depth_) {
      return parse_error(ErrorCode::LimitExceeded, "array nesting is too deep", pos_);
    }
    ++depth_;
    ++pos_;  // consume '['
    JsonValue::Array values;
    skip_whitespace();
    if (peek() == ']') {
      ++pos_;
      --depth_;
      return JsonValue::array(std::move(values));
    }
    while (true) {
      skip_whitespace();
      BSM_TRY_ASSIGN(value, parse_value());
      if (values.size() >= limits::kMaxArrayElements) {
        return parse_error(ErrorCode::LimitExceeded, "array has too many elements", pos_);
      }
      values.push_back(std::move(value));
      skip_whitespace();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        break;
      }
      return parse_error(ErrorCode::InvalidEncoding,
                         "array must continue with ',' or ']'", pos_);
    }
    --depth_;
    BSM_TRY_ASSIGN(result, JsonValue::array(std::move(values)));
    return result;
  }

  [[nodiscard]] Result<std::string> parse_string() {
    const std::size_t start = pos_;
    ++pos_;  // consume opening quote
    std::string out;
    while (true) {
      if (pos_ >= text_.size()) {
        return parse_error(ErrorCode::TruncatedInput, "string is not terminated", start);
      }
      const std::uint8_t byte = static_cast<std::uint8_t>(text_[pos_]);
      if (byte == static_cast<std::uint8_t>('"')) {
        ++pos_;
        break;
      }
      if (byte == static_cast<std::uint8_t>('\\')) {
        ++pos_;
        if (pos_ >= text_.size()) {
          return parse_error(ErrorCode::TruncatedInput, "escape is not terminated", pos_);
        }
        const char escape = text_[pos_];
        switch (escape) {
          case '"': out.push_back('"'); ++pos_; break;
          case '\\': out.push_back('\\'); ++pos_; break;
          case '/': out.push_back('/'); ++pos_; break;
          case 'b': out.push_back('\b'); ++pos_; break;
          case 'f': out.push_back('\f'); ++pos_; break;
          case 'n': out.push_back('\n'); ++pos_; break;
          case 'r': out.push_back('\r'); ++pos_; break;
          case 't': out.push_back('\t'); ++pos_; break;
          case 'u': {
            if (pos_ + 4 >= text_.size()) {
              return parse_error(ErrorCode::TruncatedInput,
                                 "unicode escape is not terminated", pos_);
            }
            for (std::size_t offset = 1; offset <= 4; ++offset) {
              if (!is_hex_digit(text_[pos_ + offset])) {
                return parse_error(ErrorCode::InvalidEncoding,
                                   "unicode escape contains a non-hexadecimal digit",
                                   pos_ + offset);
              }
            }
            std::uint32_t code_point = 0;
            for (std::size_t offset = 1; offset <= 4; ++offset) {
              code_point = (code_point << 4u) | hex_value(text_[pos_ + offset]);
            }
            pos_ += 5;
            if (code_point >= 0xd800u && code_point <= 0xdbffu) {
              if (pos_ + 5 >= text_.size() || text_[pos_] != '\\' ||
                  text_[pos_ + 1] != 'u') {
                return parse_error(ErrorCode::InvalidEncoding,
                                   "high surrogate is not followed by a low surrogate",
                                   pos_);
              }
              for (std::size_t offset = 2; offset <= 5; ++offset) {
                if (!is_hex_digit(text_[pos_ + offset])) {
                  return parse_error(
                      ErrorCode::InvalidEncoding,
                      "unicode escape contains a non-hexadecimal digit", pos_ + offset);
                }
              }
              std::uint32_t low = 0;
              for (std::size_t offset = 2; offset <= 5; ++offset) {
                low = (low << 4u) | hex_value(text_[pos_ + offset]);
              }
              if (low < 0xdc00u || low > 0xdfffu) {
                return parse_error(ErrorCode::InvalidEncoding,
                                   "high surrogate is not followed by a low surrogate",
                                   pos_);
              }
              pos_ += 6;
              code_point = 0x10000u + ((code_point - 0xd800u) << 10u) + (low - 0xdc00u);
            } else if (code_point >= 0xdc00u && code_point <= 0xdfffu) {
              return parse_error(ErrorCode::InvalidEncoding,
                                 "low surrogate appears without a high surrogate", pos_);
            }
            append_utf8(code_point, out);
            break;
          }
          default:
            return parse_error(ErrorCode::InvalidEncoding, "unknown escape sequence",
                               pos_);
        }
      } else if (byte < 0x20u) {
        return parse_error(ErrorCode::InvalidEncoding,
                           "raw control character in string", pos_);
      } else if (byte < 0x80u) {
        out.push_back(static_cast<char>(byte));
        ++pos_;
      } else {
        const std::size_t sequence_start = pos_;
        std::uint32_t code_point = 0;
        if (!decode_utf8(text_, pos_, code_point)) {
          return parse_error(ErrorCode::InvalidUtf8, "string contains invalid UTF-8",
                             sequence_start);
        }
        out.append(text_.substr(sequence_start, pos_ - sequence_start));
      }
      if (out.size() > limits::kMaxStringBytes) {
        return parse_error(ErrorCode::LimitExceeded, "string exceeds the supported size",
                           start);
      }
    }
    return out;
  }

  [[nodiscard]] Result<JsonValue> parse_number() {
    const std::size_t start = pos_;
    bool negative = false;
    if (peek() == '-') {
      negative = true;
      ++pos_;
    }
    if (pos_ >= text_.size() || peek() < '0' || peek() > '9') {
      return parse_error(ErrorCode::InvalidEncoding, "number has no digits", start);
    }
    if (peek() == '0') {
      ++pos_;
      if (pos_ < text_.size() && peek() >= '0' && peek() <= '9') {
        return parse_error(ErrorCode::InvalidEncoding, "number has a leading zero", start);
      }
      if (negative) {
        return parse_error(ErrorCode::InvalidEncoding,
                           "negative zero is not part of the canonical subset", start);
      }
    } else {
      while (pos_ < text_.size() && peek() >= '0' && peek() <= '9') {
        ++pos_;
      }
    }
    if (pos_ < text_.size() && (peek() == '.' || peek() == 'e' || peek() == 'E')) {
      return parse_error(ErrorCode::InvalidEncoding,
                         "fractional and exponent notation is not part of the canonical "
                         "subset",
                         pos_);
    }

    const std::uint64_t limit = negative ? 9223372036854775808ull : 9223372036854775807ull;
    std::uint64_t value = 0;
    for (std::size_t index = start + (negative ? 1 : 0); index < pos_; ++index) {
      const std::uint64_t digit = static_cast<std::uint64_t>(text_[index] - '0');
      if (value > (limit - digit) / 10ull) {
        return parse_error(ErrorCode::NumberOutOfRange,
                           "integer does not fit a signed 64-bit value", start);
      }
      value = value * 10ull + digit;
    }
    if (negative) {
      if (value == 9223372036854775808ull) {
        return JsonValue::integer(std::numeric_limits<std::int64_t>::min());
      }
      return JsonValue::integer(-static_cast<std::int64_t>(value));
    }
    return JsonValue::integer(static_cast<std::int64_t>(value));
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  std::size_t depth_ = 0;
  std::size_t max_depth_ = limits::kMaxJsonDepth;
};

}  // namespace

JsonValue JsonValue::boolean(bool value) noexcept {
  JsonValue result;
  result.storage_ = value;
  return result;
}

JsonValue JsonValue::integer(std::int64_t value) noexcept {
  JsonValue result;
  result.storage_ = value;
  return result;
}

Result<JsonValue> JsonValue::unsigned_integer(std::uint64_t value) {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return Error(ErrorCode::NumberOutOfRange,
                 "unsigned value does not fit the signed 64-bit canonical integer range");
  }
  return JsonValue::integer(static_cast<std::int64_t>(value));
}

JsonValue JsonValue::string(std::string value) {
  JsonValue result;
  result.storage_ = std::move(value);
  return result;
}

Result<JsonValue> JsonValue::array(Array values) {
  if (values.size() > limits::kMaxArrayElements) {
    return Error(ErrorCode::LimitExceeded, "array exceeds the supported element count");
  }
  JsonValue result;
  result.storage_ = std::move(values);
  return result;
}

Result<JsonValue> JsonValue::object(Object fields) {
  if (fields.size() > limits::kMaxObjectFields) {
    return Error(ErrorCode::LimitExceeded, "object exceeds the supported field count");
  }
  std::stable_sort(fields.begin(), fields.end(),
                   [](const Field& left, const Field& right) {
                     return left.first < right.first;
                   });
  for (std::size_t index = 1; index < fields.size(); ++index) {
    if (fields[index - 1].first == fields[index].first) {
      return Error(ErrorCode::DuplicateField,
                   "object contains the field '" + fields[index].first + "' more than once");
    }
  }
  for (const Field& field : fields) {
    if (!is_valid_utf8(field.first)) {
      return Error(ErrorCode::InvalidUtf8, "object field name is not valid UTF-8");
    }
  }
  JsonValue result;
  result.storage_ = std::move(fields);
  return result;
}

Result<JsonValue> JsonValue::object(std::initializer_list<Field> fields) {
  return JsonValue::object(Object(fields.begin(), fields.end()));
}

bool JsonValue::as_bool() const {
  if (!is_bool()) {
    std::terminate();
  }
  return std::get<1>(storage_);
}

std::int64_t JsonValue::as_integer() const {
  if (!is_integer()) {
    std::terminate();
  }
  return std::get<2>(storage_);
}

const std::string& JsonValue::as_string() const {
  if (!is_string()) {
    std::terminate();
  }
  return std::get<3>(storage_);
}

const JsonValue::Array& JsonValue::as_array() const {
  if (!is_array()) {
    std::terminate();
  }
  return std::get<4>(storage_);
}

const JsonValue::Object& JsonValue::as_object() const {
  if (!is_object()) {
    std::terminate();
  }
  return std::get<5>(storage_);
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
  if (!is_object()) {
    return nullptr;
  }
  const Object& fields = std::get<5>(storage_);
  std::size_t low = 0;
  std::size_t high = fields.size();
  while (low < high) {
    const std::size_t middle = low + (high - low) / 2;
    if (fields[middle].first < key) {
      low = middle + 1;
    } else if (fields[middle].first > key) {
      high = middle;
    } else {
      return &fields[middle].second;
    }
  }
  return nullptr;
}

std::size_t JsonValue::size() const noexcept {
  switch (storage_.index()) {
    case 3:
      return std::get<3>(storage_).size();
    case 4:
      return std::get<4>(storage_).size();
    case 5:
      return std::get<5>(storage_).size();
    default:
      return 0;
  }
}

bool operator==(const JsonValue& left, const JsonValue& right) noexcept {
  if (left.storage_.index() != right.storage_.index()) {
    return false;
  }
  switch (left.storage_.index()) {
    case 0:
      return true;
    case 1:
      return std::get<1>(left.storage_) == std::get<1>(right.storage_);
    case 2:
      return std::get<2>(left.storage_) == std::get<2>(right.storage_);
    case 3:
      return std::get<3>(left.storage_) == std::get<3>(right.storage_);
    case 4:
      return std::get<4>(left.storage_) == std::get<4>(right.storage_);
    case 5:
      return std::get<5>(left.storage_) == std::get<5>(right.storage_);
    default:
      return false;
  }
}

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    std::uint32_t code_point = 0;
    if (!decode_utf8(text, index, code_point)) {
      return false;
    }
  }
  return true;
}

namespace {

void append_escaped_string(const std::string& text, std::string& out) {
  out.push_back('"');
  for (const char character : text) {
    const unsigned char byte = static_cast<unsigned char>(character);
    switch (byte) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (byte < 0x20u) {
          static constexpr char kDigits[] = "0123456789abcdef";
          out.append("\\u00");
          out.push_back(kDigits[(byte >> 4) & 0x0fu]);
          out.push_back(kDigits[byte & 0x0fu]);
        } else {
          out.push_back(character);
        }
        break;
    }
  }
  out.push_back('"');
}

void append_value(const JsonValue& value, std::string& out) {
  switch (value.kind()) {
    case JsonValue::Kind::Null:
      out.append("null");
      return;
    case JsonValue::Kind::Bool:
      out.append(value.as_bool() ? "true" : "false");
      return;
    case JsonValue::Kind::Integer:
      out.append(std::to_string(value.as_integer()));
      return;
    case JsonValue::Kind::String:
      append_escaped_string(value.as_string(), out);
      return;
    case JsonValue::Kind::Array: {
      out.push_back('[');
      bool first = true;
      for (const JsonValue& element : value.as_array()) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        append_value(element, out);
      }
      out.push_back(']');
      return;
    }
    case JsonValue::Kind::Object: {
      out.push_back('{');
      bool first = true;
      for (const JsonValue::Field& field : value.as_object()) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        append_escaped_string(field.first, out);
        out.push_back(':');
        append_value(field.second, out);
      }
      out.push_back('}');
      return;
    }
  }
}

}  // namespace

void append_canonical_json(const JsonValue& value, std::string& out) {
  append_value(value, out);
}

std::string canonical_json(const JsonValue& value) {
  std::string out;
  append_value(value, out);
  return out;
}

Result<JsonValue> parse_json(std::string_view text, JsonParseOptions options) {
  Parser parser(text, options);
  return parser.parse_document();
}

namespace json {

bool has_key(const JsonValue& object, std::string_view key) noexcept {
  return object.find(key) != nullptr;
}

Result<const JsonValue*> require_field(const JsonValue& object, std::string_view key) {
  if (!object.is_object()) {
    return Error(ErrorCode::TypeMismatch, "value is not an object");
  }
  const JsonValue* field = object.find(key);
  if (field == nullptr) {
    return Error(ErrorCode::MissingField, "required field '" + std::string(key) +
                                              "' is missing");
  }
  return field;
}

const JsonValue* optional_field(const JsonValue& object, std::string_view key) noexcept {
  if (!object.is_object()) {
    return nullptr;
  }
  return object.find(key);
}

Result<std::int64_t> require_integer(const JsonValue& object, std::string_view key) {
  BSM_TRY_ASSIGN(field, require_field(object, key));
  if (!field->is_integer()) {
    return Error(ErrorCode::TypeMismatch,
                 "field '" + std::string(key) + "' must be an integer");
  }
  return field->as_integer();
}

Result<std::uint64_t> require_unsigned(const JsonValue& object, std::string_view key) {
  BSM_TRY_ASSIGN(value, require_integer(object, key));
  if (value < 0) {
    return Error(ErrorCode::NumberOutOfRange,
                 "field '" + std::string(key) + "' must not be negative");
  }
  return static_cast<std::uint64_t>(value);
}

Result<bool> require_bool(const JsonValue& object, std::string_view key) {
  BSM_TRY_ASSIGN(field, require_field(object, key));
  if (!field->is_bool()) {
    return Error(ErrorCode::TypeMismatch,
                 "field '" + std::string(key) + "' must be a boolean");
  }
  return field->as_bool();
}

Result<std::string> require_string(const JsonValue& object, std::string_view key) {
  BSM_TRY_ASSIGN(field, require_field(object, key));
  if (!field->is_string()) {
    return Error(ErrorCode::TypeMismatch,
                 "field '" + std::string(key) + "' must be a string");
  }
  return field->as_string();
}

Result<const JsonValue::Array*> require_array(const JsonValue& object,
                                              std::string_view key) {
  BSM_TRY_ASSIGN(field, require_field(object, key));
  if (!field->is_array()) {
    return Error(ErrorCode::TypeMismatch,
                 "field '" + std::string(key) + "' must be an array");
  }
  return &field->as_array();
}

Result<const JsonValue::Object*> require_object(const JsonValue& object,
                                                std::string_view key) {
  BSM_TRY_ASSIGN(field, require_field(object, key));
  if (!field->is_object()) {
    return Error(ErrorCode::TypeMismatch,
                 "field '" + std::string(key) + "' must be an object");
  }
  return &field->as_object();
}

Result<const JsonValue*> require_object_member(const JsonValue& object,
                                               std::string_view key) {
  if (!object.is_object()) {
    return Error(ErrorCode::TypeMismatch, "value is not an object");
  }
  BSM_TRY_ASSIGN(field, require_field(object, key));
  return field;
}

Result<Unit> reject_unknown_fields(const JsonValue& object,
                                   std::initializer_list<std::string_view> known) {
  if (!object.is_object()) {
    return Error(ErrorCode::TypeMismatch, "value is not an object");
  }
  for (const JsonValue::Field& field : object.as_object()) {
    bool found = false;
    for (const std::string_view candidate : known) {
      if (field.first == candidate) {
        found = true;
        break;
      }
    }
    if (!found) {
      return Error(ErrorCode::UnknownField,
                   "field '" + field.first + "' is not part of this schema")
          .with_detail([&field] {
            JsonObjectBuilder builder;
            builder.set_text("field", field.first);
            const Result<JsonValue> value = builder.build();
            return value.ok() ? value.value() : JsonValue();
          }());
    }
  }
  return Unit{};
}

Result<std::string> require_text(const JsonValue& value, std::string_view what,
                                 std::size_t max_length) {
  if (!value.is_string()) {
    return Error(ErrorCode::TypeMismatch, std::string(what) + " must be a string");
  }
  if (value.as_string().size() > max_length) {
    return Error(ErrorCode::LimitExceeded,
                 std::string(what) + " exceeds the supported length");
  }
  if (!is_valid_utf8(value.as_string())) {
    return Error(ErrorCode::InvalidUtf8, std::string(what) + " is not valid UTF-8");
  }
  return value.as_string();
}

}  // namespace json

JsonObjectBuilder& JsonObjectBuilder::set(std::string_view key, JsonValue value) {
  if (failed_) {
    return *this;
  }
  if (key.empty()) {
    failed_ = true;
    error_ = Error(ErrorCode::InvalidArgument, "object field name is empty");
    return *this;
  }
  if (!is_valid_utf8(key)) {
    failed_ = true;
    error_ = Error(ErrorCode::InvalidUtf8, "object field name is not valid UTF-8");
    return *this;
  }
  fields_.emplace_back(std::string(key), std::move(value));
  return *this;
}

JsonObjectBuilder& JsonObjectBuilder::set_text(std::string_view key,
                                               std::string_view value) {
  return set(key, JsonValue::string(std::string(value)));
}

JsonObjectBuilder& JsonObjectBuilder::set_bool(std::string_view key, bool value) {
  return set(key, JsonValue::boolean(value));
}

JsonObjectBuilder& JsonObjectBuilder::set_int(std::string_view key, std::int64_t value) {
  return set(key, JsonValue::integer(value));
}

JsonObjectBuilder& JsonObjectBuilder::set_uint(std::string_view key, std::uint64_t value) {
  const Result<JsonValue> converted = JsonValue::unsigned_integer(value);
  if (!converted.ok()) {
    if (!failed_) {
      failed_ = true;
      error_ = converted.error();
    }
    return *this;
  }
  return set(key, converted.value());
}

JsonObjectBuilder& JsonObjectBuilder::set_null(std::string_view key) {
  return set(key, JsonValue::null());
}

Result<JsonValue> JsonObjectBuilder::build() const {
  if (failed_) {
    return error_;
  }
  return JsonValue::object(fields_);
}

JsonArrayBuilder& JsonArrayBuilder::push(JsonValue value) {
  if (!failed_) {
    values_.push_back(std::move(value));
  }
  return *this;
}

JsonArrayBuilder& JsonArrayBuilder::push_text(std::string_view value) {
  return push(JsonValue::string(std::string(value)));
}

JsonArrayBuilder& JsonArrayBuilder::push_int(std::int64_t value) {
  return push(JsonValue::integer(value));
}

Result<JsonValue> JsonArrayBuilder::build() const {
  if (failed_) {
    return error_;
  }
  return JsonValue::array(values_);
}

}  // namespace black_start_manager
