#pragma once

// Facility binding: the exact facility epoch, topology, policy, incident, and
// adjacent authority generations a restoration session is bound to.
//
// A session derives its permit to act from this binding and from nothing else.
// Authority is never inferred from existence, observation, acknowledgement, an
// earlier successful decision, recovered state, a cached value, a matching name, or
// apparent health: it is compared, generation by generation, against the binding the
// session committed to.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "black_start_manager/canonical.hpp"
#include "black_start_manager/digest.hpp"
#include "black_start_manager/ids.hpp"

namespace black_start_manager {

// The adjacent owners a black-start boundary may request effects from or accept
// readiness evidence from. Black Start Manager never actuates these domains itself.
enum class AuthorityDomain : std::uint8_t {
  Control = 0,
  Electrical = 1,
  Cooling = 2,
  Network = 3,
  Facility = 4,
  Workload = 5,
};

[[nodiscard]] const char* to_string(AuthorityDomain domain) noexcept;
[[nodiscard]] Result<AuthorityDomain> parse_authority_domain(std::string_view text);

// One adjacent authority instance. The generation identifies the exact instance of
// the owner's authority; the attestation is the digest of the evidence the owner
// presented for it.
struct AuthorityRef {
  AuthorityId id;
  AuthorityDomain domain = AuthorityDomain::Control;
  OwnerId owner;
  std::uint64_t generation = 0;
  Digest attestation;

  friend bool operator==(const AuthorityRef& left, const AuthorityRef& right) noexcept;
  friend bool operator!=(const AuthorityRef& left, const AuthorityRef& right) noexcept {
    return !(left == right);
  }
};

struct IncidentRef {
  IncidentId id;
  std::uint64_t generation = 0;

  friend bool operator==(const IncidentRef& left, const IncidentRef& right) noexcept;
  friend bool operator!=(const IncidentRef& left, const IncidentRef& right) noexcept {
    return !(left == right);
  }
};

struct FacilityBinding {
  FacilityId facility;
  std::uint64_t facility_epoch = 0;
  Digest topology_digest;
  PolicyTagId policy;
  std::uint64_t policy_revision = 0;
  IncidentRef incident;
  // Sorted by (domain, owner); at most one entry per (domain, owner).
  std::vector<AuthorityRef> authorities;
  // Sorted by obligation id. A prerequisite generation is the plant revision the
  // obligation's prerequisites were last attested at; a change invalidates the
  // readiness evidence taken against the earlier revision.
  std::vector<std::pair<ObligationId, std::uint64_t>> prerequisite_generations;
};

enum class BindingChangeCode : std::uint8_t {
  FacilityChanged = 0,
  EpochChanged,
  TopologyChanged,
  PolicyChanged,
  IncidentChanged,
  IncidentGenerationChanged,
  AuthorityAdded,
  AuthorityRemoved,
  AuthorityOwnerChanged,
  AuthorityGenerationAdvanced,
  AuthorityGenerationStale,
  AuthorityAttestationChanged,
  PrerequisiteAdded,
  PrerequisiteRemoved,
  PrerequisiteGenerationAdvanced,
  PrerequisiteGenerationStale,
};

[[nodiscard]] const char* to_string(BindingChangeCode code) noexcept;

struct BindingChange {
  BindingChangeCode code = BindingChangeCode::FacilityChanged;
  // Domain and owner for authority changes; obligation id for prerequisite changes.
  AuthorityDomain domain = AuthorityDomain::Control;
  bool has_owner = false;
  OwnerId owner;
  bool has_obligation = false;
  ObligationId obligation;
  std::uint64_t expected = 0;
  std::uint64_t current = 0;
};

struct BindingDelta {
  std::vector<BindingChange> changes;

  [[nodiscard]] bool empty() const noexcept { return changes.empty(); }
  // True when any change makes the bound value unreachable by advancing: the current
  // plant state is older or contradictory rather than newer.
  [[nodiscard]] bool contradictory() const noexcept;
  // True when every change is an authority generation or attestation advance that an
  // explicit re-establishment may accept.
  [[nodiscard]] bool reestablishable() const noexcept;
};

// Sorts the authority and prerequisite lists into canonical order and refuses
// duplicates. Called on every decoded binding before validation.
[[nodiscard]] Result<JsonValue> authority_to_json(const AuthorityRef& authority);
[[nodiscard]] Result<AuthorityRef> authority_from_json(const JsonValue& value);

[[nodiscard]] Result<Unit> normalize_binding(FacilityBinding& binding);
[[nodiscard]] Result<Unit> validate_binding(const FacilityBinding& binding);
[[nodiscard]] Result<JsonValue> binding_to_json(const FacilityBinding& binding);
[[nodiscard]] Result<FacilityBinding> binding_from_json(const JsonValue& value);
[[nodiscard]] Result<Digest> binding_digest(const FacilityBinding& binding);

// Compares the binding a session committed to against the binding currently in force.
[[nodiscard]] Result<BindingDelta> compare_bindings(const FacilityBinding& bound,
                                                    const FacilityBinding& current);

[[nodiscard]] const AuthorityRef* find_authority(const FacilityBinding& binding,
                                                 AuthorityDomain domain,
                                                 const OwnerId& owner) noexcept;
[[nodiscard]] const AuthorityRef* find_domain_authority(const FacilityBinding& binding,
                                                        AuthorityDomain domain) noexcept;
[[nodiscard]] const AuthorityRef* find_authority_by_id(const FacilityBinding& binding,
                                                       const AuthorityId& id) noexcept;

}  // namespace black_start_manager
