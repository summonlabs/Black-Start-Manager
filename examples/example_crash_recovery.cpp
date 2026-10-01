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

// Example: the manager dies between the durable attempt record and the owner's answer.
//
// The child process is this same program with BSM_CRASH_AT set, so the crash is a real
// process death at a real durable boundary rather than an exception. The parent then
// reopens the store from a new incarnation and shows what recovery sees: an unresolved
// attempt that must be resolved by querying the owner, never repeated blindly.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "example_support.hpp"

#if defined(_WIN32)
#include <stdlib.h>
#endif

namespace {

// Reads an environment variable without the deprecated CRT entry point.
[[nodiscard]] bool environment_is_set(const char* name) {
#if defined(_WIN32)
  char* value = nullptr;
  std::size_t size = 0;
  if (::_dupenv_s(&value, &size, name) != 0) {
    return false;
  }
  const bool present = value != nullptr;
  std::free(value);
  return present;
#else
  return std::getenv(name) != nullptr;
#endif
}

int run_child(const std::string& store) {
  using namespace black_start_manager;
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
    return 1;
  }
  EstablishSessionRequest establish;
  establish.plan = facility.value().plan;
  establish.binding = facility.value().binding;
  establish.authority = facility.value().control;
  example::Result<SessionView> session = manager.value().establish_session(establish);
  if (!session.ok()) {
    return 1;
  }
  EffectRequest request;
  request.session = session.value().id;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.authority = facility.value().electrical;
  request.deadline_ticks = 50;
  // BSM_CRASH_AT=dispatch_before terminates the process after the intent record is
  // durable and before the request leaves this process.
  const example::Result<AttemptView> attempt = manager.value().request_effect(request);
  std::cout << "child returned normally: " << (attempt.ok() ? "accepted" : "refused") << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace black_start_manager;
  const std::string store = argc > 1 ? argv[1] : "example-crash-store";
  if (environment_is_set("BSM_CRASH_AT")) {
    return run_child(store);
  }

  // Parent: run the child with the crash injection armed, then reopen the store. The
  // child's output is kept beside the store rather than in the caller's directory.
  const std::string child_log = store + "/example-child.log";
  const std::string command = "\"" + std::string(argv[0]) + "\" \"" + store + "\" > \"" +
                              child_log + "\" 2>&1\"";
  const int status = std::system(("set BSM_CRASH_AT=dispatch_before&& " + command).c_str());
  std::cout << "child exit status: " << status << "\n";

  example::Result<example::ExampleFacility> facility = example::make_facility();
  if (!facility.ok()) {
    return 1;
  }
  ManualClock clock(10);
  InProcessSyntheticController owner;
  StaticFacilityState state(facility.value().binding);
  ManagerOptions options;
  options.store_root = store;
  options.create_store_if_missing = false;
  options.clock = &clock;
  options.controller = &owner;
  options.facility = &state;
  example::Result<SessionManager> manager = SessionManager::open(options);
  if (!manager.ok()) {
    std::cerr << "reopen: " << manager.error().message() << '\n';
    return 1;
  }
  std::cout << "recovered epoch=" << manager.value().epoch()
            << " incarnation=" << manager.value().incarnation().hex() << '\n';
  example::Result<SessionId> session = manager.value().active_session();
  if (!session.ok()) {
    std::cerr << "session: " << session.error().message() << '\n';
    return 1;
  }
  example::Result<Assessment> assessment = manager.value().assess(session.value());
  if (!assessment.ok()) {
    return 1;
  }
  example::print_assessment("after recovery:", assessment.value());
  for (const ObligationAssessment& obligation : assessment.value().obligations) {
    for (const AttemptId& id : obligation.unresolved_attempts) {
      std::cout << "unresolved attempt " << id.hex() << " must be resolved before "
                << obligation.id.str() << " may be requested again\n";
    }
  }
  const example::Result<Unit> closed = manager.value().close();
  return closed.ok() ? 0 : 1;
}
