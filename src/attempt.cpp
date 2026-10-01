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

#include <cstddef>
#include <cstdint>
#include <string>

#include "detail/model_json.hpp"

namespace black_start_manager {

namespace detail {

Result<JsonValue> envelope_body_json(const RequestEnvelope& envelope) {
  JsonObjectBuilder root;
  root.set_text("attempt", envelope.attempt.hex());
  root.set_text("key", envelope.key.hex());
  root.set_text("session", envelope.session.hex());
  root.set_text("obligation", envelope.obligation.str());
  root.set_text("plan_digest", envelope.plan_digest.hex());
  root.set_text("binding_digest", envelope.binding_digest.hex());
  root.set_text("capability", envelope.capability.str());
  root.set("parameters", envelope.parameters);
  BSM_TRY_ASSIGN(authority,
                 ::black_start_manager::detail::authority_to_json(envelope.authority));
  root.set("authority", authority);
  root.set_uint("attempt_sequence", envelope.attempt_sequence);
  root.set_uint("created_tick", envelope.created_tick);
  root.set_uint("deadline_tick", envelope.deadline_tick);
  return root.build();
}

Result<JsonValue> controller_reply_to_json(const ControllerReply& reply) {
  JsonObjectBuilder root;
  root.set_text("kind", to_string(reply.kind));
  root.set_text("key", reply.key.hex());
  root.set_text("detail", reply.detail);
  root.set("observed", reply.observed);
  root.set_uint("effect_generation", reply.effect_generation);
  return root.build();
}

Result<ControllerReply> controller_reply_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "controller reply must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"kind", "key", "detail", "observed", "effect_generation"}));
  if (value.as_object().size() != 5) {
    return Error(ErrorCode::MissingField,
                 "controller reply is missing one of its five fields");
  }
  BSM_TRY_ASSIGN(kind_text, json::require_string(value, "kind"));
  bool recognised = false;
  ControllerReply reply;
  for (int index = 0; index <= static_cast<int>(ControllerReplyKind::Deferred); ++index) {
    const ControllerReplyKind candidate = static_cast<ControllerReplyKind>(index);
    if (kind_text == to_string(candidate)) {
      reply.kind = candidate;
      recognised = true;
      break;
    }
  }
  if (!recognised) {
    return Error(ErrorCode::InvalidArgument,
                 "unknown controller reply kind '" + kind_text + "'");
  }
  BSM_TRY_ASSIGN(key_text, json::require_string(value, "key"));
  BSM_TRY_ASSIGN(key, RequestKey::from_hex(key_text));
  reply.key = key;
  BSM_TRY_ASSIGN(detail_text, json::require_string(value, "detail"));
  BSM_RETURN_IF_ERROR(json::require_text(JsonValue::string(detail_text), "reply detail",
                                         limits::kMaxReasonLength));
  reply.detail = std::move(detail_text);
  BSM_TRY_ASSIGN(observed, json::require_field(value, "observed"));
  reply.observed = *observed;
  BSM_TRY_ASSIGN(generation, json::require_unsigned(value, "effect_generation"));
  reply.effect_generation = generation;
  return reply;
}

Result<RequestEnvelope> request_envelope_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "request envelope must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"attempt", "key", "session", "obligation", "plan_digest", "binding_digest",
              "capability", "parameters", "authority", "attempt_sequence", "created_tick",
              "deadline_tick", "envelope_digest"}));
  RequestEnvelope envelope;
  BSM_TRY_ASSIGN(attempt_text, json::require_string(value, "attempt"));
  BSM_TRY_ASSIGN(attempt, AttemptId::from_hex(attempt_text));
  envelope.attempt = attempt;
  BSM_TRY_ASSIGN(key_text, json::require_string(value, "key"));
  BSM_TRY_ASSIGN(key, RequestKey::from_hex(key_text));
  envelope.key = key;
  BSM_TRY_ASSIGN(session_text, json::require_string(value, "session"));
  BSM_TRY_ASSIGN(session, SessionId::from_hex(session_text));
  envelope.session = session;
  BSM_TRY_ASSIGN(obligation_text, json::require_string(value, "obligation"));
  BSM_TRY_ASSIGN(obligation, ObligationId::parse(obligation_text));
  envelope.obligation = std::move(obligation);
  BSM_TRY_ASSIGN(plan_text, json::require_string(value, "plan_digest"));
  BSM_TRY_ASSIGN(plan_digest, Digest::from_hex(plan_text));
  envelope.plan_digest = plan_digest;
  BSM_TRY_ASSIGN(binding_text, json::require_string(value, "binding_digest"));
  BSM_TRY_ASSIGN(binding_digest_value, Digest::from_hex(binding_text));
  envelope.binding_digest = binding_digest_value;
  BSM_TRY_ASSIGN(capability_text, json::require_string(value, "capability"));
  BSM_TRY_ASSIGN(capability, CapabilityId::parse(capability_text));
  envelope.capability = std::move(capability);
  BSM_TRY_ASSIGN(parameters, json::require_field(value, "parameters"));
  if (!parameters->is_object()) {
    return Error(ErrorCode::TypeMismatch, "request parameters must be an object");
  }
  envelope.parameters = *parameters;
  BSM_TRY_ASSIGN(authority_value, json::require_object_member(value, "authority"));
  BSM_TRY_ASSIGN(authority, ::black_start_manager::detail::authority_from_json(
                                     *authority_value));
  envelope.authority = std::move(authority);
  BSM_TRY_ASSIGN(sequence, json::require_unsigned(value, "attempt_sequence"));
  envelope.attempt_sequence = sequence;
  BSM_TRY_ASSIGN(created, json::require_unsigned(value, "created_tick"));
  envelope.created_tick = created;
  BSM_TRY_ASSIGN(deadline, json::require_unsigned(value, "deadline_tick"));
  envelope.deadline_tick = deadline;
  BSM_TRY_ASSIGN(digest_text, json::require_string(value, "envelope_digest"));
  BSM_TRY_ASSIGN(digest, Digest::from_hex(digest_text));
  envelope.envelope_digest = digest;
  BSM_TRY_ASSIGN(computed, request_envelope_digest(envelope));
  if (!(computed == digest)) {
    return Error(ErrorCode::IntegrityMismatch,
                 "the request envelope digest does not match its content");
  }
  return envelope;
}

}  // namespace detail

