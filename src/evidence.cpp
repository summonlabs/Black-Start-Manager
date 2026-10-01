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

#include "black_start_manager/evidence.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace black_start_manager {
namespace {

// The immutable content of an evidence record. Provenance and recovered_tick are
// deliberately excluded: they are session-local state that recovery changes without
// changing what was observed.
[[nodiscard]] Result<JsonValue> content_json(const EvidenceRecord& record) {
  JsonObjectBuilder root;
  root.set_text("session", record.session.hex());
  root.set_text("kind", record.kind.str());
  root.set_text("subject", record.subject.str());
  if (record.has_obligation) {
    root.set_text("obligation", record.obligation.str());
  } else {
    root.set_null("obligation");
  }
  if (record.has_stage) {
    root.set_text("stage", record.stage.str());
  } else {
    root.set_null("stage");
  }
  root.set("value", record.value);
  root.set_text("source_owner", record.source_owner.str());
  root.set_text("source_domain", to_string(record.source_domain));
  root.set_uint("source_generation", record.source_generation);
  root.set_text("channel", to_string(record.channel));
  root.set_uint("observed_tick", record.observed_tick);
  root.set_text("plan_digest", record.plan_digest.hex());
  root.set_text("binding_digest", record.binding_digest.hex());
  root.set_uint("facility_epoch", record.facility_epoch);
  return root.build();
}

}  // namespace

Result<Digest> evidence_content_digest(const EvidenceRecord& record) {
  BSM_TRY_ASSIGN(value, content_json(record));
  return Digest::of(canonical_json(value));
}

Result<EvidenceId> evidence_id_for(const EvidenceRecord& record) {
  BSM_TRY_ASSIGN(digest, evidence_content_digest(record));
  return EvidenceId(digest.bytes());
}

Result<Unit> validate_evidence_record(const EvidenceRecord& record) {
  if (record.kind.empty()) {
    return Error(ErrorCode::InvalidArgument, "evidence kind is empty");
  }
  if (record.subject.empty()) {
    return Error(ErrorCode::InvalidArgument, "evidence subject is empty");
  }
  if (record.has_obligation && record.obligation.empty()) {
    return Error(ErrorCode::InvalidArgument, "evidence obligation identity is empty");
  }
  if (record.has_stage && record.stage.empty()) {
    return Error(ErrorCode::InvalidArgument, "evidence stage identity is empty");
  }
  if (record.session.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "evidence session identity is absent");
  }
  if (record.source_owner.empty()) {
    return Error(ErrorCode::InvalidArgument, "evidence source owner is empty");
  }
  if (record.observed_tick > limits::kMaxTick) {
    return Error(ErrorCode::NumberOutOfRange, "evidence tick exceeds the supported range");
  }
  if (record.source_generation > limits::kMaxGeneration) {
    return Error(ErrorCode::NumberOutOfRange,
                 "evidence authority generation exceeds the supported range");
  }
  if (record.facility_epoch > limits::kMaxGeneration) {
    return Error(ErrorCode::NumberOutOfRange,
                 "evidence facility epoch exceeds the supported range");
  }
  if (record.plan_digest.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "evidence plan digest is absent");
  }
  if (record.binding_digest.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "evidence binding digest is absent");
  }
  if (record.provenance == EvidenceProvenance::Recovered && record.recovered_tick == 0) {
    return Error(ErrorCode::InvalidArgument,
                 "recovered evidence must record the tick it was demoted at");
  }
  if (record.provenance == EvidenceProvenance::Live && record.recovered_tick != 0) {
    return Error(ErrorCode::InvalidArgument,
                 "live evidence must not carry a recovery tick");
  }
  BSM_TRY_ASSIGN(content, evidence_content_digest(record));
  if (record.content_digest != content) {
    return Error(ErrorCode::IntegrityMismatch,
                 "evidence content digest does not match the record content");
  }
  if (!(record.id.bytes() == content.bytes())) {
    return Error(ErrorCode::IntegrityMismatch,
                 "evidence identity does not match the record content");
  }
  return Unit{};
}

Result<JsonValue> evidence_to_json(const EvidenceRecord& record) {
  BSM_RETURN_IF_ERROR(validate_evidence_record(record));

  JsonObjectBuilder root;
  root.set_text("id", record.id.hex());
  root.set_text("session", record.session.hex());
  root.set_text("kind", record.kind.str());
  root.set_text("subject", record.subject.str());
  if (record.has_obligation) {
    root.set_text("obligation", record.obligation.str());
  } else {
    root.set_null("obligation");
  }
  if (record.has_stage) {
    root.set_text("stage", record.stage.str());
  } else {
    root.set_null("stage");
  }
  root.set("value", record.value);
  root.set_text("source_owner", record.source_owner.str());
  root.set_text("source_domain", to_string(record.source_domain));
  root.set_uint("source_generation", record.source_generation);
  root.set_text("channel", to_string(record.channel));
  root.set_text("provenance", to_string(record.provenance));
  root.set_uint("observed_tick", record.observed_tick);
  root.set_uint("recovered_tick", record.recovered_tick);
  root.set_text("plan_digest", record.plan_digest.hex());
  root.set_text("binding_digest", record.binding_digest.hex());
  root.set_uint("facility_epoch", record.facility_epoch);
  root.set_text("content_digest", record.content_digest.hex());
  return root.build();
}

