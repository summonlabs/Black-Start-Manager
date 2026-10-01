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

#include "black_start_manager/authority.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "detail/model_json.hpp"

namespace black_start_manager {
namespace detail {

[[nodiscard]] Result<AuthorityRef> authority_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "authority entry must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"id", "domain", "owner", "generation", "attestation"}));

  AuthorityRef authority;
  BSM_TRY_ASSIGN(id_text, json::require_string(value, "id"));
  BSM_TRY_ASSIGN(id, AuthorityId::parse(id_text));
  authority.id = std::move(id);
  BSM_TRY_ASSIGN(domain_text, json::require_string(value, "domain"));
  BSM_TRY_ASSIGN(domain, parse_authority_domain(domain_text));
  authority.domain = domain;
  BSM_TRY_ASSIGN(owner_text, json::require_string(value, "owner"));
  BSM_TRY_ASSIGN(owner, OwnerId::parse(owner_text));
  authority.owner = std::move(owner);
  BSM_TRY_ASSIGN(generation, json::require_unsigned(value, "generation"));
  authority.generation = generation;
  BSM_TRY_ASSIGN(attestation_text, json::require_string(value, "attestation"));
  BSM_TRY_ASSIGN(attestation, Digest::from_hex(attestation_text));
  authority.attestation = attestation;
  return authority;
}

Result<JsonValue> authority_to_json(const AuthorityRef& authority) {
  JsonObjectBuilder entry;
  entry.set_text("id", authority.id.str());
  entry.set_text("domain", to_string(authority.domain));
  entry.set_text("owner", authority.owner.str());
  entry.set_uint("generation", authority.generation);
  entry.set_text("attestation", authority.attestation.hex());
  return entry.build();
}

}  // namespace detail

namespace {

[[nodiscard]] Result<Unit> check_generation(std::uint64_t generation, const char* what) {
  if (generation > limits::kMaxGeneration) {
    return Error(ErrorCode::NumberOutOfRange,
                 std::string(what) + " exceeds the supported generation range");
  }
  return Unit{};
}

}  // namespace

bool operator==(const AuthorityRef& left, const AuthorityRef& right) noexcept {
  return left.id == right.id && left.domain == right.domain && left.owner == right.owner &&
         left.generation == right.generation && left.attestation == right.attestation;
}

bool operator==(const IncidentRef& left, const IncidentRef& right) noexcept {
  return left.id == right.id && left.generation == right.generation;
}

bool BindingDelta::contradictory() const noexcept {
  for (const BindingChange& change : changes) {
    switch (change.code) {
      case BindingChangeCode::FacilityChanged:
      case BindingChangeCode::EpochChanged:
      case BindingChangeCode::TopologyChanged:
      case BindingChangeCode::PolicyChanged:
      case BindingChangeCode::IncidentChanged:
      case BindingChangeCode::AuthorityRemoved:
      case BindingChangeCode::AuthorityOwnerChanged:
      case BindingChangeCode::AuthorityGenerationStale:
      case BindingChangeCode::AuthorityAttestationChanged:
      case BindingChangeCode::PrerequisiteRemoved:
      case BindingChangeCode::PrerequisiteGenerationStale:
        return true;
      case BindingChangeCode::IncidentGenerationChanged:
      case BindingChangeCode::AuthorityAdded:
      case BindingChangeCode::AuthorityGenerationAdvanced:
      case BindingChangeCode::PrerequisiteAdded:
      case BindingChangeCode::PrerequisiteGenerationAdvanced:
        break;
    }
  }
  return false;
}

bool BindingDelta::reestablishable() const noexcept {
  return !changes.empty() && !contradictory();
}

Result<Unit> normalize_binding(FacilityBinding& binding) {
  std::sort(binding.authorities.begin(), binding.authorities.end(),
            [](const AuthorityRef& left, const AuthorityRef& right) {
              if (left.domain != right.domain) {
                return static_cast<int>(left.domain) < static_cast<int>(right.domain);
              }
              return left.owner < right.owner;
            });
  for (std::size_t index = 1; index < binding.authorities.size(); ++index) {
    if (binding.authorities[index - 1].domain == binding.authorities[index].domain &&
        binding.authorities[index - 1].owner == binding.authorities[index].owner) {
      return Error(ErrorCode::DuplicateIdentity,
                   "binding declares owner '" + binding.authorities[index].owner.str() +
                       "' twice for domain " +
                       to_string(binding.authorities[index].domain));
    }
  }
  std::sort(binding.prerequisite_generations.begin(),
            binding.prerequisite_generations.end(),
            [](const std::pair<ObligationId, std::uint64_t>& left,
               const std::pair<ObligationId, std::uint64_t>& right) {
              return left.first < right.first;
            });
  for (std::size_t index = 1; index < binding.prerequisite_generations.size(); ++index) {
    if (binding.prerequisite_generations[index - 1].first ==
        binding.prerequisite_generations[index].first) {
      return Error(ErrorCode::DuplicateIdentity,
                   "binding declares prerequisite generation for obligation '" +
                       binding.prerequisite_generations[index].first.str() +
                       "' more than once");
    }
  }
  return Unit{};
}

