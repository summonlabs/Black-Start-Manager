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

// bsm_probe: a real independent process that the multiprocess and crash tests start.
//
//   bsm_probe hold <store>                  take the writer lock and block on stdin
//   bsm_probe session <store>               print the active session identity
//   bsm_probe step <store> <step> [options] run one standard progression step
//
// Environment: BSM_CRASH_AT selects a crash point, which terminates the process at that
// durable boundary without unwinding.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <vector>

#include "black_start_manager/manager.hpp"
#include "scenario.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

[[nodiscard]] std::string process_id_string();

// Progress trace. The multiprocess tests set BSM_PROBE_LOG so a child that blocks can be
// located exactly, without a debugger and without a timeout.
[[nodiscard]] std::string environment_text(const char* name) {
#if defined(_WIN32)
  char* value = nullptr;
  std::size_t size = 0;
  if (::_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
    return std::string();
  }
  const std::string text(value);
  std::free(value);
  return text;
#else
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
#endif
}

void trace(const char* text) {
  const std::string path = environment_text("BSM_PROBE_LOG");
  if (path.empty()) {
    return;
  }
  std::ofstream log(path, std::ios::app);
  if (log) {
    log << process_id_string() << " " << text << std::endl;
  }
}

[[nodiscard]] long process_id() {
#if defined(_WIN32)
  return static_cast<long>(::_getpid());
#else
  return static_cast<long>(::getpid());
#endif
}

std::string process_id_string() { return std::to_string(process_id()); }

[[nodiscard]] int report_error(const black_start_manager::Error& error) {
  std::cout << "{\"ok\":false,\"code\":\"" << black_start_manager::to_string(error.code())
            << "\",\"message\":\"" << error.message() << "\"}" << std::endl;
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  if (arguments.size() < 2) {
    std::cerr << "bsm_probe: usage: bsm_probe <hold|session|step> <store> [step]\n";
    return 2;
  }
  const std::string command = arguments[0];
  const std::string store = arguments[1];
  const bool create = command == "hold";

  if (command == "hold" || command == "session") {
    black_start_manager::ManualClock clock(1);
    black_start_manager::InProcessSyntheticController controller;
    black_start_manager::FacilityBinding binding;
    black_start_manager::StaticFacilityState facility(binding);
    black_start_manager::ManagerOptions options;
    options.store_root = store;
    options.create_store_if_missing = create;
    options.clock = &clock;
    options.controller = &controller;
    options.facility = &facility;
    black_start_manager::Result<black_start_manager::SessionManager> manager =
        black_start_manager::SessionManager::open(options);
    if (!manager.ok()) {
      return report_error(manager.error());
    }
    if (command == "session") {
      const black_start_manager::Result<std::vector<black_start_manager::SessionId>>
          sessions = manager.value().sessions();
      if (!sessions.ok()) {
        return report_error(sessions.error());
      }
      std::cout << "{\"ok\":true,\"sessions\":[";
      for (std::size_t index = 0; index < sessions.value().size(); ++index) {
        if (index > 0) {
          std::cout << ',';
        }
        std::cout << "\"" << sessions.value()[index].hex() << "\"";
      }
      std::cout << "]}" << std::endl;
      return 0;
    }
    std::cout << "READY " << process_id() << " epoch=" << manager.value().epoch()
              << " incarnation=" << manager.value().incarnation().hex() << std::endl;
    // Hold the writer lock until the process is terminated. Waiting on stdin would end the
    // hold immediately whenever the harness hands this process a closed stdin, which would
    // silently release the very claim the multiprocess test is proving.
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  if (command == "step") {
    if (arguments.size() < 3) {
      std::cerr << "bsm_probe: step needs a step name\n";
      return 2;
    }
    bsm_test::StepOptions step_options;
    step_options.store = store;
    step_options.step = arguments[2];
    step_options.tick = 1;
    for (std::size_t index = 3; index + 1 < arguments.size(); ++index) {
      if (arguments[index] == "--tick") {
        step_options.tick = static_cast<std::uint64_t>(std::stoull(arguments[index + 1]));
        ++index;
      } else if (arguments[index] == "--controller") {
        step_options.controller = arguments[index + 1];
        ++index;
      } else if (arguments[index] == "--no-create") {
        step_options.create_store = false;
      } else if (arguments[index] == "--compact-records") {
        step_options.compact_records =
            static_cast<std::size_t>(std::stoull(arguments[index + 1]));
        ++index;
      }
    }
    trace("step: before run_step");
    const black_start_manager::Result<std::string> result = bsm_test::run_step(step_options);
    trace(result.ok() ? "step: run_step returned ok" : "step: run_step refused");
    if (!result.ok()) {
      return report_error(result.error());
    }
    std::cout << "{\"ok\":true,\"session\":\"" << result.value() << "\"}" << std::endl;
    trace("step: done");
    return 0;
  }

  std::cerr << "bsm_probe: unknown command '" << command << "'\n";
  return 2;
}
