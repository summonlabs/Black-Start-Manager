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

#include "black_start_manager/clock.hpp"

#include <chrono>
#include <cstdint>

#include "black_start_manager/checked.hpp"

namespace black_start_manager {

Result<Unit> ManualClock::set(std::uint64_t tick) {
  if (tick > limits::kMaxTick) {
    return Error(ErrorCode::NumberOutOfRange,
                 "tick exceeds the supported logical time range");
  }
  if (tick < tick_) {
    return Error(ErrorCode::InvalidArgument,
                 "logical time may not move backwards; the store's durable state already "
                 "records a later tick");
  }
  tick_ = tick;
  return Unit{};
}

Result<Unit> ManualClock::advance(std::uint64_t delta) {
  BSM_TRY_ASSIGN(next, checked_add(tick_, delta));
  if (next > limits::kMaxTick) {
    return Error(ErrorCode::NumberOutOfRange,
                 "tick exceeds the supported logical time range");
  }
  tick_ = next;
  return Unit{};
}

SystemClock::SystemClock() : base_(std::chrono::steady_clock::now()) {}

Result<std::uint64_t> SystemClock::now_ticks() const {
  const std::chrono::steady_clock::duration elapsed =
      std::chrono::steady_clock::now() - base_;
  const std::chrono::milliseconds millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
  if (millis.count() < 0) {
    return Error(ErrorCode::InternalError, "the monotonic clock moved backwards");
  }
  const std::uint64_t ticks = static_cast<std::uint64_t>(millis.count());
  if (ticks > limits::kMaxTick) {
    return Error(ErrorCode::NumberOutOfRange,
                 "tick exceeds the supported logical time range");
  }
  return ticks;
}

}  // namespace black_start_manager