Result<Unit> validate_binding(const FacilityBinding& binding) {
  if (binding.facility.empty()) {
    return Error(ErrorCode::InvalidArgument, "binding facility identity is empty");
  }
  BSM_RETURN_IF_ERROR(check_generation(binding.facility_epoch, "facility epoch"));
  if (binding.topology_digest.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "binding topology digest is absent");
  }
  if (binding.policy.empty()) {
    return Error(ErrorCode::InvalidArgument, "binding policy identity is empty");
  }
  if (binding.policy_revision > limits::kMaxPolicyRevision) {
    return Error(ErrorCode::NumberOutOfRange,
                 "binding policy revision exceeds the supported range");
  }
  if (binding.incident.id.empty()) {
    return Error(ErrorCode::InvalidArgument, "binding incident identity is empty");
  }
  BSM_RETURN_IF_ERROR(check_generation(binding.incident.generation, "incident generation"));
  if (binding.authorities.size() > limits::kMaxControllers) {
    return Error(ErrorCode::LimitExceeded, "binding declares too many authorities");
  }
  for (std::size_t index = 0; index < binding.authorities.size(); ++index) {
    const AuthorityRef& authority = binding.authorities[index];
    if (authority.id.empty() || authority.owner.empty()) {
      return Error(ErrorCode::InvalidArgument,
                   "binding authority identity or owner is empty");
    }
    BSM_RETURN_IF_ERROR(check_generation(authority.generation, "authority generation"));
    if (authority.attestation.is_zero()) {
      return Error(ErrorCode::AuthorityUnattested,
                   "binding authority '" + authority.owner.str() +
                       "' carries no attestation digest");
    }
    if (index > 0) {
      const AuthorityRef& previous = binding.authorities[index - 1];
      if (static_cast<int>(previous.domain) > static_cast<int>(authority.domain) ||
          (previous.domain == authority.domain && !(previous.owner < authority.owner))) {
        return Error(ErrorCode::InvalidArgument,
                     "binding authorities are not in canonical order");
      }
    }
  }
  if (binding.prerequisite_generations.size() > limits::kMaxObligations) {
    return Error(ErrorCode::LimitExceeded,
                 "binding declares too many prerequisite generations");
  }
  for (std::size_t index = 0; index < binding.prerequisite_generations.size(); ++index) {
    const auto& entry = binding.prerequisite_generations[index];
    if (entry.first.empty()) {
      return Error(ErrorCode::InvalidArgument, "prerequisite obligation identity is empty");
    }
    BSM_RETURN_IF_ERROR(check_generation(entry.second, "prerequisite generation"));
    if (index > 0 && !(binding.prerequisite_generations[index - 1].first < entry.first)) {
      return Error(ErrorCode::InvalidArgument,
                   "binding prerequisite generations are not in canonical order");
    }
  }
  return Unit{};
}

Result<JsonValue> binding_to_json(const FacilityBinding& binding) {
  BSM_RETURN_IF_ERROR(validate_binding(binding));

  JsonObjectBuilder root;
  root.set_text("facility", binding.facility.str());
  root.set_uint("facility_epoch", binding.facility_epoch);
  root.set_text("topology_digest", binding.topology_digest.hex());
  root.set_text("policy", binding.policy.str());
  root.set_uint("policy_revision", binding.policy_revision);

  JsonObjectBuilder incident;
  incident.set_text("id", binding.incident.id.str());
  incident.set_uint("generation", binding.incident.generation);
  BSM_TRY_ASSIGN(incident_value, incident.build());
  root.set("incident", incident_value);

  JsonArrayBuilder authorities;
  for (const AuthorityRef& authority : binding.authorities) {
    BSM_TRY_ASSIGN(entry_value, detail::authority_to_json(authority));
    authorities.push(entry_value);
  }
  BSM_TRY_ASSIGN(authorities_value, authorities.build());
  root.set("authorities", authorities_value);

  JsonArrayBuilder prerequisites;
  for (const auto& entry : binding.prerequisite_generations) {
    JsonObjectBuilder item;
    item.set_text("obligation", entry.first.str());
    item.set_uint("generation", entry.second);
    BSM_TRY_ASSIGN(item_value, item.build());
    prerequisites.push(item_value);
  }
  BSM_TRY_ASSIGN(prerequisites_value, prerequisites.build());
  root.set("prerequisite_generations", prerequisites_value);

  return root.build();
}

