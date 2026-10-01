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

#include "black_start_manager/plan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace black_start_manager {
namespace {

[[nodiscard]] Result<EvidenceRequirement> requirement_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "evidence requirement must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"kind", "subject", "max_age_ticks", "min_sources", "required_owner",
              "required_domain", "expect"}));

  EvidenceRequirement requirement;
  BSM_TRY_ASSIGN(kind_text, json::require_string(value, "kind"));
  BSM_TRY_ASSIGN(kind, EvidenceKind::parse(kind_text));
  requirement.kind = std::move(kind);
  BSM_TRY_ASSIGN(subject_text, json::require_string(value, "subject"));
  BSM_TRY_ASSIGN(subject, SubjectId::parse(subject_text));
  requirement.subject = std::move(subject);

  if (const JsonValue* field = json::optional_field(value, "max_age_ticks")) {
    if (!field->is_integer() || field->as_integer() < 0) {
      return Error(ErrorCode::TypeMismatch,
                   "evidence requirement max_age_ticks must be a non-negative integer");
    }
    requirement.max_age_ticks = static_cast<std::uint64_t>(field->as_integer());
  }
  if (const JsonValue* field = json::optional_field(value, "min_sources")) {
    if (!field->is_integer() || field->as_integer() < 0) {
      return Error(ErrorCode::TypeMismatch,
                   "evidence requirement min_sources must be a positive integer");
    }
    requirement.min_sources = static_cast<std::uint32_t>(field->as_integer());
  }
  if (const JsonValue* field = json::optional_field(value, "required_owner")) {
    BSM_TRY_ASSIGN(owner_text,
                   json::require_text(*field, "required_owner", limits::kMaxOwnerLength));
    BSM_TRY_ASSIGN(owner, OwnerId::parse(owner_text));
    requirement.has_required_owner = true;
    requirement.required_owner = std::move(owner);
  }
  if (const JsonValue* field = json::optional_field(value, "required_domain")) {
    BSM_TRY_ASSIGN(domain_text, json::require_text(*field, "required_domain", 32));
    BSM_TRY_ASSIGN(domain, parse_authority_domain(domain_text));
    requirement.has_required_domain = true;
    requirement.required_domain = domain;
  }
  if (const JsonValue* field = json::optional_field(value, "expect")) {
    requirement.has_expect = true;
    requirement.expect = *field;
  }
  return requirement;
}

[[nodiscard]] Result<JsonValue> requirement_to_json(const EvidenceRequirement& requirement) {
  JsonObjectBuilder root;
  root.set_text("kind", requirement.kind.str());
  root.set_text("subject", requirement.subject.str());
  root.set_uint("max_age_ticks", requirement.max_age_ticks);
  root.set_uint("min_sources", requirement.min_sources);
  if (requirement.has_required_owner) {
    root.set_text("required_owner", requirement.required_owner.str());
  }
  if (requirement.has_required_domain) {
    root.set_text("required_domain", to_string(requirement.required_domain));
  }
  if (requirement.has_expect) {
    root.set("expect", requirement.expect);
  }
  return root.build();
}

[[nodiscard]] Result<Unit> validate_requirement(const EvidenceRequirement& requirement) {
  if (requirement.kind.empty() || requirement.subject.empty()) {
    return Error(ErrorCode::PlanInvalid,
                 "evidence requirement must declare a kind and a subject");
  }
  if (requirement.min_sources == 0 ||
      requirement.min_sources > limits::kMaxRequiredSources) {
    return Error(ErrorCode::PlanInvalid,
                 "evidence requirement min_sources must be between 1 and " +
                     std::to_string(limits::kMaxRequiredSources));
  }
  if (requirement.max_age_ticks > limits::kMaxEvidenceAgeTicks) {
    return Error(ErrorCode::PlanInvalid,
                 "evidence requirement max_age_ticks exceeds the supported range");
  }
  if (requirement.has_required_owner && requirement.required_owner.empty()) {
    return Error(ErrorCode::PlanInvalid,
                 "evidence requirement required_owner is empty");
  }
  if (requirement.has_required_owner && !requirement.has_required_domain) {
    return Error(ErrorCode::PlanInvalid,
                 "evidence requirement names a required owner without its authority "
                 "domain");
  }
  if (requirement.has_expect && requirement.expect.is_null()) {
    return Error(ErrorCode::PlanInvalid,
                 "evidence requirement expectation must not be null; omit expect instead");
  }
  return Unit{};
}

[[nodiscard]] Result<RequestSpec> request_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "request specification must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(value, {"capability", "parameters"}));
  RequestSpec request;
  BSM_TRY_ASSIGN(capability_text, json::require_string(value, "capability"));
  BSM_TRY_ASSIGN(capability, CapabilityId::parse(capability_text));
  request.capability = std::move(capability);
  BSM_TRY_ASSIGN(parameters, json::require_field(value, "parameters"));
  if (!parameters->is_object()) {
    return Error(ErrorCode::TypeMismatch, "request parameters must be an object");
  }
  request.parameters = *parameters;
  return request;
}

[[nodiscard]] Result<JsonValue> request_to_json(const RequestSpec& request) {
  JsonObjectBuilder root;
  root.set_text("capability", request.capability.str());
  root.set("parameters", request.parameters);
  return root.build();
}

