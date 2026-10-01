#pragma once

// A very small test harness: a test is a function, a check records the first failure with
// its file and line, and the runner reports a per-test verdict and a non-zero exit status
// when anything failed.
//
// No test is given a timeout. CTest timeouts, timeout wrappers, and watchdog processes are
// prohibited in this repository: a test that does not finish is a defect to diagnose.

#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "black_start_manager/error.hpp"

namespace bsm_test {

class Context {
 public:
  void fail(const char* file, int line, const std::string& expression) {
    if (failed_) {
      return;
    }
    failed_ = true;
    std::ostringstream stream;
    stream << file << ':' << line << ": check failed: " << expression;
    message_ = stream.str();
  }

  void note(const std::string& text) { notes_.push_back(text); }

  [[nodiscard]] bool failed() const { return failed_; }
  [[nodiscard]] const std::string& message() const { return message_; }
  [[nodiscard]] const std::vector<std::string>& notes() const { return notes_; }

 private:
  bool failed_ = false;
  std::string message_;
  std::vector<std::string> notes_;
};

using TestFunction = std::function<void(Context&)>;

struct TestCase {
  std::string name;
  TestFunction body;
};

class Registry {
 public:
  static Registry& instance();
  void add(std::string name, TestFunction body);
  [[nodiscard]] const std::vector<TestCase>& cases() const { return cases_; }

 private:
  std::vector<TestCase> cases_;
};

struct Registrar {
  Registrar(std::string name, TestFunction body) {
    Registry::instance().add(std::move(name), std::move(body));
  }
};

template <class T>
std::string render(const T& value) {
  if constexpr (requires(std::ostream& stream, const T& item) { stream << item; }) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else if constexpr (std::is_enum_v<T>) {
    return "enum#" + std::to_string(static_cast<long long>(value));
  } else {
    return "<unprintable>";
  }
}

inline void report(bool condition, const char* file, int line,
                   const std::string& expression, Context& context) {
  if (!condition) {
    context.fail(file, line, expression);
  }
}

[[nodiscard]] int run_all(int argc, char** argv, const char* suite_name);

// Deterministic random source for randomized tests: a failing run prints its seed so the
// exact sequence can be replayed.
class SeededRandom {
 public:
  explicit SeededRandom(std::uint64_t seed)
      : seed_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed), state_(seed_) {}

  [[nodiscard]] std::uint64_t seed() const { return seed_; }

  [[nodiscard]] std::uint64_t next() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
  }

  [[nodiscard]] std::uint64_t below(std::uint64_t bound) {
    if (bound == 0) {
      return 0;
    }
    return next() % bound;
  }

  [[nodiscard]] bool chance(std::uint64_t numerator, std::uint64_t denominator) {
    return below(denominator) < numerator;
  }

 private:
  std::uint64_t seed_ = 0;
  std::uint64_t state_ = 0;
};

}  // namespace bsm_test

#define BSM_TEST(name)                                                       \
  static void name##_body(::bsm_test::Context& context);                     \
  static const ::bsm_test::Registrar name##_registrar(#name, name##_body);   \
  static void name##_body(::bsm_test::Context& context)

// A condition containing a top-level comma (for example a two-argument template
// specialization) must be wrapped in parentheses, or the preprocessor will read the comma
// as an argument separator.
#define BSM_CHECK(condition)                                                 \
  do {                                                                       \
    if (!(condition)) {                                                      \
      context.fail(__FILE__, __LINE__, #condition);                           \
      return;                                                                \
    }                                                                        \
  } while (false)

#define BSM_CHECK_MSG(condition, message)                                    \
  do {                                                                       \
    if (!(condition)) {                                                      \
      context.fail(__FILE__, __LINE__, std::string(#condition) + " | " + (message)); \
      return;                                                                \
    }                                                                        \
  } while (false)

#define BSM_SOFT_CHECK(condition)                                            \
  ::bsm_test::report((condition), __FILE__, __LINE__, #condition, context)

// Checks that a call refused with a specific code, without touching the value side of a
// failed Result (which is a precondition violation).
#define BSM_CHECK_ERR(expression, expected_code)                             \
  do {                                                                       \
    const auto bsm_refusal = (expression);                                   \
    if (bsm_refusal.ok()) {                                                  \
      context.fail(__FILE__, __LINE__,                                       \
                   #expression " unexpectedly succeeded");                   \
      return;                                                                \
    }                                                                        \
    if (bsm_refusal.error().code() != (expected_code)) {                     \
      context.fail(__FILE__, __LINE__,                                       \
                   std::string(#expression " refused with ") +               \
                       ::black_start_manager::to_string(                     \
                           bsm_refusal.error().code()) +                     \
                       " instead of " +                                      \
                       ::black_start_manager::to_string(expected_code) +     \
                       ": " + bsm_refusal.error().message());                \
      return;                                                                \
    }                                                                        \
  } while (false)

#define BSM_CHECK_EQ(actual, expected)                                       \
  do {                                                                       \
    const auto bsm_actual = (actual);                                        \
    const auto bsm_expected = (expected);                                    \
    if (!(bsm_actual == bsm_expected)) {                                     \
      std::ostringstream bsm_stream;                                         \
      bsm_stream << #actual " == " #expected " (actual "                     \
                 << ::bsm_test::render(bsm_actual) << ", expected "          \
                 << ::bsm_test::render(bsm_expected) << ')';                 \
      context.fail(__FILE__, __LINE__, bsm_stream.str());                    \
      return;                                                                \
    }                                                                        \
  } while (false)

// Binds the value of a successful Result, failing the test with the library's own error
// message when the call refused. The temporary Result outlives the binding.
#define BSM_CHECK_OK(name, expression)                                       \
  auto name##_bsm_result = (expression);                                     \
  if (!name##_bsm_result.ok()) {                                             \
    context.fail(__FILE__, __LINE__,                                         \
                 std::string(#expression) + " failed: " +                    \
                     ::black_start_manager::to_string(                       \
                         name##_bsm_result.error().code()) +                 \
                     ": " + name##_bsm_result.error().message());            \
    return;                                                                  \
  }                                                                          \
  auto name = std::move(name##_bsm_result).value()

#define BSM_TEST_MAIN(suite_name)                                            \
  int main(int argc, char** argv) {                                          \
    return ::bsm_test::run_all(argc, argv, suite_name);                      \
  }