Result<FacilityBinding> binding_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "binding must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value,
      {"facility", "facility_epoch", "topology_digest", "policy", "policy_revision",
       "incident", "authorities", "prerequisite_generations"}));

  FacilityBinding binding;
  BSM_TRY_ASSIGN(facility_text, json::require_string(value, "facility"));
  BSM_TRY_ASSIGN(facility, FacilityId::parse(facility_text));
  binding.facility = std::move(facility);
  BSM_TRY_ASSIGN(epoch, json::require_unsigned(value, "facility_epoch"));
  binding.facility_epoch = epoch;
  BSM_TRY_ASSIGN(topology_text, json::require_string(value, "topology_digest"));
  BSM_TRY_ASSIGN(topology, Digest::from_hex(topology_text));
  binding.topology_digest = topology;
  BSM_TRY_ASSIGN(policy_text, json::require_string(value, "policy"));
  BSM_TRY_ASSIGN(policy, PolicyTagId::parse(policy_text));
  binding.policy = std::move(policy);
  BSM_TRY_ASSIGN(revision, json::require_unsigned(value, "policy_revision"));
  binding.policy_revision = revision;

  BSM_TRY_ASSIGN(incident_value, json::require_object_member(value, "incident"));
  BSM_RETURN_IF_ERROR(
      json::reject_unknown_fields(*incident_value, {"id", "generation"}));
  BSM_TRY_ASSIGN(incident_id_text, json::require_string(*incident_value, "id"));
  BSM_TRY_ASSIGN(incident_id, IncidentId::parse(incident_id_text));
  binding.incident.id = std::move(incident_id);
  BSM_TRY_ASSIGN(incident_generation,
                 json::require_unsigned(*incident_value, "generation"));
  binding.incident.generation = incident_generation;

  BSM_TRY_ASSIGN(authorities, json::require_array(value, "authorities"));
  if (authorities->size() > limits::kMaxControllers) {
    return Error(ErrorCode::LimitExceeded, "binding declares too many authorities");
  }
  binding.authorities.reserve(authorities->size());
  for (const JsonValue& entry : *authorities) {
    BSM_TRY_ASSIGN(authority, detail::authority_from_json(entry));
    binding.authorities.push_back(std::move(authority));
  }

  BSM_TRY_ASSIGN(prerequisites, json::require_array(value, "prerequisite_generations"));
  if (prerequisites->size() > limits::kMaxObligations) {
    return Error(ErrorCode::LimitExceeded,
                 "binding declares too many prerequisite generations");
  }
  binding.prerequisite_generations.reserve(prerequisites->size());
  for (const JsonValue& entry : *prerequisites) {
    if (!entry.is_object()) {
      return Error(ErrorCode::TypeMismatch,
                   "prerequisite generation entry must be an object");
    }
    BSM_RETURN_IF_ERROR(
        json::reject_unknown_fields(entry, {"obligation", "generation"}));
    BSM_TRY_ASSIGN(obligation_text, json::require_string(entry, "obligation"));
    BSM_TRY_ASSIGN(obligation, ObligationId::parse(obligation_text));
    BSM_TRY_ASSIGN(generation, json::require_unsigned(entry, "generation"));
    binding.prerequisite_generations.emplace_back(std::move(obligation), generation);
  }

  BSM_RETURN_IF_ERROR(normalize_binding(binding));
  BSM_RETURN_IF_ERROR(validate_binding(binding));
  return binding;
}

Result<JsonValue> authority_to_json(const AuthorityRef& authority) {
  return detail::authority_to_json(authority);
}

Result<AuthorityRef> authority_from_json(const JsonValue& value) {
  return detail::authority_from_json(value);
}

Result<Digest> binding_digest(const FacilityBinding& binding) {
  BSM_TRY_ASSIGN(value, binding_to_json(binding));
  return Digest::of(canonical_json(value));
}

const AuthorityRef* find_authority(const FacilityBinding& binding, AuthorityDomain domain,
                                   const OwnerId& owner) noexcept {
  for (const AuthorityRef& authority : binding.authorities) {
    if (authority.domain == domain && authority.owner == owner) {
      return &authority;
    }
  }
  return nullptr;
}

const AuthorityRef* find_domain_authority(const FacilityBinding& binding,
                                          AuthorityDomain domain) noexcept {
  for (const AuthorityRef& authority : binding.authorities) {
    if (authority.domain == domain) {
      return &authority;
    }
  }
  return nullptr;
}

const AuthorityRef* find_authority_by_id(const FacilityBinding& binding,
                                         const AuthorityId& id) noexcept {
  for (const AuthorityRef& authority : binding.authorities) {
    if (authority.id == id) {
      return &authority;
    }
  }
  return nullptr;
}

