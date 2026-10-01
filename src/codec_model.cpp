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

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "black_start_manager/session.hpp"
#include "detail/model_json.hpp"
#include "detail/session_parts.hpp"

namespace black_start_manager {
namespace detail {

[[nodiscard]] Result<JsonValue> hold_to_json(const HoldRecord& hold) {
  JsonObjectBuilder root;
  root.set_text("reason", hold.reason);
  if (hold.has_authority) {
    BSM_TRY_ASSIGN(authority, detail::authority_to_json(hold.authority));
    root.set("authority", authority);
  } else {
    root.set_null("authority");
  }
  root.set_uint("tick", hold.tick);
  root.set_bool("released", hold.released);
  root.set_uint("released_tick", hold.released_tick);
  root.set_text("release_reason", hold.release_reason);
  return root.build();
}

[[nodiscard]] Result<HoldRecord> hold_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "hold record must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"reason", "authority", "tick", "released", "released_tick",
              "release_reason"}));
  HoldRecord hold;
  BSM_TRY_ASSIGN(reason, json::require_string(value, "reason"));
  BSM_RETURN_IF_ERROR(json::require_text(JsonValue::string(reason), "hold reason",
                                         limits::kMaxReasonLength));
  hold.reason = std::move(reason);
  BSM_TRY_ASSIGN(authority_value, json::require_field(value, "authority"));
  if (!authority_value->is_null()) {
    BSM_TRY_ASSIGN(authority, detail::authority_from_json(*authority_value));
    hold.has_authority = true;
    hold.authority = std::move(authority);
  }
  BSM_TRY_ASSIGN(tick, json::require_unsigned(value, "tick"));
  hold.tick = tick;
  BSM_TRY_ASSIGN(released, json::require_bool(value, "released"));
  hold.released = released;
  BSM_TRY_ASSIGN(released_tick, json::require_unsigned(value, "released_tick"));
  hold.released_tick = released_tick;
  BSM_TRY_ASSIGN(release_reason, json::require_string(value, "release_reason"));
  BSM_RETURN_IF_ERROR(json::require_text(JsonValue::string(release_reason),
                                         "release reason", limits::kMaxReasonLength));
  hold.release_reason = std::move(release_reason);
  return hold;
}

[[nodiscard]] Result<JsonValue> transition_to_json(const StageTransition& transition) {
  JsonObjectBuilder root;
  if (transition.from.empty()) {
    root.set_null("from");
  } else {
    root.set_text("from", transition.from.str());
  }
  root.set_text("to", transition.to.str());
  root.set_uint("to_index", static_cast<std::uint64_t>(transition.to_index));
  root.set_uint("tick", transition.tick);
  JsonArrayBuilder exit_evidence;
  for (const EvidenceId& id : transition.exit_evidence) {
    exit_evidence.push_text(id.hex());
  }
  BSM_TRY_ASSIGN(exit_evidence_value, exit_evidence.build());
  root.set("exit_evidence", exit_evidence_value);
  JsonArrayBuilder entry_evidence;
  for (const EvidenceId& id : transition.entry_evidence) {
    entry_evidence.push_text(id.hex());
  }
  BSM_TRY_ASSIGN(entry_evidence_value, entry_evidence.build());
  root.set("entry_evidence", entry_evidence_value);
  root.set_text("plan_digest", transition.plan_digest.hex());
  root.set_text("binding_digest", transition.binding_digest.hex());
  root.set_text("incarnation", transition.incarnation.hex());
  return root.build();
}

