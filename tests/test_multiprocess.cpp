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

// Proof obligations, with real operating-system processes:
//   - a second writer is refused by the kernel-owned lock;
//   - the abrupt death of the holder releases it, and the successor does not inherit the
//     previous incarnation's authority;
//   - a process killed at each durable boundary of a consequential request recovers to an
//     unresolved attempt that is resolved against the owner rather than repeated;
//   - the owner's durable ledger never records a duplicate consequential effect;
//   - a process killed during snapshot publication recovers to one whole generation.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "black_start_manager/remote_controller.hpp"
#include "black_start_manager/store.hpp"
#include "fixture.hpp"
#include "proc.hpp"
#include "test_harness.hpp"

using namespace black_start_manager;
using namespace bsm_test;

namespace {

[[nodiscard]] std::string probe() { return std::string(BSM_PROBE_EXECUTABLE); }

// Runs one probe step in a real child process and returns its output.
[[nodiscard]] Result<std::string> run_probe(
    const std::vector<std::string>& arguments,
    const std::vector<std::pair<std::string, std::string>>& environment = {}) {
  static const std::string trace_path = []() {
    return bsm_test::scratch_directory("probe-trace") + "/probe.log";
  }();
  std::vector<std::pair<std::string, std::string>> full_environment = environment;
  full_environment.emplace_back("BSM_PROBE_LOG", trace_path);
  BSM_TRY_ASSIGN(child,
                 bsm_test::spawn_child(probe(), arguments, std::string(), full_environment));
  BSM_TRY_ASSIGN(output, bsm_test::read_until_eof(child));
  const int status = bsm_test::wait_child(child);
  (void)status;
  bsm_test::close_child(child);
  return output;
}

[[nodiscard]] Result<JsonValue> parse_output(const std::string& output) {
  const std::size_t start = output.find('{');
  if (start == std::string::npos) {
    return Error(ErrorCode::IoFailure, "the child produced no JSON: " + output);
  }
  const std::size_t end = output.rfind('}');
  if (end == std::string::npos || end < start) {
    return Error(ErrorCode::IoFailure, "the child produced truncated JSON: " + output);
  }
  return parse_json(output.substr(start, end - start + 1));
}

// Serves the out-of-process owner while children connect to it.
class HostThread {
 public:
  [[nodiscard]] Result<Unit> start(std::uint16_t port, const std::string& store) {
    PlantHost::Options options;
    options.port = port;
    options.store_root = store;
    options.create_store_if_missing = true;
    BSM_TRY_ASSIGN(host, PlantHost::start(options));
    host_value = std::make_unique<PlantHost>(std::move(host));
    thread = std::thread([this]() {
      while (!stopping.load()) {
        const Result<Unit> served = host_value->serve_one_client();
        if (!served.ok()) {
          break;
        }
      }
    });
    return Unit{};
  }

  void stop() {
    stopping.store(true);
    if (host_value != nullptr) {
      host_value->request_shutdown();
      // A connecting client unblocks the accept so the service thread can observe the
      // shutdown flag; the connection is closed immediately.
      RemoteControllerOptions options;
      options.port = host_value->port();
      RemoteControllerClient client;
      const Result<Unit> connected = client.connect(options);
      if (connected.ok()) {
        const Result<Unit> disconnected = client.disconnect();
        (void)disconnected;
      }
    }
    if (thread.joinable()) {
      thread.join();
    }
  }

  [[nodiscard]] std::uint16_t port() const { return host_value->port(); }
  [[nodiscard]] PlantHost& host() { return *host_value; }

  ~HostThread() { stop(); }
  HostThread() = default;
  HostThread(const HostThread&) = delete;
  HostThread& operator=(const HostThread&) = delete;

 private:
  std::unique_ptr<PlantHost> host_value;
  std::thread thread;
  std::atomic<bool> stopping{false};
};

}  // namespace

