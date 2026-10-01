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

// Proof obligations: the packaged command line tools work as real processes. The operator
// command line drives one durable operation per invocation, the adjacent owner runs in its
// own process with its own durable applied-key ledger, and killing and restarting that
// owner does not lose the identity of a request that was already applied.

#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "black_start_manager/remote_controller.hpp"
#include "fixture.hpp"
#include "proc.hpp"
#include "test_harness.hpp"

using namespace black_start_manager;
using namespace bsm_test;

namespace {

[[nodiscard]] Result<bsm_test::ChildProcess> start_plant(const std::string& store,
                                                        std::uint16_t& port,
                                                        bool lose_reply) {
  std::vector<std::string> arguments = {"--store", store, "--port", "0"};
  if (lose_reply) {
    arguments.push_back("--lose-reply");
    arguments.push_back("verify-isolation");
  }
  BSM_TRY_ASSIGN(child, bsm_test::spawn_child(std::string(BSM_PLANT_EXECUTABLE), arguments,
                                              std::string(), {}));
  BSM_TRY_ASSIGN(ready, bsm_test::read_line(child));
  BSM_TRY_ASSIGN(value, parse_json(ready));
  port = static_cast<std::uint16_t>(value.find("port")->as_integer());
  if (port == 0) {
    return Error(ErrorCode::InternalError, "the plant host reported no port");
  }
  return child;
}

struct Run {
  int status = -1;
  std::string output;
};

// Owns a child process and terminates it on destruction, so a failing check never leaves a
// stray owner behind.
class ChildGuard {
 public:
  explicit ChildGuard(bsm_test::ChildProcess child) : child_(child) {}
  ChildGuard(const ChildGuard&) = delete;
  ChildGuard& operator=(const ChildGuard&) = delete;
  ~ChildGuard() {
    if (child_.valid) {
      bsm_test::terminate_child(child_);
      const int status = bsm_test::wait_child(child_);
      (void)status;
      bsm_test::close_child(child_);
    }
  }
  [[nodiscard]] bsm_test::ChildProcess& child() { return child_; }

 private:
  bsm_test::ChildProcess child_;
};

[[nodiscard]] Result<Run> run_cli(const std::vector<std::string>& arguments) {
  BSM_TRY_ASSIGN(child, bsm_test::spawn_child(std::string(BSM_CLI_EXECUTABLE), arguments,
                                              std::string(), {}));
  BSM_TRY_ASSIGN(output, bsm_test::read_until_eof(child));
  Run run;
  run.output = output;
  run.status = bsm_test::wait_child(child);
  bsm_test::close_child(child);
  return run;
}

[[nodiscard]] Result<JsonValue> body_of(const Run& run) {
  const std::size_t start = run.output.find('{');
  const std::size_t end = run.output.rfind('}');
  if (start == std::string::npos || end == std::string::npos || end < start) {
    return Error(ErrorCode::IoFailure, "the command produced no canonical JSON: " +
                                           run.output);
  }
  return parse_json(run.output.substr(start, end - start + 1));
}

[[nodiscard]] Result<Unit> write_text(const std::string& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return Error(ErrorCode::IoFailure, "cannot write '" + path + "'");
  }
  output << text;
  return Unit{};
}

}  // namespace