[[nodiscard]] Result<ObligationDefinition> obligation_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "obligation must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"id", "title", "stage", "priority", "depends_on", "owner_domain", "owner",
              "requires", "consequential", "request", "attempt_budget", "required"}));

  ObligationDefinition obligation;
  BSM_TRY_ASSIGN(id_text, json::require_string(value, "id"));
  BSM_TRY_ASSIGN(id, ObligationId::parse(id_text));
  obligation.id = std::move(id);
  if (const JsonValue* field = json::optional_field(value, "title")) {
    BSM_TRY_ASSIGN(title, json::require_text(*field, "obligation title", limits::kMaxTextLength));
    obligation.title = std::move(title);
  }
  BSM_TRY_ASSIGN(stage_text, json::require_string(value, "stage"));
  BSM_TRY_ASSIGN(stage, StageId::parse(stage_text));
  obligation.stage = std::move(stage);

  if (const JsonValue* field = json::optional_field(value, "priority")) {
    BSM_TRY_ASSIGN(priority_text, json::require_text(*field, "priority", 32));
    BSM_TRY_ASSIGN(priority, parse_priority(priority_text));
    obligation.priority = priority;
  }
  if (const JsonValue* field = json::optional_field(value, "depends_on")) {
    if (!field->is_array()) {
      return Error(ErrorCode::TypeMismatch, "depends_on must be an array");
    }
    if (field->as_array().size() > limits::kMaxDependenciesPerObligation) {
      return Error(ErrorCode::LimitExceeded, "obligation declares too many dependencies");
    }
    for (const JsonValue& entry : field->as_array()) {
      BSM_TRY_ASSIGN(dependency_text,
                     json::require_text(entry, "dependency identity",
                                        limits::kMaxIdentifierLength));
      BSM_TRY_ASSIGN(dependency, ObligationId::parse(dependency_text));
      obligation.depends_on.push_back(std::move(dependency));
    }
  }
  BSM_TRY_ASSIGN(owner_domain_text, json::require_string(value, "owner_domain"));
  BSM_TRY_ASSIGN(owner_domain, parse_authority_domain(owner_domain_text));
  obligation.owner_domain = owner_domain;
  BSM_TRY_ASSIGN(owner_text, json::require_string(value, "owner"));
  BSM_TRY_ASSIGN(owner, OwnerId::parse(owner_text));
  obligation.owner = std::move(owner);

  BSM_TRY_ASSIGN(required_evidence_value, json::require_array(value, "requires"));
  if (required_evidence_value->size() > limits::kMaxEvidenceRequirements) {
    return Error(ErrorCode::LimitExceeded,
                 "obligation declares too many evidence requirements");
  }
  obligation.required_evidence.reserve(required_evidence_value->size());
  for (const JsonValue& entry : *required_evidence_value) {
    BSM_TRY_ASSIGN(requirement, requirement_from_json(entry));
    obligation.required_evidence.push_back(std::move(requirement));
  }

  if (const JsonValue* field = json::optional_field(value, "consequential")) {
    if (!field->is_bool()) {
      return Error(ErrorCode::TypeMismatch, "consequential must be a boolean");
    }
    obligation.consequential = field->as_bool();
  }
  if (obligation.consequential) {
    BSM_TRY_ASSIGN(request_value, json::require_object_member(value, "request"));
    BSM_TRY_ASSIGN(request, request_from_json(*request_value));
    obligation.request = std::move(request);
  } else if (json::has_key(value, "request")) {
    return Error(ErrorCode::PlanInvalid,
                 "an evidence-only obligation must not declare a request");
  }
  if (const JsonValue* field = json::optional_field(value, "attempt_budget")) {
    if (!field->is_integer() || field->as_integer() < 0) {
      return Error(ErrorCode::TypeMismatch,
                   "attempt_budget must be a positive integer");
    }
    obligation.attempt_budget = static_cast<std::uint32_t>(field->as_integer());
  }
  if (const JsonValue* field = json::optional_field(value, "required")) {
    if (!field->is_bool()) {
      return Error(ErrorCode::TypeMismatch, "required must be a boolean");
    }
    obligation.required = field->as_bool();
  }
  return obligation;
}

[[nodiscard]] Result<JsonValue> obligation_to_json(const ObligationDefinition& obligation) {
  JsonObjectBuilder root;
  root.set_text("id", obligation.id.str());
  root.set_text("title", obligation.title);
  root.set_text("stage", obligation.stage.str());
  root.set_text("priority", to_string(obligation.priority));
  JsonArrayBuilder dependencies;
  for (const ObligationId& dependency : obligation.depends_on) {
    dependencies.push_text(dependency.str());
  }
  BSM_TRY_ASSIGN(dependencies_value, dependencies.build());
  root.set("depends_on", dependencies_value);
  root.set_text("owner_domain", to_string(obligation.owner_domain));
  root.set_text("owner", obligation.owner.str());
  JsonArrayBuilder required_evidence_array;
  for (const EvidenceRequirement& requirement : obligation.required_evidence) {
    BSM_TRY_ASSIGN(requirement_value, requirement_to_json(requirement));
    required_evidence_array.push(requirement_value);
  }
  BSM_TRY_ASSIGN(required_evidence_json, required_evidence_array.build());
  root.set("requires", required_evidence_json);
  root.set_bool("consequential", obligation.consequential);
  if (obligation.consequential) {
    BSM_TRY_ASSIGN(request_value, request_to_json(obligation.request));
    root.set("request", request_value);
  }
  root.set_uint("attempt_budget", obligation.attempt_budget);
  root.set_bool("required", obligation.required);
  return root.build();
}

[[nodiscard]] Result<StageDefinition> stage_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "stage must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"id", "rank", "title", "entry_evidence", "exit_evidence"}));

  StageDefinition stage;
  BSM_TRY_ASSIGN(id_text, json::require_string(value, "id"));
  BSM_TRY_ASSIGN(id, StageId::parse(id_text));
  stage.id = std::move(id);
  BSM_TRY_ASSIGN(rank, json::require_unsigned(value, "rank"));
  stage.rank = rank;
  if (const JsonValue* field = json::optional_field(value, "title")) {
    BSM_TRY_ASSIGN(title, json::require_text(*field, "stage title", limits::kMaxTextLength));
    stage.title = std::move(title);
  }
  for (const auto& [key, destination] :
       {std::pair<std::string_view, std::vector<EvidenceRequirement>*>{
            "entry_evidence", &stage.entry_evidence},
        std::pair<std::string_view, std::vector<EvidenceRequirement>*>{
            "exit_evidence", &stage.exit_evidence}}) {
    if (const JsonValue* field = json::optional_field(value, key)) {
      if (!field->is_array()) {
        return Error(ErrorCode::TypeMismatch,
                     std::string(key) + " must be an array of evidence requirements");
      }
      if (field->as_array().size() > limits::kMaxEvidenceRequirements) {
        return Error(ErrorCode::LimitExceeded,
                     std::string(key) + " declares too many evidence requirements");
      }
      destination->reserve(field->as_array().size());
      for (const JsonValue& entry : field->as_array()) {
        BSM_TRY_ASSIGN(requirement, requirement_from_json(entry));
        destination->push_back(std::move(requirement));
      }
    }
  }
  return stage;
}

[[nodiscard]] Result<JsonValue> stage_to_json(const StageDefinition& stage) {
  JsonObjectBuilder root;
  root.set_text("id", stage.id.str());
  root.set_uint("rank", stage.rank);
  root.set_text("title", stage.title);
  JsonArrayBuilder entry;
  for (const EvidenceRequirement& requirement : stage.entry_evidence) {
    BSM_TRY_ASSIGN(value, requirement_to_json(requirement));
    entry.push(value);
  }
  BSM_TRY_ASSIGN(entry_value, entry.build());
  root.set("entry_evidence", entry_value);
  JsonArrayBuilder exit;
  for (const EvidenceRequirement& requirement : stage.exit_evidence) {
    BSM_TRY_ASSIGN(value, requirement_to_json(requirement));
    exit.push(value);
  }
  BSM_TRY_ASSIGN(exit_value, exit.build());
  root.set("exit_evidence", exit_value);
  return root.build();
}

