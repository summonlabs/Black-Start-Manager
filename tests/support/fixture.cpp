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

#include "fixture.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace bsm_test {

using namespace black_start_manager;

namespace {

[[nodiscard]] Result<AuthorityRef> make_authority(AuthorityDomain domain,
                                                  const char* id, const char* owner,
                                                  std::uint64_t generation) {
  AuthorityRef authority;
  BSM_TRY_ASSIGN(authority_id, AuthorityId::parse(id));
  authority.id = std::move(authority_id);
  authority.domain = domain;
  BSM_TRY_ASSIGN(owner_id, OwnerId::parse(owner));
  authority.owner = std::move(owner_id);
  authority.generation = generation;
  authority.attestation = Digest::of(std::string("attestation:") + owner + ":" +
                                     std::to_string(generation));
  return authority;
}

}  // namespace

std::string standard_plan_text() {
  return R"JSON({
  "format_version": 1,
  "facility": "facility-1",
  "policy": {
    "id": "black-start-standard",
    "revision": 1,
    "default_max_age_ticks": 50,
    "default_attempt_budget": 2,
    "strict_within_stage_order": true
  },
  "stages": [
    {
      "id": "stage-isolation",
      "rank": 0,
      "title": "Establish and verify isolation",
      "exit_evidence": [
        {"kind": "isolation_verified", "subject": "isolation:a", "min_sources": 1,
         "required_owner": "electrical-owner", "required_domain": "electrical",
         "expect": {"isolated": true}}
      ]
    },
    {
      "id": "stage-energize",
      "rank": 1,
      "title": "Restore control power, electrical domains, and cooling",
      "entry_evidence": [
        {"kind": "isolation_verified", "subject": "isolation:a", "min_sources": 1,
         "required_owner": "electrical-owner", "required_domain": "electrical",
         "expect": {"isolated": true}}
      ],
      "exit_evidence": [
        {"kind": "domain_energized", "subject": "domain:a", "min_sources": 2,
         "required_owner": "electrical-owner", "required_domain": "electrical",
         "expect": {"energized": true}},
        {"kind": "cooling_ready", "subject": "cooling:primary", "min_sources": 1,
         "required_owner": "cooling-owner", "required_domain": "cooling",
         "expect": {"ready": true}}
      ]
    },
    {
      "id": "stage-admit",
      "rank": 2,
      "title": "Re-establish capacity evidence and admit workload",
      "entry_evidence": [
        {"kind": "domain_energized", "subject": "domain:a", "min_sources": 2,
         "required_owner": "electrical-owner", "required_domain": "electrical",
         "expect": {"energized": true}},
        {"kind": "cooling_ready", "subject": "cooling:primary", "min_sources": 1,
         "required_owner": "cooling-owner", "required_domain": "cooling",
         "expect": {"ready": true}}
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
        {"kind": "domain_energized", "subject": "domain:a", "min_sources": 2,
         "required_owner": "electrical-owner", "required_domain": "electrical",
         "expect": {"energized": true}}
      ],
      "request": {"capability": "energize-domain", "parameters": {"domain": "a"}},
      "attempt_budget": 2
    },
    {
      "id": "control-power",
      "title": "Restore control and monitoring power",
      "stage": "stage-energize",
      "priority": "critical",
      "owner_domain": "facility",
      "owner": "facility-owner",
      "requires": [
        {"kind": "control_power_available", "subject": "control-power:main",
         "min_sources": 1, "required_owner": "facility-owner",
         "required_domain": "facility", "expect": {"available": true}}
      ],
      "request": {"capability": "restore-control-power", "parameters": {"bus": "main"}},
      "attempt_budget": 2
    },
    {
      "id": "cooling-primary",
      "title": "Restore the primary cooling loop",
      "stage": "stage-energize",
      "priority": "critical",
      "owner_domain": "cooling",
      "owner": "cooling-owner",
      "requires": [
        {"kind": "cooling_ready", "subject": "cooling:primary", "min_sources": 1,
         "required_owner": "cooling-owner", "required_domain": "cooling",
         "expect": {"ready": true}}
      ],
      "request": {"capability": "restore-cooling", "parameters": {"loop": "primary"}},
      "attempt_budget": 2
    },
    {
      "id": "admit-r1",
      "title": "Admit rack r1 into service",
      "stage": "stage-admit",
      "priority": "standard",
      "owner_domain": "facility",
      "owner": "facility-owner",
      "depends_on": ["energize-a", "cooling-primary"],
      "requires": [
        {"kind": "rack_admitted", "subject": "rack:r1", "min_sources": 1,
         "required_owner": "facility-owner", "required_domain": "facility",
         "expect": {"admitted": true}}
      ],
      "request": {"capability": "admit-rack", "parameters": {"rack": "r1"}},
      "attempt_budget": 2
    },
    {
      "id": "return-to-service",
      "title": "Verify the facility return-to-service proof",
      "stage": "stage-admit",
      "priority": "standard",
      "owner_domain": "facility",
      "owner": "facility-owner",
      "depends_on": ["admit-r1"],
      "requires": [
        {"kind": "return_to_service", "subject": "facility", "min_sources": 1,
         "required_owner": "facility-owner", "required_domain": "facility",
         "expect": {"return_to_service": true}}
      ],
      "request": {"capability": "verify-return-to-service", "parameters": {}},
      "attempt_budget": 2
    }
  ],
  "return_to_service": {
    "evidence": [
      {"kind": "return_to_service", "subject": "facility", "min_sources": 1,
       "required_owner": "facility-owner", "required_domain": "facility",
       "expect": {"return_to_service": true}}
    ],
    "required_obligations": [
      "isolation-a", "energize-a", "control-power", "cooling-primary", "admit-r1",
      "return-to-service"
    ]
  }
})JSON";
}

