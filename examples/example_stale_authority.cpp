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

// Example: authority advance fences the session, re-establishment is explicit, and
// recovered evidence never becomes current by itself.

#include <cstdint>
#include <iostream>
#include <string>

#include "example_support.hpp"

int main(int argc, char** argv) {
  using namespace black_start_manager;
  const std::string store = argc > 1 ? argv[1] : "example-stale-store";

  example::Result<example::ExampleFacility> facility = example::make_facility();
  if (!facility.ok()) {
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

  // The electrical authority is replaced: the owner is the same, the generation is not.
  example::Result<example::ExampleFacility> advanced = example::make_facility(2);
  if (!advanced.ok()) {
    return 1;
  }
  state.set(advanced.value().binding);

  example::Result<Assessment> fenced = manager.value().assess(session.value().id);
  if (!fenced.ok()) {
    return 1;
  }
  std::cout << "after the authority advanced: fenced=" << (fenced.value().fenced ? "true" : "false")
            << " authority_reestablished="
            << (fenced.value().authority_reestablished ? "true" : "false") << '\n';
  for (const BindingChange& change : fenced.value().delta.changes) {
    std::cout << "  binding change: " << to_string(change.code) << " expected="
              << change.expected << " current=" << change.current << '\n';
  }

  // A request under the old generation is refused rather than retried blindly.
  EffectRequest request;
  request.session = session.value().id;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.authority = facility.value().electrical;
  request.deadline_ticks = 50;
  example::Result<AttemptView> refused = manager.value().request_effect(request);
  std::cout << "request under the stale authority: "
            << (refused.ok() ? "accepted" : to_string(refused.error().code())) << '\n';

  // Re-establishment is an explicit act that adopts the observed generations.
  ReestablishRequest reestablish;
  reestablish.session = session.value().id;
  reestablish.authority = advanced.value().control;
  reestablish.reason = "operations control handover";
  example::Result<SessionView> reestablished =
      manager.value().reestablish_authority(reestablish);
  std::cout << "re-establishment: "
            << (reestablished.ok() ? "accepted" : reestablished.error().message()) << '\n';

  example::Result<Assessment> after = manager.value().assess(session.value().id);
  if (after.ok()) {
    example::print_assessment("after re-establishment:", after.value());
  }
  const example::Result<Unit> closed = manager.value().close();
  return closed.ok() ? 0 : 1;
}