[[nodiscard]] Result<ReturnToServiceRequirement> return_to_service_from_json(
    const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "return_to_service must be an object");
  }
  BSM_RETURN_IF_ERROR(
      json::reject_unknown_fields(value, {"evidence", "required_obligations"}));
  ReturnToServiceRequirement requirement;
  BSM_TRY_ASSIGN(evidence, json::require_array(value, "evidence"));
  if (evidence->size() > limits::kMaxEvidenceRequirements) {
    return Error(ErrorCode::LimitExceeded,
                 "return_to_service declares too many evidence requirements");
  }
  requirement.evidence.reserve(evidence->size());
  for (const JsonValue& entry : *evidence) {
    BSM_TRY_ASSIGN(item, requirement_from_json(entry));
    requirement.evidence.push_back(std::move(item));
  }
  BSM_TRY_ASSIGN(obligations, json::require_array(value, "required_obligations"));
  if (obligations->size() > limits::kMaxObligations) {
    return Error(ErrorCode::LimitExceeded,
                 "return_to_service declares too many obligations");
  }
  requirement.required_obligations.reserve(obligations->size());
  for (const JsonValue& entry : *obligations) {
    BSM_TRY_ASSIGN(id_text,
                   json::require_text(entry, "required obligation identity",
                                      limits::kMaxIdentifierLength));
    BSM_TRY_ASSIGN(id, ObligationId::parse(id_text));
    requirement.required_obligations.push_back(std::move(id));
  }
  return requirement;
}

[[nodiscard]] Result<JsonValue> return_to_service_to_json(
    const ReturnToServiceRequirement& requirement) {
  JsonObjectBuilder root;
  JsonArrayBuilder evidence;
  for (const EvidenceRequirement& item : requirement.evidence) {
    BSM_TRY_ASSIGN(value, requirement_to_json(item));
    evidence.push(value);
  }
  BSM_TRY_ASSIGN(evidence_value, evidence.build());
  root.set("evidence", evidence_value);
  JsonArrayBuilder obligations;
  for (const ObligationId& id : requirement.required_obligations) {
    obligations.push_text(id.str());
  }
  BSM_TRY_ASSIGN(obligations_value, obligations.build());
  root.set("required_obligations", obligations_value);
  return root.build();
}

[[nodiscard]] Result<JsonValue> policy_to_json(const PlanPolicy& policy) {
  JsonObjectBuilder root;
  root.set_text("id", policy.id.str());
  root.set_uint("revision", policy.revision);
  root.set_uint("default_max_age_ticks", policy.default_max_age_ticks);
  root.set_uint("default_attempt_budget", policy.default_attempt_budget);
  root.set_bool("strict_within_stage_order", policy.strict_within_stage_order);
  return root.build();
}

[[nodiscard]] Result<PlanPolicy> policy_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "policy must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"id", "revision", "default_max_age_ticks", "default_attempt_budget",
              "strict_within_stage_order"}));
  PlanPolicy policy;
  BSM_TRY_ASSIGN(id_text, json::require_string(value, "id"));
  BSM_TRY_ASSIGN(id, PolicyTagId::parse(id_text));
  policy.id = std::move(id);
  BSM_TRY_ASSIGN(revision, json::require_unsigned(value, "revision"));
  policy.revision = revision;
  BSM_TRY_ASSIGN(max_age, json::require_unsigned(value, "default_max_age_ticks"));
  policy.default_max_age_ticks = max_age;
  BSM_TRY_ASSIGN(budget, json::require_unsigned(value, "default_attempt_budget"));
  if (budget > limits::kMaxAttemptBudget) {
    return Error(ErrorCode::NumberOutOfRange,
                 "default_attempt_budget exceeds the supported maximum");
  }
  policy.default_attempt_budget = static_cast<std::uint32_t>(budget);
  BSM_TRY_ASSIGN(strict, json::require_bool(value, "strict_within_stage_order"));
  policy.strict_within_stage_order = strict;
  return policy;
}

// Depth-first search over the obligation dependency graph. The first cycle found is
// reported with its exact path, so a plan author sees the contradiction rather than a
// generic failure.
[[nodiscard]] Result<Unit> detect_dependency_cycle(const PlanDocument& document) {
  const std::size_t count = document.obligations.size();
  const auto index_of = [&document, count](const ObligationId& id) -> std::size_t {
    for (std::size_t index = 0; index < count; ++index) {
      if (document.obligations[index].id == id) {
        return index;
      }
    }
    return count;
  };

  std::vector<std::vector<std::size_t>> adjacency(count);
  for (std::size_t index = 0; index < count; ++index) {
    for (const ObligationId& dependency : document.obligations[index].depends_on) {
      const std::size_t target = index_of(dependency);
      if (target >= count) {
        return Error(ErrorCode::PlanUnknownReference,
                     "obligation '" + document.obligations[index].id.str() +
                         "' depends on unknown obligation '" + dependency.str() + "'");
      }
      adjacency[index].push_back(target);
    }
  }

  struct Frame {
    std::size_t node = 0;
    std::size_t next = 0;
  };

  std::vector<int> colour(count, 0);  // 0 unvisited, 1 on stack, 2 complete
  std::vector<std::size_t> path;
  for (std::size_t start = 0; start < count; ++start) {
    if (colour[start] != 0) {
      continue;
    }
    std::vector<Frame> frames;
    frames.push_back(Frame{start, 0});
    colour[start] = 1;
    path.push_back(start);
    while (!frames.empty()) {
      Frame& frame = frames.back();
      if (frame.next < adjacency[frame.node].size()) {
        const std::size_t child = adjacency[frame.node][frame.next];
        ++frame.next;
        if (colour[child] == 1) {
          JsonArrayBuilder cycle;
          bool started = false;
          for (const std::size_t node : path) {
            if (node == child) {
              started = true;
            }
            if (started) {
              cycle.push_text(document.obligations[node].id.str());
            }
          }
          cycle.push_text(document.obligations[child].id.str());
          BSM_TRY_ASSIGN(cycle_value, cycle.build());
          JsonObjectBuilder detail;
          detail.set("cycle", cycle_value);
          BSM_TRY_ASSIGN(detail_value, detail.build());
          return Error(ErrorCode::PlanCycle,
                       "obligation dependency graph contains a cycle through '" +
                           document.obligations[child].id.str() + "'")
              .with_detail(detail_value);
        }
        if (colour[child] == 0) {
          colour[child] = 1;
          path.push_back(child);
          frames.push_back(Frame{child, 0});
        }
      } else {
        colour[frame.node] = 2;
        frames.pop_back();
        path.pop_back();
      }
    }
  }
  return Unit{};
}

