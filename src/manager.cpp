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

#include "black_start_manager/manager.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "black_start_manager/checked.hpp"
#include "detail/assessment.hpp"
#include "detail/crash_point.hpp"
#include "detail/model_json.hpp"
#include "detail/session_parts.hpp"

namespace black_start_manager {
namespace {

// Re-entrancy guard. A controller callback or a facility provider that calls back into a
// mutating operation on this thread would otherwise deadlock on the operation mutex; the
// manager refuses the nested call instead. This is the positive half of the
// lock-reentrancy audit: locked state is never handed to external code.
thread_local int g_operation_depth = 0;

class OperationScope {
 public:
  OperationScope() { ++g_operation_depth; }
  ~OperationScope() { --g_operation_depth; }
  OperationScope(const OperationScope&) = delete;
  OperationScope& operator=(const OperationScope&) = delete;
};

[[nodiscard]] Error reentrant_error() {
  return Error(ErrorCode::ReentrantOperation,
               "a mutating operation was invoked from inside another operation; the "
               "manager refuses the nested call rather than deadlocking");
}

[[nodiscard]] Result<AuthorityRef> require_authority(
    const FacilityBinding& binding, const AuthorityRef& presented,
    std::initializer_list<AuthorityDomain> allowed) {
  bool allowed_domain = false;
  for (const AuthorityDomain domain : allowed) {
    if (presented.domain == domain) {
      allowed_domain = true;
    }
  }
  if (!allowed_domain) {
    return Error(ErrorCode::AuthorityMismatch,
                 "this operation requires an authority in a different domain than " +
                     std::string(to_string(presented.domain)));
  }
  if (presented.owner.empty()) {
    return Error(ErrorCode::AuthorityMissing, "presented authority names no owner");
  }
  const AuthorityRef* bound = find_authority(binding, presented.domain, presented.owner);
  if (bound == nullptr) {
    return Error(ErrorCode::AuthorityMissing,
                 "presented authority owner '" + presented.owner.str() +
                     "' is not part of the bound facility state");
  }
  if (bound->generation != presented.generation) {
    return Error(ErrorCode::AuthorityStale,
                 "presented authority generation " + std::to_string(presented.generation) +
                     " does not match the bound generation " +
                     std::to_string(bound->generation));
  }
  if (!(bound->attestation == presented.attestation) || !(bound->id == presented.id)) {
    return Error(ErrorCode::AuthorityMismatch,
                 "presented authority attestation does not match the bound attestation");
  }
  return *bound;
}

struct SessionContext {
  std::shared_ptr<const DerivedPlan> plan;
  SessionRecord record;
};

[[nodiscard]] Result<JsonValue> snapshot_payload_of(
    const std::map<SessionId, SessionContext>& sessions, const IncarnationId& incarnation) {
  JsonArrayBuilder entries;
  for (const auto& entry : sessions) {
    JsonObjectBuilder item;
    BSM_TRY_ASSIGN(plan_value, entry.second.plan->to_json());
    item.set("plan", plan_value);
    BSM_TRY_ASSIGN(record_value, session_to_json(entry.second.record));
    item.set("record", record_value);
    BSM_TRY_ASSIGN(item_value, item.build());
    entries.push(item_value);
  }
  BSM_TRY_ASSIGN(entries_value, entries.build());
  JsonObjectBuilder root;
  root.set_uint("format_version", limits::kStateFormatVersion);
  root.set_text("incarnation", incarnation.hex());
  root.set("sessions", entries_value);
  return root.build();
}

// Rebuilds the session table from a published snapshot payload. An initialized store has
// an empty payload, which is not an error.
[[nodiscard]] Result<Unit> load_snapshot_state(
    std::map<SessionId, SessionContext>& sessions, const JsonValue& payload) {
  if (!payload.is_object()) {
    return Error(ErrorCode::SchemaViolation,
                 "a store snapshot payload must be a canonical object");
  }
  if (payload.as_object().empty()) {
    return Unit{};
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      payload, {"format_version", "incarnation", "sessions"}));
  BSM_TRY_ASSIGN(version, json::require_unsigned(payload, "format_version"));
  if (version != limits::kStateFormatVersion) {
    return Error(ErrorCode::UnsupportedFormatVersion,
                 "the store snapshot format version " + std::to_string(version) +
                     " is not supported by this build");
  }
  BSM_TRY_ASSIGN(entries, json::require_array(payload, "sessions"));
  for (const JsonValue& entry : *entries) {
    if (!entry.is_object()) {
      return Error(ErrorCode::SchemaViolation, "a snapshot session entry must be an object");
    }
    BSM_RETURN_IF_ERROR(json::reject_unknown_fields(entry, {"plan", "record"}));
    BSM_TRY_ASSIGN(record_value, json::require_object_member(entry, "record"));
    BSM_TRY_ASSIGN(session, session_from_json(*record_value));
    BSM_TRY_ASSIGN(plan_value, json::require_object_member(entry, "plan"));
    BSM_TRY_ASSIGN(plan, DerivedPlan::from_json(*plan_value));
    if (!(plan.digest() == session.plan_digest)) {
      return Error(ErrorCode::PlanDigestMismatch,
                   "a snapshot session does not match the plan it records");
    }
    if (sessions.find(session.id) != sessions.end()) {
      return Error(ErrorCode::DuplicateIdentity,
                   "a snapshot records the same session twice");
    }
    SessionContext context;
    context.plan = std::make_shared<const DerivedPlan>(std::move(plan));
    context.record = std::move(session);
    sessions.emplace(context.record.id, std::move(context));
  }
  return Unit{};
}

[[nodiscard]] AttemptRecord* find_attempt(SessionRecord& session, const AttemptId& id) {
  for (AttemptRecord& attempt : session.attempts) {
    if (attempt.id == id) {
      return &attempt;
    }
  }
  return nullptr;
}

[[nodiscard]] const AttemptRecord* find_attempt(const SessionRecord& session,
                                                const AttemptId& id) {
  for (const AttemptRecord& attempt : session.attempts) {
    if (attempt.id == id) {
      return &attempt;
    }
  }
  return nullptr;
}

[[nodiscard]] Result<AttemptId> read_optional_attempt(const JsonValue& value) {
  BSM_TRY_ASSIGN(field, json::require_field(value, "attempt"));
  if (field->is_null()) {
    return AttemptId();
  }
  BSM_TRY_ASSIGN(text, json::require_text(*field, "attempt identity", 64));
  return AttemptId::from_hex(text);
}

[[nodiscard]] Result<std::vector<AttemptId>> read_attempt_list(const JsonValue& value,
                                                               const char* field) {
  BSM_TRY_ASSIGN(array, json::require_array(value, field));
  std::vector<AttemptId> ids;
  ids.reserve(array->size());
  for (const JsonValue& item : *array) {
    BSM_TRY_ASSIGN(text, json::require_text(item, "attempt identity", 64));
    BSM_TRY_ASSIGN(id, AttemptId::from_hex(text));
    ids.push_back(id);
  }
  return ids;
}

[[nodiscard]] Result<std::vector<EvidenceId>> read_evidence_list(const JsonValue& value,
                                                                 const char* field) {
  BSM_TRY_ASSIGN(array, json::require_array(value, field));
  std::vector<EvidenceId> ids;
  ids.reserve(array->size());
  for (const JsonValue& item : *array) {
    BSM_TRY_ASSIGN(text, json::require_text(item, "evidence identity", 64));
    BSM_TRY_ASSIGN(id, EvidenceId::from_hex(text));
    ids.push_back(id);
  }
  return ids;
}

[[nodiscard]] SessionContext* find_context(std::map<SessionId, SessionContext>& sessions,
                                           const SessionId& id) {
  const auto found = sessions.find(id);
  return found == sessions.end() ? nullptr : &found->second;
}

[[nodiscard]] Error terminal_error(const SessionRecord& session) {
  return Error(ErrorCode::SessionTerminal,
               "session is " + std::string(to_string(session.state)) +
                   " and accepts no further progress");
}

[[nodiscard]] Error fenced_error(const Assessment& assessment) {
  Error error(ErrorCode::SessionFenced,
              "the bound facility state no longer matches the session binding");
  JsonObjectBuilder detail;
  JsonArrayBuilder codes;
  for (const FenceCode code : assessment.fence_codes) {
    codes.push_text(to_string(code));
  }
  const Result<JsonValue> codes_value = codes.build();
  if (codes_value.ok()) {
    detail.set("fence_codes", codes_value.value());
  }
  const Result<JsonValue> detail_value = detail.build();
  if (detail_value.ok()) {
    return error.with_detail(detail_value.value());
  }
  return error;
}

// Maps a derived obligation block to the error the caller sees. The mapping is total so
// a refusal always names the gate that stopped it.
[[nodiscard]] Error block_error(BlockCode code, const ObligationAssessment& entry) {
  Error error(ErrorCode::GateNotReady,
              std::string("obligation '") + entry.id.str() + "' is blocked by " +
                  to_string(code));
  switch (code) {
    case BlockCode::SessionFenced:
      return Error(ErrorCode::SessionFenced,
                   "the session is fenced and may not request effects");
    case BlockCode::AuthorityNotReestablished:
      return Error(ErrorCode::AuthorityNotReestablished,
                   "process authority has not been re-established by this incarnation");
    case BlockCode::HoldActive:
      return Error(ErrorCode::SessionHeld, "the session is held");
    case BlockCode::UnresolvedAttempt:
      error = Error(ErrorCode::AttemptUnresolved,
                    "obligation '" + entry.id.str() +
                        "' has an unresolved attempt that must be resolved first");
      break;
    case BlockCode::EffectNotVerified:
      error = Error(ErrorCode::GateNotReady,
                    "obligation '" + entry.id.str() +
                        "' has an acknowledged effect whose readiness evidence is not "
                        "verified; record the readiness observation instead of repeating "
                        "the consequential request");
      break;
    case BlockCode::AttemptBudgetExhausted:
      error = Error(ErrorCode::AttemptBudgetExhausted,
                    "obligation '" + entry.id.str() +
                        "' has exhausted its bounded attempt budget");
      break;
    case BlockCode::PrecedenceBlocked:
      error = Error(ErrorCode::GatePrecedenceBlocked,
                    "an earlier obligation of the same stage is not satisfied, so "
                    "obligation '" + entry.id.str() + "' is not yet eligible");
      break;
    case BlockCode::PrerequisiteUnsatisfied:
      error = Error(ErrorCode::GatePrerequisiteUnsatisfied,
                    "a prerequisite of obligation '" + entry.id.str() +
                        "' is not satisfied");
      break;
    case BlockCode::StageEntryEvidenceMissing:
      error = Error(ErrorCode::GateStageEvidenceMissing,
                    "the current stage entry evidence is missing, so obligation '" +
                        entry.id.str() + "' is not yet eligible");
      break;
    case BlockCode::StageEntryEvidenceStale:
      error = Error(ErrorCode::GateStageEvidenceStale,
                    "the current stage entry evidence is stale, so obligation '" +
                        entry.id.str() + "' is not yet eligible");
      break;
    default:
      break;
  }
  JsonObjectBuilder detail;
  detail.set_text("obligation", entry.id.str());
  detail.set_text("block", to_string(code));
  JsonArrayBuilder deficits;
  for (const EvidenceDeficit& deficit : entry.deficits) {
    JsonObjectBuilder item;
    item.set_text("kind", deficit.kind.str());
    item.set_text("subject", deficit.subject.str());
    item.set_text("deficit", to_string(deficit.code));
    item.set_uint("required_sources", deficit.required_sources);
    item.set_uint("current_sources", deficit.current_sources);
    const Result<JsonValue> item_value = item.build();
    if (item_value.ok()) {
      deficits.push(item_value.value());
    }
  }
  const Result<JsonValue> deficits_value = deficits.build();
  if (deficits_value.ok()) {
    detail.set("deficits", deficits_value.value());
  }
  const Result<JsonValue> detail_value = detail.build();
  if (detail_value.ok()) {
    return error.with_detail(detail_value.value());
  }
  return error;
}

[[nodiscard]] Result<Unit> apply_journal_record(std::map<SessionId, SessionContext>& sessions,
                                                const JournalRecord& record) {
  const JsonValue& payload = record.payload;
  BSM_TRY_ASSIGN(session_text, json::require_string(payload, "session"));
  BSM_TRY_ASSIGN(session_id, SessionId::from_hex(session_text));

  switch (record.kind) {
    case JournalRecordKind::SessionEstablished: {
      if (sessions.find(session_id) != sessions.end()) {
        return Error(ErrorCode::DuplicateIdentity,
                     "the journal establishes the same session twice");
      }
      BSM_TRY_ASSIGN(record_value, json::require_object_member(payload, "record"));
      BSM_TRY_ASSIGN(session, session_from_json(*record_value));
      BSM_TRY_ASSIGN(plan_value, json::require_object_member(payload, "plan"));
      BSM_TRY_ASSIGN(plan, DerivedPlan::from_json(*plan_value));
      if (!(session.id == session_id)) {
        return Error(ErrorCode::SchemaViolation,
                     "an established session record does not match its identity");
      }
      if (!(plan.digest() == session.plan_digest)) {
        return Error(ErrorCode::PlanDigestMismatch,
                     "an established session does not match the plan it records");
      }
      SessionContext context;
      context.plan = std::make_shared<const DerivedPlan>(std::move(plan));
      context.record = std::move(session);
      sessions.emplace(session_id, std::move(context));
      return Unit{};
    }
    case JournalRecordKind::EvidenceAdmitted: {
      SessionContext* context = find_context(sessions, session_id);
      if (context == nullptr) {
        return Error(ErrorCode::SessionNotFound,
                     "an admitted evidence record names an unknown session");
      }
      BSM_TRY_ASSIGN(evidence_value, json::require_object_member(payload, "evidence"));
      BSM_TRY_ASSIGN(evidence, evidence_from_json(*evidence_value));
      if (!(evidence.session == session_id)) {
        return Error(ErrorCode::EvidenceForeignSession,
                     "admitted evidence belongs to a different session");
      }
      for (const EvidenceRecord& existing : context->record.evidence) {
        if (existing.id == evidence.id) {
          return Error(ErrorCode::DuplicateIdentity,
                       "the journal admits the same evidence identity twice");
        }
      }
      BSM_TRY_ASSIGN(attempt_id, read_optional_attempt(payload));
      context->record.evidence.push_back(evidence);
      if (!attempt_id.is_zero()) {
        AttemptRecord* attempt = find_attempt(context->record, attempt_id);
        if (attempt == nullptr) {
          return Error(ErrorCode::AttemptNotFound,
                       "admitted evidence names an attempt the session does not record");
        }
        if (std::find(attempt->evidence.begin(), attempt->evidence.end(), evidence.id) ==
            attempt->evidence.end()) {
          attempt->evidence.push_back(evidence.id);
        }
      }
      return Unit{};
    }
    case JournalRecordKind::AttemptRecorded: {
      SessionContext* context = find_context(sessions, session_id);
      if (context == nullptr) {
        return Error(ErrorCode::SessionNotFound,
                     "a recorded attempt names an unknown session");
      }
      BSM_TRY_ASSIGN(attempt_value, json::require_object_member(payload, "attempt"));
      BSM_TRY_ASSIGN(attempt, attempt_from_json(*attempt_value));
      if (!(attempt.session == session_id)) {
        return Error(ErrorCode::SchemaViolation,
                     "a recorded attempt belongs to a different session");
      }
      if (find_attempt(context->record, attempt.id) != nullptr) {
        return Error(ErrorCode::DuplicateIdentity,
                     "the journal records the same attempt twice");
      }
      context->record.attempts.push_back(std::move(attempt));
      return Unit{};
    }
    case JournalRecordKind::AttemptSettled:
    case JournalRecordKind::AttemptResolved: {
      SessionContext* context = find_context(sessions, session_id);
      if (context == nullptr) {
        return Error(ErrorCode::SessionNotFound,
                     "a settled attempt names an unknown session");
      }
      BSM_TRY_ASSIGN(attempt_value, json::require_object_member(payload, "attempt"));
      BSM_TRY_ASSIGN(attempt, attempt_from_json(*attempt_value));
      AttemptRecord* existing = find_attempt(context->record, attempt.id);
      if (existing == nullptr) {
        return Error(ErrorCode::AttemptNotFound,
                     "the journal settles an attempt that was never recorded");
      }
      if (existing->sequence != attempt.sequence || !(existing->key == attempt.key) ||
          !(existing->plan_digest == attempt.plan_digest)) {
        return Error(ErrorCode::AttemptStateInvalid,
                     "a settled attempt does not match the recorded request identity");
      }
      *existing = std::move(attempt);
      return Unit{};
    }
    case JournalRecordKind::StageAdvanced: {
      SessionContext* context = find_context(sessions, session_id);
      if (context == nullptr) {
        return Error(ErrorCode::SessionNotFound,
                     "a stage transition names an unknown session");
      }
      BSM_TRY_ASSIGN(transition_value, json::require_object_member(payload, "transition"));
      BSM_TRY_ASSIGN(transition, detail::transition_from_json(*transition_value));
      if (transition.to_index >= limits::kMaxStages) {
        return Error(ErrorCode::SchemaViolation, "stage transition index is out of range");
      }
      context->record.stage_index = transition.to_index;
      context->record.stage_history.push_back(std::move(transition));
      return Unit{};
    }
    case JournalRecordKind::SessionHeld: {
      SessionContext* context = find_context(sessions, session_id);
      if (context == nullptr) {
        return Error(ErrorCode::SessionNotFound, "a hold names an unknown session");
      }
      BSM_TRY_ASSIGN(hold_value, json::require_object_member(payload, "hold"));
      BSM_TRY_ASSIGN(hold, detail::hold_from_json(*hold_value));
      context->record.state = SessionState::Held;
      context->record.holds.push_back(std::move(hold));
      return Unit{};
    }
    case JournalRecordKind::SessionResumed: {
      SessionContext* context = find_context(sessions, session_id);
      if (context == nullptr) {
        return Error(ErrorCode::SessionNotFound, "a resume names an unknown session");
      }
      BSM_TRY_ASSIGN(index, json::require_unsigned(payload, "hold_index"));
      BSM_TRY_ASSIGN(tick, json::require_unsigned(payload, "tick"));
      BSM_TRY_ASSIGN(reason, json::require_string(payload, "reason"));
      if (index >= context->record.holds.size()) {
        return Error(ErrorCode::SchemaViolation, "a resumed hold index is out of range");
      }
      HoldRecord& hold = context->record.holds[static_cast<std::size_t>(index)];
      if (hold.released) {
        return Error(ErrorCode::SessionNotHeld, "the journal resumes a released hold");
      }
      hold.released = true;
      hold.released_tick = tick;
      hold.release_reason = std::move(reason);
      context->record.state = SessionState::Active;
      return Unit{};
    }
    case JournalRecordKind::SessionAborted:
    case JournalRecordKind::SessionCompleted:
    case JournalRecordKind::SessionSuperseded: {
      SessionContext* context = find_context(sessions, session_id);
      if (context == nullptr) {
        return Error(ErrorCode::SessionNotFound,
                     "a terminal record names an unknown session");
      }
      BSM_TRY_ASSIGN(terminal_value, json::require_object_member(payload, "terminal"));
      BSM_TRY_ASSIGN(terminal, detail::terminal_from_json(*terminal_value));
      if (context->record.has_terminal) {
        return Error(ErrorCode::SessionTerminal,
                     "the journal terminates the same session twice");
      }
      context->record.state = terminal.state;
      context->record.has_terminal = true;
      context->record.terminal = std::move(terminal);
      BSM_TRY_ASSIGN(fenced, read_attempt_list(payload, "fenced_attempts"));
      for (const AttemptId& id : fenced) {
        AttemptRecord* attempt = find_attempt(context->record, id);
        if (attempt == nullptr) {
          return Error(ErrorCode::AttemptNotFound,
                       "a terminal record fences an attempt the session does not record");
        }
        attempt->state = AttemptState::Fenced;
      }
      return Unit{};
    }
    case JournalRecordKind::AuthorityReestablished: {
      SessionContext* context = find_context(sessions, session_id);
      if (context == nullptr) {
        return Error(ErrorCode::SessionNotFound,
                     "an authority record names an unknown session");
      }
      BSM_TRY_ASSIGN(binding_value, json::require_object_member(payload, "binding"));
      BSM_TRY_ASSIGN(binding, binding_from_json(*binding_value));
      BSM_TRY_ASSIGN(incarnation_text, json::require_string(payload, "incarnation"));
      BSM_TRY_ASSIGN(incarnation, IncarnationId::from_hex(incarnation_text));
      BSM_TRY_ASSIGN(tick, json::require_unsigned(payload, "tick"));
      BSM_TRY_ASSIGN(digest, binding_digest(binding));
      context->record.binding = std::move(binding);
      context->record.binding_digest = digest;
      context->record.authority_incarnation = incarnation;
      context->record.authority_reestablished = true;
      context->record.updated_tick = tick;
      return Unit{};
    }
    case JournalRecordKind::SessionReplanned: {
      SessionContext* context = find_context(sessions, session_id);
      if (context == nullptr) {
        return Error(ErrorCode::SessionNotFound, "a replan names an unknown session");
      }
      BSM_TRY_ASSIGN(plan_value, json::require_object_member(payload, "plan"));
      BSM_TRY_ASSIGN(plan, DerivedPlan::from_json(*plan_value));
      BSM_TRY_ASSIGN(binding_value, json::require_object_member(payload, "binding"));
      BSM_TRY_ASSIGN(binding, binding_from_json(*binding_value));
      BSM_TRY_ASSIGN(replan_value, json::require_object_member(payload, "replan"));
      BSM_TRY_ASSIGN(replan, detail::replan_from_json(*replan_value));
      BSM_TRY_ASSIGN(stage_index, json::require_unsigned(payload, "stage_index"));
      BSM_TRY_ASSIGN(incarnation_text, json::require_string(payload, "incarnation"));
      BSM_TRY_ASSIGN(incarnation, IncarnationId::from_hex(incarnation_text));
      BSM_TRY_ASSIGN(tick, json::require_unsigned(payload, "tick"));
      BSM_TRY_ASSIGN(digest, binding_digest(binding));
      if (!(replan.new_plan_digest == plan.digest())) {
        return Error(ErrorCode::PlanDigestMismatch,
                     "a replan record does not match the plan it carries");
      }
      if (!(replan.previous_plan_digest == context->record.plan_digest)) {
        return Error(ErrorCode::PlanDigestMismatch,
                     "a replan record does not continue from the plan it replaced");
      }
      context->plan = std::make_shared<const DerivedPlan>(std::move(plan));
      context->record.plan_digest = replan.new_plan_digest;
      context->record.binding = std::move(binding);
      context->record.binding_digest = digest;
      context->record.authority_incarnation = incarnation;
      context->record.authority_reestablished = true;
      context->record.updated_tick = tick;
      context->record.stage_index = static_cast<std::size_t>(stage_index);
      context->record.replans.push_back(replan);
      BSM_TRY_ASSIGN(fenced, read_attempt_list(payload, "fenced_attempts"));
      for (const AttemptId& id : fenced) {
        AttemptRecord* attempt = find_attempt(context->record, id);
        if (attempt == nullptr) {
          return Error(ErrorCode::AttemptNotFound,
                       "a replan fences an attempt the session does not record");
        }
        attempt->state = AttemptState::Fenced;
      }
      return Unit{};
    }
    case JournalRecordKind::SessionsDemoted: {
      BSM_TRY_ASSIGN(tick, json::require_unsigned(payload, "tick"));
      BSM_TRY_ASSIGN(entries, json::require_array(payload, "sessions"));
      for (const JsonValue& entry : *entries) {
        BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
            entry, {"id", "tick", "demoted_evidence", "unresolved_attempts"}));
        BSM_TRY_ASSIGN(entry_text, json::require_string(entry, "id"));
        BSM_TRY_ASSIGN(entry_id, SessionId::from_hex(entry_text));
        BSM_TRY_ASSIGN(recorded_tick, json::require_unsigned(entry, "tick"));
        const std::uint64_t entry_tick = std::max(tick, recorded_tick);
        SessionContext* context = find_context(sessions, entry_id);
        if (context == nullptr) {
          return Error(ErrorCode::SessionNotFound,
                       "a demotion names an unknown session");
        }
        BSM_TRY_ASSIGN(evidence_ids, read_evidence_list(entry, "demoted_evidence"));
        for (const EvidenceId& id : evidence_ids) {
          bool found = false;
          for (EvidenceRecord& evidence_record : context->record.evidence) {
            if (evidence_record.id == id) {
              evidence_record.provenance = EvidenceProvenance::Recovered;
              evidence_record.recovered_tick = entry_tick;
              found = true;
              break;
            }
          }
          if (!found) {
            return Error(ErrorCode::EvidenceNotFound,
                         "a demotion names evidence the session does not record");
          }
        }
        BSM_TRY_ASSIGN(attempt_ids, read_attempt_list(entry, "unresolved_attempts"));
        for (const AttemptId& id : attempt_ids) {
          AttemptRecord* attempt = find_attempt(context->record, id);
          if (attempt == nullptr) {
            return Error(ErrorCode::AttemptNotFound,
                         "a demotion names an attempt the session does not record");
          }
          if (attempt->state == AttemptState::Requested) {
            attempt->state = AttemptState::Unresolved;
            attempt->settled_tick = entry_tick;
            attempt->resolution_note =
                "recovered unresolved: the recording incarnation ended with this "
                "attempt in flight";
          }
        }
        context->record.authority_reestablished = false;
        context->record.updated_tick = std::max(entry_tick, context->record.updated_tick);
        if (context->record.revision < limits::kMaxGeneration) {
          ++context->record.revision;
        }
      }
      return Unit{};
    }
  }
  return Error(ErrorCode::UnsupportedFormatVersion, "unknown journal record kind");
}