[[nodiscard]] Result<StageTransition> transition_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "stage transition must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"from", "to", "to_index", "tick", "exit_evidence", "entry_evidence",
              "plan_digest", "binding_digest", "incarnation"}));
  StageTransition transition;
  BSM_TRY_ASSIGN(from_value, json::require_field(value, "from"));
  if (!from_value->is_null()) {
    BSM_TRY_ASSIGN(from_text, json::require_text(*from_value, "stage identity",
                                                 limits::kMaxIdentifierLength));
    BSM_TRY_ASSIGN(from, StageId::parse(from_text));
    transition.from = std::move(from);
  }
  BSM_TRY_ASSIGN(to_text, json::require_string(value, "to"));
  BSM_TRY_ASSIGN(to, StageId::parse(to_text));
  transition.to = std::move(to);
  BSM_TRY_ASSIGN(to_index, json::require_unsigned(value, "to_index"));
  transition.to_index = static_cast<std::size_t>(to_index);
  BSM_TRY_ASSIGN(tick, json::require_unsigned(value, "tick"));
  transition.tick = tick;
  for (const auto& [key, destination] :
       {std::pair<std::string_view, std::vector<EvidenceId>*>{"exit_evidence",
                                                              &transition.exit_evidence},
        std::pair<std::string_view, std::vector<EvidenceId>*>{"entry_evidence",
                                                              &transition.entry_evidence}}) {
    BSM_TRY_ASSIGN(array, json::require_array(value, key));
    destination->reserve(array->size());
    for (const JsonValue& item : *array) {
      BSM_TRY_ASSIGN(item_text, json::require_text(item, "evidence identity", 64));
      BSM_TRY_ASSIGN(id, EvidenceId::from_hex(item_text));
      destination->push_back(id);
    }
  }
  BSM_TRY_ASSIGN(plan_digest_text, json::require_string(value, "plan_digest"));
  BSM_TRY_ASSIGN(plan_digest, Digest::from_hex(plan_digest_text));
  transition.plan_digest = plan_digest;
  BSM_TRY_ASSIGN(binding_digest_text, json::require_string(value, "binding_digest"));
  BSM_TRY_ASSIGN(binding_digest_value, Digest::from_hex(binding_digest_text));
  transition.binding_digest = binding_digest_value;
  BSM_TRY_ASSIGN(incarnation_text, json::require_string(value, "incarnation"));
  BSM_TRY_ASSIGN(incarnation, IncarnationId::from_hex(incarnation_text));
  transition.incarnation = incarnation;
  return transition;
}

[[nodiscard]] Result<JsonValue> replan_to_json(const ReplanRecord& replan) {
  JsonObjectBuilder root;
  root.set_text("previous_plan_digest", replan.previous_plan_digest.hex());
  root.set_text("new_plan_digest", replan.new_plan_digest.hex());
  root.set_text("reason", replan.reason);
  BSM_TRY_ASSIGN(authority, detail::authority_to_json(replan.authority));
  root.set("authority", authority);
  root.set_uint("tick", replan.tick);
  root.set_uint("carried_obligations", static_cast<std::uint64_t>(replan.carried_obligations));
  root.set_uint("reset_obligations", static_cast<std::uint64_t>(replan.reset_obligations));
  root.set_bool("accepted_topology_change", replan.accepted_topology_change);
  return root.build();
}

[[nodiscard]] Result<ReplanRecord> replan_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "replan record must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"previous_plan_digest", "new_plan_digest", "reason", "authority", "tick",
              "carried_obligations", "reset_obligations", "accepted_topology_change"}));
  ReplanRecord replan;
  BSM_TRY_ASSIGN(previous_text, json::require_string(value, "previous_plan_digest"));
  BSM_TRY_ASSIGN(previous, Digest::from_hex(previous_text));
  replan.previous_plan_digest = previous;
  BSM_TRY_ASSIGN(new_text, json::require_string(value, "new_plan_digest"));
  BSM_TRY_ASSIGN(current, Digest::from_hex(new_text));
  replan.new_plan_digest = current;
  BSM_TRY_ASSIGN(reason, json::require_string(value, "reason"));
  BSM_RETURN_IF_ERROR(json::require_text(JsonValue::string(reason), "replan reason",
                                         limits::kMaxReasonLength));
  replan.reason = std::move(reason);
  BSM_TRY_ASSIGN(authority_value, json::require_object_member(value, "authority"));
  BSM_TRY_ASSIGN(authority, detail::authority_from_json(*authority_value));
  replan.authority = std::move(authority);
  BSM_TRY_ASSIGN(tick, json::require_unsigned(value, "tick"));
  replan.tick = tick;
  BSM_TRY_ASSIGN(carried, json::require_unsigned(value, "carried_obligations"));
  replan.carried_obligations = static_cast<std::size_t>(carried);
  BSM_TRY_ASSIGN(reset, json::require_unsigned(value, "reset_obligations"));
  replan.reset_obligations = static_cast<std::size_t>(reset);
  BSM_TRY_ASSIGN(accepted, json::require_bool(value, "accepted_topology_change"));
  replan.accepted_topology_change = accepted;
  return replan;
}