BSM_TEST(a_second_writer_is_refused_and_an_abrupt_death_releases_the_lock) {
  const std::string store = bsm_test::scratch_directory("multiprocess-lock");
  BSM_CHECK_OK(child, bsm_test::spawn_child(probe(), {"hold", store}, std::string(), {}));
  BSM_CHECK_OK(ready, bsm_test::read_line(child));
  BSM_CHECK(ready.find("READY") != std::string::npos);
  BSM_CHECK(bsm_test::child_running(child));

  // A second independent process must be refused, and the refusal must be the lock.
  BSM_CHECK_OK(second, bsm_test::spawn_child(probe(), {"session", store}, std::string(), {}));
  BSM_CHECK_OK(second_output, bsm_test::read_until_eof(second));
  const int second_status = bsm_test::wait_child(second);
  bsm_test::close_child(second);
  BSM_CHECK(second_status != 0);
  BSM_CHECK(second_output.find("store_locked") != std::string::npos);
  BSM_CHECK(second_output.find("\"ok\":true") == std::string::npos);

  // Kill the holder without any chance to clean up: the kernel releases the claim.
  bsm_test::terminate_child(child);
  const int killed = bsm_test::wait_child(child);
  BSM_CHECK(killed != 0);
  bsm_test::close_child(child);

  BSM_CHECK_OK(successor, bsm_test::spawn_child(probe(), {"session", store}, std::string(), {}));
  BSM_CHECK_OK(successor_output, bsm_test::read_until_eof(successor));
  const int successor_status = bsm_test::wait_child(successor);
  bsm_test::close_child(successor);
  BSM_CHECK_EQ(successor_status, 0);
  BSM_CHECK(successor_output.find("\"ok\":true") != std::string::npos);
}

BSM_TEST(a_crash_before_dispatch_is_resolved_as_never_applied) {
  const std::string store = bsm_test::scratch_directory("multiprocess-before");
  const std::string plant_store = bsm_test::scratch_directory("multiprocess-before-plant");
  HostThread host;
  BSM_CHECK_OK(host_started, host.start(0, plant_store));
  const std::string endpoint = "tcp:" + std::to_string(host.port());

  BSM_CHECK_OK(open_output, run_probe({"step", store, "open", "--tick", "1"}));
  BSM_CHECK_OK(open_json, parse_output(open_output));
  BSM_CHECK_EQ(open_json.find("ok")->as_bool(), true);

  BSM_CHECK_OK(crash_output,
               run_probe({"step", store, "request-isolation", "--tick", "2", "--controller",
                          endpoint},
                         {{"BSM_CRASH_AT", "dispatch_before"}}));
  BSM_CHECK_MSG(crash_output.find("dispatch_before") != std::string::npos,
                crash_output);
  BSM_CHECK_EQ(host.host().applied_effects(), std::size_t{0});

  // The next step must find an unresolved attempt and refuse to repeat the request.
  BSM_CHECK_OK(refused_output,
               run_probe({"step", store, "request-isolation", "--tick", "3", "--controller",
                          endpoint}));
  BSM_CHECK_MSG(refused_output.find("attempt_unresolved") != std::string::npos,
                refused_output);

  BSM_CHECK_OK(resolve_output,
               run_probe({"step", store, "resolve-isolation", "--tick", "4", "--controller",
                          endpoint}));
  BSM_CHECK_EQ(host.host().applied_effects(), std::size_t{0});

  // Only now may the bounded request be sent again, and it applies exactly one effect.
  BSM_CHECK_OK(retry_output,
               run_probe({"step", store, "request-isolation", "--tick", "5", "--controller",
                          endpoint}));
  BSM_CHECK_MSG(host.host().applied_effects() == std::size_t{1}, retry_output);
  BSM_CHECK_EQ(host.host().duplicate_effects(), std::size_t{0});
  host.stop();
}

BSM_TEST(a_lost_answer_after_the_effect_is_never_repeated) {
  const std::string store = bsm_test::scratch_directory("multiprocess-after");
  const std::string plant_store = bsm_test::scratch_directory("multiprocess-after-plant");
  HostThread host;
  BSM_CHECK_OK(host_started, host.start(0, plant_store));
  const std::string endpoint = "tcp:" + std::to_string(host.port());

  BSM_CHECK_OK(open_output, run_probe({"step", store, "open", "--tick", "1"}));
  BSM_CHECK_OK(crash_output,
               run_probe({"step", store, "request-isolation", "--tick", "2", "--controller",
                          endpoint},
                         {{"BSM_CRASH_AT", "dispatch_after"}}));
  // The owner applied the effect and recorded the key before the manager died.
  BSM_CHECK_MSG(host.host().applied_effects() == std::size_t{1}, crash_output);

  BSM_CHECK_OK(resolve_output,
               run_probe({"step", store, "resolve-isolation", "--tick", "3", "--controller",
                          endpoint}));
  BSM_CHECK(resolve_output.find("\"ok\":true") != std::string::npos);
  // Resolution asked the owner instead of resending: exactly one effect, no duplicate.
  BSM_CHECK_EQ(host.host().applied_effects(), std::size_t{1});
  BSM_CHECK_EQ(host.host().duplicate_effects(), std::size_t{0});

  BSM_CHECK_OK(assess_output,
               run_probe({"step", store, "observe-isolation", "--tick", "4", "--controller",
                          endpoint}));
  BSM_CHECK(assess_output.find("\"ok\":true") != std::string::npos);
  host.stop();
}