// True when the plan requires evidence of this kind and subject anywhere: on an
// obligation, on a stage entry or exit gate, or as return-to-service proof.
[[nodiscard]] bool plan_requires_evidence(const DerivedPlan& plan, const EvidenceKind& kind,
                                          const SubjectId& subject) {
  const auto matches = [&kind, &subject](const std::vector<EvidenceRequirement>& list) {
    for (const EvidenceRequirement& requirement : list) {
      if (requirement.kind == kind && requirement.subject == subject) {
        return true;
      }
    }
    return false;
  };
  for (const ObligationDefinition& obligation : plan.obligations()) {
    if (matches(obligation.required_evidence)) {
      return true;
    }
  }
  for (const StageDefinition& stage : plan.document().stages) {
    if (matches(stage.entry_evidence) || matches(stage.exit_evidence)) {
      return true;
    }
  }
  return matches(plan.document().return_to_service.evidence);
}

[[nodiscard]] Digest completion_proof_digest(const SessionRecord& session,
                                             const Assessment& assessment,
                                             const std::vector<EvidenceId>& proof,
                                             std::uint64_t tick) {
  JsonObjectBuilder root;
  root.set_text("session", session.id.hex());
  root.set_text("plan_digest", session.plan_digest.hex());
  root.set_text("binding_digest", session.binding_digest.hex());
  root.set_uint("tick", tick);
  JsonArrayBuilder obligations;
  for (const ObligationAssessment& obligation : assessment.obligations) {
    JsonObjectBuilder entry;
    entry.set_text("id", obligation.id.str());
    entry.set_bool("satisfied", obligation.satisfied);
    JsonArrayBuilder evidence;
    for (const EvidenceId& id : obligation.satisfied_by) {
      evidence.push_text(id.hex());
    }
    const Result<JsonValue> evidence_value = evidence.build();
    if (evidence_value.ok()) {
      entry.set("evidence", evidence_value.value());
    }
    const Result<JsonValue> entry_value = entry.build();
    if (entry_value.ok()) {
      obligations.push(entry_value.value());
    }
  }
  const Result<JsonValue> obligations_value = obligations.build();
  if (obligations_value.ok()) {
    root.set("obligations", obligations_value.value());
  }
  JsonArrayBuilder proof_ids;
  for (const EvidenceId& id : proof) {
    proof_ids.push_text(id.hex());
  }
  const Result<JsonValue> proof_value = proof_ids.build();
  if (proof_value.ok()) {
    root.set("proof_evidence", proof_value.value());
  }
  const Result<JsonValue> value = root.build();
  if (!value.ok()) {
    return Digest();
  }
  return Digest::of(canonical_json(value.value()));
}

