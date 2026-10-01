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

#include "black_start_manager/controller.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "black_start_manager/checked.hpp"

namespace black_start_manager {
namespace {

// Every modelled value is an integer or a boolean so canonical comparison is exact and
// an observation can never drift through floating point.
[[nodiscard]] Result<std::string> require_string_parameter(const JsonValue& parameters,
                                                           const char* name) {
  BSM_TRY_ASSIGN(field, json::require_field(parameters, name));
  if (!field->is_string()) {
    return Error(ErrorCode::TypeMismatch,
                 std::string("parameter '") + name + "' must be a string");
  }
  BSM_TRY_ASSIGN(text, json::require_text(*field, name, limits::kMaxIdentifierLength));
  BSM_TRY_ASSIGN(identifier, IdentifierText::parse(text));
  (void)identifier;
  return text;
}

[[nodiscard]] Result<std::int64_t> require_integer_parameter(const JsonValue& parameters,
                                                             const char* name) {
  BSM_TRY_ASSIGN(field, json::require_field(parameters, name));
  if (!field->is_integer()) {
    return Error(ErrorCode::TypeMismatch,
                 std::string("parameter '") + name + "' must be an integer");
  }
  return field->as_integer();
}

[[nodiscard]] std::uint64_t generation_field(const JsonValue& subject,
                                              const char* field) {
  if (!subject.is_object()) {
    return 0;
  }
  const JsonValue* counter = subject.find(field);
  if (counter == nullptr || !counter->is_integer() || counter->as_integer() < 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(counter->as_integer());
}

}  // namespace

SyntheticPlant::SyntheticPlant() = default;

bool SyntheticPlant::has_subject(const SubjectId& subject) const noexcept {
  for (const auto& entry : subjects_) {
    if (entry.first == subject.str()) {
      return true;
    }
  }
  return false;
}

JsonValue* SyntheticPlant::find_subject(const SubjectId& subject) {
  for (auto& entry : subjects_) {
    if (entry.first == subject.str()) {
      return &entry.second;
    }
  }
  return nullptr;
}

Result<JsonValue> SyntheticPlant::observe(const SubjectId& subject) const {
  return observe_text(subject.str());
}

Result<JsonValue> SyntheticPlant::observe_text(const std::string& text) const {
  for (const auto& entry : subjects_) {
    if (entry.first == text) {
      return entry.second;
    }
  }
  // A subject that has never been actuated still has a defined synthetic state, so an
  // observation taken before any request is meaningful rather than absent.
  JsonObjectBuilder builder;
  const auto prefix_is = [&text](const char* prefix, std::size_t length) {
    return text.size() > length && text.compare(0, length, prefix) == 0;
  };
  if (prefix_is("domain:", 7)) {
    builder.set_bool("energized", false);
    builder.set_uint("energize_generation", 0);
  } else if (prefix_is("isolation:", 10)) {
    builder.set_bool("isolated", false);
    builder.set_uint("verify_generation", 0);
  } else if (prefix_is("control-power:", 14)) {
    builder.set_bool("available", false);
    builder.set_uint("restore_generation", 0);
  } else if (prefix_is("cooling:", 8)) {
    builder.set_bool("ready", false);
    builder.set_int("supply_temp_c", 0);
    builder.set_uint("restore_generation", 0);
  } else if (prefix_is("network:", 8)) {
    builder.set_bool("reachable", false);
    builder.set_uint("restore_generation", 0);
  } else if (prefix_is("capacity:", 9)) {
    builder.set_uint("available_mw", 0);
    builder.set_uint("restore_generation", 0);
  } else if (prefix_is("rack:", 5)) {
    builder.set_bool("admitted", false);
    builder.set_uint("admit_generation", 0);
  } else if (text == "facility") {
    builder.set_bool("return_to_service", false);
    builder.set_uint("verify_generation", 0);
  } else {
    return Error(ErrorCode::UnknownIdentity,
                 "the synthetic plant does not model subject '" + text + "'");
  }
  return builder.build();
}

Result<Unit> SyntheticPlant::restore(const JsonValue& state) {
  if (!state.is_object()) {
    return Error(ErrorCode::SchemaViolation,
                 "a restored synthetic plant state must be an object");
  }
  subjects_.clear();
  subjects_.reserve(state.as_object().size());
  for (const JsonValue::Field& field : state.as_object()) {
    BSM_TRY_ASSIGN(subject, SubjectId::parse(field.first));
    if (!field.second.is_object()) {
      return Error(ErrorCode::SchemaViolation,
                   "a restored synthetic subject state must be an object");
    }
    subjects_.emplace_back(field.first, field.second);
  }
  return Unit{};
}

Result<JsonValue> SyntheticPlant::state_json() const {
  JsonObjectBuilder root;
  for (const auto& entry : subjects_) {
    root.set(entry.first, entry.second);
  }
  return root.build();
}

Result<Digest> SyntheticPlant::effect_identity(const CapabilityId& capability,
                                               const JsonValue& parameters) {
  JsonObjectBuilder root;
  root.set_text("capability", capability.str());
  root.set("parameters", parameters);
  BSM_TRY_ASSIGN(value, root.build());
  return Digest::of(canonical_json(value));
}

Result<Unit> SyntheticPlant::apply(const CapabilityId& capability,
                                   const JsonValue& parameters) {
  if (capability.empty()) {
    return Error(ErrorCode::InvalidArgument, "capability identity is empty");
  }
  if (!parameters.is_object()) {
    return Error(ErrorCode::TypeMismatch, "capability parameters must be an object");
  }

  const std::string name = capability.str();
  std::string subject_text;
  JsonObjectBuilder state;
  bool modelled = false;

  if (name == "energize-domain" || name == "deenergize-domain") {
    const bool energize = name == "energize-domain";
    BSM_TRY_ASSIGN(domain, require_string_parameter(parameters, "domain"));
    subject_text = "domain:" + domain;
    BSM_TRY_ASSIGN(existing, observe_text(subject_text));
    BSM_TRY_ASSIGN(next, checked_increment(generation_field(existing, "energize_generation")));
    state.set_bool("energized", energize);
    state.set_uint("energize_generation", next);
    modelled = true;
  } else if (name == "verify-isolation") {
    BSM_TRY_ASSIGN(domain, require_string_parameter(parameters, "domain"));
    subject_text = "isolation:" + domain;
    BSM_TRY_ASSIGN(existing, observe_text(subject_text));
    BSM_TRY_ASSIGN(next, checked_increment(generation_field(existing, "verify_generation")));
    state.set_bool("isolated", true);
    state.set_uint("verify_generation", next);
    modelled = true;
  } else if (name == "restore-control-power") {
    BSM_TRY_ASSIGN(bus, require_string_parameter(parameters, "bus"));
    subject_text = "control-power:" + bus;
    BSM_TRY_ASSIGN(existing, observe_text(subject_text));
    BSM_TRY_ASSIGN(next, checked_increment(generation_field(existing, "restore_generation")));
    state.set_bool("available", true);
    state.set_uint("restore_generation", next);
    modelled = true;
  } else if (name == "restore-cooling") {
    BSM_TRY_ASSIGN(loop, require_string_parameter(parameters, "loop"));
    subject_text = "cooling:" + loop;
    const std::int64_t supply = [&parameters]() -> std::int64_t {
      const JsonValue* field = parameters.find("supply_temp_c");
      if (field != nullptr && field->is_integer()) {
        return field->as_integer();
      }
      return 18;
    }();
    if (supply < -40 || supply > 80) {
      return Error(ErrorCode::NumberOutOfRange,
                   "supply_temp_c is outside the modelled synthetic range");
    }
    BSM_TRY_ASSIGN(existing, observe_text(subject_text));
    BSM_TRY_ASSIGN(next, checked_increment(generation_field(existing, "restore_generation")));
    state.set_bool("ready", true);
    state.set_int("supply_temp_c", supply);
    state.set_uint("restore_generation", next);
    modelled = true;
  } else if (name == "restore-network-path") {
    BSM_TRY_ASSIGN(path, require_string_parameter(parameters, "path"));
    subject_text = "network:" + path;
    BSM_TRY_ASSIGN(existing, observe_text(subject_text));
    BSM_TRY_ASSIGN(next, checked_increment(generation_field(existing, "restore_generation")));
    state.set_bool("reachable", true);
    state.set_uint("restore_generation", next);
    modelled = true;
  } else if (name == "restore-capacity") {
    BSM_TRY_ASSIGN(pool, require_string_parameter(parameters, "pool"));
    BSM_TRY_ASSIGN(megawatts, require_integer_parameter(parameters, "available_mw"));
    if (megawatts < 0) {
      return Error(ErrorCode::NumberOutOfRange, "available_mw must not be negative");
    }
    subject_text = "capacity:" + pool;
    BSM_TRY_ASSIGN(existing, observe_text(subject_text));
    BSM_TRY_ASSIGN(next, checked_increment(generation_field(existing, "restore_generation")));
    state.set_uint("available_mw", static_cast<std::uint64_t>(megawatts));
    state.set_uint("restore_generation", next);
    modelled = true;
  } else if (name == "admit-rack") {
    BSM_TRY_ASSIGN(rack, require_string_parameter(parameters, "rack"));
    subject_text = "rack:" + rack;
    BSM_TRY_ASSIGN(existing, observe_text(subject_text));
    BSM_TRY_ASSIGN(next, checked_increment(generation_field(existing, "admit_generation")));
    state.set_bool("admitted", true);
    state.set_uint("admit_generation", next);
    modelled = true;
  } else if (name == "verify-return-to-service") {
    subject_text = "facility";
    BSM_TRY_ASSIGN(existing, observe_text(subject_text));
    BSM_TRY_ASSIGN(next, checked_increment(generation_field(existing, "verify_generation")));
    state.set_bool("return_to_service", true);
    state.set_uint("verify_generation", next);
    modelled = true;
  }

  if (!modelled) {
    return Error(ErrorCode::RequestRefused,
                 "the synthetic plant does not model capability '" + name + "'");
  }

  BSM_TRY_ASSIGN(state_value, state.build());
  BSM_TRY_ASSIGN(subject, SubjectId::parse(subject_text));
  last_subject_ = subject_text;
  if (JsonValue* slot = find_subject(subject)) {
    *slot = state_value;
    return Unit{};
  }
  subjects_.emplace_back(subject_text, state_value);
  std::sort(subjects_.begin(), subjects_.end(),
            [](const std::pair<std::string, JsonValue>& left,
               const std::pair<std::string, JsonValue>& right) {
              return left.first < right.first;
            });
  return Unit{};
}

InProcessSyntheticController::InProcessSyntheticController(SyntheticControllerPolicy policy)
    : policy_(std::move(policy)) {}

Result<Unit> InProcessSyntheticController::restore_outcome(const AppliedOutcome& outcome) {
  if (outcome.key.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "a restored outcome carries no request key");
  }
  const auto position = std::lower_bound(
      applied_.begin(), applied_.end(), outcome.key,
      [](const AppliedOutcome& record, const RequestKey& key) { return record.key < key; });
  if (position != applied_.end() && position->key == outcome.key) {
    if (!(position->effect == outcome.effect)) {
      return Error(ErrorCode::IntegrityMismatch,
                   "the durable ledger records two different effects for one request key");
    }
    return Unit{};
  }
  applied_.insert(position, outcome);
  if (!outcome.effect.is_zero() &&
      std::find(applied_effects_.begin(), applied_effects_.end(), outcome.effect) ==
          applied_effects_.end()) {
    applied_effects_.push_back(outcome.effect);
  }
  return Unit{};
}

Result<ControllerReply> InProcessSyntheticController::request_effect(
    const RequestEnvelope& envelope) {
  received_.push_back(envelope);
  const std::string capability = envelope.capability.str();
  const auto contains = [](const std::vector<CapabilityId>& list, const CapabilityId& id) {
    return std::find(list.begin(), list.end(), id) != list.end();
  };

  // Idempotent replay comes first: a repeated key never performs a second effect.
  const auto recorded = std::lower_bound(
      applied_.begin(), applied_.end(), envelope.key,
      [](const AppliedOutcome& record, const RequestKey& key) { return record.key < key; });
  if (recorded != applied_.end() && recorded->key == envelope.key) {
    ControllerReply replay = recorded->reply;
    if (replay.kind == ControllerReplyKind::Applied) {
      replay.kind = ControllerReplyKind::AlreadyApplied;
    }
    replay.detail = "replayed a recorded outcome for this request key";
    return replay;
  }

  if (contains(policy_.refuse, envelope.capability)) {
    ControllerReply reply;
    reply.kind = ControllerReplyKind::Refused;
    reply.key = envelope.key;
    reply.detail = "the owner refuses capability '" + capability + "' under policy";
    AppliedOutcome record;
    record.key = envelope.key;
    record.reply = reply;
    record.effect = Digest();
    applied_.insert(recorded, record);
    return reply;
  }
  if (contains(policy_.defer, envelope.capability)) {
    ControllerReply reply;
    reply.kind = ControllerReplyKind::Deferred;
    reply.key = envelope.key;
    reply.detail = "the owner defers capability '" + capability + "'";
    return reply;
  }
  if (contains(policy_.unavailable_once, envelope.capability)) {
    const auto seen = std::find(unavailable_.begin(), unavailable_.end(), envelope.key);
    const bool first_for_capability =
        std::find(unavailable_capabilities_.begin(), unavailable_capabilities_.end(),
                  envelope.capability) == unavailable_capabilities_.end();
    if (seen == unavailable_.end() && first_for_capability) {
      unavailable_.push_back(envelope.key);
      unavailable_capabilities_.push_back(envelope.capability);
      ControllerReply reply;
      reply.kind = ControllerReplyKind::Unavailable;
      reply.key = envelope.key;
      reply.detail = "the owner is temporarily unavailable and applied nothing";
      return reply;
    }
  }

  BSM_TRY_ASSIGN(effect,
                 SyntheticPlant::effect_identity(envelope.capability, envelope.parameters));
  const auto duplicate =
      std::find(applied_effects_.begin(), applied_effects_.end(), effect);
  if (duplicate != applied_effects_.end()) {
    ++duplicate_effects_;
  }
  applied_effects_.push_back(effect);

  BSM_RETURN_IF_ERROR(plant_.apply(envelope.capability, envelope.parameters));

  ControllerReply outcome;
  outcome.key = envelope.key;
  outcome.kind = ControllerReplyKind::Applied;
  outcome.detail = "the synthetic plant applied the capability";
  BSM_TRY_ASSIGN(observed, plant_.observe_text(plant_.last_subject()));
  outcome.observed = observed;
  outcome.effect_generation = 0;

  // The owner's durable outcome is what a later query must reveal. The reply handed to
  // the caller may be degraded; the record never is.
  AppliedOutcome record;
  record.key = envelope.key;
  record.reply = outcome;
  record.effect = effect;
  applied_.insert(recorded, record);

  ControllerReply reply = outcome;
  if (contains(policy_.fail_after_apply, envelope.capability)) {
    reply.kind = ControllerReplyKind::Failed;
    reply.detail = "the owner applied the capability and then reported a failure";
  } else if (contains(policy_.lose_first_reply, envelope.capability) &&
             std::find(lost_replies_.begin(), lost_replies_.end(), envelope.key) ==
                 lost_replies_.end()) {
    // The effect is applied and recorded, but the reply never reaches the manager.
    lost_replies_.push_back(envelope.key);
    reply.kind = ControllerReplyKind::Unavailable;
    reply.detail = "the reply to this request was lost after the effect was applied";
  }
  return reply;
}

Result<ControllerReply> InProcessSyntheticController::query_effect(const RequestKey& key) {
  const auto recorded = std::lower_bound(
      applied_.begin(), applied_.end(), key,
      [](const AppliedOutcome& record, const RequestKey& candidate) {
        return record.key < candidate;
      });
  if (recorded == applied_.end() || !(recorded->key == key)) {
    ControllerReply reply;
    reply.kind = ControllerReplyKind::Unknown;
    reply.key = key;
    reply.detail = "the owner has no record of this request key";
    return reply;
  }
  ControllerReply reply = recorded->reply;
  reply.key = key;
  if (reply.kind == ControllerReplyKind::Applied ||
      reply.kind == ControllerReplyKind::AlreadyApplied) {
    reply.detail = "the owner recorded that this request was applied";
  }
  return reply;
}

Result<ControllerObservation> InProcessSyntheticController::observe(
    const ObservationRequest& request) {
  if (std::find(policy_.blind_observers.begin(), policy_.blind_observers.end(),
                request.observer) != policy_.blind_observers.end()) {
    return Error(ErrorCode::ControllerUnavailable,
                 "observer '" + request.observer.str() + "' cannot see this subject");
  }
  BSM_TRY_ASSIGN(value, plant_.observe(request.subject));
  if (!request.capability.empty() &&
      std::find(policy_.contradictory_observation.begin(),
                policy_.contradictory_observation.end(),
                request.capability) != policy_.contradictory_observation.end()) {
    // Models an owner whose instrument disagrees with the applied effect.
    JsonObjectBuilder conflicting;
    for (const JsonValue::Field& field : value.as_object()) {
      if (field.second.is_bool()) {
        conflicting.set_bool(field.first, !field.second.as_bool());
      } else {
        conflicting.set(field.first, field.second);
      }
    }
    BSM_TRY_ASSIGN(conflicting_value, conflicting.build());
    value = conflicting_value;
  }
  ControllerObservation observation;
  observation.owner = request.observer;
  observation.domain = request.authority.domain;
  observation.generation = request.authority.generation;
  observation.value = value;
  observation.detail = "synthetic owner observation";
  return observation;
}

}  // namespace black_start_manager