Result<BindingDelta> compare_bindings(const FacilityBinding& bound,
                                      const FacilityBinding& current) {
  BSM_RETURN_IF_ERROR(validate_binding(bound));
  BSM_RETURN_IF_ERROR(validate_binding(current));

  BindingDelta delta;
  const auto add = [&delta](BindingChangeCode code) -> BindingChange& {
    BindingChange change;
    change.code = code;
    delta.changes.push_back(change);
    return delta.changes.back();
  };

  if (bound.facility != current.facility) {
    add(BindingChangeCode::FacilityChanged);
  }
  if (bound.facility_epoch != current.facility_epoch) {
    BindingChange& change = add(BindingChangeCode::EpochChanged);
    change.expected = bound.facility_epoch;
    change.current = current.facility_epoch;
  }
  if (bound.topology_digest != current.topology_digest) {
    add(BindingChangeCode::TopologyChanged);
  }
  if (bound.policy != current.policy || bound.policy_revision != current.policy_revision) {
    BindingChange& change = add(BindingChangeCode::PolicyChanged);
    change.expected = bound.policy_revision;
    change.current = current.policy_revision;
  }
  if (bound.incident.id != current.incident.id) {
    add(BindingChangeCode::IncidentChanged);
  } else if (bound.incident.generation != current.incident.generation) {
    BindingChange& change = add(BindingChangeCode::IncidentGenerationChanged);
    change.expected = bound.incident.generation;
    change.current = current.incident.generation;
  }

  for (const AuthorityRef& authority : bound.authorities) {
    const AuthorityRef* observed =
        find_authority(current, authority.domain, authority.owner);
    if (observed == nullptr) {
      BindingChange& change = add(BindingChangeCode::AuthorityRemoved);
      change.domain = authority.domain;
      change.has_owner = true;
      change.owner = authority.owner;
      change.expected = authority.generation;
      continue;
    }
    if (observed->generation < authority.generation) {
      BindingChange& change = add(BindingChangeCode::AuthorityGenerationStale);
      change.domain = authority.domain;
      change.has_owner = true;
      change.owner = authority.owner;
      change.expected = authority.generation;
      change.current = observed->generation;
      continue;
    }
    if (observed->generation > authority.generation) {
      BindingChange& change = add(BindingChangeCode::AuthorityGenerationAdvanced);
      change.domain = authority.domain;
      change.has_owner = true;
      change.owner = authority.owner;
      change.expected = authority.generation;
      change.current = observed->generation;
      continue;
    }
    if (observed->attestation != authority.attestation || observed->id != authority.id) {
      BindingChange& change = add(BindingChangeCode::AuthorityAttestationChanged);
      change.domain = authority.domain;
      change.has_owner = true;
      change.owner = authority.owner;
      change.expected = authority.generation;
      change.current = observed->generation;
    }
  }
  for (const AuthorityRef& authority : current.authorities) {
    if (find_authority(bound, authority.domain, authority.owner) == nullptr) {
      BindingChange& change = add(BindingChangeCode::AuthorityAdded);
      change.domain = authority.domain;
      change.has_owner = true;
      change.owner = authority.owner;
      change.current = authority.generation;
    }
  }

  for (const auto& entry : bound.prerequisite_generations) {
    const auto found = std::lower_bound(
        current.prerequisite_generations.begin(), current.prerequisite_generations.end(),
        entry.first,
        [](const std::pair<ObligationId, std::uint64_t>& item, const ObligationId& id) {
          return item.first < id;
        });
    if (found == current.prerequisite_generations.end() || !(found->first == entry.first)) {
      BindingChange& change = add(BindingChangeCode::PrerequisiteRemoved);
      change.has_obligation = true;
      change.obligation = entry.first;
      change.expected = entry.second;
      continue;
    }
    if (found->second < entry.second) {
      BindingChange& change = add(BindingChangeCode::PrerequisiteGenerationStale);
      change.has_obligation = true;
      change.obligation = entry.first;
      change.expected = entry.second;
      change.current = found->second;
      continue;
    }
    if (found->second > entry.second) {
      BindingChange& change = add(BindingChangeCode::PrerequisiteGenerationAdvanced);
      change.has_obligation = true;
      change.obligation = entry.first;
      change.expected = entry.second;
      change.current = found->second;
    }
  }
  for (const auto& entry : current.prerequisite_generations) {
    const auto found = std::lower_bound(
        bound.prerequisite_generations.begin(), bound.prerequisite_generations.end(),
        entry.first,
        [](const std::pair<ObligationId, std::uint64_t>& item, const ObligationId& id) {
          return item.first < id;
        });
    if (found == bound.prerequisite_generations.end() || !(found->first == entry.first)) {
      BindingChange& change = add(BindingChangeCode::PrerequisiteAdded);
      change.has_obligation = true;
      change.obligation = entry.first;
      change.current = entry.second;
    }
  }

  return delta;
}

}  // namespace black_start_manager
