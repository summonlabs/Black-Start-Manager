#pragma once

// Checked arithmetic for every externally influenced counter, generation, sequence,
// quantity, tick, and duration. Overflow and underflow are refused, never wrapped.

#include <limits>
#include <type_traits>

#include "black_start_manager/error.hpp"

namespace black_start_manager {

template <class T>
[[nodiscard]] Error numeric_overflow_error(const char* operation) {
  return Error(ErrorCode::NumberOutOfRange,
               std::string("checked ") + operation + " overflowed the representable range");
}

template <class T>
[[nodiscard]] Result<T> checked_add(T left, T right) {
  static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>,
                "checked_add requires an integral type");
  if constexpr (std::is_unsigned_v<T>) {
    if (right > static_cast<T>(std::numeric_limits<T>::max() - left)) {
      return numeric_overflow_error<T>("addition");
    }
  } else {
    if (right > 0 && left > static_cast<T>(std::numeric_limits<T>::max() - right)) {
      return numeric_overflow_error<T>("addition");
    }
    if (right < 0 && left < static_cast<T>(std::numeric_limits<T>::min() - right)) {
      return numeric_overflow_error<T>("addition");
    }
  }
  return static_cast<T>(left + right);
}

template <class T>
[[nodiscard]] Result<T> checked_sub(T left, T right) {
  static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>,
                "checked_sub requires an integral type");
  if constexpr (std::is_unsigned_v<T>) {
    if (right > left) {
      return numeric_overflow_error<T>("subtraction");
    }
  } else {
    if (right < 0 && left > static_cast<T>(std::numeric_limits<T>::max() + right)) {
      return numeric_overflow_error<T>("subtraction");
    }
    if (right > 0 && left < static_cast<T>(std::numeric_limits<T>::min() + right)) {
      return numeric_overflow_error<T>("subtraction");
    }
  }
  return static_cast<T>(left - right);
}

template <class T>
[[nodiscard]] Result<T> checked_mul(T left, T right) {
  static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>,
                "checked_mul requires an integral type");
  if (left == 0 || right == 0) {
    return static_cast<T>(0);
  }
  if constexpr (std::is_unsigned_v<T>) {
    if (left > static_cast<T>(std::numeric_limits<T>::max() / right)) {
      return numeric_overflow_error<T>("multiplication");
    }
  } else {
    const bool negative = (left < 0) != (right < 0);
    const T limit = negative ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
    if (left > 0) {
      if (right > 0 && left > static_cast<T>(limit / right)) {
        return numeric_overflow_error<T>("multiplication");
      }
      if (right < 0 && right < static_cast<T>(limit / left)) {
        return numeric_overflow_error<T>("multiplication");
      }
    } else {
      if (right > 0 && left < static_cast<T>(limit / right)) {
        return numeric_overflow_error<T>("multiplication");
      }
      if (right < 0 && left < static_cast<T>(limit / right)) {
        return numeric_overflow_error<T>("multiplication");
      }
    }
  }
  return static_cast<T>(left * right);
}

template <class T>
[[nodiscard]] Result<T> checked_increment(T value) {
  return checked_add<T>(value, static_cast<T>(1));
}

template <class T>
[[nodiscard]] Result<T> checked_decrement(T value) {
  return checked_sub<T>(value, static_cast<T>(1));
}

// Bounds an externally supplied value against a compile-time limit.
template <class T, T Limit>
[[nodiscard]] Result<T> checked_within(T value, const char* what) {
  if (value > Limit) {
    return Error(ErrorCode::LimitExceeded,
                 std::string(what) + " exceeds the supported maximum");
  }
  return value;
}

}  // namespace black_start_manager