Result<Digest> request_envelope_digest(const RequestEnvelope& envelope) {
  BSM_TRY_ASSIGN(body, detail::envelope_body_json(envelope));
  return Digest::of(canonical_json(body));
}

Result<RequestKey> request_key_for(const SessionId& session,
                                   const ObligationId& obligation,
                                   std::uint64_t attempt_sequence,
                                   const Digest& plan_digest,
                                   const Digest& binding_digest,
                                   const CapabilityId& capability,
                                   const JsonValue& parameters) {
  JsonObjectBuilder root;
  root.set_text("session", session.hex());
  root.set_text("obligation", obligation.str());
  root.set_uint("attempt_sequence", attempt_sequence);
  root.set_text("plan_digest", plan_digest.hex());
  root.set_text("binding_digest", binding_digest.hex());
  root.set_text("capability", capability.str());
  root.set("parameters", parameters);
  BSM_TRY_ASSIGN(value, root.build());
  const Digest digest = Digest::of(canonical_json(value));
  return RequestKey(digest.bytes());
}

Result<AttemptId> attempt_id_for(const SessionId& session, const ObligationId& obligation,
                                 std::uint64_t attempt_sequence,
                                 const Digest& plan_digest,
                                 const Digest& binding_digest) {
  JsonObjectBuilder root;
  root.set_text("session", session.hex());
  root.set_text("obligation", obligation.str());
  root.set_uint("attempt_sequence", attempt_sequence);
  root.set_text("plan_digest", plan_digest.hex());
  root.set_text("binding_digest", binding_digest.hex());
  BSM_TRY_ASSIGN(value, root.build());
  const Digest digest = Digest::of(canonical_json(value));
  return AttemptId(digest.bytes());
}