[[nodiscard]] Result<Unit> normalize_obligation(ObligationDefinition& obligation) {
  std::sort(obligation.depends_on.begin(), obligation.depends_on.end());
  for (std::size_t index = 1; index < obligation.depends_on.size(); ++index) {
    if (obligation.depends_on[index - 1] == obligation.depends_on[index]) {
      return Error(ErrorCode::DuplicateIdentity,
                   "obligation '" + obligation.id.str() +
                       "' declares dependency '" + obligation.depends_on[index].str() +
                       "' more than once");
    }
  }
  std::sort(obligation.required_evidence.begin(), obligation.required_evidence.end(),
            [](const EvidenceRequirement& left, const EvidenceRequirement& right) {
              if (!(left.kind == right.kind)) {
                return left.kind < right.kind;
              }
              return left.subject < right.subject;
            });
  if (obligation.depends_on.size() > limits::kMaxDependenciesPerObligation) {
    return Error(ErrorCode::LimitExceeded,
                 "obligation '" + obligation.id.str() + "' declares too many dependencies");
  }
  return Unit{};
}

[[nodiscard]] Error conflict_error(std::string message, JsonValue detail) {
  return Error(ErrorCode::PlanStageOrderConflict, std::move(message))
      .with_detail(std::move(detail));
}

}  // namespace

bool operator==(const EvidenceRequirement& left, const EvidenceRequirement& right) noexcept {
  return left.kind == right.kind && left.subject == right.subject &&
         left.max_age_ticks == right.max_age_ticks &&
         left.min_sources == right.min_sources &&
         left.has_required_owner == right.has_required_owner &&
         (!left.has_required_owner || left.required_owner == right.required_owner) &&
         left.has_required_domain == right.has_required_domain &&
         (!left.has_required_domain || left.required_domain == right.required_domain) &&
         left.has_expect == right.has_expect &&
         (!left.has_expect || left.expect == right.expect);
}

bool operator==(const ObligationDefinition& left,
                const ObligationDefinition& right) noexcept {
  if (!(left.id == right.id) || left.title != right.title || !(left.stage == right.stage) ||
      left.priority != right.priority || left.depends_on != right.depends_on ||
      left.owner_domain != right.owner_domain || !(left.owner == right.owner) ||
      left.required_evidence != right.required_evidence ||
      left.consequential != right.consequential ||
      left.attempt_budget != right.attempt_budget || left.required != right.required) {
    return false;
  }
  if (left.consequential) {
    return left.request.capability == right.request.capability &&
           left.request.parameters == right.request.parameters;
  }
  return true;
}

Result<JsonValue> PlanDocument::to_json() const {
  JsonObjectBuilder root;
  root.set_uint("format_version", format_version);
  root.set_text("facility", facility.str());
  BSM_TRY_ASSIGN(policy_value, policy_to_json(policy));
  root.set("policy", policy_value);

  JsonArrayBuilder stages_array;
  for (const StageDefinition& stage : stages) {
    BSM_TRY_ASSIGN(value, stage_to_json(stage));
    stages_array.push(value);
  }
  BSM_TRY_ASSIGN(stages_value, stages_array.build());
  root.set("stages", stages_value);

  JsonArrayBuilder obligations_array;
  for (const ObligationDefinition& obligation : obligations) {
    BSM_TRY_ASSIGN(value, obligation_to_json(obligation));
    obligations_array.push(value);
  }
  BSM_TRY_ASSIGN(obligations_value, obligations_array.build());
  root.set("obligations", obligations_value);

  BSM_TRY_ASSIGN(rts_value, return_to_service_to_json(return_to_service));
  root.set("return_to_service", rts_value);
  return root.build();
}

Result<PlanDocument> PlanDocument::parse(std::string_view text) {
  BSM_TRY_ASSIGN(value, parse_json(text));
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "plan document must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"format_version", "facility", "policy", "stages", "obligations",
              "return_to_service"}));

  PlanDocument document;
  BSM_TRY_ASSIGN(version, json::require_unsigned(value, "format_version"));
  if (version != limits::kPlanFormatVersion) {
    return Error(ErrorCode::UnsupportedFormatVersion,
                 "plan format version " + std::to_string(version) +
                     " is not supported by this build");
  }
  document.format_version = static_cast<std::uint16_t>(version);
  BSM_TRY_ASSIGN(facility_text, json::require_string(value, "facility"));
  BSM_TRY_ASSIGN(facility, FacilityId::parse(facility_text));
  document.facility = std::move(facility);

  BSM_TRY_ASSIGN(policy_value, json::require_object_member(value, "policy"));
  BSM_TRY_ASSIGN(policy, policy_from_json(*policy_value));
  document.policy = std::move(policy);

  BSM_TRY_ASSIGN(stages, json::require_array(value, "stages"));
  if (stages->empty()) {
    return Error(ErrorCode::PlanEmpty, "plan declares no restoration stage");
  }
  if (stages->size() > limits::kMaxStages) {
    return Error(ErrorCode::LimitExceeded, "plan declares too many stages");
  }
  document.stages.reserve(stages->size());
  for (const JsonValue& entry : *stages) {
    BSM_TRY_ASSIGN(stage, stage_from_json(entry));
    document.stages.push_back(std::move(stage));
  }

  BSM_TRY_ASSIGN(obligations, json::require_array(value, "obligations"));
  if (obligations->size() > limits::kMaxObligations) {
    return Error(ErrorCode::LimitExceeded, "plan declares too many obligations");
  }
  document.obligations.reserve(obligations->size());
  for (const JsonValue& entry : *obligations) {
    BSM_TRY_ASSIGN(obligation, obligation_from_json(entry));
    document.obligations.push_back(std::move(obligation));
  }

  BSM_TRY_ASSIGN(rts_value, json::require_object_member(value, "return_to_service"));
  BSM_TRY_ASSIGN(rts, return_to_service_from_json(*rts_value));
  document.return_to_service = std::move(rts);
  return document;
}