[[nodiscard]] SessionView view_of(const SessionRecord& record, const DerivedPlan& plan) {
  SessionView view;
  view.id = record.id;
  view.state = record.state;
  view.plan_digest = record.plan_digest;
  view.binding_digest = record.binding_digest;
  view.revision = record.revision;
  view.updated_tick = record.updated_tick;
  view.stage_index = record.stage_index;
  if (record.stage_index < plan.stages().size()) {
    view.stage = plan.stages()[record.stage_index].id;
  }
  return view;
}

}  // namespace

struct SessionManager::Impl {
  ManagerOptions options;
  Store store;
  std::map<SessionId, SessionContext> sessions;
  mutable std::mutex state_mutex;
  std::mutex operation_mutex;

  [[nodiscard]] Result<std::uint64_t> now_tick() const {
    if (options.clock == nullptr) {
      return Error(ErrorCode::InvalidArgument, "manager has no clock configured");
    }
    return options.clock->now_ticks();
  }

  [[nodiscard]] Result<FacilityBinding> observed_binding() const {
    if (options.facility == nullptr) {
      return Error(ErrorCode::InvalidArgument,
                   "manager has no facility state provider configured");
    }
    BSM_TRY_ASSIGN(binding, options.facility->current_binding());
    BSM_RETURN_IF_ERROR(validate_binding(binding));
    return binding;
  }

  [[nodiscard]] Result<SessionContext> context_of(const SessionId& id) const {
    const std::lock_guard<std::mutex> guard(state_mutex);
    const auto found = sessions.find(id);
    if (found == sessions.end()) {
      return Error(ErrorCode::SessionNotFound,
                   "session " + id.hex() + " is not present in this store");
    }
    return found->second;
  }

  [[nodiscard]] Result<SessionContext> single_context() const {
    const std::lock_guard<std::mutex> guard(state_mutex);
    if (sessions.empty()) {
      return Error(ErrorCode::SessionNotFound, "no restoration session exists");
    }
    if (sessions.size() > 1) {
      return Error(ErrorCode::InvalidArgument,
                   "more than one session exists; name the session explicitly");
    }
    return sessions.begin()->second;
  }

  [[nodiscard]] Result<JsonValue> snapshot() const {
    const std::lock_guard<std::mutex> guard(state_mutex);
    return snapshot_payload_of(sessions, store.incarnation());
  }

  // Commit point: the durable record is written and flushed before the in-memory state
  // changes, so a crash between the two is recovered from the journal alone.
  [[nodiscard]] Result<Unit> commit(JournalRecordKind kind, JsonValue payload,
                                    SessionContext updated) {
    BSM_RETURN_IF_ERROR(store.append(kind, std::move(payload)));
    {
      const std::lock_guard<std::mutex> guard(state_mutex);
      auto found = sessions.find(updated.record.id);
      if (found == sessions.end()) {
        sessions.emplace(updated.record.id, std::move(updated));
      } else {
        found->second = std::move(updated);
      }
    }
    BSM_TRY_ASSIGN(payload_value, snapshot());
    BSM_TRY_ASSIGN(compacted, store.maybe_compact(payload_value));
    (void)compacted;
    return Unit{};
  }

  [[nodiscard]] Result<Unit> fulfill_progress_gate(const SessionRecord& record,
                                                   const Assessment& assessment) const {
    if (session_is_terminal(record.state)) {
      return terminal_error(record);
    }
    if (assessment.fenced) {
      return fenced_error(assessment);
    }
    if (!record.authority_reestablished) {
      return Error(ErrorCode::AuthorityNotReestablished,
                   "process authority has not been re-established by this incarnation; "
                   "re-establish authority before the session may progress");
    }
    if (record.state == SessionState::Held) {
      return Error(ErrorCode::SessionHeld, "the session is held");
    }
    return Unit{};
  }

  [[nodiscard]] Result<SessionView> establish(const EstablishSessionRequest& request);
  [[nodiscard]] Result<SessionView> reestablish(const ReestablishRequest& request);
  [[nodiscard]] Result<SessionView> hold(const HoldRequest& request);
  [[nodiscard]] Result<SessionView> resume(const ResumeRequest& request);
  [[nodiscard]] Result<SessionView> replan(const ReplanRequest& request);
  [[nodiscard]] Result<SessionView> abort(const AbortRequest& request);
  [[nodiscard]] Result<AttemptView> request_effect(const EffectRequest& request);
  [[nodiscard]] Result<AttemptView> resolve_attempt(const AttemptResolution& request);
  [[nodiscard]] Result<EvidenceView> record_observation(const Observation& observation);
  [[nodiscard]] Result<EvidenceView> observe_subject(const ObserveRequest& request);
  [[nodiscard]] Result<StageTransitionView> advance_stage(const StageAdvance& request);
  [[nodiscard]] Result<CompletionView> complete_session(const CompletionRequest& request);
  [[nodiscard]] Result<Assessment> assess(const SessionId& id) const;
  [[nodiscard]] Result<JsonValue> report(const SessionId& id,
                                         const ReportOptions& report_options) const;
  [[nodiscard]] Result<JsonValue> status() const;
  [[nodiscard]] Result<JsonValue> verify_store() const;
  [[nodiscard]] Result<std::vector<SessionId>> session_ids() const;
  [[nodiscard]] Result<Unit> demote_recovered_sessions(std::uint64_t tick);
};

