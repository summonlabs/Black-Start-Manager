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

// Out-of-tree consumer of the installed Black Start Manager package.
//
// This program only uses the installed public headers and the exported CMake target. It
// exercises a complete lifecycle: build a plan and a facility binding, open a durable
// store, establish a session, admit readiness evidence through the synthetic adjacent
// owner, assess the gate, verify the store, and close.

#include <cstdint>
#include <iostream>
#include <string>

#include <black_start_manager/controller.hpp>
#include <black_start_manager/manager.hpp>
#include <black_start_manager/version.hpp>

namespace {

using namespace black_start_manager;

[[nodiscard]] int fail(const std::string& what, const Error& error) {
  std::cerr << what << ": " << to_string(error.code()) << ": " << error.message() << '\n';
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string store = argc > 1 ? argv[1] : "consumer-store";

  Result<PlanDocument> plan = PlanDocument::parse(R"JSON({
    "format_version": 1,
    "facility": "consumer-facility",
    "policy": {"id": "consumer-policy", "revision": 1, "default_max_age_ticks": 100,
               "default_attempt_budget": 2, "strict_within_stage_order": true},
    "stages": [
      {"id": "stage-0", "rank": 0, "title": "verify isolation",
       "exit_evidence": [{"kind": "isolation_verified", "subject": "isolation:a",
                          "min_sources": 1, "required_owner": "electrical-owner",
                          "required_domain": "electrical", "expect": {"isolated": true}}]}
    ],
    "obligations": [
      {"id": "isolation-a", "title": "verify isolation", "stage": "stage-0",
       "priority": "protected", "owner_domain": "electrical", "owner": "electrical-owner",
       "requires": [{"kind": "isolation_verified", "subject": "isolation:a",
                     "min_sources": 1, "required_owner": "electrical-owner",
                     "required_domain": "electrical", "expect": {"isolated": true}}],
       "request": {"capability": "verify-isolation", "parameters": {"domain": "a"}},
       "attempt_budget": 2}
    ],
    "return_to_service": {"evidence": [{"kind": "isolation_verified",
                                        "subject": "isolation:a", "min_sources": 1,
                                        "required_owner": "electrical-owner",
                                        "required_domain": "electrical",
                                        "expect": {"isolated": true}}],
                          "required_obligations": ["isolation-a"]}
  })JSON");
  if (!plan.ok()) {
    return fail("plan", plan.error());
  }
  const Result<JsonValue> binding_document = parse_json(R"JSON({
    "facility": "consumer-facility", "facility_epoch": 1,
    "topology_digest": "3333333333333333333333333333333333333333333333333333333333333333",
    "policy": "consumer-policy", "policy_revision": 1,
    "incident": {"id": "consumer-incident", "generation": 1},
    "authorities": [{"id": "authority-control", "domain": "control", "owner": "ops-control",
                     "generation": 1,
                     "attestation":
                     "4444444444444444444444444444444444444444444444444444444444444444"},
                    {"id": "authority-electrical", "domain": "electrical",
                     "owner": "electrical-owner", "generation": 1,
                     "attestation":
                     "5555555555555555555555555555555555555555555555555555555555555555"}],
    "prerequisite_generations": []
  })JSON");
  if (!binding_document.ok()) {
    return fail("binding document", binding_document.error());
  }
  const Result<FacilityBinding> binding = binding_from_json(binding_document.value());
  if (!binding.ok()) {
    return fail("binding", binding.error());
  }
  const Result<JsonValue> control_document = parse_json(R"JSON({
    "id": "authority-control", "domain": "control", "owner": "ops-control",
    "generation": 1,
    "attestation": "4444444444444444444444444444444444444444444444444444444444444444"
  })JSON");
  if (!control_document.ok()) {
    return fail("control document", control_document.error());
  }
  const Result<AuthorityRef> control = authority_from_json(control_document.value());
  if (!control.ok()) {
    return fail("authority", control.error());
  }
  const Result<JsonValue> electrical_document = parse_json(R"JSON({
    "id": "authority-electrical", "domain": "electrical", "owner": "electrical-owner",
    "generation": 1,
    "attestation": "5555555555555555555555555555555555555555555555555555555555555555"
  })JSON");
  if (!electrical_document.ok()) {
    return fail("electrical document", electrical_document.error());
  }
  const Result<AuthorityRef> electrical = authority_from_json(electrical_document.value());
  if (!electrical.ok()) {
    return fail("authority", electrical.error());
  }

  ManualClock clock(1);
  InProcessSyntheticController owner;
  StaticFacilityState state(binding.value());
  ManagerOptions options;
  options.store_root = store;
  options.create_store_if_missing = true;
  options.clock = &clock;
  options.controller = &owner;
  options.facility = &state;
  Result<SessionManager> manager = SessionManager::open(options);
  if (!manager.ok()) {
    return fail("open", manager.error());
  }

  EstablishSessionRequest establish;
  establish.plan = plan.value();
  establish.binding = binding.value();
  establish.authority = control.value();
  Result<SessionView> session = manager.value().establish_session(establish);
  if (!session.ok()) {
    return fail("establish", session.error());
  }

  EffectRequest request;
  request.session = session.value().id;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.authority = electrical.value();
  request.deadline_ticks = 50;
  Result<AttemptView> attempt = manager.value().request_effect(request);
  if (!attempt.ok()) {
    return fail("request", attempt.error());
  }
  if (attempt.value().attempt.state != AttemptState::Acknowledged) {
    std::cerr << "the synthetic owner did not acknowledge the request\n";
    return 1;
  }

  ObserveRequest observation;
  observation.session = session.value().id;
  observation.kind = EvidenceKind::parse("isolation_verified").value();
  observation.subject = SubjectId::parse("isolation:a").value();
  observation.has_obligation = true;
  observation.obligation = request.obligation;
  observation.observer = OwnerId::parse("electrical-owner").value();
  observation.authority = electrical.value();
  observation.observed_tick = clock.now_ticks().value();
  Result<EvidenceView> evidence = manager.value().observe_subject(observation);
  if (!evidence.ok()) {
    return fail("observe", evidence.error());
  }

  Result<Assessment> assessment = manager.value().assess(session.value().id);
  if (!assessment.ok()) {
    return fail("assess", assessment.error());
  }
  if (!assessment.value().completion_ready) {
    std::cerr << "the session should be ready for return-to-service proof: "
              << assessment.value().explanation << '\n';
    return 1;
  }

  Result<JsonValue> report = manager.value().report(session.value().id);
  if (!report.ok()) {
    return fail("report", report.error());
  }
  const Result<JsonValue> verification = manager.value().verify_store();
  if (!verification.ok()) {
    return fail("verify", verification.error());
  }
  const Result<Unit> closed = manager.value().close();
  if (!closed.ok()) {
    return fail("close", closed.error());
  }

  std::cout << "consumer ok: version " << kVersionString << ", DCCP boundary "
            << kDccpBoundary << ", plan digest "
            << assessment.value().plan_digest.hex().substr(0, 16) << ", report bytes "
            << canonical_json(report.value()).size() << '\n';
  return 0;
}