Result<EvidenceRecord> evidence_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "evidence record must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value,
      {"id", "session", "kind", "subject", "obligation", "stage", "value",
       "source_owner", "source_domain", "source_generation", "channel", "provenance",
       "observed_tick", "recovered_tick", "plan_digest", "binding_digest",
       "facility_epoch", "content_digest"}));

  EvidenceRecord record;
  BSM_TRY_ASSIGN(id_text, json::require_string(value, "id"));
  BSM_TRY_ASSIGN(id, EvidenceId::from_hex(id_text));
  record.id = id;
  BSM_TRY_ASSIGN(session_text, json::require_string(value, "session"));
  BSM_TRY_ASSIGN(session, SessionId::from_hex(session_text));
  record.session = session;
  BSM_TRY_ASSIGN(kind_text, json::require_string(value, "kind"));
  BSM_TRY_ASSIGN(kind, EvidenceKind::parse(kind_text));
  record.kind = std::move(kind);
  BSM_TRY_ASSIGN(subject_text, json::require_string(value, "subject"));
  BSM_TRY_ASSIGN(subject, SubjectId::parse(subject_text));
  record.subject = std::move(subject);

  BSM_TRY_ASSIGN(obligation_value, json::require_field(value, "obligation"));
  if (obligation_value->is_null()) {
    record.has_obligation = false;
  } else {
    BSM_TRY_ASSIGN(obligation_text,
                   json::require_text(*obligation_value, "obligation", limits::kMaxIdentifierLength));
    BSM_TRY_ASSIGN(obligation, ObligationId::parse(obligation_text));
    record.has_obligation = true;
    record.obligation = std::move(obligation);
  }
  BSM_TRY_ASSIGN(stage_value, json::require_field(value, "stage"));
  if (stage_value->is_null()) {
    record.has_stage = false;
  } else {
    BSM_TRY_ASSIGN(stage_text,
                   json::require_text(*stage_value, "stage", limits::kMaxIdentifierLength));
    BSM_TRY_ASSIGN(stage, StageId::parse(stage_text));
    record.has_stage = true;
    record.stage = std::move(stage);
  }

  BSM_TRY_ASSIGN(observation_value, json::require_field(value, "value"));
  record.value = *observation_value;

  BSM_TRY_ASSIGN(owner_text, json::require_string(value, "source_owner"));
  BSM_TRY_ASSIGN(owner, OwnerId::parse(owner_text));
  record.source_owner = std::move(owner);
  BSM_TRY_ASSIGN(domain_text, json::require_string(value, "source_domain"));
  BSM_TRY_ASSIGN(domain, parse_authority_domain(domain_text));
  record.source_domain = domain;
  BSM_TRY_ASSIGN(source_generation, json::require_unsigned(value, "source_generation"));
  record.source_generation = source_generation;
  BSM_TRY_ASSIGN(channel_text, json::require_string(value, "channel"));
  BSM_TRY_ASSIGN(channel, parse_evidence_channel(channel_text));
  record.channel = channel;
  BSM_TRY_ASSIGN(provenance_text, json::require_string(value, "provenance"));
  if (provenance_text == "live") {
    record.provenance = EvidenceProvenance::Live;
  } else if (provenance_text == "recovered") {
    record.provenance = EvidenceProvenance::Recovered;
  } else {
    return Error(ErrorCode::InvalidArgument,
                 "unknown evidence provenance '" + provenance_text + "'");
  }
  BSM_TRY_ASSIGN(observed_tick, json::require_unsigned(value, "observed_tick"));
  record.observed_tick = observed_tick;
  BSM_TRY_ASSIGN(recovered_tick, json::require_unsigned(value, "recovered_tick"));
  record.recovered_tick = recovered_tick;
  BSM_TRY_ASSIGN(plan_digest_text, json::require_string(value, "plan_digest"));
  BSM_TRY_ASSIGN(plan_digest, Digest::from_hex(plan_digest_text));
  record.plan_digest = plan_digest;
  BSM_TRY_ASSIGN(binding_digest_text, json::require_string(value, "binding_digest"));
  BSM_TRY_ASSIGN(binding_digest_value, Digest::from_hex(binding_digest_text));
  record.binding_digest = binding_digest_value;
  BSM_TRY_ASSIGN(facility_epoch, json::require_unsigned(value, "facility_epoch"));
  record.facility_epoch = facility_epoch;
  BSM_TRY_ASSIGN(content_text, json::require_string(value, "content_digest"));
  BSM_TRY_ASSIGN(content, Digest::from_hex(content_text));
  record.content_digest = content;

  BSM_TRY_ASSIGN(computed, evidence_content_digest(record));
  if (computed != content) {
    return Error(ErrorCode::IntegrityMismatch,
                 "evidence content digest does not match the record content");
  }
  BSM_RETURN_IF_ERROR(validate_evidence_record(record));
  return record;
}

}  // namespace black_start_manager