namespace {

// Builds the admission record for one observation. Everything the gate depends on is
// decided here, so admission and assessment can never disagree about what a record means.
[[nodiscard]] Result<EvidenceRecord> build_evidence_record(const SessionRecord& session,
                                                           const Observation& observation,
                                                           std::uint64_t tick) {
  EvidenceRecord record;
  record.session = observation.session;
  record.kind = observation.kind;
  record.subject = observation.subject;
  record.has_obligation = observation.has_obligation;
  record.obligation = observation.obligation;
  record.has_stage = observation.has_stage;
  record.stage = observation.stage;
  record.value = observation.value;
  record.source_owner = observation.source_owner;
  record.source_domain = observation.source_domain;
  record.source_generation = observation.source_generation;
  record.channel = observation.channel;
  record.observed_tick = observation.observed_tick;
  record.plan_digest = session.plan_digest;
  record.binding_digest = session.binding_digest;
  record.facility_epoch = session.binding.facility_epoch;
  // Evidence observed before the session existed, or recovered from earlier state, is
  // recorded as recovered and can never satisfy a gate. Freshness is re-established by a
  // new observation, never by promotion.
  if (observation.observed_tick < session.opened_tick) {
    record.provenance = EvidenceProvenance::Recovered;
    record.recovered_tick = tick;
  } else {
    record.provenance = EvidenceProvenance::Live;
    record.recovered_tick = 0;
  }
  BSM_TRY_ASSIGN(content, evidence_content_digest(record));
  record.content_digest = content;
  record.id = EvidenceId(content.bytes());
  BSM_RETURN_IF_ERROR(validate_evidence_record(record));
  return record;
}

}  // namespace

Result<Unit> SessionManager::Impl::demote_recovered_sessions(std::uint64_t tick) {
  if (sessions.empty()) {
    return Unit{};
  }
  JsonArrayBuilder entries;
  for (const auto& entry : sessions) {
    JsonObjectBuilder item;
    item.set_text("id", entry.first.hex());
    // A session that already recorded later logical time keeps it: recovery never moves a
    // durable session backwards, even when the recovering process has a smaller clock.
    const std::uint64_t session_tick = std::max(tick, entry.second.record.updated_tick);
    item.set_uint("tick", session_tick);
    JsonArrayBuilder demoted;
    for (const EvidenceRecord& record : entry.second.record.evidence) {
      if (record.provenance == EvidenceProvenance::Live) {
        demoted.push_text(record.id.hex());
      }
    }
    BSM_TRY_ASSIGN(demoted_value, demoted.build());
    item.set("demoted_evidence", demoted_value);
    JsonArrayBuilder unresolved;
    for (const AttemptRecord& attempt : entry.second.record.attempts) {
      if (attempt.state == AttemptState::Requested) {
        unresolved.push_text(attempt.id.hex());
      }
    }
    BSM_TRY_ASSIGN(unresolved_value, unresolved.build());
    item.set("unresolved_attempts", unresolved_value);
    BSM_TRY_ASSIGN(item_value, item.build());
    entries.push(item_value);
  }
  BSM_TRY_ASSIGN(entries_value, entries.build());
  JsonObjectBuilder root;
  root.set_text("session", SessionId().hex());
  root.set_text("incarnation", store.incarnation().hex());
  root.set_uint("tick", tick);
  root.set("sessions", entries_value);
  BSM_TRY_ASSIGN(payload, root.build());
  BSM_RETURN_IF_ERROR(store.append(JournalRecordKind::SessionsDemoted, payload));
  return apply_journal_record(sessions, store.replayed_records().back());
}

Result<SessionView> SessionManager::Impl::establish(const EstablishSessionRequest& request) {
  BSM_TRY_ASSIGN(plan, DerivedPlan::compile(request.plan));
  BSM_RETURN_IF_ERROR(validate_binding(request.binding));
  if (!(plan.document().facility == request.binding.facility)) {
    return Error(ErrorCode::InvalidArgument,
                 "the plan belongs to a different facility than the bound facility state");
  }
  if (!(plan.document().policy.id == request.binding.policy) ||
      plan.document().policy.revision != request.binding.policy_revision) {
    return Error(ErrorCode::AuthorityMismatch,
                 "the plan policy revision does not match the bound policy revision");
  }
  BSM_TRY_ASSIGN(control,
                 require_authority(request.binding, request.authority,
                                   {AuthorityDomain::Control}));
  (void)control;

  // The binding must cover every owner the plan will ask for, or the session could be
  // opened only to discover that a prerequisite can never be evidenced.
  for (const ObligationDefinition& obligation : plan.obligations()) {
    if (find_authority(request.binding, obligation.owner_domain, obligation.owner) ==
        nullptr) {
      return Error(ErrorCode::BindingIncomplete,
                   "obligation '" + obligation.id.str() + "' names owner '" +
                       obligation.owner.str() +
                       "', which the bound facility state does not include");
    }
    for (const EvidenceRequirement& requirement : obligation.required_evidence) {
      if (requirement.has_required_owner &&
          find_authority(request.binding, requirement.required_domain,
                         requirement.required_owner) == nullptr) {
        return Error(ErrorCode::BindingIncomplete,
                     "obligation '" + obligation.id.str() +
                         "' requires evidence from owner '" +
                         requirement.required_owner.str() +
                         "', which the bound facility state does not include");
      }
    }
  }
  for (const StageDefinition& stage : plan.document().stages) {
    for (const std::vector<EvidenceRequirement>* list :
         {&stage.entry_evidence, &stage.exit_evidence}) {
      for (const EvidenceRequirement& requirement : *list) {
        if (requirement.has_required_owner &&
            find_authority(request.binding, requirement.required_domain,
                           requirement.required_owner) == nullptr) {
          return Error(ErrorCode::BindingIncomplete,
                       "stage '" + stage.id.str() + "' requires evidence from owner '" +
                           requirement.required_owner.str() +
                           "', which the bound facility state does not include");
        }
      }
    }
  }
  for (const EvidenceRequirement& requirement :
       plan.document().return_to_service.evidence) {
    if (requirement.has_required_owner &&
        find_authority(request.binding, requirement.required_domain,
                       requirement.required_owner) == nullptr) {
      return Error(ErrorCode::BindingIncomplete,
                   "the return-to-service proof requires evidence from owner '" +
                       requirement.required_owner.str() +
                       "', which the bound facility state does not include");
    }
  }

  std::vector<SessionId> superseded;
  {
    const std::lock_guard<std::mutex> guard(state_mutex);
    for (const auto& entry : sessions) {
      if (session_is_terminal(entry.second.record.state)) {
        continue;
      }
      if (entry.second.record.binding.facility == request.binding.facility) {
        superseded.push_back(entry.first);
      }
    }
  }
  if (!superseded.empty() && !request.supersede_existing) {
    return Error(ErrorCode::SessionExists,
                 "a non-terminal session already exists for this facility; supersede it "
                 "explicitly or abort it first");
  }
  if (!superseded.empty() && request.reason.empty()) {
    return Error(ErrorCode::InvalidArgument,
                 "superseding a session requires a reason");
  }

  BSM_TRY_ASSIGN(tick, now_tick());
  BSM_TRY_ASSIGN(session_id, SessionId::generate());
  BSM_TRY_ASSIGN(binding_digest_value, binding_digest(request.binding));

  for (const SessionId& id : superseded) {
    BSM_TRY_ASSIGN(context, context_of(id));
    SessionContext updated = context;
    TerminalRecord terminal;
    terminal.state = SessionState::Superseded;
    terminal.reason = "superseded by session " + session_id.hex() +
                      (request.reason.empty() ? std::string() : ": " + request.reason);
    terminal.has_authority = true;
    terminal.authority = request.authority;
    terminal.tick = tick;
    JsonArrayBuilder fenced;
    for (AttemptRecord& attempt : updated.record.attempts) {
      if (!attempt_is_in_flight(attempt.state)) {
        attempt.state = AttemptState::Fenced;
        fenced.push_text(attempt.id.hex());
      }
    }
    updated.record.state = SessionState::Superseded;
    updated.record.has_terminal = true;
    updated.record.terminal = terminal;
    if (updated.record.revision < limits::kMaxGeneration) {
      ++updated.record.revision;
    }
    updated.record.updated_tick = tick;
    BSM_TRY_ASSIGN(fenced_value, fenced.build());
    BSM_TRY_ASSIGN(terminal_value, detail::terminal_to_json(terminal));
    JsonObjectBuilder root;
    root.set_text("session", id.hex());
    root.set("terminal", terminal_value);
    root.set("fenced_attempts", fenced_value);
    BSM_TRY_ASSIGN(payload, root.build());
    BSM_RETURN_IF_ERROR(
        commit(JournalRecordKind::SessionSuperseded, payload, std::move(updated)));
  }

  SessionContext context;
  context.plan = std::make_shared<const DerivedPlan>(std::move(plan));
  context.record.id = session_id;
  context.record.plan_digest = context.plan->digest();
  context.record.binding = request.binding;
  context.record.binding_digest = binding_digest_value;
  context.record.state = SessionState::Active;
  context.record.opened_by = store.incarnation();
  context.record.authority_incarnation = store.incarnation();
  context.record.authority_reestablished = true;
  context.record.opened_tick = tick;
  context.record.updated_tick = tick;
  context.record.revision = 1;
  context.record.stage_index = 0;

  BSM_TRY_ASSIGN(record_value, session_to_json(context.record));
  BSM_TRY_ASSIGN(plan_value, context.plan->to_json());
  JsonObjectBuilder root;
  root.set_text("session", session_id.hex());
  root.set("record", record_value);
  root.set("plan", plan_value);
  BSM_TRY_ASSIGN(payload, root.build());
  const SessionView view = view_of(context.record, *context.plan);
  BSM_RETURN_IF_ERROR(commit(JournalRecordKind::SessionEstablished, payload,
                             std::move(context)));
  return view;
}

Result<SessionView> SessionManager::Impl::reestablish(const ReestablishRequest& request) {
  BSM_TRY_ASSIGN(context, context_of(request.session));
  if (session_is_terminal(context.record.state)) {
    return terminal_error(context.record);
  }
  BSM_TRY_ASSIGN(observed, observed_binding());
  BSM_TRY_ASSIGN(delta, compare_bindings(context.record.binding, observed));
  if (delta.contradictory()) {
    Error error(ErrorCode::SessionFenced,
                "the observed facility state contradicts the bound state; the session "
                "must be replanned or aborted instead of re-established");
    JsonObjectBuilder detail;
    JsonArrayBuilder changes;
    for (const BindingChange& change : delta.changes) {
      changes.push_text(to_string(change.code));
    }
    const Result<JsonValue> changes_value = changes.build();
    if (changes_value.ok()) {
      detail.set("binding_changes", changes_value.value());
    }
    const Result<JsonValue> detail_value = detail.build();
    if (detail_value.ok()) {
      return error.with_detail(detail_value.value());
    }
    return error;
  }
  BSM_TRY_ASSIGN(bound, require_authority(observed, request.authority,
                                          {AuthorityDomain::Control}));
  BSM_TRY_ASSIGN(tick, now_tick());
  BSM_TRY_ASSIGN(digest, binding_digest(observed));

  SessionContext updated = context;
  updated.record.binding = observed;
  updated.record.binding_digest = digest;
  updated.record.authority_incarnation = store.incarnation();
  updated.record.authority_reestablished = true;
  updated.record.updated_tick = tick;
  if (updated.record.revision < limits::kMaxGeneration) {
    ++updated.record.revision;
  }

  BSM_TRY_ASSIGN(binding_value, binding_to_json(observed));
  JsonArrayBuilder changes;
  for (const BindingChange& change : delta.changes) {
    changes.push_text(to_string(change.code));
  }
  BSM_TRY_ASSIGN(changes_value, changes.build());
  JsonObjectBuilder root;
  root.set_text("session", request.session.hex());
  root.set("binding", binding_value);
  root.set("binding_changes", changes_value);
  root.set_text("incarnation", store.incarnation().hex());
  root.set_text("authority", bound.owner.str());
  root.set_text("reason", request.reason);
  root.set_uint("tick", tick);
  BSM_TRY_ASSIGN(payload, root.build());
  const SessionView view = view_of(updated.record, *updated.plan);
  BSM_RETURN_IF_ERROR(
      commit(JournalRecordKind::AuthorityReestablished, payload, std::move(updated)));
  return view;
}