Result<DerivedPlan> DerivedPlan::compile(PlanDocument document) {
  if (document.format_version != limits::kPlanFormatVersion) {
    return Error(ErrorCode::UnsupportedFormatVersion,
                 "plan format version is not supported by this build");
  }
  if (document.facility.empty()) {
    return Error(ErrorCode::PlanInvalid, "plan facility identity is empty");
  }
  if (document.stages.empty()) {
    return Error(ErrorCode::PlanEmpty, "plan declares no restoration stage");
  }
  if (document.stages.size() > limits::kMaxStages) {
    return Error(ErrorCode::LimitExceeded, "plan declares too many stages");
  }
  if (document.obligations.size() > limits::kMaxObligations) {
    return Error(ErrorCode::LimitExceeded, "plan declares too many obligations");
  }
  if (document.policy.id.empty()) {
    return Error(ErrorCode::PlanInvalid, "plan policy identity is empty");
  }
  if (document.policy.revision > limits::kMaxPolicyRevision) {
    return Error(ErrorCode::PlanInvalid, "plan policy revision exceeds the supported range");
  }
  if (document.policy.default_max_age_ticks > limits::kMaxEvidenceAgeTicks) {
    return Error(ErrorCode::PlanInvalid,
                 "plan policy default evidence age exceeds the supported range");
  }
  if (document.policy.default_attempt_budget == 0 ||
      document.policy.default_attempt_budget > limits::kMaxAttemptBudget) {
    return Error(ErrorCode::PlanInvalid,
                 "plan policy default attempt budget is outside the supported range");
  }

  // Unique identities.
  for (std::size_t index = 0; index < document.stages.size(); ++index) {
    if (document.stages[index].id.empty()) {
      return Error(ErrorCode::PlanInvalid, "stage identity is empty");
    }
    for (std::size_t other = index + 1; other < document.stages.size(); ++other) {
      if (document.stages[index].id == document.stages[other].id) {
        return Error(ErrorCode::DuplicateIdentity,
                     "stage '" + document.stages[index].id.str() +
                         "' is declared more than once");
      }
    }
    for (const EvidenceRequirement& requirement : document.stages[index].entry_evidence) {
      BSM_RETURN_IF_ERROR(validate_requirement(requirement));
    }
    for (const EvidenceRequirement& requirement : document.stages[index].exit_evidence) {
      BSM_RETURN_IF_ERROR(validate_requirement(requirement));
    }
  }
  for (std::size_t index = 0; index < document.obligations.size(); ++index) {
    if (document.obligations[index].id.empty()) {
      return Error(ErrorCode::PlanInvalid, "obligation identity is empty");
    }
    for (std::size_t other = index + 1; other < document.obligations.size(); ++other) {
      if (document.obligations[index].id == document.obligations[other].id) {
        return Error(ErrorCode::DuplicateIdentity,
                     "obligation '" + document.obligations[index].id.str() +
                         "' is declared more than once");
      }
    }
  }

  // Per-obligation validation and normalization.
  for (ObligationDefinition& obligation : document.obligations) {
    if (obligation.stage.empty()) {
      return Error(ErrorCode::PlanInvalid,
                   "obligation '" + obligation.id.str() + "' declares no stage");
    }
    bool stage_found = false;
    for (const StageDefinition& stage : document.stages) {
      if (stage.id == obligation.stage) {
        stage_found = true;
        break;
      }
    }
    if (!stage_found) {
      return Error(ErrorCode::PlanUnknownReference,
                   "obligation '" + obligation.id.str() + "' names unknown stage '" +
                       obligation.stage.str() + "'");
    }
    if (obligation.owner.empty()) {
      return Error(ErrorCode::PlanInvalid,
                   "obligation '" + obligation.id.str() + "' declares no owner");
    }
    if (obligation.title.size() > limits::kMaxTextLength) {
      return Error(ErrorCode::LimitExceeded,
                   "obligation '" + obligation.id.str() + "' title is too long");
    }
    if (obligation.required_evidence.empty()) {
      return Error(ErrorCode::PlanInvalid,
                   "obligation '" + obligation.id.str() +
                       "' declares no readiness evidence requirement");
    }
    if (obligation.required_evidence.size() > limits::kMaxEvidenceRequirements) {
      return Error(ErrorCode::LimitExceeded,
                   "obligation '" + obligation.id.str() +
                       "' declares too many evidence requirements");
    }
    for (const EvidenceRequirement& requirement : obligation.required_evidence) {
      BSM_RETURN_IF_ERROR(validate_requirement(requirement));
    }
    for (std::size_t index = 1; index < obligation.required_evidence.size(); ++index) {
      for (std::size_t other = 0; other < index; ++other) {
        if (obligation.required_evidence[other].kind == obligation.required_evidence[index].kind &&
            obligation.required_evidence[other].subject == obligation.required_evidence[index].subject) {
          return Error(ErrorCode::DuplicateIdentity,
                       "obligation '" + obligation.id.str() +
                           "' requires the same evidence kind and subject twice");
        }
      }
    }
    if (obligation.attempt_budget == 0 ||
        obligation.attempt_budget > limits::kMaxAttemptBudget) {
      return Error(ErrorCode::PlanInvalid,
                   "obligation '" + obligation.id.str() +
                       "' attempt budget is outside the supported range");
    }
    if (obligation.consequential) {
      if (obligation.request.capability.empty()) {
        return Error(ErrorCode::PlanInvalid,
                     "obligation '" + obligation.id.str() +
                         "' declares no capability to request");
      }
      if (!obligation.request.parameters.is_object()) {
        return Error(ErrorCode::PlanInvalid,
                     "obligation '" + obligation.id.str() +
                         "' request parameters must be a canonical object");
      }
    }
    BSM_RETURN_IF_ERROR(normalize_obligation(obligation));
  }

  // Dependency references and cycles.
  for (std::size_t index = 0; index < document.obligations.size(); ++index) {
    const ObligationDefinition& obligation = document.obligations[index];
    for (const ObligationId& dependency : obligation.depends_on) {
      if (dependency == obligation.id) {
        return Error(ErrorCode::PlanCycle,
                     "obligation '" + obligation.id.str() + "' depends on itself");
      }
      bool found = false;
      for (const ObligationDefinition& candidate : document.obligations) {
        if (candidate.id == dependency) {
          found = true;
          break;
        }
      }
      if (!found) {
        return Error(ErrorCode::PlanUnknownReference,
                     "obligation '" + obligation.id.str() +
                         "' depends on unknown obligation '" + dependency.str() + "'");
      }
    }
  }
  BSM_RETURN_IF_ERROR(detect_dependency_cycle(document));

  // Derived stage order: declared rank first, then identity. The dependency graph
  // must agree with that order; a contradiction is refused rather than reordered.
  std::vector<std::size_t> stage_order(document.stages.size());
  for (std::size_t index = 0; index < stage_order.size(); ++index) {
    stage_order[index] = index;
  }
  std::sort(stage_order.begin(), stage_order.end(),
            [&document](std::size_t left, std::size_t right) {
              const StageDefinition& first = document.stages[left];
              const StageDefinition& second = document.stages[right];
              if (first.rank != second.rank) {
                return first.rank < second.rank;
              }
              return first.id < second.id;
            });
  std::vector<std::size_t> stage_position(document.stages.size(), 0);
  for (std::size_t position = 0; position < stage_order.size(); ++position) {
    stage_position[stage_order[position]] = position;
  }
  const auto stage_position_of = [&document, &stage_position](
                                     const StageId& id) -> std::size_t {
    for (std::size_t index = 0; index < document.stages.size(); ++index) {
      if (document.stages[index].id == id) {
        return stage_position[index];
      }
    }
    return document.stages.size();
  };
  const auto obligation_index_of = [&document](const ObligationId& id) -> std::size_t {
    for (std::size_t index = 0; index < document.obligations.size(); ++index) {
      if (document.obligations[index].id == id) {
        return index;
      }
    }
    return document.obligations.size();
  };
  const auto rank_of_stage = [&document](const StageId& id) -> std::uint64_t {
    for (const StageDefinition& stage : document.stages) {
      if (stage.id == id) {
        return stage.rank;
      }
    }
    return 0;
  };

  for (const ObligationDefinition& obligation : document.obligations) {
    const std::size_t dependent_position = stage_position_of(obligation.stage);
    for (const ObligationId& dependency : obligation.depends_on) {
      const ObligationDefinition& source = document.obligations[obligation_index_of(dependency)];
      const std::size_t source_position = stage_position_of(source.stage);
      if (source_position > dependent_position) {
        JsonObjectBuilder detail;
        detail.set_text("dependency", source.id.str());
        detail.set_text("dependency_stage", source.stage.str());
        detail.set_uint("dependency_rank", rank_of_stage(source.stage));
        detail.set_text("dependent", obligation.id.str());
        detail.set_text("dependent_stage", obligation.stage.str());
        detail.set_uint("dependent_rank", rank_of_stage(obligation.stage));
        BSM_TRY_ASSIGN(detail_value, detail.build());
        return conflict_error(
            "obligation '" + obligation.id.str() + "' depends on '" + source.id.str() +
                "', which the declared stage ranks place later",
            detail_value);
      }
    }
  }

  // Derived obligation order: stages in derived order, then priority, then
  // declaration order, with identity as the final deterministic tie-break.
  std::vector<std::size_t> obligation_order(document.obligations.size());
  for (std::size_t index = 0; index < obligation_order.size(); ++index) {
    obligation_order[index] = index;
  }
  std::sort(obligation_order.begin(), obligation_order.end(),
            [&document, &stage_position_of](std::size_t left, std::size_t right) {
              const ObligationDefinition& first = document.obligations[left];
              const ObligationDefinition& second = document.obligations[right];
              const std::size_t first_stage = stage_position_of(first.stage);
              const std::size_t second_stage = stage_position_of(second.stage);
              if (first_stage != second_stage) {
                return first_stage < second_stage;
              }
              if (first.priority != second.priority) {
                return static_cast<int>(first.priority) < static_cast<int>(second.priority);
              }
              return left < right;
            });
  std::vector<std::size_t> obligation_position(document.obligations.size(), 0);
  for (std::size_t position = 0; position < obligation_order.size(); ++position) {
    obligation_position[obligation_order[position]] = position;
  }

  // Within one stage, a dependency must be ordered before its dependent, or the
  // strict in-stage order would deadlock deterministically.
  for (std::size_t index = 0; index < document.obligations.size(); ++index) {
    const ObligationDefinition& obligation = document.obligations[index];
    for (const ObligationId& dependency : obligation.depends_on) {
      const std::size_t dependency_index = obligation_index_of(dependency);
      if (document.obligations[dependency_index].stage == obligation.stage &&
          obligation_position[dependency_index] >= obligation_position[index]) {
        JsonObjectBuilder detail;
        detail.set_text("dependency", dependency.str());
        detail.set_text("dependent", obligation.id.str());
        detail.set_text("stage", obligation.stage.str());
        BSM_TRY_ASSIGN(detail_value, detail.build());
        return conflict_error(
            "obligation '" + obligation.id.str() + "' depends on '" + dependency.str() +
                "' inside the same stage, but the derived in-stage order places it later",
            detail_value);
      }
    }
  }

  DerivedPlan plan;
  // The document is copied rather than moved: the helpers above hold references to it, and
  // a moved-from document would silently make every stage lookup succeed with an empty
  // list, assigning obligations to the wrong stage.
  plan.document_ = document;

  plan.stages_.reserve(plan.document_.stages.size());
  for (std::size_t position = 0; position < stage_order.size(); ++position) {
    const StageDefinition& source = plan.document_.stages[stage_order[position]];
    CompiledStage stage;
    stage.id = source.id;
    stage.rank = source.rank;
    stage.index = position;
    stage.title = source.title;
    plan.stages_.push_back(std::move(stage));
  }

  plan.obligations_.reserve(plan.document_.obligations.size());
  plan.obligation_digests_.reserve(plan.document_.obligations.size());
  for (std::size_t position = 0; position < obligation_order.size(); ++position) {
    const ObligationDefinition& source = plan.document_.obligations[obligation_order[position]];
    BSM_TRY_ASSIGN(definition_value, obligation_to_json(source));
    plan.obligation_digests_.push_back(Digest::of(canonical_json(definition_value)));
    plan.obligations_.push_back(source);

    const std::size_t stage_index = stage_position_of(source.stage);
    plan.stages_[stage_index].obligation_indices.push_back(position);
  }

  for (const ObligationDefinition& obligation : plan.obligations_) {
    for (const ObligationId& dependency : obligation.depends_on) {
      const ObligationDefinition* source = plan.obligation(dependency);
      if (source == nullptr) {
        return Error(ErrorCode::InternalError,
                     "compiled plan lost a dependency it validated");
      }
      if (source->stage == obligation.stage) {
        continue;
      }
      CompiledStage& stage = plan.stages_[plan.stage_index(obligation.stage)];
      if (std::find(stage.depends_on_stages.begin(), stage.depends_on_stages.end(),
                    source->stage) == stage.depends_on_stages.end()) {
        stage.depends_on_stages.push_back(source->stage);
      }
    }
  }
  for (CompiledStage& stage : plan.stages_) {
    std::sort(stage.depends_on_stages.begin(), stage.depends_on_stages.end());
  }

  for (const ObligationId& id : plan.document_.return_to_service.required_obligations) {
    if (!plan.has_obligation(id)) {
      return Error(ErrorCode::PlanUnknownReference,
                   "return_to_service names unknown obligation '" + id.str() + "'");
    }
  }
  for (const EvidenceRequirement& requirement :
       plan.document_.return_to_service.evidence) {
    BSM_RETURN_IF_ERROR(validate_requirement(requirement));
  }
  if (plan.document_.return_to_service.evidence.empty() &&
      plan.document_.return_to_service.required_obligations.empty()) {
    return Error(ErrorCode::PlanInvalid,
                 "return_to_service declares no proof obligation");
  }

  BSM_TRY_ASSIGN(derived_value, plan.to_json());
  plan.digest_ = Digest::of(canonical_json(derived_value));
  return plan;
}