BSM_TEST(the_command_line_and_the_owner_survive_an_owner_restart) {
  const std::string store = bsm_test::scratch_directory("cli-store");
  const std::string plant_store = bsm_test::scratch_directory("cli-plant");
  BSM_CHECK_OK(definition, bsm_test::make_facility());
  BSM_CHECK_OK(plan_json, definition.plan.to_json());
  BSM_CHECK_OK(state_json, binding_to_json(definition.binding));
  BSM_CHECK_OK(control_json, authority_to_json(definition.control));
  BSM_CHECK_OK(electrical_json, authority_to_json(definition.electrical));
  const std::string documents = bsm_test::scratch_directory("cli-documents");
  const std::string state_path = documents + "/state.json";
  const std::string plan_path = documents + "/plan.json";
  const std::string control_path = documents + "/control.json";
  const std::string electrical_path = documents + "/electrical.json";
  BSM_CHECK_OK(wrote_plan, write_text(plan_path, canonical_json(plan_json)));
  BSM_CHECK_OK(wrote_state, write_text(state_path, canonical_json(state_json)));
  BSM_CHECK_OK(wrote_control, write_text(control_path, canonical_json(control_json)));
  BSM_CHECK_OK(wrote_electrical,
               write_text(electrical_path, canonical_json(electrical_json)));

  std::uint16_t port = 0;
  BSM_CHECK_OK(plant_child, start_plant(plant_store, port, true));
  ChildGuard plant(std::move(plant_child));
  (void)plant;
  const std::string endpoint = "tcp:" + std::to_string(port);
  const std::string store_argument = "--store";
  const std::string state_argument = "--state";

  BSM_CHECK_OK(open_run,
               run_cli({"open", store_argument, store, "--create", state_argument,
                        "@" + state_path, "--plan", "@" + plan_path, "--authority",
                        "@" + control_path, "--tick", "1"}));
  BSM_CHECK_MSG(open_run.status == 0, open_run.output);
  BSM_CHECK_OK(open_body, body_of(open_run));
  BSM_CHECK_EQ(open_body.find("ok")->as_bool(), true);
  const std::string session = open_body.find("id")->as_string();

  // A blocked obligation is refused with exit status 1 and a named block.
  BSM_CHECK_OK(blocked_run,
               run_cli({"request", store_argument, store, state_argument, "@" + state_path,
                        "--session", session, "--adopt", "@" + control_path, "--obligation",
                        "control-power", "--authority", "@" + control_path, "--tick",
                        "2"}));
  BSM_CHECK_EQ(blocked_run.status, 1);
  BSM_CHECK_OK(blocked_body, body_of(blocked_run));
  BSM_CHECK_EQ(blocked_body.find("ok")->as_bool(), false);

  BSM_CHECK_OK(request_run,
               run_cli({"request", store_argument, store, state_argument, "@" + state_path,
                        "--session", session, "--adopt", "@" + control_path, "--obligation",
                        "isolation-a", "--authority", "@" + electrical_path,
                        "--controller", endpoint, "--tick", "3"}));
  BSM_CHECK_EQ(request_run.status, 0);
  BSM_CHECK_OK(request_body, body_of(request_run));
  const JsonValue* attempt = request_body.find("attempt");
  BSM_CHECK(attempt != nullptr);
  BSM_CHECK_EQ(attempt->find("state")->as_string(), std::string("unresolved"));
  const std::string attempt_id = attempt->find("id")->as_string();

  // Kill the adjacent owner and start it again from the same durable ledger.
  bsm_test::terminate_child(plant.child());
  const int plant_status = bsm_test::wait_child(plant.child());
  BSM_CHECK(plant_status != 0);
  bsm_test::close_child(plant.child());

  std::uint16_t second_port = 0;
  BSM_CHECK_OK(restarted_child, start_plant(plant_store, second_port, false));
  ChildGuard restarted(std::move(restarted_child));
  (void)restarted;
  const std::string second_endpoint = "tcp:" + std::to_string(second_port);
  BSM_CHECK(!(second_port == port));

  // Resolution asks the restarted owner, which still remembers the applied key.
  BSM_CHECK_OK(resolve_run,
               run_cli({"resolve", store_argument, store, state_argument, "@" + state_path,
                        "--session", session, "--adopt", "@" + control_path, "--attempt",
                        attempt_id, "--authority", "@" + control_path, "--controller",
                        second_endpoint, "--tick", "4"}));
  BSM_CHECK_EQ(resolve_run.status, 0);
  BSM_CHECK_OK(resolve_body, body_of(resolve_run));
  BSM_CHECK_EQ(resolve_body.find("attempt")->find("state")->as_string(),
               std::string("acknowledged"));

  BSM_CHECK_OK(observe_run,
               run_cli({"observe", store_argument, store, state_argument, "@" + state_path,
                        "--session", session, "--adopt", "@" + control_path, "--kind",
                        "isolation_verified", "--subject", "isolation:a", "--obligation",
                        "isolation-a", "--observer", "electrical-owner", "--authority",
                        "@" + electrical_path, "--controller", second_endpoint, "--tick",
                        "5"}));
  BSM_CHECK_MSG(observe_run.status == 0, observe_run.output);

  // The observation satisfied the readiness gate in the incarnation that made it.
  BSM_CHECK_EQ(observe_run.status, 0);
  BSM_CHECK_OK(observe_body, body_of(observe_run));
  const JsonValue* observed_assessment = observe_body.find("assessment");
  BSM_CHECK(observed_assessment != nullptr);
  BSM_CHECK_MSG(observed_assessment->find("obligations")
                        ->as_array()[0]
                        .find("satisfied")
                        ->as_bool(),
                canonical_json(observe_body));

  // A later incarnation does not promote the earlier observation: recovered evidence is
  // never current, and the obligation has to be observed again by the acting incarnation.
  BSM_CHECK_OK(assess_run,
               run_cli({"assess", store_argument, store, state_argument, "@" + state_path,
                        "--session", session, "--tick", "6"}));
  BSM_CHECK_EQ(assess_run.status, 0);
  BSM_CHECK_OK(assess_body, body_of(assess_run));
  BSM_CHECK_EQ(assess_body.find("obligations")->as_array()[0].find("satisfied")->as_bool(),
               false);
  BSM_CHECK_EQ(assess_body.find("obligations")
                   ->as_array()[0]
                   .find("deficits")
                   ->as_array()[0]
                   .find("deficit")
                   ->as_string(),
               std::string("recovered_only"));

  BSM_CHECK_OK(advance_run,
               run_cli({"advance", store_argument, store, state_argument, "@" + state_path,
                        "--session", session, "--adopt", "@" + control_path, "--authority",
                        "@" + control_path, "--tick", "7"}));
  BSM_CHECK_MSG(advance_run.status == 1, advance_run.output);
  BSM_CHECK_OK(advance_body, body_of(advance_run));
  BSM_CHECK_EQ(advance_body.find("ok")->as_bool(), false);

  BSM_CHECK_OK(verify_run, run_cli({"verify", store_argument, store}));
  BSM_CHECK_EQ(verify_run.status, 0);
  BSM_CHECK_OK(verify_body, body_of(verify_run));
  BSM_CHECK(verify_body.find("ok")->as_bool());

  BSM_CHECK_OK(report_run,
               run_cli({"report", store_argument, store, state_argument, "@" + state_path,
                        "--session", session, "--tick", "8"}));
  BSM_CHECK_EQ(report_run.status, 0);
  BSM_CHECK_OK(report_body, body_of(report_run));
  BSM_CHECK_EQ(report_body.find("attempts")->as_array().size(), std::size_t{1});
  BSM_CHECK_EQ(report_body.find("evidence")->as_array().size(), std::size_t{1});

}