Result<AttemptView> SessionManager::Impl::request_effect(const EffectRequest& request) {
  BSM_TRY_ASSIGN(context, context_of(request.session));
  BSM_TRY_ASSIGN(observed, observed_binding());
  BSM_TRY_ASSIGN(tick, now_tick());
  BSM_TRY_ASSIGN(assessment,
                 detail::assess_session(*context.plan, context.record, observed, tick));
  BSM_RETURN_IF_ERROR(fulfill_progress_gate(context.record, assessment));
  if (!assessment.delta.empty()) {
    return Error(ErrorCode::AuthorityStale,
                 "the bound facility state has advanced since the session bound it; "
                 "re-establish authority before requesting effects");
  }
  const std::size_t index = context.plan->obligation_index(request.obligation);
  if (index == context.plan->obligations().size()) {
    return Error(ErrorCode::UnknownIdentity,
                 "obligation '" + request.obligation.str() +
                     "' is not part of the plan bound to this session");
  }
  const ObligationDefinition& obligation = context.plan->obligations()[index];
  if (!obligation.consequential) {
    return Error(ErrorCode::InvalidArgument,
                 "obligation '" + obligation.id.str() +
                     "' is an evidence-only obligation and has no bounded request");
  }
  if (request.deadline_ticks == 0 || request.deadline_ticks > limits::kMaxDeadlineTicks) {
    return Error(ErrorCode::InvalidArgument,
                 "a bounded request requires a positive deadline window within the "
                 "supported range");
  }
  const ObligationAssessment& entry = assessment.obligations[index];
  if (entry.satisfied) {
    return Error(ErrorCode::GateNotReady,
                 "obligation '" + obligation.id.str() + "' is already satisfied");
  }
  if (entry.block != BlockCode::None) {
    return block_error(entry.block, entry);
  }
  BSM_TRY_ASSIGN(bound, require_authority(context.record.binding, request.authority,
                                          {obligation.owner_domain}));
  if (!(bound.owner == obligation.owner)) {
    return Error(ErrorCode::AuthorityMismatch,
                 "presented authority owner '" + bound.owner.str() +
                     "' does not own obligation '" + obligation.id.str() + "'");
  }

  std::uint64_t highest_sequence = 0;
  for (const AttemptRecord& attempt : context.record.attempts) {
    if (attempt.obligation == obligation.id) {
      highest_sequence = std::max(highest_sequence, attempt.sequence);
    }
  }
  BSM_TRY_ASSIGN(sequence, checked_increment(highest_sequence));
  BSM_TRY_ASSIGN(attempt_id,
                 attempt_id_for(context.record.id, obligation.id, sequence,
                                context.record.plan_digest, context.record.binding_digest));
  BSM_TRY_ASSIGN(key, request_key_for(context.record.id, obligation.id, sequence,
                                      context.record.plan_digest,
                                      context.record.binding_digest,
                                      obligation.request.capability,
                                      obligation.request.parameters));
  BSM_TRY_ASSIGN(deadline, checked_add(tick, request.deadline_ticks));

  RequestEnvelope envelope;
  envelope.attempt = attempt_id;
  envelope.key = key;
  envelope.session = context.record.id;
  envelope.obligation = obligation.id;
  envelope.plan_digest = context.record.plan_digest;
  envelope.binding_digest = context.record.binding_digest;
  envelope.capability = obligation.request.capability;
  envelope.parameters = obligation.request.parameters;
  envelope.authority = bound;
  envelope.attempt_sequence = sequence;
  envelope.created_tick = tick;
  envelope.deadline_tick = deadline;
  BSM_TRY_ASSIGN(envelope_digest, request_envelope_digest(envelope));
  envelope.envelope_digest = envelope_digest;

  AttemptRecord attempt;
  attempt.id = attempt_id;
  attempt.session = context.record.id;
  attempt.obligation = obligation.id;
  attempt.sequence = sequence;
  attempt.state = AttemptState::Requested;
  attempt.key = key;
  attempt.request_digest = envelope_digest;
  attempt.plan_digest = context.record.plan_digest;
  attempt.binding_digest = context.record.binding_digest;
  attempt.incarnation = store.incarnation();
  attempt.requested_tick = tick;

  BSM_TRY_ASSIGN(attempt_value, attempt_to_json(attempt));
  BSM_TRY_ASSIGN(envelope_value, request_envelope_to_json(envelope));
  JsonObjectBuilder root;
  root.set_text("session", context.record.id.hex());
  root.set("attempt", attempt_value);
  root.set("envelope", envelope_value);
  BSM_TRY_ASSIGN(payload, root.build());

  SessionContext updated = context;
  updated.record.attempts.push_back(attempt);
  updated.record.updated_tick = tick;
  if (updated.record.revision < limits::kMaxGeneration) {
    ++updated.record.revision;
  }
  BSM_RETURN_IF_ERROR(commit(JournalRecordKind::AttemptRecorded, payload, updated));

  // The durable intent exists. Only now may the bounded request leave this process, and
  // it leaves with no internal lock held.
  detail::crash_point("dispatch_before");
  Result<ControllerReply> outcome = options.controller->request_effect(envelope);
  detail::crash_point("dispatch_after");

  ControllerReply reply;
  if (!outcome.ok()) {
    reply.key = key;
    reply.kind = ControllerReplyKind::Unknown;
    reply.detail = "the adjacent owner did not answer: " + outcome.error().message();
    attempt.state = AttemptState::Unresolved;
    attempt.resolution_note =
        "the request was recorded and dispatched but no definitive answer arrived; "
        "resolve it by querying the owner rather than repeating it";
  } else {
    reply = outcome.value();
    switch (reply.kind) {
      case ControllerReplyKind::Applied:
      case ControllerReplyKind::AlreadyApplied:
        attempt.state = AttemptState::Acknowledged;
        attempt.resolution_note =
            reply.kind == ControllerReplyKind::AlreadyApplied
                ? "the owner replayed an already applied request"
                : "the owner acknowledged the request";
        break;
      case ControllerReplyKind::Refused:
        attempt.state = AttemptState::Refused;
        attempt.resolution_note = "the owner refused the request";
        break;
      case ControllerReplyKind::Failed:
        attempt.state = AttemptState::Failed;
        attempt.resolution_note = "the owner reported a failure";
        break;
      case ControllerReplyKind::Unknown:
      case ControllerReplyKind::Unavailable:
      case ControllerReplyKind::Deferred:
        attempt.state = AttemptState::Unresolved;
        attempt.resolution_note =
            "the owner gave no definitive answer; resolve the attempt by querying the "
            "owner rather than repeating it";
        break;
    }
  }
  attempt.reply = reply;
  attempt.settled_tick = tick;

  BSM_TRY_ASSIGN(settled_value, attempt_to_json(attempt));
  JsonObjectBuilder settled_root;
  settled_root.set_text("session", context.record.id.hex());
  settled_root.set("attempt", settled_value);
  BSM_TRY_ASSIGN(settled_payload, settled_root.build());

  SessionContext settled_context = updated;
  AttemptRecord* slot = find_attempt(settled_context.record, attempt.id);
  if (slot == nullptr) {
    return Error(ErrorCode::InternalError, "the recorded attempt disappeared");
  }
  *slot = attempt;
  settled_context.record.updated_tick = tick;
  BSM_RETURN_IF_ERROR(
      commit(JournalRecordKind::AttemptSettled, settled_payload, std::move(settled_context)));
  detail::crash_point("attempt_settled");
  return AttemptView{attempt, false};
}

Result<AttemptView> SessionManager::Impl::resolve_attempt(const AttemptResolution& request) {
  BSM_TRY_ASSIGN(context, context_of(request.session));
  if (session_is_terminal(context.record.state)) {
    return terminal_error(context.record);
  }
  const AttemptRecord* existing = find_attempt(context.record, request.attempt);
  if (existing == nullptr) {
    return Error(ErrorCode::AttemptNotFound,
                 "attempt " + request.attempt.hex() + " is not recorded by this session");
  }
  if (!attempt_blocks_requests(existing->state)) {
    return Error(ErrorCode::AttemptStateInvalid,
                 "attempt " + request.attempt.hex() + " is already " +
                     to_string(existing->state) + " and needs no resolution");
  }
  // Resolution is the recovery path: it must be possible before authority has been
  // re-established, but it still requires a current control authority.
  BSM_TRY_ASSIGN(bound, require_authority(context.record.binding, request.authority,
                                          {AuthorityDomain::Control}));
  (void)bound;
  Result<ControllerReply> answer = options.controller->query_effect(existing->key);
  if (!answer.ok()) {
    return Error(ErrorCode::ControllerUnavailable,
                 "the adjacent owner did not answer the resolution query: " +
                     answer.error().message());
  }
  const ControllerReply& reply = answer.value();
  AttemptRecord attempt = *existing;
  switch (reply.kind) {
    case ControllerReplyKind::Applied:
    case ControllerReplyKind::AlreadyApplied:
      attempt.state = AttemptState::Acknowledged;
      attempt.resolution_note =
          "resolution confirmed that the owner applied this request";
      break;
    case ControllerReplyKind::Unknown:
      attempt.state = AttemptState::NotApplied;
      attempt.resolution_note =
          "resolution confirmed that the owner has no record of this request, so no "
          "effect was applied and the attempt does not consume the budget";
      break;
    case ControllerReplyKind::Refused:
      attempt.state = AttemptState::Refused;
      attempt.resolution_note = "resolution confirmed that the owner refused the request";
      break;
    case ControllerReplyKind::Failed:
      attempt.state = AttemptState::Failed;
      attempt.resolution_note = "resolution confirmed that the request failed";
      break;
    case ControllerReplyKind::Unavailable:
    case ControllerReplyKind::Deferred:
      return Error(ErrorCode::ControllerUnavailable,
                   "the owner cannot resolve this attempt yet: " + reply.detail);
  }
  BSM_TRY_ASSIGN(tick, now_tick());
  attempt.reply = reply;
  attempt.reply.key = existing->key;
  attempt.settled_tick = tick;
  if (!request.reason.empty()) {
    attempt.resolution_note += " (" + request.reason + ")";
  }

  BSM_TRY_ASSIGN(attempt_value, attempt_to_json(attempt));
  JsonObjectBuilder root;
  root.set_text("session", context.record.id.hex());
  root.set("attempt", attempt_value);
  root.set_uint("tick", tick);
  BSM_TRY_ASSIGN(payload, root.build());

  SessionContext updated = context;
  AttemptRecord* slot = find_attempt(updated.record, attempt.id);
  if (slot == nullptr) {
    return Error(ErrorCode::InternalError, "the resolved attempt disappeared");
  }
  *slot = attempt;
  updated.record.updated_tick = tick;
  if (updated.record.revision < limits::kMaxGeneration) {
    ++updated.record.revision;
  }
  BSM_RETURN_IF_ERROR(
      commit(JournalRecordKind::AttemptResolved, payload, std::move(updated)));
  return AttemptView{attempt, false};
}

