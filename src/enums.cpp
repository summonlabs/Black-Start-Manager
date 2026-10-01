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

#include "black_start_manager/attempt.hpp"
#include "black_start_manager/authority.hpp"
#include "black_start_manager/evidence.hpp"
#include "black_start_manager/plan.hpp"
#include "black_start_manager/session.hpp"
#include "black_start_manager/store.hpp"

namespace black_start_manager {

const char* to_string(AuthorityDomain domain) noexcept {
  switch (domain) {
    case AuthorityDomain::Control: return "control";
    case AuthorityDomain::Electrical: return "electrical";
    case AuthorityDomain::Cooling: return "cooling";
    case AuthorityDomain::Network: return "network";
    case AuthorityDomain::Facility: return "facility";
    case AuthorityDomain::Workload: return "workload";
  }
  return "unknown";
}

Result<AuthorityDomain> parse_authority_domain(std::string_view text) {
  if (text == "control") return AuthorityDomain::Control;
  if (text == "electrical") return AuthorityDomain::Electrical;
  if (text == "cooling") return AuthorityDomain::Cooling;
  if (text == "network") return AuthorityDomain::Network;
  if (text == "facility") return AuthorityDomain::Facility;
  if (text == "workload") return AuthorityDomain::Workload;
  return Error(ErrorCode::InvalidArgument,
               "unknown authority domain '" + std::string(text) + "'");
}

const char* to_string(BindingChangeCode code) noexcept {
  switch (code) {
    case BindingChangeCode::FacilityChanged: return "facility_changed";
    case BindingChangeCode::EpochChanged: return "epoch_changed";
    case BindingChangeCode::TopologyChanged: return "topology_changed";
    case BindingChangeCode::PolicyChanged: return "policy_changed";
    case BindingChangeCode::IncidentChanged: return "incident_changed";
    case BindingChangeCode::IncidentGenerationChanged: return "incident_generation_changed";
    case BindingChangeCode::AuthorityAdded: return "authority_added";
    case BindingChangeCode::AuthorityRemoved: return "authority_removed";
    case BindingChangeCode::AuthorityOwnerChanged: return "authority_owner_changed";
    case BindingChangeCode::AuthorityGenerationAdvanced: return "authority_generation_advanced";
    case BindingChangeCode::AuthorityGenerationStale: return "authority_generation_stale";
    case BindingChangeCode::AuthorityAttestationChanged: return "authority_attestation_changed";
    case BindingChangeCode::PrerequisiteAdded: return "prerequisite_added";
    case BindingChangeCode::PrerequisiteRemoved: return "prerequisite_removed";
    case BindingChangeCode::PrerequisiteGenerationAdvanced:
      return "prerequisite_generation_advanced";
    case BindingChangeCode::PrerequisiteGenerationStale: return "prerequisite_generation_stale";
  }
  return "unknown";
}

const char* to_string(EvidenceProvenance provenance) noexcept {
  switch (provenance) {
    case EvidenceProvenance::Live: return "live";
    case EvidenceProvenance::Recovered: return "recovered";
  }
  return "unknown";
}

const char* to_string(EvidenceChannel channel) noexcept {
  switch (channel) {
    case EvidenceChannel::OwnerReported: return "owner_reported";
    case EvidenceChannel::InstrumentedMeasurement: return "instrumented_measurement";
    case EvidenceChannel::SyntheticModel: return "synthetic_model";
    case EvidenceChannel::ManagerDerived: return "manager_derived";
    case EvidenceChannel::OperatorAsserted: return "operator_asserted";
  }
  return "unknown";
}

Result<EvidenceChannel> parse_evidence_channel(std::string_view text) {
  if (text == "owner_reported") return EvidenceChannel::OwnerReported;
  if (text == "instrumented_measurement") return EvidenceChannel::InstrumentedMeasurement;
  if (text == "synthetic_model") return EvidenceChannel::SyntheticModel;
  if (text == "manager_derived") return EvidenceChannel::ManagerDerived;
  if (text == "operator_asserted") return EvidenceChannel::OperatorAsserted;
  return Error(ErrorCode::InvalidArgument,
               "unknown evidence channel '" + std::string(text) + "'");
}

bool channel_is_independent(EvidenceChannel channel) noexcept {
  return channel != EvidenceChannel::ManagerDerived;
}

const char* to_string(EvidenceDeficitCode code) noexcept {
  switch (code) {
    case EvidenceDeficitCode::Missing: return "missing";
    case EvidenceDeficitCode::ExpectationMismatch: return "expectation_mismatch";
    case EvidenceDeficitCode::RecoveredOnly: return "recovered_only";
    case EvidenceDeficitCode::Expired: return "expired";
    case EvidenceDeficitCode::ForeignGeneration: return "foreign_generation";
    case EvidenceDeficitCode::ForeignOwner: return "foreign_owner";
    case EvidenceDeficitCode::NotIndependent: return "not_independent";
    case EvidenceDeficitCode::Contradictory: return "contradictory";
    case EvidenceDeficitCode::InsufficientSources: return "insufficient_sources";
    case EvidenceDeficitCode::KindMismatch: return "kind_mismatch";
  }
  return "unknown";
}

const char* to_string(Priority priority) noexcept {
  switch (priority) {
    case Priority::Protected: return "protected";
    case Priority::Critical: return "critical";
    case Priority::Standard: return "standard";
  }
  return "unknown";
}

Result<Priority> parse_priority(std::string_view text) {
  if (text == "protected") return Priority::Protected;
  if (text == "critical") return Priority::Critical;
  if (text == "standard") return Priority::Standard;
  return Error(ErrorCode::InvalidArgument, "unknown priority '" + std::string(text) + "'");
}

const char* to_string(AttemptState state) noexcept {
  switch (state) {
    case AttemptState::Requested: return "requested";
    case AttemptState::Acknowledged: return "acknowledged";
    case AttemptState::Unresolved: return "unresolved";
    case AttemptState::NotApplied: return "not_applied";
    case AttemptState::Refused: return "refused";
    case AttemptState::Failed: return "failed";
    case AttemptState::Fenced: return "fenced";
  }
  return "unknown";
}

bool attempt_is_in_flight(AttemptState state) noexcept {
  return state == AttemptState::Requested || state == AttemptState::Unresolved;
}

bool attempt_blocks_requests(AttemptState state) noexcept {
  return attempt_is_in_flight(state);
}

bool attempt_consumes_budget(AttemptState state) noexcept {
  // A resolution that proves the owner never applied the effect does not consume the
  // bounded attempt budget: nothing consequential happened.
  return state != AttemptState::NotApplied;
}

const char* to_string(ControllerReplyKind kind) noexcept {
  switch (kind) {
    case ControllerReplyKind::Applied: return "applied";
    case ControllerReplyKind::AlreadyApplied: return "already_applied";
    case ControllerReplyKind::Refused: return "refused";
    case ControllerReplyKind::Failed: return "failed";
    case ControllerReplyKind::Unknown: return "unknown";
    case ControllerReplyKind::Unavailable: return "unavailable";
    case ControllerReplyKind::Deferred: return "deferred";
  }
  return "unknown";
}

const char* to_string(SessionState state) noexcept {
  switch (state) {
    case SessionState::Active: return "active";
    case SessionState::Held: return "held";
    case SessionState::Aborted: return "aborted";
    case SessionState::Completed: return "completed";
    case SessionState::Superseded: return "superseded";
  }
  return "unknown";
}

bool session_is_terminal(SessionState state) noexcept {
  return state == SessionState::Aborted || state == SessionState::Completed ||
         state == SessionState::Superseded;
}

const char* to_string(ObligationState state) noexcept {
  switch (state) {
    case ObligationState::Pending: return "pending";
    case ObligationState::InFlight: return "in_flight";
    case ObligationState::Acknowledged: return "acknowledged";
    case ObligationState::Satisfied: return "satisfied";
    case ObligationState::Failed: return "failed";
    case ObligationState::Fenced: return "fenced";
  }
  return "unknown";
}

const char* to_string(FenceCode code) noexcept {
  switch (code) {
    case FenceCode::EpochChanged: return "epoch_changed";
    case FenceCode::TopologyChanged: return "topology_changed";
    case FenceCode::PolicyChanged: return "policy_changed";
    case FenceCode::IncidentChanged: return "incident_changed";
    case FenceCode::AuthorityRemoved: return "authority_removed";
    case FenceCode::AuthorityOwnerChanged: return "authority_owner_changed";
    case FenceCode::AuthorityGenerationStale: return "authority_generation_stale";
    case FenceCode::AuthorityAttestationChanged: return "authority_attestation_changed";
    case FenceCode::PrerequisiteGenerationStale: return "prerequisite_generation_stale";
    case FenceCode::IncarnationSuperseded: return "incarnation_superseded";
  }
  return "unknown";
}

const char* to_string(BlockCode code) noexcept {
  switch (code) {
    case BlockCode::None: return "none";
    case BlockCode::SessionSuperseded: return "session_superseded";
    case BlockCode::SessionAborted: return "session_aborted";
    case BlockCode::SessionCompleted: return "session_completed";
    case BlockCode::SessionFenced: return "session_fenced";
    case BlockCode::AuthorityNotReestablished: return "authority_not_reestablished";
    case BlockCode::HoldActive: return "hold_active";
    case BlockCode::UnresolvedAttempt: return "unresolved_attempt";
    case BlockCode::EffectNotVerified: return "effect_not_verified";
    case BlockCode::AttemptBudgetExhausted: return "attempt_budget_exhausted";
    case BlockCode::StageEntryEvidenceMissing: return "stage_entry_evidence_missing";
    case BlockCode::StageEntryEvidenceStale: return "stage_entry_evidence_stale";
    case BlockCode::PrecedenceBlocked: return "precedence_blocked";
    case BlockCode::PrerequisiteUnsatisfied: return "prerequisite_unsatisfied";
    case BlockCode::EvidenceMissing: return "evidence_missing";
    case BlockCode::EvidenceStale: return "evidence_stale";
    case BlockCode::EvidenceRecovered: return "evidence_recovered";
    case BlockCode::EvidenceNotIndependent: return "evidence_not_independent";
    case BlockCode::EvidenceContradictory: return "evidence_contradictory";
    case BlockCode::EvidenceInsufficient: return "evidence_insufficient";
    case BlockCode::CompletionEvidenceMissing: return "completion_evidence_missing";
    case BlockCode::NoEligibleObligation: return "no_eligible_obligation";
  }
  return "unknown";
}

int block_precedence(BlockCode code) noexcept {
  const int value = static_cast<int>(code);
  return value < 0 ? 0 : value;
}

const char* to_string(JournalRecordKind kind) noexcept {
  switch (kind) {
    case JournalRecordKind::SessionEstablished: return "session_established";
    case JournalRecordKind::SessionReplanned: return "session_replanned";
    case JournalRecordKind::AuthorityReestablished: return "authority_reestablished";
    case JournalRecordKind::EvidenceAdmitted: return "evidence_admitted";
    case JournalRecordKind::AttemptRecorded: return "attempt_recorded";
    case JournalRecordKind::AttemptSettled: return "attempt_settled";
    case JournalRecordKind::AttemptResolved: return "attempt_resolved";
    case JournalRecordKind::StageAdvanced: return "stage_advanced";
    case JournalRecordKind::SessionHeld: return "session_held";
    case JournalRecordKind::SessionResumed: return "session_resumed";
    case JournalRecordKind::SessionAborted: return "session_aborted";
    case JournalRecordKind::SessionCompleted: return "session_completed";
    case JournalRecordKind::SessionSuperseded: return "session_superseded";
    case JournalRecordKind::SessionsDemoted: return "sessions_demoted";
    case JournalRecordKind::PlantEffectApplied: return "plant_effect_applied";
  }
  return "unknown";
}

Result<JournalRecordKind> parse_journal_record_kind(std::uint64_t value) {
  if (value >= 1 && value <= 15) {
    return static_cast<JournalRecordKind>(value);
  }
  return Error(ErrorCode::UnsupportedFormatVersion,
               "journal record kind " + std::to_string(value) + " is not supported");
}

}  // namespace black_start_manager