Result<JsonValue> DerivedPlan::to_json() const {
  JsonObjectBuilder root;
  root.set_uint("format_version", limits::kPlanFormatVersion);
  root.set_text("facility", document_.facility.str());
  BSM_TRY_ASSIGN(policy_value, policy_to_json(document_.policy));
  root.set("policy", policy_value);

  JsonArrayBuilder stages;
  for (const CompiledStage& stage : stages_) {
    const StageDefinition* source = nullptr;
    for (const StageDefinition& candidate : document_.stages) {
      if (candidate.id == stage.id) {
        source = &candidate;
        break;
      }
    }
    if (source == nullptr) {
      return Error(ErrorCode::InternalError, "compiled stage is missing from the document");
    }
    JsonObjectBuilder entry;
    entry.set_text("id", stage.id.str());
    entry.set_uint("rank", stage.rank);
    entry.set_uint("index", static_cast<std::uint64_t>(stage.index));
    entry.set_text("title", stage.title);

    std::size_t declaration_index = 0;
    for (std::size_t index = 0; index < document_.stages.size(); ++index) {
      if (document_.stages[index].id == stage.id) {
        declaration_index = index;
        break;
      }
    }
    entry.set_uint("declaration_index", static_cast<std::uint64_t>(declaration_index));
    entry.set_uint("declaration_rank", source->rank);

    JsonArrayBuilder dependencies;
    for (const StageId& dependency : stage.depends_on_stages) {
      dependencies.push_text(dependency.str());
    }
    BSM_TRY_ASSIGN(dependencies_value, dependencies.build());
    entry.set("depends_on_stages", dependencies_value);

    JsonArrayBuilder obligations;
    for (const std::size_t obligation : stage.obligation_indices) {
      obligations.push_text(obligations_[obligation].id.str());
    }
    BSM_TRY_ASSIGN(obligations_value, obligations.build());
    entry.set("obligations", obligations_value);

    JsonArrayBuilder entry_evidence;
    for (const EvidenceRequirement& requirement : source->entry_evidence) {
      BSM_TRY_ASSIGN(value, requirement_to_json(requirement));
      entry_evidence.push(value);
    }
    BSM_TRY_ASSIGN(entry_evidence_value, entry_evidence.build());
    entry.set("entry_evidence", entry_evidence_value);

    JsonArrayBuilder exit_evidence;
    for (const EvidenceRequirement& requirement : source->exit_evidence) {
      BSM_TRY_ASSIGN(value, requirement_to_json(requirement));
      exit_evidence.push(value);
    }
    BSM_TRY_ASSIGN(exit_evidence_value, exit_evidence.build());
    entry.set("exit_evidence", exit_evidence_value);
    BSM_TRY_ASSIGN(stage_value, entry.build());
    stages.push(stage_value);
  }
  BSM_TRY_ASSIGN(stages_value, stages.build());
  root.set("stages", stages_value);

  JsonArrayBuilder obligations;
  for (std::size_t position = 0; position < obligations_.size(); ++position) {
    std::size_t declaration_index = 0;
    for (std::size_t index = 0; index < document_.obligations.size(); ++index) {
      if (document_.obligations[index].id == obligations_[position].id) {
        declaration_index = index;
        break;
      }
    }
    JsonObjectBuilder entry;
    entry.set_uint("declaration_index", static_cast<std::uint64_t>(declaration_index));
    entry.set_uint("index", static_cast<std::uint64_t>(position));
    entry.set_text("digest", obligation_digests_[position].hex());
    BSM_TRY_ASSIGN(definition_value, obligation_to_json(obligations_[position]));
    entry.set("definition", definition_value);
    BSM_TRY_ASSIGN(obligation_value, entry.build());
    obligations.push(obligation_value);
  }
  BSM_TRY_ASSIGN(obligations_value, obligations.build());
  root.set("obligations", obligations_value);

  BSM_TRY_ASSIGN(rts_value, return_to_service_to_json(document_.return_to_service));
  root.set("return_to_service", rts_value);
  return root.build();
}