Result<EvidenceView> SessionManager::Impl::record_observation(const Observation& observation) {
  BSM_TRY_ASSIGN(context, context_of(observation.session));
  if (session_is_terminal(context.record.state)) {
    return terminal_error(context.record);
  }
  BSM_TRY_ASSIGN(observed, observed_binding());
  BSM_TRY_ASSIGN(tick, now_tick());
  BSM_TRY_ASSIGN(assessment,
                 detail::assess_session(*context.plan, context.record, observed, tick));
  if (assessment.fenced) {
    return fenced_error(assessment);
  }
  if (!plan_requires_evidence(*context.plan, observation.kind, observation.subject)) {
    return Error(ErrorCode::EvidenceUnknownRequirement,
                 "no part of the bound plan requires evidence of kind '" +
                     observation.kind.str() + "' about subject '" +
                     observation.subject.str() + "'");
  }
  if (observation.has_obligation) {
    const std::size_t index = context.plan->obligation_index(observation.obligation);
    if (index == context.plan->obligations().size()) {
      return Error(ErrorCode::UnknownIdentity,
                   "evidence names obligation '" + observation.obligation.str() +
                       "', which the bound plan does not declare");
    }
    bool required_by_obligation = false;
    for (const EvidenceRequirement& requirement :
         context.plan->obligations()[index].required_evidence) {
      if (requirement.kind == observation.kind &&
          requirement.subject == observation.subject) {
        required_by_obligation = true;
      }
    }
    if (!required_by_obligation) {
      return Error(ErrorCode::EvidenceUnknownRequirement,
                   "obligation '" + observation.obligation.str() +
                       "' does not require this evidence kind and subject");
    }
  }
  if (observation.has_stage &&
      context.plan->stage_index(observation.stage) == context.plan->stages().size()) {
    return Error(ErrorCode::UnknownIdentity,
                 "evidence names stage '" + observation.stage.str() +
                     "', which the bound plan does not declare");
  }
  const AuthorityRef* source =
      find_authority(context.record.binding, observation.source_domain,
                     observation.source_owner);
  if (source == nullptr) {
    return Error(ErrorCode::AuthorityMissing,
                 "evidence source '" + observation.source_owner.str() +
                     "' is not part of the bound facility state");
  }
  if (source->generation != observation.source_generation) {
    return Error(ErrorCode::EvidenceStaleAuthority,
                 "evidence was observed under authority generation " +
                     std::to_string(observation.source_generation) +
                     " but the bound generation is " +
                     std::to_string(source->generation));
  }
  if (observation.observed_tick > tick) {
    return Error(ErrorCode::EvidenceFromFuture,
                 "evidence claims an observation tick in the future");
  }
  if (observation.has_attempt) {
    const AttemptRecord* attempt = find_attempt(context.record, observation.attempt);
    if (attempt == nullptr) {
      return Error(ErrorCode::AttemptNotFound,
                   "evidence names an attempt this session does not record");
    }
    if (attempt->state == AttemptState::Fenced) {
      return Error(ErrorCode::AttemptStateInvalid,
                   "evidence names an attempt that has been fenced");
    }
  }
  BSM_TRY_ASSIGN(record, build_evidence_record(context.record, observation, tick));

  {
    const std::lock_guard<std::mutex> guard(state_mutex);
    const auto found = sessions.find(observation.session);
    if (found != sessions.end()) {
      for (const EvidenceRecord& existing : found->second.record.evidence) {
        if (existing.id == record.id) {
          // Idempotent replay: the same observation is the same record, and admitting it
          // again changes nothing.
          return EvidenceView{existing, true};
        }
      }
    }
  }

  BSM_TRY_ASSIGN(evidence_value, evidence_to_json(record));
  JsonObjectBuilder root;
  root.set_text("session", context.record.id.hex());
  root.set("evidence", evidence_value);
  if (observation.has_attempt) {
    root.set_text("attempt", observation.attempt.hex());
  } else {
    root.set_null("attempt");
  }
  BSM_TRY_ASSIGN(payload, root.build());

  SessionContext updated = context;
  updated.record.evidence.push_back(record);
  if (observation.has_attempt) {
    AttemptRecord* attempt = find_attempt(updated.record, observation.attempt);
    if (attempt != nullptr) {
      attempt->evidence.push_back(record.id);
    }
  }
  updated.record.updated_tick = tick;
  if (updated.record.revision < limits::kMaxGeneration) {
    ++updated.record.revision;
  }
  BSM_RETURN_IF_ERROR(
      commit(JournalRecordKind::EvidenceAdmitted, payload, std::move(updated)));
  detail::crash_point("evidence_committed");
  return EvidenceView{record, false};
}

Result<EvidenceView> SessionManager::Impl::observe_subject(const ObserveRequest& request) {
  BSM_TRY_ASSIGN(context, context_of(request.session));
  if (session_is_terminal(context.record.state)) {
    return terminal_error(context.record);
  }
  BSM_TRY_ASSIGN(observed, observed_binding());
  BSM_TRY_ASSIGN(tick, now_tick());
  BSM_TRY_ASSIGN(assessment,
                 detail::assess_session(*context.plan, context.record, observed, tick));
  if (assessment.fenced) {
    return fenced_error(assessment);
  }
  if (!plan_requires_evidence(*context.plan, request.kind, request.subject)) {
    return Error(ErrorCode::EvidenceUnknownRequirement,
                 "no part of the bound plan requires evidence of kind '" +
                     request.kind.str() + "' about subject '" + request.subject.str() +
                     "'");
  }
  BSM_TRY_ASSIGN(bound, require_authority(context.record.binding, request.authority,
                                          {request.authority.domain}));
  if (!(bound.owner == request.observer)) {
    return Error(ErrorCode::AuthorityMismatch,
                 "the presented authority belongs to '" + bound.owner.str() +
                     "' but the observation names observer '" + request.observer.str() +
                     "'");
  }

  CapabilityId capability;
  JsonValue parameters = JsonValue::object(JsonValue::Object{}).value();
  if (request.has_obligation) {
    const std::size_t index = context.plan->obligation_index(request.obligation);
    if (index == context.plan->obligations().size()) {
      return Error(ErrorCode::UnknownIdentity,
                   "the observation names an obligation the plan does not declare");
    }
    const ObligationDefinition& obligation = context.plan->obligations()[index];
    if (obligation.consequential) {
      capability = obligation.request.capability;
      parameters = obligation.request.parameters;
    }
  }

  ObservationRequest controller_request;
  controller_request.capability = capability;
  controller_request.parameters = parameters;
  controller_request.subject = request.subject;
  controller_request.observer = request.observer;
  controller_request.authority = bound;
  Result<ControllerObservation> answer = options.controller->observe(controller_request);
  if (!answer.ok()) {
    return Error(ErrorCode::ControllerUnavailable,
                 "the adjacent owner did not answer the observation: " +
                     answer.error().message());
  }
  const ControllerObservation& observation = answer.value();
  if (!(observation.owner == request.observer)) {
    return Error(ErrorCode::ControllerProtocolViolation,
                 "the adjacent owner answered for a different observer");
  }
  if (observation.domain != bound.domain || observation.generation != bound.generation) {
    return Error(ErrorCode::ControllerProtocolViolation,
                 "the adjacent owner answered for a different authority generation");
  }

  Observation record_observation_request;
  record_observation_request.session = request.session;
  record_observation_request.kind = request.kind;
  record_observation_request.subject = request.subject;
  record_observation_request.has_obligation = request.has_obligation;
  record_observation_request.obligation = request.obligation;
  record_observation_request.has_stage = request.has_stage;
  record_observation_request.stage = request.stage;
  record_observation_request.value = observation.value;
  record_observation_request.source_owner = observation.owner;
  record_observation_request.source_domain = observation.domain;
  record_observation_request.source_generation = observation.generation;
  record_observation_request.channel = EvidenceChannel::OwnerReported;
  record_observation_request.observed_tick =
      request.observed_tick == 0 ? tick : request.observed_tick;
  return record_observation(record_observation_request);
}

Result<StageTransitionView> SessionManager::Impl::advance_stage(const StageAdvance& request) {
  BSM_TRY_ASSIGN(context, context_of(request.session));
  BSM_TRY_ASSIGN(observed, observed_binding());
  BSM_TRY_ASSIGN(tick, now_tick());
  BSM_TRY_ASSIGN(assessment,
                 detail::assess_session(*context.plan, context.record, observed, tick));
  BSM_RETURN_IF_ERROR(fulfill_progress_gate(context.record, assessment));
  if (!assessment.delta.empty()) {
    return Error(ErrorCode::AuthorityStale,
                 "the bound facility state has advanced since the session bound it; "
                 "re-establish authority before advancing the stage");
  }
  BSM_TRY_ASSIGN(bound, require_authority(context.record.binding, request.authority,
                                          {AuthorityDomain::Control}));
  if (!assessment.has_next_stage) {
    return Error(ErrorCode::GateNotReady,
                 "the final stage has no successor; complete the session instead");
  }
  const StageAssessment& current = assessment.stages[assessment.stage_index];
  if (!current.obligations_satisfied) {
    return Error(ErrorCode::GateNotReady,
                 "stage '" + current.id.str() +
                     "' still has unsatisfied required obligations");
  }
  if (!current.exit_deficits.empty()) {
    return Error(ErrorCode::GateStageEvidenceMissing,
                 "stage '" + current.id.str() + "' exit evidence is not satisfied");
  }
  if (!assessment.next_stage_entry_deficits.empty()) {
    return Error(ErrorCode::GateStageEvidenceMissing,
                 "stage '" + assessment.next_stage.str() +
                     "' entry evidence is not satisfied");
  }

  const StageDefinition* definition = nullptr;
  for (const StageDefinition& candidate : context.plan->document().stages) {
    if (candidate.id == current.id) {
      definition = &candidate;
      break;
    }
  }
  const StageDefinition* next_definition = nullptr;
  for (const StageDefinition& candidate : context.plan->document().stages) {
    if (candidate.id == assessment.next_stage) {
      next_definition = &candidate;
      break;
    }
  }
  if (definition == nullptr || next_definition == nullptr) {
    return Error(ErrorCode::InternalError, "a compiled stage is missing from the document");
  }

  StageTransition transition;
  transition.from = current.id;
  transition.to = assessment.next_stage;
  transition.to_index = assessment.stage_index + 1;
  transition.tick = tick;
  transition.exit_evidence = detail::satisfied_evidence_ids(
      definition->exit_evidence, context.record.evidence, context.record,
      context.plan->document().policy, tick);
  transition.entry_evidence = detail::satisfied_evidence_ids(
      next_definition->entry_evidence, context.record.evidence, context.record,
      context.plan->document().policy, tick);
  transition.plan_digest = context.record.plan_digest;
  transition.binding_digest = context.record.binding_digest;
  transition.incarnation = store.incarnation();

  BSM_TRY_ASSIGN(transition_value, detail::transition_to_json(transition));
  JsonObjectBuilder root;
  root.set_text("session", context.record.id.hex());
  root.set("transition", transition_value);
  root.set_text("authority", bound.owner.str());
  BSM_TRY_ASSIGN(payload, root.build());

  SessionContext updated = context;
  updated.record.stage_index = transition.to_index;
  updated.record.stage_history.push_back(transition);
  updated.record.updated_tick = tick;
  if (updated.record.revision < limits::kMaxGeneration) {
    ++updated.record.revision;
  }
  BSM_RETURN_IF_ERROR(commit(JournalRecordKind::StageAdvanced, payload, updated));
  detail::crash_point("stage_published");
  BSM_TRY_ASSIGN(next_assessment, detail::assess_session(*updated.plan, updated.record,
                                                         observed, tick));
  return StageTransitionView{transition, next_assessment};
}

