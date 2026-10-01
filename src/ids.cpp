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

#include "black_start_manager/ids.hpp"

#include <cstddef>

#include "black_start_manager/canonical.hpp"
#include "black_start_manager/digest.hpp"

namespace black_start_manager {
namespace {

[[nodiscard]] bool is_identifier_byte(char character) noexcept {
  const unsigned char byte = static_cast<unsigned char>(character);
  if (byte >= 'a' && byte <= 'z') {
    return true;
  }
  if (byte >= 'A' && byte <= 'Z') {
    return true;
  }
  if (byte >= '0' && byte <= '9') {
    return true;
  }
  switch (byte) {
    case '_':
    case '-':
    case '.':
    case ':':
    case '@':
      return true;
    default:
      return false;
  }
}

}  // namespace

bool IdentifierText::is_valid(std::string_view text) noexcept {
  if (text.empty() || text.size() > limits::kMaxIdentifierLength) {
    return false;
  }
  if (text == "." || text == "..") {
    return false;
  }
  if (text.front() == '.' || text.back() == '.') {
    return false;
  }
  for (const char character : text) {
    if (!is_identifier_byte(character)) {
      return false;
    }
  }
  return true;
}

Result<IdentifierText> IdentifierText::parse(std::string_view text) {
  if (text.empty()) {
    return Error(ErrorCode::InvalidIdentity, "identifier is empty");
  }
  if (text.size() > limits::kMaxIdentifierLength) {
    return Error(ErrorCode::InvalidIdentity, "identifier exceeds the supported length");
  }
  if (!is_valid(text)) {
    Error error(ErrorCode::InvalidIdentity,
                "identifier contains a character outside [A-Za-z0-9._:@-] or is a "
                "relative path name");
    JsonObjectBuilder detail;
    detail.set_text("identifier", text);
    const Result<JsonValue> value = detail.build();
    if (value.ok()) {
      error = error.with_detail(value.value());
    }
    return error;
  }
  IdentifierText result;
  result.text_.assign(text);
  return result;
}

template <class Tag, std::size_t ByteCount>
Result<RawId<Tag, ByteCount>> RawId<Tag, ByteCount>::from_hex(std::string_view text) {
  BSM_TRY_ASSIGN(bytes, hex_decode(text));
  if (bytes.size() != ByteCount) {
    return Error(ErrorCode::InvalidIdentity,
                 "identity requires exactly " + std::to_string(ByteCount * 2) +
                     " hexadecimal characters");
  }
  Bytes value{};
  for (std::size_t index = 0; index < ByteCount; ++index) {
    value[index] = bytes[index];
  }
  return RawId(value);
}

template <class Tag, std::size_t ByteCount>
Result<RawId<Tag, ByteCount>> RawId<Tag, ByteCount>::generate() {
  BSM_TRY_ASSIGN(bytes, random_bytes(ByteCount));
  Bytes value{};
  for (std::size_t index = 0; index < ByteCount; ++index) {
    value[index] = bytes[index];
  }
  RawId identity(value);
  if (identity.is_zero()) {
    return Error(ErrorCode::InternalError,
                 "operating system randomness produced an all-zero identity");
  }
  return identity;
}

template <class Tag, std::size_t ByteCount>
std::string RawId<Tag, ByteCount>::hex() const {
  return hex_encode(std::span<const std::uint8_t>(bytes_.data(), bytes_.size()));
}

template <class Tag, std::size_t ByteCount>
bool RawId<Tag, ByteCount>::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes_) {
    if (byte != 0u) {
      return false;
    }
  }
  return true;
}

template class RawId<SessionTag, 16>;
template class RawId<IncarnationTag, 16>;
template class RawId<AttemptTag, 32>;
template class RawId<EvidenceTag, 32>;
template class RawId<RequestKeyTag, 32>;

}  // namespace black_start_manager