Result<DerivedPlan> DerivedPlan::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "derived plan must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"format_version", "facility", "policy", "stages", "obligations",
              "return_to_service"}));
  BSM_TRY_ASSIGN(version, json::require_unsigned(value, "format_version"));
  if (version != limits::kPlanFormatVersion) {
    return Error(ErrorCode::UnsupportedFormatVersion,
                 "plan format version " + std::to_string(version) + " is not supported");
  }

  PlanDocument document;
  document.format_version = static_cast<std::uint16_t>(version);
  BSM_TRY_ASSIGN(facility_text, json::require_string(value, "facility"));
  BSM_TRY_ASSIGN(facility, FacilityId::parse(facility_text));
  document.facility = std::move(facility);
  BSM_TRY_ASSIGN(policy_value, json::require_object_member(value, "policy"));
  BSM_TRY_ASSIGN(policy, policy_from_json(*policy_value));
  document.policy = std::move(policy);

  struct StageEntry {
    StageDefinition definition;
    std::size_t declaration_index = 0;
    std::size_t derived_index = 0;
  };
  BSM_TRY_ASSIGN(stages, json::require_array(value, "stages"));
  std::vector<StageEntry> stage_entries;
  stage_entries.reserve(stages->size());
  for (const JsonValue& entry : *stages) {
    if (!entry.is_object()) {
      return Error(ErrorCode::TypeMismatch, "derived plan stage must be an object");
    }
    BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
        entry, {"id", "rank", "index", "title", "declaration_index", "declaration_rank",
                "depends_on_stages", "obligations", "entry_evidence", "exit_evidence"}));
    StageEntry record;
    BSM_TRY_ASSIGN(id_text, json::require_string(entry, "id"));
    BSM_TRY_ASSIGN(id, StageId::parse(id_text));
    record.definition.id = std::move(id);
    BSM_TRY_ASSIGN(rank, json::require_unsigned(entry, "declaration_rank"));
    record.definition.rank = rank;
    BSM_TRY_ASSIGN(stage_index, json::require_unsigned(entry, "index"));
    record.derived_index = static_cast<std::size_t>(stage_index);
    BSM_TRY_ASSIGN(declaration_index, json::require_unsigned(entry, "declaration_index"));
    record.declaration_index = static_cast<std::size_t>(declaration_index);
    BSM_TRY_ASSIGN(title, json::require_string(entry, "title"));
    record.definition.title = std::move(title);
    for (const auto& [key, destination] :
         {std::pair<std::string_view, std::vector<EvidenceRequirement>*>{
              "entry_evidence", &record.definition.entry_evidence},
          std::pair<std::string_view, std::vector<EvidenceRequirement>*>{
              "exit_evidence", &record.definition.exit_evidence}}) {
      BSM_TRY_ASSIGN(array, json::require_array(entry, key));
      destination->reserve(array->size());
      for (const JsonValue& item : *array) {
        BSM_TRY_ASSIGN(requirement, requirement_from_json(item));
        destination->push_back(std::move(requirement));
      }
    }
    stage_entries.push_back(std::move(record));
  }

  struct ObligationEntry {
    ObligationDefinition definition;
    std::size_t declaration_index = 0;
    std::size_t derived_index = 0;
    Digest digest;
  };
  BSM_TRY_ASSIGN(obligations, json::require_array(value, "obligations"));
  std::vector<ObligationEntry> obligation_entries;
  obligation_entries.reserve(obligations->size());
  for (const JsonValue& entry : *obligations) {
    if (!entry.is_object()) {
      return Error(ErrorCode::TypeMismatch, "derived plan obligation must be an object");
    }
    BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
        entry, {"declaration_index", "index", "digest", "definition"}));
    ObligationEntry record;
    BSM_TRY_ASSIGN(declaration_index, json::require_unsigned(entry, "declaration_index"));
    record.declaration_index = static_cast<std::size_t>(declaration_index);
    BSM_TRY_ASSIGN(derived_index, json::require_unsigned(entry, "index"));
    record.derived_index = static_cast<std::size_t>(derived_index);
    BSM_TRY_ASSIGN(digest_text, json::require_string(entry, "digest"));
    BSM_TRY_ASSIGN(digest, Digest::from_hex(digest_text));
    record.digest = digest;
    BSM_TRY_ASSIGN(definition_value, json::require_object_member(entry, "definition"));
    BSM_TRY_ASSIGN(definition, obligation_from_json(*definition_value));
    record.definition = std::move(definition);
    obligation_entries.push_back(std::move(record));
  }

  BSM_TRY_ASSIGN(rts_value, json::require_object_member(value, "return_to_service"));
  BSM_TRY_ASSIGN(rts, return_to_service_from_json(*rts_value));
  document.return_to_service = std::move(rts);

  // Rebuild the declared document order, then compile it again. Compilation is
  // deterministic, so a plan that was not tampered with reproduces the same derived
  // order and the same digests.
  std::sort(stage_entries.begin(), stage_entries.end(),
            [](const StageEntry& left, const StageEntry& right) {
              return left.declaration_index < right.declaration_index;
            });
  for (const StageEntry& entry : stage_entries) {
    if (entry.declaration_index != document.stages.size()) {
      return Error(ErrorCode::SchemaViolation,
                   "derived plan stage declaration indices are not a permutation");
    }
    // The entry stays intact: the consistency check below reads its identity and derived
    // index, and a moved-from entry would silently look like a mismatch.
    document.stages.push_back(entry.definition);
  }
  std::sort(obligation_entries.begin(), obligation_entries.end(),
            [](const ObligationEntry& left, const ObligationEntry& right) {
              return left.declaration_index < right.declaration_index;
            });
  for (const ObligationEntry& entry : obligation_entries) {
    if (entry.declaration_index != document.obligations.size()) {
      return Error(ErrorCode::SchemaViolation,
                   "derived plan obligation declaration indices are not a permutation");
    }
    document.obligations.push_back(entry.definition);
  }

  BSM_TRY_ASSIGN(plan, DerivedPlan::compile(std::move(document)));
  if (plan.stages_.size() != stage_entries.size() ||
      plan.obligations_.size() != obligation_entries.size()) {
    return Error(ErrorCode::IntegrityMismatch,
                 "derived plan shape does not match the compiled document");
  }
  for (std::size_t index = 0; index < plan.stages_.size(); ++index) {
    std::size_t expected = 0;
    for (const StageEntry& entry : stage_entries) {
      if (entry.definition.id == plan.stages_[index].id) {
        expected = entry.derived_index;
        break;
      }
    }
    if (expected != index) {
      return Error(ErrorCode::IntegrityMismatch,
                   "derived plan stage order does not match the compiled document");
    }
  }
  for (std::size_t index = 0; index < plan.obligations_.size(); ++index) {
    const Digest expected_digest = plan.obligation_digests_[index];
    Digest stored;
    std::size_t derived = 0;
    bool found = false;
    for (const ObligationEntry& entry : obligation_entries) {
      if (entry.definition.id == plan.obligations_[index].id) {
        stored = entry.digest;
        derived = entry.derived_index;
        found = true;
        break;
      }
    }
    if (!found || derived != index || !(stored == expected_digest)) {
      return Error(ErrorCode::IntegrityMismatch,
                   "derived plan obligation order or digest does not match the compiled "
                   "document");
    }
  }
  return plan;
}

std::size_t DerivedPlan::stage_index(const StageId& id) const noexcept {
  for (std::size_t index = 0; index < stages_.size(); ++index) {
    if (stages_[index].id == id) {
      return index;
    }
  }
  return stages_.size();
}

std::size_t DerivedPlan::obligation_index(const ObligationId& id) const noexcept {
  for (std::size_t index = 0; index < obligations_.size(); ++index) {
    if (obligations_[index].id == id) {
      return index;
    }
  }
  return obligations_.size();
}

bool DerivedPlan::has_obligation(const ObligationId& id) const noexcept {
  return obligation_index(id) != obligations_.size();
}

const ObligationDefinition* DerivedPlan::obligation(const ObligationId& id) const noexcept {
  const std::size_t index = obligation_index(id);
  if (index == obligations_.size()) {
    return nullptr;
  }
  return &obligations_[index];
}

Digest DerivedPlan::obligation_digest(const ObligationId& id) const {
  const std::size_t index = obligation_index(id);
  if (index == obligations_.size()) {
    return Digest();
  }
  return obligation_digests_[index];
}

}  // namespace black_start_manager