Result<CompletionView> SessionManager::Impl::complete_session(
    const CompletionRequest& request) {
  BSM_TRY_ASSIGN(context, context_of(request.session));
  BSM_TRY_ASSIGN(observed, observed_binding());
  BSM_TRY_ASSIGN(tick, now_tick());
  BSM_TRY_ASSIGN(assessment,
                 detail::assess_session(*context.plan, context.record, observed, tick));
  BSM_RETURN_IF_ERROR(fulfill_progress_gate(context.record, assessment));
  if (!assessment.delta.empty()) {
    return Error(ErrorCode::AuthorityStale,
                 "the bound facility state has advanced since the session bound it; "
                 "re-establish authority before completing the session");
  }
  BSM_TRY_ASSIGN(bound, require_authority(context.record.binding, request.authority,
                                          {AuthorityDomain::Control}));
  for (const AttemptRecord& attempt : context.record.attempts) {
    if (attempt_is_in_flight(attempt.state)) {
      return Error(ErrorCode::AttemptUnresolved,
                   "attempt " + attempt.id.hex() +
                       " is still unresolved; completion requires every attempt to be "
                       "resolved");
    }
  }
  if (!assessment.completion_ready) {
    Error error(ErrorCode::GateCompletionUnsatisfied,
                "the return-to-service proof is not satisfied");
    JsonObjectBuilder detail;
    JsonArrayBuilder missing;
    for (const ObligationId& id : assessment.completion_missing_obligations) {
      missing.push_text(id.str());
    }
    const Result<JsonValue> missing_value = missing.build();
    if (missing_value.ok()) {
      detail.set("missing_obligations", missing_value.value());
    }
    JsonArrayBuilder deficits;
    for (const EvidenceDeficit& deficit : assessment.completion_deficits) {
      JsonObjectBuilder item;
      item.set_text("kind", deficit.kind.str());
      item.set_text("subject", deficit.subject.str());
      item.set_text("deficit", to_string(deficit.code));
      const Result<JsonValue> item_value = item.build();
      if (item_value.ok()) {
        deficits.push(item_value.value());
      }
    }
    const Result<JsonValue> deficits_value = deficits.build();
    if (deficits_value.ok()) {
      detail.set("deficits", deficits_value.value());
    }
    const Result<JsonValue> detail_value = detail.build();
    if (detail_value.ok()) {
      return error.with_detail(detail_value.value());
    }
    return error;
  }

  std::vector<EvidenceId> proof;
  for (const ObligationAssessment& obligation : assessment.obligations) {
    for (const EvidenceId& id : obligation.satisfied_by) {
      if (std::find(proof.begin(), proof.end(), id) == proof.end()) {
        proof.push_back(id);
      }
    }
  }
  const std::vector<EvidenceId> return_to_service = detail::satisfied_evidence_ids(
      context.plan->document().return_to_service.evidence, context.record.evidence,
      context.record, context.plan->document().policy, tick);
  for (const EvidenceId& id : return_to_service) {
    if (std::find(proof.begin(), proof.end(), id) == proof.end()) {
      proof.push_back(id);
    }
  }
  const Digest proof_digest = completion_proof_digest(context.record, assessment, proof, tick);

  TerminalRecord terminal;
  terminal.state = SessionState::Completed;
  terminal.reason = "return-to-service proof satisfied";
  terminal.has_authority = true;
  terminal.authority = bound;
  terminal.tick = tick;
  terminal.proof_evidence = proof;
  terminal.proof_digest = proof_digest;

  BSM_TRY_ASSIGN(terminal_value, detail::terminal_to_json(terminal));
  JsonArrayBuilder fenced;
  BSM_TRY_ASSIGN(fenced_value, fenced.build());
  JsonObjectBuilder root;
  root.set_text("session", context.record.id.hex());
  root.set("terminal", terminal_value);
  root.set("fenced_attempts", fenced_value);
  BSM_TRY_ASSIGN(payload, root.build());

  SessionContext updated = context;
  updated.record.state = SessionState::Completed;
  updated.record.has_terminal = true;
  updated.record.terminal = terminal;
  updated.record.updated_tick = tick;
  if (updated.record.revision < limits::kMaxGeneration) {
    ++updated.record.revision;
  }
  BSM_RETURN_IF_ERROR(
      commit(JournalRecordKind::SessionCompleted, payload, std::move(updated)));

  CompletionView view;
  view.session = context.record.id;
  view.tick = tick;
  view.proof_evidence = proof;
  view.proof_digest = proof_digest;
  return view;
}

Result<SessionView> SessionManager::Impl::hold(const HoldRequest& request) {
  BSM_TRY_ASSIGN(context, context_of(request.session));
  if (session_is_terminal(context.record.state)) {
    return terminal_error(context.record);
  }
  if (request.reason.empty()) {
    return Error(ErrorCode::InvalidArgument, "a hold requires a reason");
  }
  if (request.reason.size() > limits::kMaxReasonLength) {
    return Error(ErrorCode::LimitExceeded, "the hold reason exceeds the supported length");
  }
  if (context.record.state == SessionState::Held) {
    return Error(ErrorCode::SessionHeld, "the session is already held");
  }
  BSM_TRY_ASSIGN(bound, require_authority(context.record.binding, request.authority,
                                          {AuthorityDomain::Control,
                                           AuthorityDomain::Facility}));
  BSM_TRY_ASSIGN(tick, now_tick());

  HoldRecord hold;
  hold.reason = request.reason;
  hold.has_authority = true;
  hold.authority = bound;
  hold.tick = tick;

  BSM_TRY_ASSIGN(hold_value, detail::hold_to_json(hold));
  JsonObjectBuilder root;
  root.set_text("session", context.record.id.hex());
  root.set("hold", hold_value);
  BSM_TRY_ASSIGN(payload, root.build());

  SessionContext updated = context;
  updated.record.holds.push_back(std::move(hold));
  updated.record.state = SessionState::Held;
  updated.record.updated_tick = tick;
  if (updated.record.revision < limits::kMaxGeneration) {
    ++updated.record.revision;
  }
  const SessionView view = view_of(updated.record, *updated.plan);
  BSM_RETURN_IF_ERROR(commit(JournalRecordKind::SessionHeld, payload, std::move(updated)));
  return view;
}

Result<SessionView> SessionManager::Impl::resume(const ResumeRequest& request) {
  BSM_TRY_ASSIGN(context, context_of(request.session));
  if (session_is_terminal(context.record.state)) {
    return terminal_error(context.record);
  }
  if (context.record.state != SessionState::Held) {
    return Error(ErrorCode::SessionNotHeld, "the session is not held");
  }
  BSM_TRY_ASSIGN(bound, require_authority(context.record.binding, request.authority,
                                          {AuthorityDomain::Control,
                                           AuthorityDomain::Facility}));
  (void)bound;
  BSM_TRY_ASSIGN(tick, now_tick());

  std::size_t index = context.record.holds.size();
  for (std::size_t position = context.record.holds.size(); position > 0; --position) {
    if (!context.record.holds[position - 1].released) {
      index = position - 1;
      break;
    }
  }
  if (index == context.record.holds.size()) {
    return Error(ErrorCode::SessionNotHeld,
                 "no unreleased hold record exists for this session");
  }

  JsonObjectBuilder root;
  root.set_text("session", context.record.id.hex());
  root.set_uint("hold_index", static_cast<std::uint64_t>(index));
  root.set_uint("tick", tick);
  root.set_text("reason", request.reason);
  BSM_TRY_ASSIGN(payload, root.build());

  SessionContext updated = context;
  updated.record.holds[index].released = true;
  updated.record.holds[index].released_tick = tick;
  updated.record.holds[index].release_reason = request.reason;
  updated.record.state = SessionState::Active;
  updated.record.updated_tick = tick;
  if (updated.record.revision < limits::kMaxGeneration) {
    ++updated.record.revision;
  }
  const SessionView view = view_of(updated.record, *updated.plan);
  BSM_RETURN_IF_ERROR(commit(JournalRecordKind::SessionResumed, payload, std::move(updated)));
  return view;
}

Result<SessionView> SessionManager::Impl::abort(const AbortRequest& request) {
  BSM_TRY_ASSIGN(context, context_of(request.session));
  if (session_is_terminal(context.record.state)) {
    return terminal_error(context.record);
  }
  if (request.reason.empty()) {
    return Error(ErrorCode::InvalidArgument, "an abort requires a reason");
  }
  if (request.reason.size() > limits::kMaxReasonLength) {
    return Error(ErrorCode::LimitExceeded, "the abort reason exceeds the supported length");
  }
  BSM_TRY_ASSIGN(bound, require_authority(context.record.binding, request.authority,
                                          {AuthorityDomain::Control,
                                           AuthorityDomain::Facility}));
  BSM_TRY_ASSIGN(tick, now_tick());

  TerminalRecord terminal;
  terminal.state = SessionState::Aborted;
  terminal.reason = request.reason;
  terminal.has_authority = true;
  terminal.authority = bound;
  terminal.tick = tick;

  BSM_TRY_ASSIGN(terminal_value, detail::terminal_to_json(terminal));
  JsonArrayBuilder fenced;
  BSM_TRY_ASSIGN(fenced_value, fenced.build());
  JsonObjectBuilder root;
  root.set_text("session", context.record.id.hex());
  root.set("terminal", terminal_value);
  root.set("fenced_attempts", fenced_value);
  BSM_TRY_ASSIGN(payload, root.build());

  SessionContext updated = context;
  updated.record.state = SessionState::Aborted;
  updated.record.has_terminal = true;
  updated.record.terminal = terminal;
  updated.record.updated_tick = tick;
  if (updated.record.revision < limits::kMaxGeneration) {
    ++updated.record.revision;
  }
  const SessionView view = view_of(updated.record, *updated.plan);
  BSM_RETURN_IF_ERROR(commit(JournalRecordKind::SessionAborted, payload, std::move(updated)));
  return view;
}

