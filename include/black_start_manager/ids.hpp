#pragma once

// Strongly typed identities.
//
// Distinct tag types make it impossible to pass a session where an obligation is
// expected, or to compare an incarnation against a generation. Identifier tokens are
// validated on construction: a token that cannot appear in canonical output or in a
// store artifact name never enters the model.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "black_start_manager/error.hpp"
#include "black_start_manager/limits.hpp"

namespace black_start_manager {

// A validated identifier: 1..kMaxIdentifierLength bytes from a fixed ASCII set.
// Rejected: empty, over-long, whitespace, path separators, control bytes, any byte
// above 0x7f, leading or trailing '.', and the relative path names "." and "..".
class IdentifierText {
 public:
  IdentifierText() = default;

  [[nodiscard]] static Result<IdentifierText> parse(std::string_view text);
  [[nodiscard]] static bool is_valid(std::string_view text) noexcept;

  [[nodiscard]] const std::string& str() const noexcept { return text_; }
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

  friend bool operator==(const IdentifierText& left, const IdentifierText& right) noexcept {
    return left.text_ == right.text_;
  }
  friend bool operator!=(const IdentifierText& left, const IdentifierText& right) noexcept {
    return !(left == right);
  }
  friend std::strong_ordering operator<=>(const IdentifierText& left,
                                          const IdentifierText& right) noexcept {
    return left.text_ <=> right.text_;
  }

 private:
  std::string text_;
};

struct SessionTag;
struct IncarnationTag;
struct AttemptTag;
struct EvidenceTag;
struct RequestKeyTag;
struct StoreTag;

struct ObligationTag;
struct StageTag;
struct DomainTag;
struct SubjectTag;
struct OwnerTag;
struct FacilityTag;
struct IncidentTag;
struct PolicyTag;
struct EvidenceKindTag;
struct CapabilityTag;
struct AuthorityTag;

// Fixed-width identity rendered as lower-case hex.
template <class Tag, std::size_t ByteCount>
class RawId {
 public:
  static constexpr std::size_t kByteCount = ByteCount;
  using Bytes = std::array<std::uint8_t, ByteCount>;

  RawId() noexcept = default;
  explicit RawId(Bytes bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] static Result<RawId> from_hex(std::string_view text);
  [[nodiscard]] static Result<RawId> generate();

  [[nodiscard]] const Bytes& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::string hex() const;
  [[nodiscard]] bool is_zero() const noexcept;

  friend bool operator==(const RawId& left, const RawId& right) noexcept {
    return left.bytes_ == right.bytes_;
  }
  friend bool operator!=(const RawId& left, const RawId& right) noexcept {
    return !(left == right);
  }
  friend std::strong_ordering operator<=>(const RawId& left,
                                          const RawId& right) noexcept {
    return left.bytes_ <=> right.bytes_;
  }

 private:
  Bytes bytes_{};
};

template <class Tag>
class TaggedIdentifier {
 public:
  TaggedIdentifier() = default;
  explicit TaggedIdentifier(IdentifierText text) : text_(std::move(text)) {}

  [[nodiscard]] static Result<TaggedIdentifier> parse(std::string_view text) {
    BSM_TRY_ASSIGN(parsed, IdentifierText::parse(text));
    return TaggedIdentifier(std::move(parsed));
  }
  [[nodiscard]] static bool is_valid(std::string_view text) noexcept {
    return IdentifierText::is_valid(text);
  }

  [[nodiscard]] const std::string& str() const noexcept { return text_.str(); }
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
  [[nodiscard]] const IdentifierText& text() const noexcept { return text_; }

  friend bool operator==(const TaggedIdentifier& left,
                         const TaggedIdentifier& right) noexcept {
    return left.text_ == right.text_;
  }
  friend bool operator!=(const TaggedIdentifier& left,
                         const TaggedIdentifier& right) noexcept {
    return !(left == right);
  }
  friend std::strong_ordering operator<=>(const TaggedIdentifier& left,
                                          const TaggedIdentifier& right) noexcept {
    return left.text_ <=> right.text_;
  }

 private:
  IdentifierText text_;
};

using SessionId = RawId<SessionTag, 16>;
using IncarnationId = RawId<IncarnationTag, 16>;
using AttemptId = RawId<AttemptTag, 32>;
using EvidenceId = RawId<EvidenceTag, 32>;
using RequestKey = RawId<RequestKeyTag, 32>;

using ObligationId = TaggedIdentifier<ObligationTag>;
using StageId = TaggedIdentifier<StageTag>;
using DomainId = TaggedIdentifier<DomainTag>;
using SubjectId = TaggedIdentifier<SubjectTag>;
using OwnerId = TaggedIdentifier<OwnerTag>;
using FacilityId = TaggedIdentifier<FacilityTag>;
using IncidentId = TaggedIdentifier<IncidentTag>;
using PolicyTagId = TaggedIdentifier<PolicyTag>;
using EvidenceKind = TaggedIdentifier<EvidenceKindTag>;
using CapabilityId = TaggedIdentifier<CapabilityTag>;
using AuthorityId = TaggedIdentifier<AuthorityTag>;

}  // namespace black_start_manager