[[nodiscard]] Result<JsonValue> fence_to_json(const FenceRecord& fence) {
  JsonObjectBuilder root;
  root.set_text("code", to_string(fence.code));
  root.set_text("detail", fence.detail);
  root.set_uint("tick", fence.tick);
  root.set_text("observed_binding_digest", fence.observed_binding_digest.hex());
  return root.build();
}

[[nodiscard]] Result<FenceRecord> fence_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "fence record must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"code", "detail", "tick", "observed_binding_digest"}));
  FenceRecord fence;
  BSM_TRY_ASSIGN(code_text, json::require_string(value, "code"));
  bool recognised = false;
  for (int index = 0; index <= static_cast<int>(FenceCode::IncarnationSuperseded); ++index) {
    const FenceCode candidate = static_cast<FenceCode>(index);
    if (code_text == to_string(candidate)) {
      fence.code = candidate;
      recognised = true;
      break;
    }
  }
  if (!recognised) {
    return Error(ErrorCode::InvalidArgument, "unknown fence code '" + code_text + "'");
  }
  BSM_TRY_ASSIGN(detail, json::require_string(value, "detail"));
  BSM_RETURN_IF_ERROR(json::require_text(JsonValue::string(detail), "fence detail",
                                         limits::kMaxReasonLength));
  fence.detail = std::move(detail);
  BSM_TRY_ASSIGN(tick, json::require_unsigned(value, "tick"));
  fence.tick = tick;
  BSM_TRY_ASSIGN(digest_text, json::require_string(value, "observed_binding_digest"));
  BSM_TRY_ASSIGN(digest, Digest::from_hex(digest_text));
  fence.observed_binding_digest = digest;
  return fence;
}

[[nodiscard]] Result<JsonValue> terminal_to_json(const TerminalRecord& terminal) {
  JsonObjectBuilder root;
  root.set_text("state", to_string(terminal.state));
  root.set_text("reason", terminal.reason);
  if (terminal.has_authority) {
    BSM_TRY_ASSIGN(authority, detail::authority_to_json(terminal.authority));
    root.set("authority", authority);
  } else {
    root.set_null("authority");
  }
  root.set_uint("tick", terminal.tick);
  JsonArrayBuilder proof;
  for (const EvidenceId& id : terminal.proof_evidence) {
    proof.push_text(id.hex());
  }
  BSM_TRY_ASSIGN(proof_value, proof.build());
  root.set("proof_evidence", proof_value);
  root.set_text("proof_digest", terminal.proof_digest.hex());
  return root.build();
}

[[nodiscard]] Result<TerminalRecord> terminal_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "terminal record must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"state", "reason", "authority", "tick", "proof_evidence",
              "proof_digest"}));
  TerminalRecord terminal;
  BSM_TRY_ASSIGN(state_text, json::require_string(value, "state"));
  if (state_text == "aborted") {
    terminal.state = SessionState::Aborted;
  } else if (state_text == "completed") {
    terminal.state = SessionState::Completed;
  } else if (state_text == "superseded") {
    terminal.state = SessionState::Superseded;
  } else {
    return Error(ErrorCode::InvalidArgument,
                 "terminal record state '" + state_text + "' is not terminal");
  }
  BSM_TRY_ASSIGN(reason, json::require_string(value, "reason"));
  BSM_RETURN_IF_ERROR(json::require_text(JsonValue::string(reason), "terminal reason",
                                         limits::kMaxReasonLength));
  terminal.reason = std::move(reason);
  BSM_TRY_ASSIGN(authority_value, json::require_field(value, "authority"));
  if (!authority_value->is_null()) {
    BSM_TRY_ASSIGN(authority, detail::authority_from_json(*authority_value));
    terminal.has_authority = true;
    terminal.authority = std::move(authority);
  }
  BSM_TRY_ASSIGN(tick, json::require_unsigned(value, "tick"));
  terminal.tick = tick;
  BSM_TRY_ASSIGN(proof, json::require_array(value, "proof_evidence"));
  terminal.proof_evidence.reserve(proof->size());
  for (const JsonValue& item : *proof) {
    BSM_TRY_ASSIGN(item_text, json::require_text(item, "evidence identity", 64));
    BSM_TRY_ASSIGN(id, EvidenceId::from_hex(item_text));
    terminal.proof_evidence.push_back(id);
  }
  BSM_TRY_ASSIGN(digest_text, json::require_string(value, "proof_digest"));
  BSM_TRY_ASSIGN(digest, Digest::from_hex(digest_text));
  terminal.proof_digest = digest;
  return terminal;
}

}  // namespace detail