Result<SessionView> SessionManager::Impl::replan(const ReplanRequest& request) {
  BSM_TRY_ASSIGN(context, context_of(request.session));
  if (session_is_terminal(context.record.state)) {
    return terminal_error(context.record);
  }
  BSM_TRY_ASSIGN(plan, DerivedPlan::compile(request.plan));
  BSM_RETURN_IF_ERROR(validate_binding(request.binding));
  BSM_TRY_ASSIGN(bound, require_authority(request.binding, request.authority,
                                          {AuthorityDomain::Control}));
  (void)bound;
  if (!(context.record.binding.facility == request.binding.facility) ||
      !(plan.document().facility == request.binding.facility)) {
    return Error(ErrorCode::InvalidArgument,
                 "a replan must stay within the same facility");
  }
  if (context.record.binding.facility_epoch != request.binding.facility_epoch) {
    return Error(ErrorCode::AuthorityMismatch,
                 "the facility epoch changed; a new de-energization epoch requires a new "
                 "session rather than a replan");
  }
  if (!(context.record.binding.incident.id == request.binding.incident.id)) {
    return Error(ErrorCode::AuthorityMismatch,
                 "the incident changed; a different incident requires a new session");
  }
  if (context.record.binding.topology_digest != request.binding.topology_digest &&
      !request.accept_topology_change) {
    return Error(ErrorCode::AuthorityMismatch,
                 "the topology digest changed; replan explicitly with "
                 "accept_topology_change to adopt the new topology");
  }
  if (plan.digest() == context.record.plan_digest) {
    return Error(ErrorCode::PlanUnchanged,
                 "the replanned declaration compiles to the plan already bound");
  }
  BSM_TRY_ASSIGN(tick, now_tick());

  std::size_t carried = 0;
  std::size_t reset = 0;
  for (const ObligationDefinition& obligation : plan.obligations()) {
    if (!context.plan->has_obligation(obligation.id)) {
      ++reset;
      continue;
    }
    if (context.plan->obligation_digest(obligation.id) == plan.obligation_digest(obligation.id)) {
      ++carried;
    } else {
      ++reset;
    }
  }

  SessionContext updated = context;
  JsonArrayBuilder fenced;
  for (AttemptRecord& attempt : updated.record.attempts) {
    // In-flight attempts are never fenced: the owner may have applied the effect, so the
    // attempt must stay resolvable by its idempotency key.
    if (attempt_is_in_flight(attempt.state)) {
      continue;
    }
    if (!plan.has_obligation(attempt.obligation) ||
        !(plan.obligation_digest(attempt.obligation) ==
          context.plan->obligation_digest(attempt.obligation))) {
      attempt.state = AttemptState::Fenced;
      fenced.push_text(attempt.id.hex());
    }
  }
  std::size_t stage_index = 0;
  if (context.record.stage_index < context.plan->stages().size()) {
    const StageId current = context.plan->stages()[context.record.stage_index].id;
    const std::size_t mapped = plan.stage_index(current);
    if (mapped != plan.stages().size()) {
      stage_index = mapped;
    }
  }

  ReplanRecord record;
  record.previous_plan_digest = context.record.plan_digest;
  record.new_plan_digest = plan.digest();
  record.reason = request.reason;
  record.authority = request.authority;
  record.tick = tick;
  record.carried_obligations = carried;
  record.reset_obligations = reset;
  record.accepted_topology_change = request.accept_topology_change;

  BSM_TRY_ASSIGN(plan_value, plan.to_json());
  BSM_TRY_ASSIGN(binding_value, binding_to_json(request.binding));
  BSM_TRY_ASSIGN(replan_value, detail::replan_to_json(record));
  BSM_TRY_ASSIGN(fenced_value, fenced.build());
  JsonObjectBuilder root;
  root.set_text("session", context.record.id.hex());
  root.set("plan", plan_value);
  root.set("binding", binding_value);
  root.set("replan", replan_value);
  root.set("fenced_attempts", fenced_value);
  root.set_uint("stage_index", static_cast<std::uint64_t>(stage_index));
  root.set_text("incarnation", store.incarnation().hex());
  root.set_uint("tick", tick);
  BSM_TRY_ASSIGN(payload, root.build());

  updated.plan = std::make_shared<const DerivedPlan>(std::move(plan));
  const Digest digest = updated.plan->digest();
  BSM_TRY_ASSIGN(binding_digest_value, binding_digest(request.binding));
  updated.record.plan_digest = digest;
  updated.record.binding = request.binding;
  updated.record.binding_digest = binding_digest_value;
  updated.record.replans.push_back(record);
  updated.record.stage_index = stage_index;
  updated.record.authority_incarnation = store.incarnation();
  updated.record.authority_reestablished = true;
  updated.record.updated_tick = tick;
  if (updated.record.revision < limits::kMaxGeneration) {
    ++updated.record.revision;
  }
  const SessionView view = view_of(updated.record, *updated.plan);
  BSM_RETURN_IF_ERROR(commit(JournalRecordKind::SessionReplanned, payload, std::move(updated)));
  return view;
}

Result<Assessment> SessionManager::Impl::assess(const SessionId& id) const {
  BSM_TRY_ASSIGN(context, context_of(id));
  BSM_TRY_ASSIGN(observed, observed_binding());
  BSM_TRY_ASSIGN(tick, now_tick());
  return detail::assess_session(*context.plan, context.record, observed, tick);
}

Result<JsonValue> SessionManager::Impl::report(const SessionId& id,
                                               const ReportOptions& report_options) const {
  BSM_TRY_ASSIGN(context, context_of(id));
  BSM_TRY_ASSIGN(observed, observed_binding());
  BSM_TRY_ASSIGN(tick, now_tick());
  BSM_TRY_ASSIGN(assessment,
                 detail::assess_session(*context.plan, context.record, observed, tick));
  return session_report(*context.plan, context.record, assessment, report_options);
}

Result<JsonValue> SessionManager::Impl::status() const {
  BSM_TRY_ASSIGN(verification, store.verify());
  std::size_t count = 0;
  {
    const std::lock_guard<std::mutex> guard(state_mutex);
    count = sessions.size();
  }
  return store_status_json(store.recovery(), verification, count);
}

Result<JsonValue> SessionManager::Impl::verify_store() const {
  BSM_TRY_ASSIGN(verification, store.verify());
  return verification.to_json();
}

Result<std::vector<SessionId>> SessionManager::Impl::session_ids() const {
  const std::lock_guard<std::mutex> guard(state_mutex);
  std::vector<SessionId> ids;
  ids.reserve(sessions.size());
  for (const auto& entry : sessions) {
    ids.push_back(entry.first);
  }
  return ids;
}

#define BSM_MANAGER_OPERATION(expression)                                          \
  do {                                                                             \
    if (impl_ == nullptr) {                                                        \
      return Error(ErrorCode::StoreNotOpen, "manager is not open");                \
    }                                                                              \
    if (g_operation_depth > 0) {                                                   \
      return reentrant_error();                                                    \
    }                                                                              \
    const std::lock_guard<std::mutex> bsm_operation_guard(impl_->operation_mutex); \
    const OperationScope bsm_operation_scope;                                      \
    return (expression);                                                           \
  } while (false)

SessionManager::SessionManager(SessionManager&& other) noexcept : impl_(other.impl_) {
  other.impl_ = nullptr;
}

SessionManager& SessionManager::operator=(SessionManager&& other) noexcept {
  if (this != &other) {
    delete impl_;
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

SessionManager::~SessionManager() { delete impl_; }

Result<SessionManager> SessionManager::open(const ManagerOptions& options) {
  if (options.store_root.empty()) {
    return Error(ErrorCode::InvalidArgument, "manager requires a store root");
  }
  if (options.clock == nullptr) {
    return Error(ErrorCode::InvalidArgument, "manager requires a clock");
  }
  if (options.controller == nullptr) {
    return Error(ErrorCode::InvalidArgument, "manager requires an adjacent controller");
  }
  if (options.facility == nullptr) {
    return Error(ErrorCode::InvalidArgument, "manager requires a facility state provider");
  }
  StoreOptions store_options;
  store_options.root = options.store_root;
  store_options.create_if_missing = options.create_store_if_missing;
  store_options.compact_journal_bytes = options.compact_journal_bytes;
  store_options.compact_journal_records = options.compact_journal_records;
  BSM_TRY_ASSIGN(store, Store::open(store_options));

  auto impl = std::make_unique<Impl>();
  impl->options = options;
  impl->store = std::move(store);

  BSM_RETURN_IF_ERROR(load_snapshot_state(impl->sessions, impl->store.snapshot_payload()));
  for (const JournalRecord& record : impl->store.replayed_records()) {
    const Result<Unit> applied = apply_journal_record(impl->sessions, record);
    if (!applied.ok()) {
      // A recovery failure must name the exact record that could not be applied.
      JsonObjectBuilder detail;
      detail.set_uint("sequence", record.sequence);
      detail.set_text("record_kind", to_string(record.kind));
      const Result<JsonValue> detail_value = detail.build();
      Error error(applied.error().code(),
                  "journal record " + std::to_string(record.sequence) + " (" +
                      to_string(record.kind) + "): " + applied.error().message());
      if (detail_value.ok()) {
        error = error.with_detail(detail_value.value());
      }
      return error;
    }
  }

  // Re-establishing process authority is explicit and durable. The recovered incarnation
  // demotes every live evidence record and converts in-flight attempts into unresolved
  // ones before it accepts any further work.
  BSM_TRY_ASSIGN(tick, impl->now_tick());
  BSM_RETURN_IF_ERROR(impl->demote_recovered_sessions(tick));

  SessionManager manager;
  manager.impl_ = impl.release();
  return manager;
}

Result<Unit> SessionManager::close() {
  BSM_MANAGER_OPERATION(impl_->store.close());
}

const RecoveryReport& SessionManager::recovery() const noexcept {
  static const RecoveryReport kEmpty;
  return impl_ == nullptr ? kEmpty : impl_->store.recovery();
}

IncarnationId SessionManager::incarnation() const noexcept {
  return impl_ == nullptr ? IncarnationId() : impl_->store.incarnation();
}

std::uint64_t SessionManager::epoch() const noexcept {
  return impl_ == nullptr ? 0 : impl_->store.epoch();
}

Result<JsonValue> SessionManager::verify_store() const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::StoreNotOpen, "manager is not open");
  }
  return impl_->verify_store();
}

Result<SessionId> SessionManager::active_session() const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::StoreNotOpen, "manager is not open");
  }
  BSM_TRY_ASSIGN(context, impl_->single_context());
  return context.record.id;
}

Result<std::vector<SessionId>> SessionManager::sessions() const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::StoreNotOpen, "manager is not open");
  }
  return impl_->session_ids();
}

Result<SessionView> SessionManager::establish_session(
    const EstablishSessionRequest& request) {
  BSM_MANAGER_OPERATION(impl_->establish(request));
}

Result<SessionView> SessionManager::reestablish_authority(const ReestablishRequest& request) {
  BSM_MANAGER_OPERATION(impl_->reestablish(request));
}

Result<SessionView> SessionManager::hold(const HoldRequest& request) {
  BSM_MANAGER_OPERATION(impl_->hold(request));
}

Result<SessionView> SessionManager::resume(const ResumeRequest& request) {
  BSM_MANAGER_OPERATION(impl_->resume(request));
}

Result<SessionView> SessionManager::replan(const ReplanRequest& request) {
  BSM_MANAGER_OPERATION(impl_->replan(request));
}

Result<SessionView> SessionManager::abort(const AbortRequest& request) {
  BSM_MANAGER_OPERATION(impl_->abort(request));
}

Result<AttemptView> SessionManager::request_effect(const EffectRequest& request) {
  BSM_MANAGER_OPERATION(impl_->request_effect(request));
}

Result<AttemptView> SessionManager::resolve_attempt(const AttemptResolution& request) {
  BSM_MANAGER_OPERATION(impl_->resolve_attempt(request));
}

Result<EvidenceView> SessionManager::record_observation(const Observation& observation) {
  BSM_MANAGER_OPERATION(impl_->record_observation(observation));
}

Result<EvidenceView> SessionManager::observe_subject(const ObserveRequest& request) {
  BSM_MANAGER_OPERATION(impl_->observe_subject(request));
}

Result<StageTransitionView> SessionManager::advance_stage(const StageAdvance& request) {
  BSM_MANAGER_OPERATION(impl_->advance_stage(request));
}

Result<CompletionView> SessionManager::complete_session(const CompletionRequest& request) {
  BSM_MANAGER_OPERATION(impl_->complete_session(request));
}

Result<Assessment> SessionManager::assess() const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::StoreNotOpen, "manager is not open");
  }
  BSM_TRY_ASSIGN(context, impl_->single_context());
  return impl_->assess(context.record.id);
}

Result<Assessment> SessionManager::assess(const SessionId& session) const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::StoreNotOpen, "manager is not open");
  }
  return impl_->assess(session);
}

Result<JsonValue> SessionManager::report() const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::StoreNotOpen, "manager is not open");
  }
  BSM_TRY_ASSIGN(context, impl_->single_context());
  return impl_->report(context.record.id, ReportOptions{});
}

Result<JsonValue> SessionManager::report(const SessionId& session,
                                         const ReportOptions& options) const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::StoreNotOpen, "manager is not open");
  }
  return impl_->report(session, options);
}

Result<JsonValue> SessionManager::status() const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::StoreNotOpen, "manager is not open");
  }
  return impl_->status();
}

}  // namespace black_start_manager
