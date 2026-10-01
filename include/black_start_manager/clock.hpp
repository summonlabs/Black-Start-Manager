#pragma once

// The time source for restoration sessions.
//
// Ticks are logical units owned by the caller's clock. Canonical output, digests,
// and evidence freshness are all expressed in ticks, never in wall-clock time, so a
// replay of the same inputs produces the same bytes. Tests drive a ManualClock; a
// deployment injects a monotonic clock.

#include <chrono>
#include <cstdint>

#include "black_start_manager/error.hpp"
#include "black_start_manager/limits.hpp"

namespace black_start_manager {

class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock() = default;

  // Returns the current tick. Refuses rather than wrapping when the tick would leave
  // the supported range.
  [[nodiscard]] virtual Result<std::uint64_t> now_ticks() const = 0;
};

// Deterministic clock for tests and for replaying a recorded session.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(std::uint64_t start = 0) : tick_(start) {}

  [[nodiscard]] Result<std::uint64_t> now_ticks() const override { return tick_; }

  [[nodiscard]] Result<Unit> set(std::uint64_t tick);
  [[nodiscard]] Result<Unit> advance(std::uint64_t delta);

 private:
  std::uint64_t tick_ = 0;
};

// Monotonic millisecond clock. The base is captured at construction; ticks never
// move backwards and never depend on the wall clock.
class SystemClock final : public Clock {
 public:
  SystemClock();

  [[nodiscard]] Result<std::uint64_t> now_ticks() const override;

 private:
  std::chrono::steady_clock::time_point base_;
};

}  // namespace black_start_manager
