#pragma once

// Error codes and the Result carrier used across the whole public API.
//
// The library never throws across its boundary and never returns a partially
// applied outcome: an operation either commits its durable records and reports
// success, or refuses with an Error that names the exact invariant that stopped it.

#include <cstdint>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace black_start_manager {

class JsonValue;

enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // Input shape and encoding.
  InvalidArgument,
  InvalidEncoding,
  SchemaViolation,
  MissingField,
  UnknownField,
  TypeMismatch,
  NumberOutOfRange,
  LimitExceeded,
  InvalidUtf8,
  TrailingData,
  DuplicateField,
  UnsupportedFormatVersion,
  IntegrityMismatch,
  TruncatedInput,
  AmbiguousInput,

  // Identity.
  InvalidIdentity,
  DuplicateIdentity,
  UnknownIdentity,

  // Store and filesystem.
  IoFailure,
  PathUnsafe,
  PathTooLong,
  StoreLocked,
  StoreNotFound,
  StoreCorrupt,
  StoreAmbiguous,
  StoreNotOpen,
  StoreAlreadyOpen,
  StoreReadOnly,
  StoreGenerationStale,

  // Plan compilation.
  PlanInvalid,
  PlanCycle,
  PlanStageOrderConflict,
  PlanUnknownReference,
  PlanDigestMismatch,
  PlanUnchanged,
  PlanEmpty,

  // Session lifecycle.
  SessionNotFound,
  SessionExists,
  SessionTerminal,
  SessionFenced,
  SessionHeld,
  SessionNotHeld,
  SessionStateInvalid,
  SessionNotRecovered,

  // Authority and binding.
  AuthorityMissing,
  AuthorityStale,
  AuthorityMismatch,
  AuthorityNotReestablished,
  AuthorityUnattested,
  BindingIncomplete,

  // Evidence.
  EvidenceUnknownRequirement,
  EvidenceForeignSession,
  EvidenceStaleAuthority,
  EvidenceFromFuture,
  EvidenceRecoveredNotCurrent,
  EvidenceNotFound,
  EvidenceContradictory,
  EvidenceInsufficient,
  EvidenceNotIndependent,
  EvidenceExpired,

  // Readiness gates and precedence.
  GateNotReady,
  GatePrerequisiteUnsatisfied,
  GatePrecedenceBlocked,
  GateStageEvidenceMissing,
  GateStageEvidenceStale,
  GateCompletionUnsatisfied,

  // Attempts and controller interaction.
  AttemptNotFound,
  AttemptUnresolved,
  AttemptBudgetExhausted,
  AttemptStateInvalid,
  AttemptNotReplayable,
  RequestRefused,
  RequestUnavailable,
  ControllerUnavailable,
  ControllerProtocolViolation,
  ControllerRejected,
  ControllerDeferred,

  // Concurrency and process state.
  ReentrantOperation,
  ConcurrentModification,

  InternalError,
};

[[nodiscard]] const char* to_string(ErrorCode code) noexcept;

class Error {
 public:
  Error() = default;
  Error(ErrorCode code, std::string message)
      : code_(code), message_(std::move(message)) {}
  Error(ErrorCode code, std::string message, std::string detail_json)
      : code_(code), message_(std::move(message)), detail_json_(std::move(detail_json)) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  // Canonical JSON text of the structured detail, empty when there is none.
  [[nodiscard]] const std::string& detail_json() const noexcept { return detail_json_; }

  // Attaches a structured canonical detail object. Defined where JsonValue is
  // complete so this header does not depend on the encoder.
  [[nodiscard]] Error with_detail(JsonValue detail) const;

  [[nodiscard]] std::string to_json_text() const;

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
  std::string detail_json_;
};

struct Unit {
  friend constexpr bool operator==(Unit, Unit) noexcept { return true; }
};

namespace detail {
[[noreturn]] inline void result_precondition_failed(const char* what) {
  // A violated Result precondition is a programming defect. The library terminates
  // deterministically instead of unwinding through std::get: there is no exception
  // path in the public API, and crash-injection tests require silent termination.
  (void)what;
  std::terminate();
}
}  // namespace detail

// Result either holds a value or an Error. Storage index 0 is the value.
template <class T>
class Result {
 public:
  using value_type = T;

  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] T& value() & {
    if (!ok()) {
      detail::result_precondition_failed("Result::value on an error");
    }
    return std::get<0>(storage_);
  }
  [[nodiscard]] const T& value() const& {
    if (!ok()) {
      detail::result_precondition_failed("Result::value on an error");
    }
    return std::get<0>(storage_);
  }
  [[nodiscard]] T&& value() && {
    if (!ok()) {
      detail::result_precondition_failed("Result::value on an error");
    }
    return std::get<0>(std::move(storage_));
  }

  [[nodiscard]] const Error& error() const& {
    if (ok()) {
      detail::result_precondition_failed("Result::error on a value");
    }
    return std::get<1>(storage_);
  }
  [[nodiscard]] Error& error() & {
    if (ok()) {
      detail::result_precondition_failed("Result::error on a value");
    }
    return std::get<1>(storage_);
  }
  [[nodiscard]] Error&& error() && {
    if (ok()) {
      detail::result_precondition_failed("Result::error on a value");
    }
    return std::get<1>(std::move(storage_));
  }

 private:
  std::variant<T, Error> storage_;
};

using Status = Result<Unit>;

[[nodiscard]] inline Status ok_status() noexcept { return Status(Unit{}); }

[[nodiscard]] inline Error make_error(ErrorCode code, std::string message) {
  return Error(code, std::move(message));
}

}  // namespace black_start_manager

// Propagates a failed Result. The macro is deliberately statement-shaped so a
// failing call cannot be accidentally ignored.
#define BSM_RETURN_IF_ERROR(expression)                     \
  do {                                                      \
    const auto bsm_status_ = (expression);                  \
    if (!bsm_status_.ok()) {                                \
      return bsm_status_.error();                           \
    }                                                       \
  } while (false)

// Binds the value of a successful Result to a name, or returns its error. The
// temporary Result outlives the binding, so the bound name never dangles.
#define BSM_TRY_ASSIGN(name, expression)                    \
  auto name##_bsm_result_ = (expression);                   \
  if (!name##_bsm_result_.ok()) {                           \
    return name##_bsm_result_.error();                      \
  }                                                         \
  auto name = std::move(name##_bsm_result_).value()