Result<JsonValue> session_to_json(const SessionRecord& session) {
  BSM_RETURN_IF_ERROR(validate_session_record(session));

  JsonObjectBuilder root;
  root.set_text("id", session.id.hex());
  root.set_text("plan_digest", session.plan_digest.hex());
  BSM_TRY_ASSIGN(binding, binding_to_json(session.binding));
  root.set("binding", binding);
  root.set_text("binding_digest", session.binding_digest.hex());
  root.set_text("state", to_string(session.state));
  root.set_text("opened_by", session.opened_by.hex());
  root.set_text("authority_incarnation", session.authority_incarnation.hex());
  root.set_bool("authority_reestablished", session.authority_reestablished);
  root.set_uint("opened_tick", session.opened_tick);
  root.set_uint("updated_tick", session.updated_tick);
  root.set_uint("revision", session.revision);
  root.set_uint("stage_index", static_cast<std::uint64_t>(session.stage_index));

  JsonArrayBuilder history;
  for (const StageTransition& transition : session.stage_history) {
    BSM_TRY_ASSIGN(value, detail::transition_to_json(transition));
    history.push(value);
  }
  BSM_TRY_ASSIGN(history_value, history.build());
  root.set("stage_history", history_value);

  JsonArrayBuilder evidence;
  for (const EvidenceRecord& record : session.evidence) {
    BSM_TRY_ASSIGN(value, evidence_to_json(record));
    evidence.push(value);
  }
  BSM_TRY_ASSIGN(evidence_value, evidence.build());
  root.set("evidence", evidence_value);

  JsonArrayBuilder attempts;
  for (const AttemptRecord& attempt : session.attempts) {
    BSM_TRY_ASSIGN(value, attempt_to_json(attempt));
    attempts.push(value);
  }
  BSM_TRY_ASSIGN(attempts_value, attempts.build());
  root.set("attempts", attempts_value);

  JsonArrayBuilder holds;
  for (const HoldRecord& hold : session.holds) {
    BSM_TRY_ASSIGN(value, detail::hold_to_json(hold));
    holds.push(value);
  }
  BSM_TRY_ASSIGN(holds_value, holds.build());
  root.set("holds", holds_value);

  JsonArrayBuilder replans;
  for (const ReplanRecord& replan : session.replans) {
    BSM_TRY_ASSIGN(value, detail::replan_to_json(replan));
    replans.push(value);
  }
  BSM_TRY_ASSIGN(replans_value, replans.build());
  root.set("replans", replans_value);

  JsonArrayBuilder fences;
  for (const FenceRecord& fence : session.fences) {
    BSM_TRY_ASSIGN(value, detail::fence_to_json(fence));
    fences.push(value);
  }
  BSM_TRY_ASSIGN(fences_value, fences.build());
  root.set("fences", fences_value);

  if (session.has_terminal) {
    BSM_TRY_ASSIGN(value, detail::terminal_to_json(session.terminal));
    root.set("terminal", value);
  } else {
    root.set_null("terminal");
  }
  return root.build();
}