Result<JsonValue> request_envelope_to_json(const RequestEnvelope& envelope) {
  BSM_TRY_ASSIGN(body, detail::envelope_body_json(envelope));
  JsonObjectBuilder root;
  for (const JsonValue::Field& field : body.as_object()) {
    root.set(field.first, field.second);
  }
  root.set_text("envelope_digest", envelope.envelope_digest.hex());
  return root.build();
}

Result<Unit> validate_attempt_record(const AttemptRecord& attempt) {
  if (attempt.session.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "attempt session identity is absent");
  }
  if (attempt.obligation.empty()) {
    return Error(ErrorCode::InvalidArgument, "attempt obligation identity is empty");
  }
  if (attempt.sequence == 0) {
    return Error(ErrorCode::InvalidArgument, "attempt sequence starts at one");
  }
  if (attempt.key.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "attempt request key is absent");
  }
  if (attempt.plan_digest.is_zero() || attempt.binding_digest.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "attempt plan or binding digest is absent");
  }
  if (attempt.requested_tick > limits::kMaxTick ||
      attempt.settled_tick > limits::kMaxTick) {
    return Error(ErrorCode::NumberOutOfRange, "attempt tick exceeds the supported range");
  }
  if (attempt.settled_tick != 0 && attempt.settled_tick < attempt.requested_tick) {
    return Error(ErrorCode::InvalidArgument, "attempt settled before it was requested");
  }
  switch (attempt.state) {
    case AttemptState::Requested:
      if (attempt.settled_tick != 0) {
        return Error(ErrorCode::AttemptStateInvalid,
                     "a requested attempt has not settled yet");
      }
      break;
    case AttemptState::Acknowledged:
    case AttemptState::NotApplied:
    case AttemptState::Refused:
    case AttemptState::Failed:
      if (attempt.settled_tick == 0) {
        return Error(ErrorCode::AttemptStateInvalid,
                     "a settled attempt must record the tick it settled at");
      }
      if (!(attempt.reply.key == attempt.key)) {
        return Error(ErrorCode::AttemptStateInvalid,
                     "a settled attempt must carry the reply for its own request key");
      }
      break;
    case AttemptState::Unresolved:
    case AttemptState::Fenced:
      break;
  }
  BSM_TRY_ASSIGN(id, attempt_id_for(attempt.session, attempt.obligation, attempt.sequence,
                                    attempt.plan_digest, attempt.binding_digest));
  if (!(id == attempt.id)) {
    return Error(ErrorCode::IntegrityMismatch,
                 "attempt identity does not match its session, obligation, sequence, and "
                 "bound digests");
  }
  return Unit{};
}

Result<JsonValue> attempt_to_json(const AttemptRecord& attempt) {
  BSM_RETURN_IF_ERROR(validate_attempt_record(attempt));

  JsonObjectBuilder root;
  root.set_text("id", attempt.id.hex());
  root.set_text("session", attempt.session.hex());
  root.set_text("obligation", attempt.obligation.str());
  root.set_uint("sequence", attempt.sequence);
  root.set_text("state", to_string(attempt.state));
  root.set_text("key", attempt.key.hex());
  root.set_text("request_digest", attempt.request_digest.hex());
  root.set_text("plan_digest", attempt.plan_digest.hex());
  root.set_text("binding_digest", attempt.binding_digest.hex());
  root.set_text("incarnation", attempt.incarnation.hex());
  root.set_uint("requested_tick", attempt.requested_tick);
  root.set_uint("settled_tick", attempt.settled_tick);
  BSM_TRY_ASSIGN(reply, detail::controller_reply_to_json(attempt.reply));
  root.set("reply", reply);
  root.set_text("resolution_note", attempt.resolution_note);
  JsonArrayBuilder evidence;
  for (const EvidenceId& id : attempt.evidence) {
    evidence.push_text(id.hex());
  }
  BSM_TRY_ASSIGN(evidence_value, evidence.build());
  root.set("evidence", evidence_value);
  return root.build();
}

