#pragma once

// Content identity: SHA-256 digests, CRC-32C frame integrity, and the checked hex
// and randomness helpers the rest of the library builds identities from.
//
// Every digest in this library is taken over canonical JSON bytes or over an exact
// documented byte range of a framed record, so a digest can always be recomputed by
// a third party from the published bytes alone.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "black_start_manager/error.hpp"

namespace black_start_manager {

class Digest {
 public:
  static constexpr std::size_t kByteCount = 32;
  using Bytes = std::array<std::uint8_t, kByteCount>;

  Digest() noexcept = default;
  explicit Digest(Bytes bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] static Result<Digest> from_hex(std::string_view text);
  [[nodiscard]] static Digest of(std::string_view bytes);
  [[nodiscard]] static Digest of(std::span<const std::uint8_t> bytes);

  [[nodiscard]] const Bytes& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::string hex() const;
  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] std::span<const std::uint8_t> span() const noexcept {
    return std::span<const std::uint8_t>(bytes_.data(), bytes_.size());
  }

  friend bool operator==(const Digest& left, const Digest& right) noexcept {
    return left.bytes_ == right.bytes_;
  }
  friend bool operator!=(const Digest& left, const Digest& right) noexcept {
    return !(left == right);
  }
  friend std::strong_ordering operator<=>(const Digest& left,
                                          const Digest& right) noexcept {
    return left.bytes_ <=> right.bytes_;
  }

 private:
  Bytes bytes_{};
};

class Sha256 {
 public:
  Sha256() noexcept;
  void update(std::span<const std::uint8_t> bytes);
  void update(std::string_view bytes);
  [[nodiscard]] Digest finish();

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
  bool finished_ = false;
};

[[nodiscard]] Digest sha256(std::string_view bytes);
[[nodiscard]] Digest sha256(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::uint32_t crc32c(std::span<const std::uint8_t> bytes) noexcept;

[[nodiscard]] std::string hex_encode(std::span<const std::uint8_t> bytes);
[[nodiscard]] Result<std::vector<std::uint8_t>> hex_decode(std::string_view text);

// Operating-system randomness. Used only for identities that must not collide
// across processes (session, incarnation); never used in a canonical value.
[[nodiscard]] Result<std::vector<std::uint8_t>> random_bytes(std::size_t count);
[[nodiscard]] Result<std::string> random_hex(std::size_t count);

}  // namespace black_start_manager
