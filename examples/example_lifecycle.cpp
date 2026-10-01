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

// Example: a complete restoration session against the synthetic adjacent owner.
//
// SYNTHETIC: the plant, its observations, and the owner behaviour. REAL: the durable
// store, its filesystem publication, and the process that runs this program.

#include <cstdint>
#include <iostream>
#include <string>

#include "example_support.hpp"

int main(int argc, char** argv) {
  using namespace black_start_manager;
  const std::string store = argc > 1 ? argv[1] : "example-store";

  example::Result<example::ExampleFacility> facility = example::make_facility();
  if (!facility.ok()) {
    std::cerr << "facility: " << facility.error().message() << '\n';
    return 1;
  }
  ManualClock clock(1);
  InProcessSyntheticController owner;
  StaticFacilityState state(facility.value().binding);
  ManagerOptions options;
  options.store_root = store;
  options.create_store_if_missing = true;
  options.clock = &clock;
  options.controller = &owner;
  options.facility = &state;
  example::Result<SessionManager> manager = SessionManager::open(options);
  if (!manager.ok()) {
    std::cerr << "open: " << manager.error().message() << '\n';
    return 1;
  }

  EstablishSessionRequest establish;
  establish.plan = facility.value().plan;
  establish.binding = facility.value().binding;
  establish.authority = facility.value().control;
  example::Result<SessionView> session = manager.value().establish_session(establish);
  if (!session.ok()) {
    std::cerr << "establish: " << session.error().message() << '\n';
    return 1;
  }
  std::cout << "session " << session.value().id.hex() << " established\n";

  example::Result<Assessment> initial = manager.value().assess(session.value().id);
  if (!initial.ok()) {
    std::cerr << "assess: " << initial.error().message() << '\n';
    return 1;
  }
  example::print_assessment("initial:", initial.value());

  // Request the bounded effect that verifies isolation, then observe it and verify.
  EffectRequest isolation;
  isolation.session = session.value().id;
  isolation.obligation = ObligationId::parse("isolation-a").value();
  isolation.authority = facility.value().electrical;
  isolation.deadline_ticks = 50;
  example::Result<AttemptView> attempt = manager.value().request_effect(isolation);
  if (!attempt.ok()) {
    std::cerr << "request: " << attempt.error().message() << '\n';
    return 1;
  }
  std::cout << "attempt " << attempt.value().attempt.id.hex()
            << " state=" << to_string(attempt.value().attempt.state) << '\n';

  ObserveRequest observation;
  observation.session = session.value().id;
  observation.kind = EvidenceKind::parse("isolation_verified").value();
  observation.subject = SubjectId::parse("isolation:a").value();
  observation.has_obligation = true;
  observation.obligation = isolation.obligation;
  observation.observer = OwnerId::parse("electrical-owner").value();
  observation.authority = facility.value().electrical;
  observation.observed_tick = clock.now_ticks().value();
  example::Result<EvidenceView> evidence = manager.value().observe_subject(observation);
  if (!evidence.ok()) {
    std::cerr << "observe: " << evidence.error().message() << '\n';
    return 1;
  }
  std::cout << "evidence " << evidence.value().record.id.hex()
            << " provenance=" << to_string(evidence.value().record.provenance) << '\n';

  example::Result<Assessment> ready = manager.value().assess(session.value().id);
  if (!ready.ok()) {
    std::cerr << "assess: " << ready.error().message() << '\n';
    return 1;
  }
  example::print_assessment("after observation:", ready.value());

  // A black-start plan is not actuation authority: the next stage may only be entered
  // when its entry evidence is current, which it now is.
  StageAdvance advance;
  advance.session = session.value().id;
  advance.authority = facility.value().control;
  example::Result<StageTransitionView> transition = manager.value().advance_stage(advance);
  if (!transition.ok()) {
    std::cerr << "advance: " << transition.error().message() << '\n';
    return 1;
  }
  std::cout << "advanced from " << transition.value().transition.from.str() << " to "
            << transition.value().transition.to.str() << '\n';

  example::Result<JsonValue> report = manager.value().report(session.value().id);
  if (report.ok()) {
    std::cout << "report bytes: " << canonical_json(report.value()).size() << '\n';
  }
  const example::Result<Unit> closed = manager.value().close();
  return closed.ok() ? 0 : 1;
}