Result<SessionRecord> session_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "session record must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"id", "plan_digest", "binding", "binding_digest", "state", "opened_by",
              "authority_incarnation", "authority_reestablished", "opened_tick",
              "updated_tick", "revision", "stage_index", "stage_history", "evidence",
              "attempts", "holds", "replans", "fences", "terminal"}));

  SessionRecord session;
  BSM_TRY_ASSIGN(id_text, json::require_string(value, "id"));
  BSM_TRY_ASSIGN(id, SessionId::from_hex(id_text));
  session.id = id;
  BSM_TRY_ASSIGN(plan_digest_text, json::require_string(value, "plan_digest"));
  BSM_TRY_ASSIGN(plan_digest, Digest::from_hex(plan_digest_text));
  session.plan_digest = plan_digest;
  BSM_TRY_ASSIGN(binding_value, json::require_object_member(value, "binding"));
  BSM_TRY_ASSIGN(binding, binding_from_json(*binding_value));
  session.binding = std::move(binding);
  BSM_TRY_ASSIGN(binding_digest_text, json::require_string(value, "binding_digest"));
  BSM_TRY_ASSIGN(binding_digest_value, Digest::from_hex(binding_digest_text));
  session.binding_digest = binding_digest_value;
  BSM_TRY_ASSIGN(state_text, json::require_string(value, "state"));
  if (state_text == "active") {
    session.state = SessionState::Active;
  } else if (state_text == "held") {
    session.state = SessionState::Held;
  } else if (state_text == "aborted") {
    session.state = SessionState::Aborted;
  } else if (state_text == "completed") {
    session.state = SessionState::Completed;
  } else if (state_text == "superseded") {
    session.state = SessionState::Superseded;
  } else {
    return Error(ErrorCode::InvalidArgument, "unknown session state '" + state_text + "'");
  }
  BSM_TRY_ASSIGN(opened_by_text, json::require_string(value, "opened_by"));
  BSM_TRY_ASSIGN(opened_by, IncarnationId::from_hex(opened_by_text));
  session.opened_by = opened_by;
  BSM_TRY_ASSIGN(authority_incarnation_text,
                 json::require_string(value, "authority_incarnation"));
  BSM_TRY_ASSIGN(authority_incarnation, IncarnationId::from_hex(authority_incarnation_text));
  session.authority_incarnation = authority_incarnation;
  BSM_TRY_ASSIGN(authority_reestablished,
                 json::require_bool(value, "authority_reestablished"));
  session.authority_reestablished = authority_reestablished;
  BSM_TRY_ASSIGN(opened_tick, json::require_unsigned(value, "opened_tick"));
  session.opened_tick = opened_tick;
  BSM_TRY_ASSIGN(updated_tick, json::require_unsigned(value, "updated_tick"));
  session.updated_tick = updated_tick;
  BSM_TRY_ASSIGN(revision, json::require_unsigned(value, "revision"));
  session.revision = revision;
  BSM_TRY_ASSIGN(stage_index, json::require_unsigned(value, "stage_index"));
  session.stage_index = static_cast<std::size_t>(stage_index);

  BSM_TRY_ASSIGN(history, json::require_array(value, "stage_history"));
  session.stage_history.reserve(history->size());
  for (const JsonValue& item : *history) {
    BSM_TRY_ASSIGN(transition, detail::transition_from_json(item));
    session.stage_history.push_back(std::move(transition));
  }
  BSM_TRY_ASSIGN(evidence, json::require_array(value, "evidence"));
  session.evidence.reserve(evidence->size());
  for (const JsonValue& item : *evidence) {
    BSM_TRY_ASSIGN(record, evidence_from_json(item));
    session.evidence.push_back(std::move(record));
  }
  BSM_TRY_ASSIGN(attempts, json::require_array(value, "attempts"));
  session.attempts.reserve(attempts->size());
  for (const JsonValue& item : *attempts) {
    BSM_TRY_ASSIGN(attempt, attempt_from_json(item));
    session.attempts.push_back(std::move(attempt));
  }
  BSM_TRY_ASSIGN(holds, json::require_array(value, "holds"));
  session.holds.reserve(holds->size());
  for (const JsonValue& item : *holds) {
    BSM_TRY_ASSIGN(hold, detail::hold_from_json(item));
    session.holds.push_back(std::move(hold));
  }
  BSM_TRY_ASSIGN(replans, json::require_array(value, "replans"));
  session.replans.reserve(replans->size());
  for (const JsonValue& item : *replans) {
    BSM_TRY_ASSIGN(replan, detail::replan_from_json(item));
    session.replans.push_back(std::move(replan));
  }
  BSM_TRY_ASSIGN(fences, json::require_array(value, "fences"));
  session.fences.reserve(fences->size());
  for (const JsonValue& item : *fences) {
    BSM_TRY_ASSIGN(fence, detail::fence_from_json(item));
    session.fences.push_back(std::move(fence));
  }
  BSM_TRY_ASSIGN(terminal_value, json::require_field(value, "terminal"));
  if (!terminal_value->is_null()) {
    BSM_TRY_ASSIGN(terminal, detail::terminal_from_json(*terminal_value));
    session.has_terminal = true;
    session.terminal = std::move(terminal);
  }

  BSM_RETURN_IF_ERROR(validate_session_record(session));
  return session;
}