Result<PlanDocument> standard_plan() { return PlanDocument::parse(standard_plan_text()); }

Result<Facility> make_facility(std::uint64_t epoch, std::uint64_t generation,
                               std::uint64_t incident_generation) {
  Facility facility;
  BSM_TRY_ASSIGN(control,
                 make_authority(AuthorityDomain::Control, "authority-control", "ops-control",
                                generation));
  facility.control = control;
  BSM_TRY_ASSIGN(electrical,
                 make_authority(AuthorityDomain::Electrical, "authority-electrical",
                                "electrical-owner", generation));
  facility.electrical = electrical;
  BSM_TRY_ASSIGN(cooling, make_authority(AuthorityDomain::Cooling, "authority-cooling",
                                         "cooling-owner", generation));
  facility.cooling = cooling;
  BSM_TRY_ASSIGN(facility_owner,
                 make_authority(AuthorityDomain::Facility, "authority-facility",
                                "facility-owner", generation));
  facility.facility_owner = facility_owner;
  BSM_TRY_ASSIGN(instrumentation,
                 make_authority(AuthorityDomain::Facility, "authority-instrumentation",
                                "instrumentation-owner", generation));
  facility.instrumentation = instrumentation;

  FacilityBinding binding;
  BSM_TRY_ASSIGN(facility_id, FacilityId::parse("facility-1"));
  binding.facility = std::move(facility_id);
  binding.facility_epoch = epoch;
  binding.topology_digest = Digest::of("topology-revision-1");
  BSM_TRY_ASSIGN(policy, PolicyTagId::parse("black-start-standard"));
  binding.policy = std::move(policy);
  binding.policy_revision = 1;
  BSM_TRY_ASSIGN(incident, IncidentId::parse("incident-77"));
  binding.incident.id = std::move(incident);
  binding.incident.generation = incident_generation;
  binding.authorities = {facility.control, facility.electrical, facility.cooling,
                         facility.facility_owner, facility.instrumentation};
  BSM_TRY_ASSIGN(prerequisite, ObligationId::parse("isolation-a"));
  binding.prerequisite_generations.emplace_back(std::move(prerequisite), generation);
  BSM_RETURN_IF_ERROR(normalize_binding(binding));
  BSM_RETURN_IF_ERROR(validate_binding(binding));
  binding.facility_epoch = epoch;
  facility.binding = binding;
  BSM_TRY_ASSIGN(plan, standard_plan());
  facility.plan = plan;
  return facility;
}

std::string scratch_directory(const std::string& name) {
  const std::filesystem::path path =
      std::filesystem::current_path() / "bsm-test-state" / name;
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
  return path.string();
}

Result<Unit> Harness::start(const std::string& directory, std::uint64_t tick,
                            const SyntheticControllerPolicy& policy,
                            std::uint64_t compact_bytes, std::size_t compact_records) {
  root = directory;
  BSM_TRY_ASSIGN(definition_value, make_facility());
  definition = definition_value;
  BSM_RETURN_IF_ERROR(clock.set(tick));
  controller.set_policy(policy);
  facility = std::make_unique<StaticFacilityState>(definition.binding);
  ManagerOptions options;
  options.store_root = root;
  options.create_store_if_missing = true;
  options.clock = &clock;
  options.controller = &controller;
  options.facility = facility.get();
  options.compact_journal_bytes = compact_bytes;
  options.compact_journal_records = compact_records;
  BSM_TRY_ASSIGN(opened, SessionManager::open(options));
  manager = std::make_unique<SessionManager>(std::move(opened));
  return Unit{};
}

Result<Unit> Harness::reopen(std::uint64_t tick) {
  manager.reset();
  BSM_RETURN_IF_ERROR(clock.set(tick));
  ManagerOptions options;
  options.store_root = root;
  options.create_store_if_missing = false;
  options.clock = &clock;
  options.controller = &controller;
  options.facility = facility.get();
  BSM_TRY_ASSIGN(opened, SessionManager::open(options));
  manager = std::make_unique<SessionManager>(std::move(opened));
  return Unit{};
}

Result<SessionId> Harness::open_session() {
  EstablishSessionRequest request;
  request.plan = definition.plan;
  request.binding = definition.binding;
  request.authority = definition.control;
  BSM_TRY_ASSIGN(view, manager->establish_session(request));
  return view.id;
}

Result<EvidenceView> Harness::observe(const SessionId& session,
                                      const ObligationId& obligation,
                                      const EvidenceKind& kind, const SubjectId& subject,
                                      const OwnerId& observer,
                                      const AuthorityRef& authority) {
  ObserveRequest request;
  request.session = session;
  request.kind = kind;
  request.subject = subject;
  request.has_obligation = true;
  request.obligation = obligation;
  request.observer = observer;
  request.authority = authority;
  BSM_TRY_ASSIGN(tick_now, clock.now_ticks());
  request.observed_tick = tick_now;
  return manager->observe_subject(request);
}

}  // namespace bsm_test