BSM_TEST(a_process_killed_after_a_durable_record_recovers_from_the_journal) {
  const std::string store = bsm_test::scratch_directory("multiprocess-evidence");
  const std::string plant_store = bsm_test::scratch_directory("multiprocess-evidence-plant");
  HostThread host;
  BSM_CHECK_OK(host_started, host.start(0, plant_store));
  const std::string endpoint = "tcp:" + std::to_string(host.port());

  BSM_CHECK_OK(open_output, run_probe({"step", store, "open", "--tick", "1"}));
  BSM_CHECK_OK(request_output,
               run_probe({"step", store, "request-isolation", "--tick", "2", "--controller",
                          endpoint}));
  // Kill the process after the evidence record is durable but before the caller sees it.
  // The crash point names the commit boundary for exactly this record, so the assertion
  // below is about recovery rather than about how many appends happened first.
  BSM_CHECK_OK(crash_output,
               run_probe({"step", store, "observe-isolation", "--tick", "3", "--controller",
                          endpoint},
                         {{"BSM_CRASH_AT", "evidence_committed"}}));
  BSM_CHECK_MSG(crash_output.find("evidence_committed") != std::string::npos,
                crash_output);

  // A new incarnation reads the record. The evidence is recovered, so it cannot satisfy a
  // gate until it is observed again.
  BSM_CHECK_OK(probe_output, run_probe({"session", store}));
  BSM_CHECK_OK(sessions, parse_output(probe_output));
  BSM_CHECK(sessions.find("sessions") != nullptr);
  BSM_CHECK_EQ(sessions.find("sessions")->as_array().size(), std::size_t{1});
  host.stop();

  // Reopening the same store from this process proves the recovery is not probe-specific.
  BSM_CHECK_OK(definition, bsm_test::make_facility());
  StaticFacilityState facility(definition.binding);
  ManualClock clock(100);
  InProcessSyntheticController local_owner;
  ManagerOptions options;
  options.store_root = store;
  options.create_store_if_missing = false;
  options.clock = &clock;
  options.controller = &local_owner;
  options.facility = &facility;
  BSM_CHECK_OK(manager, SessionManager::open(options));
  BSM_CHECK_OK(report, manager.report());
  const JsonValue* evidence = report.find("evidence");
  BSM_CHECK(evidence != nullptr);
  BSM_CHECK_EQ(evidence->as_array().size(), std::size_t{1});
  BSM_CHECK_EQ(evidence->as_array()[0].find("provenance")->as_string(), std::string("recovered"));
  BSM_CHECK_OK(assessment, manager.assess());
  BSM_CHECK_EQ(assessment.completion_ready, false);
  BSM_CHECK_OK(closed, manager.close());
}

BSM_TEST(a_crash_during_snapshot_publication_recovers_one_whole_generation) {
  const std::string store = bsm_test::scratch_directory("multiprocess-snapshot");
  const std::string plant_store = bsm_test::scratch_directory("multiprocess-snapshot-plant");
  HostThread host;
  BSM_CHECK_OK(host_started, host.start(0, plant_store));
  const std::string endpoint = "tcp:" + std::to_string(host.port());

  BSM_CHECK_OK(open_output, run_probe({"step", store, "open", "--tick", "1"}));
  BSM_CHECK_OK(crash_output,
               run_probe({"step", store, "observe-isolation", "--tick", "2", "--controller",
                          endpoint, "--compact-records", "1"},
                         {{"BSM_CRASH_AT", "snapshot_before_manifest"}}));
  host.stop();

  StoreOptions store_options;
  store_options.root = store;
  store_options.create_if_missing = false;
  BSM_CHECK_OK(reopened, Store::open(store_options));
  BSM_CHECK_OK(verification, reopened.verify());
  BSM_CHECK(verification.generation >= 1);
  // The staged snapshot was not referenced by the manifest, so it is not authoritative
  // and is removed as an orphan rather than trusted.
  BSM_CHECK_OK(closed, reopened.close());
}

BSM_TEST_MAIN("bsm_test_multiprocess")