Result<Unit> validate_session_record(const SessionRecord& session) {
  if (session.id.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "session identity is absent");
  }
  if (session.plan_digest.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "session plan digest is absent");
  }
  BSM_RETURN_IF_ERROR(validate_binding(session.binding));
  BSM_TRY_ASSIGN(expected_binding_digest, binding_digest(session.binding));
  if (!(expected_binding_digest == session.binding_digest)) {
    return Error(ErrorCode::IntegrityMismatch,
                 "session binding digest does not match the committed binding");
  }
  if (session.opened_tick > limits::kMaxTick || session.updated_tick > limits::kMaxTick) {
    return Error(ErrorCode::NumberOutOfRange, "session tick exceeds the supported range");
  }
  if (session.updated_tick < session.opened_tick) {
    return Error(ErrorCode::InvalidArgument, "session was updated before it was opened");
  }
  if (session.revision > limits::kMaxGeneration) {
    return Error(ErrorCode::NumberOutOfRange,
                 "session revision exceeds the supported range");
  }
  if (session.stage_index > limits::kMaxStages) {
    return Error(ErrorCode::NumberOutOfRange, "session stage index is out of range");
  }
  if (session.authority_reestablished && session.authority_incarnation.is_zero()) {
    return Error(ErrorCode::InvalidArgument,
                 "session claims re-established authority without an incarnation");
  }
  if (session.stage_history.size() > limits::kMaxStageTransitions) {
    return Error(ErrorCode::LimitExceeded, "session records too many stage transitions");
  }
  std::size_t previous_index = 0;
  bool first = true;
  std::uint64_t previous_tick = 0;
  for (const StageTransition& transition : session.stage_history) {
    if (transition.to.empty()) {
      return Error(ErrorCode::InvalidArgument, "stage transition target is empty");
    }
    if (transition.tick > limits::kMaxTick) {
      return Error(ErrorCode::NumberOutOfRange,
                   "stage transition tick exceeds the supported range");
    }
    if (!first) {
      if (transition.tick < previous_tick) {
        return Error(ErrorCode::InvalidArgument,
                     "stage transitions are not in tick order");
      }
      if (transition.to_index <= previous_index) {
        return Error(ErrorCode::InvalidArgument,
                     "stage transitions do not advance monotonically");
      }
    }
    first = false;
    previous_index = transition.to_index;
    previous_tick = transition.tick;
  }
  if (session.evidence.size() > limits::kMaxEvidencePerSession) {
    return Error(ErrorCode::LimitExceeded, "session records too many evidence records");
  }
  for (std::size_t index = 0; index < session.evidence.size(); ++index) {
    BSM_RETURN_IF_ERROR(validate_evidence_record(session.evidence[index]));
    if (!(session.evidence[index].session == session.id)) {
      return Error(ErrorCode::EvidenceForeignSession,
                   "evidence record belongs to a different session");
    }
    if (session.evidence[index].recovered_tick != 0 &&
        session.evidence[index].recovered_tick < session.evidence[index].observed_tick) {
      return Error(ErrorCode::InvalidArgument,
                   "evidence was recovered before it was observed");
    }
    for (std::size_t other = index + 1; other < session.evidence.size(); ++other) {
      if (session.evidence[index].id == session.evidence[other].id) {
        return Error(ErrorCode::DuplicateIdentity,
                     "session records the same evidence identity twice");
      }
    }
  }
  if (session.attempts.size() > limits::kMaxAttemptsPerSession) {
    return Error(ErrorCode::LimitExceeded, "session records too many attempts");
  }
  for (std::size_t index = 0; index < session.attempts.size(); ++index) {
    BSM_RETURN_IF_ERROR(validate_attempt_record(session.attempts[index]));
    if (!(session.attempts[index].session == session.id)) {
      return Error(ErrorCode::EvidenceForeignSession,
                   "attempt record belongs to a different session");
    }
    for (std::size_t other = index + 1; other < session.attempts.size(); ++other) {
      if (session.attempts[index].id == session.attempts[other].id) {
        return Error(ErrorCode::DuplicateIdentity,
                     "session records the same attempt identity twice");
      }
      if (session.attempts[index].obligation == session.attempts[other].obligation &&
          session.attempts[index].sequence == session.attempts[other].sequence) {
        return Error(ErrorCode::DuplicateIdentity,
                     "session records two attempts with the same obligation and sequence");
      }
    }
  }
  if (session.holds.size() > limits::kMaxHoldsPerSession) {
    return Error(ErrorCode::LimitExceeded, "session records too many holds");
  }
  for (const HoldRecord& hold : session.holds) {
    if (hold.tick > limits::kMaxTick || hold.released_tick > limits::kMaxTick) {
      return Error(ErrorCode::NumberOutOfRange, "hold tick exceeds the supported range");
    }
    if (hold.reason.size() > limits::kMaxReasonLength) {
      return Error(ErrorCode::LimitExceeded, "hold reason exceeds the supported length");
    }
    if (hold.released && hold.released_tick < hold.tick) {
      return Error(ErrorCode::InvalidArgument, "hold was released before it was taken");
    }
  }
  bool has_unreleased_hold = false;
  for (const HoldRecord& hold : session.holds) {
    if (!hold.released) {
      has_unreleased_hold = true;
    }
  }
  if (session.state == SessionState::Held && !has_unreleased_hold) {
    return Error(ErrorCode::InvalidArgument,
                 "session is held without an unreleased hold record");
  }
  if (session.state != SessionState::Held && has_unreleased_hold) {
    return Error(ErrorCode::InvalidArgument,
                 "session records an unreleased hold while it is not held");
  }
  if (session.replans.size() > limits::kMaxStageTransitions) {
    return Error(ErrorCode::LimitExceeded, "session records too many replans");
  }
  for (const ReplanRecord& replan : session.replans) {
    if (replan.tick > limits::kMaxTick) {
      return Error(ErrorCode::NumberOutOfRange, "replan tick exceeds the supported range");
    }
    if (replan.previous_plan_digest.is_zero() || replan.new_plan_digest.is_zero()) {
      return Error(ErrorCode::InvalidArgument, "replan record is missing a plan digest");
    }
    if (replan.previous_plan_digest == replan.new_plan_digest) {
      return Error(ErrorCode::PlanUnchanged, "replan record kept the same plan digest");
    }
  }
  if (session.fences.size() > limits::kMaxStageTransitions) {
    return Error(ErrorCode::LimitExceeded, "session records too many fence records");
  }
  for (const FenceRecord& fence : session.fences) {
    if (fence.tick > limits::kMaxTick) {
      return Error(ErrorCode::NumberOutOfRange, "fence tick exceeds the supported range");
    }
  }
  if (session_is_terminal(session.state) != session.has_terminal) {
    return Error(ErrorCode::InvalidArgument,
                 "terminal sessions must carry a terminal record and live sessions must "
                 "not");
  }
  if (session.has_terminal) {
    if (session.terminal.state != session.state) {
      return Error(ErrorCode::InvalidArgument,
                   "terminal record state does not match the session state");
    }
    if (session.terminal.reason.size() > limits::kMaxReasonLength) {
      return Error(ErrorCode::LimitExceeded,
                   "terminal reason exceeds the supported length");
    }
    if (session.terminal.tick > limits::kMaxTick) {
      return Error(ErrorCode::NumberOutOfRange, "terminal tick exceeds the supported range");
    }
  }
  return Unit{};
}

Result<Digest> session_digest(const SessionRecord& session) {
  BSM_TRY_ASSIGN(value, session_to_json(session));
  return Digest::of(canonical_json(value));
}

}  // namespace black_start_manager
