#pragma once

// Shared example plumbing: one small synthetic facility and one small two-stage plan, so
// each example states the behaviour it demonstrates instead of restating the model.

#include <cstdint>
#include <memory>
#include <string>

#include "black_start_manager/controller.hpp"
#include "black_start_manager/manager.hpp"

namespace example {

inline const char* kPlanText = R"JSON({
  "format_version": 1,
  "facility": "facility-example",
  "policy": {
    "id": "black-start-standard",
    "revision": 1,
    "default_max_age_ticks": 100,
    "default_attempt_budget": 2,
    "strict_within_stage_order": true
  },
  "stages": [
    {
      "id": "stage-isolation",
      "rank": 0,
      "title": "Verify isolation",
      "exit_evidence": [
        {"kind": "isolation_verified", "subject": "isolation:a", "min_sources": 1,
         "required_owner": "electrical-owner", "required_domain": "electrical",
         "expect": {"isolated": true}}
      ]
    },
    {
      "id": "stage-energize",
      "rank": 1,
      "title": "Energize domain A",
      "entry_evidence": [
        {"kind": "isolation_verified", "subject": "isolation:a", "min_sources": 1,
         "required_owner": "electrical-owner", "required_domain": "electrical",
         "expect": {"isolated": true}}
      ]
    }
  ],
  "obligations": [
    {
      "id": "isolation-a",
      "title": "Verify that domain A is isolated",
      "stage": "stage-isolation",
      "priority": "protected",
      "owner_domain": "electrical",
      "owner": "electrical-owner",
      "requires": [
        {"kind": "isolation_verified", "subject": "isolation:a", "min_sources": 1,
         "required_owner": "electrical-owner", "required_domain": "electrical",
         "expect": {"isolated": true}}
      ],
      "request": {"capability": "verify-isolation", "parameters": {"domain": "a"}},
      "attempt_budget": 2
    },
    {
      "id": "energize-a",
      "title": "Energize electrical domain A",
      "stage": "stage-energize",
      "priority": "protected",
      "owner_domain": "electrical",
      "owner": "electrical-owner",
      "depends_on": ["isolation-a"],
      "requires": [
        {"kind": "domain_energized", "subject": "domain:a", "min_sources": 1,
         "required_owner": "electrical-owner", "required_domain": "electrical",
         "expect": {"energized": true}}
      ],
      "request": {"capability": "energize-domain", "parameters": {"domain": "a"}},
      "attempt_budget": 2
    }
  ],
  "return_to_service": {
    "evidence": [
      {"kind": "isolation_verified", "subject": "isolation:a", "min_sources": 1,
       "required_owner": "electrical-owner", "required_domain": "electrical",
       "expect": {"isolated": true}}
    ],
    "required_obligations": ["isolation-a", "energize-a"]
  }
})JSON";

using namespace black_start_manager;

struct ExampleFacility {
  FacilityBinding binding;
  PlanDocument plan;
  AuthorityRef control;
  AuthorityRef electrical;
};

[[nodiscard]] inline Result<AuthorityRef> make_authority(AuthorityDomain domain,
                                                         const char* id,
                                                         const char* owner,
                                                         std::uint64_t generation) {
  AuthorityRef authority;
  BSM_TRY_ASSIGN(authority_id, AuthorityId::parse(id));
  authority.id = std::move(authority_id);
  authority.domain = domain;
  BSM_TRY_ASSIGN(owner_id, OwnerId::parse(owner));
  authority.owner = std::move(owner_id);
  authority.generation = generation;
  authority.attestation =
      Digest::of(std::string("attestation:") + owner + ":" + std::to_string(generation));
  return authority;
}

[[nodiscard]] inline Result<ExampleFacility> make_facility(std::uint64_t generation = 1) {
  ExampleFacility facility;
  BSM_TRY_ASSIGN(control, make_authority(AuthorityDomain::Control, "authority-control",
                                         "ops-control", generation));
  facility.control = control;
  BSM_TRY_ASSIGN(electrical, make_authority(AuthorityDomain::Electrical,
                                            "authority-electrical", "electrical-owner",
                                            generation));
  facility.electrical = electrical;
  FacilityBinding binding;
  BSM_TRY_ASSIGN(facility_id, FacilityId::parse("facility-example"));
  binding.facility = std::move(facility_id);
  binding.facility_epoch = 1;
  binding.topology_digest = Digest::of("topology-example");
  BSM_TRY_ASSIGN(policy, PolicyTagId::parse("black-start-standard"));
  binding.policy = std::move(policy);
  binding.policy_revision = 1;
  BSM_TRY_ASSIGN(incident, IncidentId::parse("incident-example"));
  binding.incident.id = std::move(incident);
  binding.incident.generation = 1;
  binding.authorities = {facility.control, facility.electrical};
  BSM_RETURN_IF_ERROR(normalize_binding(binding));
  facility.binding = binding;
  BSM_TRY_ASSIGN(plan, PlanDocument::parse(kPlanText));
  facility.plan = plan;
  return facility;
}

inline void print_assessment(const char* label, const Assessment& assessment) {
  const Result<JsonValue> value = assessment_to_json(assessment);
  if (!value.ok()) {
    return;
  }
  std::cout << label << " state=" << to_string(assessment.state)
            << " stage=" << assessment.current_stage.str()
            << " primary_block=" << to_string(assessment.primary_block) << '\n'
            << "  explanation: " << assessment.explanation << '\n'
            << "  eligible:";
  for (const ObligationId& id : assessment.eligible) {
    std::cout << ' ' << id.str();
  }
  std::cout << '\n';
}

}  // namespace example
