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

#include "black_start_manager/error.hpp"

#include "black_start_manager/canonical.hpp"

namespace black_start_manager {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::InvalidEncoding: return "invalid_encoding";
    case ErrorCode::SchemaViolation: return "schema_violation";
    case ErrorCode::MissingField: return "missing_field";
    case ErrorCode::UnknownField: return "unknown_field";
    case ErrorCode::TypeMismatch: return "type_mismatch";
    case ErrorCode::NumberOutOfRange: return "number_out_of_range";
    case ErrorCode::LimitExceeded: return "limit_exceeded";
    case ErrorCode::InvalidUtf8: return "invalid_utf8";
    case ErrorCode::TrailingData: return "trailing_data";
    case ErrorCode::DuplicateField: return "duplicate_field";
    case ErrorCode::UnsupportedFormatVersion: return "unsupported_format_version";
    case ErrorCode::IntegrityMismatch: return "integrity_mismatch";
    case ErrorCode::TruncatedInput: return "truncated_input";
    case ErrorCode::AmbiguousInput: return "ambiguous_input";
    case ErrorCode::InvalidIdentity: return "invalid_identity";
    case ErrorCode::DuplicateIdentity: return "duplicate_identity";
    case ErrorCode::UnknownIdentity: return "unknown_identity";
    case ErrorCode::IoFailure: return "io_failure";
    case ErrorCode::PathUnsafe: return "path_unsafe";
    case ErrorCode::PathTooLong: return "path_too_long";
    case ErrorCode::StoreLocked: return "store_locked";
    case ErrorCode::StoreNotFound: return "store_not_found";
    case ErrorCode::StoreCorrupt: return "store_corrupt";
    case ErrorCode::StoreAmbiguous: return "store_ambiguous";
    case ErrorCode::StoreNotOpen: return "store_not_open";
    case ErrorCode::StoreAlreadyOpen: return "store_already_open";
    case ErrorCode::StoreReadOnly: return "store_read_only";
    case ErrorCode::StoreGenerationStale: return "store_generation_stale";
    case ErrorCode::PlanInvalid: return "plan_invalid";
    case ErrorCode::PlanCycle: return "plan_cycle";
    case ErrorCode::PlanStageOrderConflict: return "plan_stage_order_conflict";
    case ErrorCode::PlanUnknownReference: return "plan_unknown_reference";
    case ErrorCode::PlanDigestMismatch: return "plan_digest_mismatch";
    case ErrorCode::PlanUnchanged: return "plan_unchanged";
    case ErrorCode::PlanEmpty: return "plan_empty";
    case ErrorCode::SessionNotFound: return "session_not_found";
    case ErrorCode::SessionExists: return "session_exists";
    case ErrorCode::SessionTerminal: return "session_terminal";
    case ErrorCode::SessionFenced: return "session_fenced";
    case ErrorCode::SessionHeld: return "session_held";
    case ErrorCode::SessionNotHeld: return "session_not_held";
    case ErrorCode::SessionStateInvalid: return "session_state_invalid";
    case ErrorCode::SessionNotRecovered: return "session_not_recovered";
    case ErrorCode::AuthorityMissing: return "authority_missing";
    case ErrorCode::AuthorityStale: return "authority_stale";
    case ErrorCode::AuthorityMismatch: return "authority_mismatch";
    case ErrorCode::AuthorityNotReestablished: return "authority_not_reestablished";
    case ErrorCode::AuthorityUnattested: return "authority_unattested";
    case ErrorCode::BindingIncomplete: return "binding_incomplete";
    case ErrorCode::EvidenceUnknownRequirement: return "evidence_unknown_requirement";
    case ErrorCode::EvidenceForeignSession: return "evidence_foreign_session";
    case ErrorCode::EvidenceStaleAuthority: return "evidence_stale_authority";
    case ErrorCode::EvidenceFromFuture: return "evidence_from_future";
    case ErrorCode::EvidenceRecoveredNotCurrent: return "evidence_recovered_not_current";
    case ErrorCode::EvidenceNotFound: return "evidence_not_found";
    case ErrorCode::EvidenceContradictory: return "evidence_contradictory";
    case ErrorCode::EvidenceInsufficient: return "evidence_insufficient";
    case ErrorCode::EvidenceNotIndependent: return "evidence_not_independent";
    case ErrorCode::EvidenceExpired: return "evidence_expired";
    case ErrorCode::GateNotReady: return "gate_not_ready";
    case ErrorCode::GatePrerequisiteUnsatisfied: return "gate_prerequisite_unsatisfied";
    case ErrorCode::GatePrecedenceBlocked: return "gate_precedence_blocked";
    case ErrorCode::GateStageEvidenceMissing: return "gate_stage_evidence_missing";
    case ErrorCode::GateStageEvidenceStale: return "gate_stage_evidence_stale";
    case ErrorCode::GateCompletionUnsatisfied: return "gate_completion_unsatisfied";
    case ErrorCode::AttemptNotFound: return "attempt_not_found";
    case ErrorCode::AttemptUnresolved: return "attempt_unresolved";
    case ErrorCode::AttemptBudgetExhausted: return "attempt_budget_exhausted";
    case ErrorCode::AttemptStateInvalid: return "attempt_state_invalid";
    case ErrorCode::AttemptNotReplayable: return "attempt_not_replayable";
    case ErrorCode::RequestRefused: return "request_refused";
    case ErrorCode::RequestUnavailable: return "request_unavailable";
    case ErrorCode::ControllerUnavailable: return "controller_unavailable";
    case ErrorCode::ControllerProtocolViolation: return "controller_protocol_violation";
    case ErrorCode::ControllerRejected: return "controller_rejected";
    case ErrorCode::ControllerDeferred: return "controller_deferred";
    case ErrorCode::ReentrantOperation: return "reentrant_operation";
    case ErrorCode::ConcurrentModification: return "concurrent_modification";
    case ErrorCode::InternalError: return "internal_error";
  }
  return "unknown_error_code";
}

Error Error::with_detail(JsonValue detail) const {
  Error copy = *this;
  copy.detail_json_ = canonical_json(detail);
  return copy;
}

std::string Error::to_json_text() const {
  JsonObjectBuilder builder;
  builder.set_text("code", to_string(code_));
  builder.set_int("code_value", static_cast<std::int64_t>(code_));
  builder.set_text("message", message_);
  if (!detail_json_.empty()) {
    const Result<JsonValue> detail = parse_json(detail_json_);
    if (detail.ok()) {
      builder.set("detail", detail.value());
    } else {
      builder.set_text("detail_unparsed", detail_json_);
    }
  }
  const Result<JsonValue> value = builder.build();
  if (!value.ok()) {
    return std::string("{\"code\":\"internal_error\"}");
  }
  return canonical_json(value.value());
}

}  // namespace black_start_manager