Result<AttemptRecord> attempt_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "attempt record must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"id", "session", "obligation", "sequence", "state", "key",
              "request_digest", "plan_digest", "binding_digest", "incarnation",
              "requested_tick", "settled_tick", "reply", "resolution_note", "evidence"}));

  AttemptRecord attempt;
  BSM_TRY_ASSIGN(id_text, json::require_string(value, "id"));
  BSM_TRY_ASSIGN(id, AttemptId::from_hex(id_text));
  attempt.id = id;
  BSM_TRY_ASSIGN(session_text, json::require_string(value, "session"));
  BSM_TRY_ASSIGN(session, SessionId::from_hex(session_text));
  attempt.session = session;
  BSM_TRY_ASSIGN(obligation_text, json::require_string(value, "obligation"));
  BSM_TRY_ASSIGN(obligation, ObligationId::parse(obligation_text));
  attempt.obligation = std::move(obligation);
  BSM_TRY_ASSIGN(sequence, json::require_unsigned(value, "sequence"));
  attempt.sequence = sequence;
  BSM_TRY_ASSIGN(state_text, json::require_string(value, "state"));
  bool recognised = false;
  for (int index = 0; index <= static_cast<int>(AttemptState::Fenced); ++index) {
    const AttemptState candidate = static_cast<AttemptState>(index);
    if (state_text == to_string(candidate)) {
      attempt.state = candidate;
      recognised = true;
      break;
    }
  }
  if (!recognised) {
    return Error(ErrorCode::InvalidArgument, "unknown attempt state '" + state_text + "'");
  }
  BSM_TRY_ASSIGN(key_text, json::require_string(value, "key"));
  BSM_TRY_ASSIGN(key, RequestKey::from_hex(key_text));
  attempt.key = key;
  BSM_TRY_ASSIGN(request_digest_text, json::require_string(value, "request_digest"));
  BSM_TRY_ASSIGN(request_digest, Digest::from_hex(request_digest_text));
  attempt.request_digest = request_digest;
  BSM_TRY_ASSIGN(plan_digest_text, json::require_string(value, "plan_digest"));
  BSM_TRY_ASSIGN(plan_digest, Digest::from_hex(plan_digest_text));
  attempt.plan_digest = plan_digest;
  BSM_TRY_ASSIGN(binding_digest_text, json::require_string(value, "binding_digest"));
  BSM_TRY_ASSIGN(binding_digest_value, Digest::from_hex(binding_digest_text));
  attempt.binding_digest = binding_digest_value;
  BSM_TRY_ASSIGN(incarnation_text, json::require_string(value, "incarnation"));
  BSM_TRY_ASSIGN(incarnation, IncarnationId::from_hex(incarnation_text));
  attempt.incarnation = incarnation;
  BSM_TRY_ASSIGN(requested_tick, json::require_unsigned(value, "requested_tick"));
  attempt.requested_tick = requested_tick;
  BSM_TRY_ASSIGN(settled_tick, json::require_unsigned(value, "settled_tick"));
  attempt.settled_tick = settled_tick;
  BSM_TRY_ASSIGN(reply_value, json::require_object_member(value, "reply"));
  BSM_TRY_ASSIGN(reply, detail::controller_reply_from_json(*reply_value));
  attempt.reply = std::move(reply);
  BSM_TRY_ASSIGN(note, json::require_string(value, "resolution_note"));
  BSM_RETURN_IF_ERROR(json::require_text(JsonValue::string(note), "resolution note",
                                         limits::kMaxReasonLength));
  attempt.resolution_note = std::move(note);
  BSM_TRY_ASSIGN(evidence, json::require_array(value, "evidence"));
  if (evidence->size() > limits::kMaxEvidencePerSession) {
    return Error(ErrorCode::LimitExceeded, "attempt lists too many evidence records");
  }
  attempt.evidence.reserve(evidence->size());
  for (const JsonValue& entry : *evidence) {
    BSM_TRY_ASSIGN(entry_text, json::require_text(entry, "evidence identity", 64));
    BSM_TRY_ASSIGN(entry_id, EvidenceId::from_hex(entry_text));
    attempt.evidence.push_back(entry_id);
  }

  BSM_RETURN_IF_ERROR(validate_attempt_record(attempt));
  return attempt;
}

}  // namespace black_start_manager