BSM_TEST(the_restoration_driver_returns_the_facility_to_service) {
  const std::string store = bsm_test::scratch_directory("driver-store");
  const std::string plant_store = bsm_test::scratch_directory("driver-plant");
  const std::string documents = bsm_test::scratch_directory("driver-documents");
  BSM_CHECK_OK(definition, bsm_test::make_facility());
  BSM_CHECK_OK(plan_json, definition.plan.to_json());
  BSM_CHECK_OK(state_json, binding_to_json(definition.binding));
  BSM_CHECK_OK(control_json, authority_to_json(definition.control));
  BSM_CHECK_OK(electrical_json, authority_to_json(definition.electrical));
  BSM_CHECK_OK(cooling_json, authority_to_json(definition.cooling));
  BSM_CHECK_OK(facility_json, authority_to_json(definition.facility_owner));
  BSM_CHECK_OK(instrumentation_json, authority_to_json(definition.instrumentation));
  const std::string plan_path = documents + "/plan.json";
  const std::string state_path = documents + "/state.json";
  const std::string control_path = documents + "/control.json";
  const std::string electrical_path = documents + "/electrical.json";
  const std::string cooling_path = documents + "/cooling.json";
  const std::string facility_path = documents + "/facility.json";
  const std::string instrumentation_path = documents + "/instrumentation.json";
  BSM_CHECK_OK(wrote_plan, write_text(plan_path, canonical_json(plan_json)));
  BSM_CHECK_OK(wrote_state, write_text(state_path, canonical_json(state_json)));
  BSM_CHECK_OK(wrote_control, write_text(control_path, canonical_json(control_json)));
  BSM_CHECK_OK(wrote_electrical,
               write_text(electrical_path, canonical_json(electrical_json)));
  BSM_CHECK_OK(wrote_cooling, write_text(cooling_path, canonical_json(cooling_json)));
  BSM_CHECK_OK(wrote_facility, write_text(facility_path, canonical_json(facility_json)));
  BSM_CHECK_OK(wrote_instrumentation,
               write_text(instrumentation_path, canonical_json(instrumentation_json)));

  std::uint16_t port = 0;
  BSM_CHECK_OK(plant_child, start_plant(plant_store, port, false));
  ChildGuard plant(std::move(plant_child));
  (void)plant;
  const std::string endpoint = "tcp:" + std::to_string(port);
  const std::string store_argument = "--store";

  BSM_CHECK_OK(open_run,
               run_cli({"open", store_argument, store, "--create", "--state",
                        "@" + state_path, "--plan", "@" + plan_path, "--authority",
                        "@" + control_path, "--tick", "1"}));
  BSM_CHECK_MSG(open_run.status == 0, open_run.output);
  BSM_CHECK_OK(open_body, body_of(open_run));
  const std::string session = open_body.find("id")->as_string();

  BSM_CHECK_OK(run,
               run_cli({"run", store_argument, store, "--state", "@" + state_path,
                        "--plan", "@" + plan_path, "--session", session, "--adopt",
                        "@" + control_path, "--authority", "@" + control_path,
                        "--authority", "@" + electrical_path, "--authority",
                        "@" + facility_path, "--authority", "@" + cooling_path,
                        "--authority", "@" + instrumentation_path,
                        "--controller", endpoint, "--deadline", "50", "--tick", "2"}));
  BSM_CHECK_MSG(run.status == 0, run.output);
  BSM_CHECK_OK(run_body, body_of(run));
  const JsonValue* final_assessment = run_body.find("assessment");
  BSM_CHECK(final_assessment != nullptr);
  BSM_CHECK_MSG(final_assessment->find("state")->as_string() == std::string("completed"),
                canonical_json(run_body));
  BSM_CHECK_EQ(final_assessment->find("stage_index")->as_integer(), 2);
  // A completed session has nothing left to complete, and it reports exactly that.
  BSM_CHECK_EQ(final_assessment->find("completion_ready")->as_bool(), false);
  BSM_CHECK_EQ(final_assessment->find("completion_missing_obligations")->as_array().size(),
               std::size_t{0});
  BSM_CHECK_EQ(final_assessment->find("primary_block")->as_string(),
               std::string("session_completed"));
  // The driver performed the whole protocol: requests, stage publications, and the final
  // return-to-service proof.
  bool advanced = false;
  bool completed = false;
  std::size_t requested = 0;
  for (const JsonValue& step : run_body.find("steps")->as_array()) {
    const JsonValue* action = step.find("action");
    if (action == nullptr) {
      continue;
    }
    if (action->as_string() == "advanced") {
      advanced = true;
    } else if (action->as_string() == "completed") {
      completed = true;
    } else if (action->as_string() == "requested") {
      ++requested;
    }
  }
  BSM_CHECK(advanced);
  BSM_CHECK(completed);
  BSM_CHECK_EQ(requested, std::size_t{6});
  BSM_CHECK(run_body.find("actions")->as_integer() >= 8);

  // The owner applied exactly one consequential effect per obligation and never repeated
  // one, which the ledger proves after the fact.
  BSM_CHECK_OK(verify_run, run_cli({"verify", store_argument, store}));
  BSM_CHECK_EQ(verify_run.status, 0);
}

BSM_TEST(the_command_line_reports_usage_errors_and_version) {
  BSM_CHECK_OK(version_run, run_cli({"version"}));
  BSM_CHECK_EQ(version_run.status, 0);
  BSM_CHECK_OK(version_body, body_of(version_run));
  BSM_CHECK_EQ(version_body.find("dccp_boundary")->as_integer(), 56);
  BSM_CHECK(version_body.find("state_format_version") != nullptr);

  BSM_CHECK_OK(unknown_run, run_cli({"nonsense"}));
  BSM_CHECK_EQ(unknown_run.status, 2);

  BSM_CHECK_OK(missing_store_run, run_cli({"assess"}));
  BSM_CHECK_EQ(missing_store_run.status, 2);
}

BSM_TEST_MAIN("bsm_test_cli")
