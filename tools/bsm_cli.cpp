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

// bsm: the black-start operator command line.
//
// Every command opens the durable store, performs one bounded operation through the
// SessionManager public API, and closes it. The durable store is the only state two
// invocations share, which is what makes the command line usable for crash and restart
// walkthroughs.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "black_start_manager/controller.hpp"
#include "black_start_manager/manager.hpp"
#include "black_start_manager/remote_controller.hpp"
#include "black_start_manager/version.hpp"

namespace {

using namespace black_start_manager;

struct Options {
  std::map<std::string, std::string> values;
  // A flag may be repeated (for example one --authority per adjacent owner); every value is
  // kept in order, and the last one is also the plain value.
  std::map<std::string, std::vector<std::string>> lists;
  std::vector<std::string> flags;

  [[nodiscard]] bool has(const std::string& name) const {
    return values.count(name) != 0 ||
           std::find(flags.begin(), flags.end(), name) != flags.end();
  }
  [[nodiscard]] std::string get(const std::string& name,
                                const std::string& fallback = std::string()) const {
    const auto found = values.find(name);
    return found == values.end() ? fallback : found->second;
  }
  [[nodiscard]] std::uint64_t get_unsigned(const std::string& name,
                                           std::uint64_t fallback) const {
    const auto found = values.find(name);
    if (found == values.end()) {
      return fallback;
    }
    return static_cast<std::uint64_t>(std::stoull(found->second));
  }
};

void usage() {
  std::cout << "bsm " << kVersionString
            << " - dependency-gated black-start authority protocol (DCCP boundary "
            << kDccpBoundary << ")\n"
            << "\n"
            << "usage: bsm <command> [options]\n"
            << "\n"
            << "commands:\n"
            << "  verify      re-read the store and verify every digest and chain\n"
            << "  status      store recovery and verification summary\n"
            << "  open        establish a restoration session from a plan and state\n"
            << "  assess      explain what may happen next\n"
            << "  request     send one bounded request to an adjacent owner\n"
            << "  resolve     resolve an unresolved attempt by querying the owner\n"
            << "  observe     ask an adjacent owner for a readiness observation\n"
            << "  evidence    record a readiness observation supplied by an operator\n"
            << "  advance     publish the transition to the next restoration stage\n"
            << "  complete    publish the return-to-service proof and close the session\n"
            << "  hold        hold the session\n"
            << "  resume      release the current hold\n"
            << "  replan      bind a new plan and facility state to the session\n"
            << "  abort       abort the session\n"
            << "  report      full audit lineage for one session\n"
            << "  run         drive restoration inside one incarnation until it stops\n"
            << "  version     print the version and boundary\n"
            << "\n"
            << "common options:\n"
            << "  --store DIR            durable store directory (required)\n"
            << "  --create               create the store when it does not exist\n"
            << "  --state JSON|@FILE     current facility state document\n"
            << "  --plan JSON|@FILE      restoration plan document\n"
            << "  --session HEX          session identity (default: the only session)\n"
            << "  --authority JSON|@FILE authority the caller acts under\n"
            << "  --adopt JSON|@FILE     control authority that adopts the observed state\n"
            << "  --tick N               logical tick for this command (default 1)\n"
            << "  --controller tcp:PORT  out-of-process adjacent owner\n"
            << "  --seed N               in-process synthetic owner seed\n"
            << "  --out FILE             write the canonical result to a file as well\n"
            << "  --compact-bytes N      journal compaction threshold in bytes\n"
            << "  --compact-records N    journal compaction threshold in records\n"
            << "\n"
            << "exit status: 0 success, 1 refused by the boundary, 2 usage error.\n";
}

[[nodiscard]] Result<std::string> read_text_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Error(ErrorCode::IoFailure, "cannot read '" + path + "'");
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

// A value may be given inline or as @path, which keeps long JSON out of the command line.
[[nodiscard]] Result<std::string> resolve_value(const std::string& value) {
  if (!value.empty() && value.front() == '@') {
    return read_text_file(value.substr(1));
  }
  return value;
}

[[nodiscard]] Result<JsonValue> parse_argument_json(const std::string& name,
                                                    const std::string& value) {
  BSM_TRY_ASSIGN(text, resolve_value(value));
  BSM_TRY_ASSIGN(parsed, parse_json(text));
  if (!parsed.is_object()) {
    return Error(ErrorCode::SchemaViolation,
                 "the --" + name + " argument must be a JSON object");
  }
  return parsed;
}

[[nodiscard]] Result<AuthorityRef> parse_authority(const Options& options) {
  if (!options.has("--authority")) {
    return Error(ErrorCode::InvalidArgument, "--authority is required for this command");
  }
  BSM_TRY_ASSIGN(value, parse_argument_json("authority", options.get("--authority")));
  return authority_from_json(value);
}

// Every authority the caller presents, in the order given.
[[nodiscard]] Result<std::vector<AuthorityRef>> parse_authorities(const Options& options,
                                                                 const char* name) {
  std::vector<AuthorityRef> authorities;
  const auto found = options.lists.find(name);
  if (found == options.lists.end()) {
    return Error(ErrorCode::InvalidArgument, std::string(name) + " is required");
  }
  for (const std::string& text : found->second) {
    BSM_TRY_ASSIGN(document, parse_argument_json(name, text));
    BSM_TRY_ASSIGN(authority, authority_from_json(document));
    authorities.push_back(std::move(authority));
  }
  return authorities;
}

[[nodiscard]] Result<FacilityBinding> parse_state(const Options& options) {
  if (!options.has("--state")) {
    return Error(ErrorCode::InvalidArgument, "--state is required for this command");
  }
  BSM_TRY_ASSIGN(value, parse_argument_json("state", options.get("--state")));
  return binding_from_json(value);
}

[[nodiscard]] Result<PlanDocument> parse_plan(const Options& options) {
  if (!options.has("--plan")) {
    return Error(ErrorCode::InvalidArgument, "--plan is required for this command");
  }
  BSM_TRY_ASSIGN(text, resolve_value(options.get("--plan")));
  return PlanDocument::parse(text);
}

[[nodiscard]] Result<SessionId> parse_session(const Options& options) {
  if (!options.has("--session")) {
    return SessionId();
  }
  return SessionId::from_hex(options.get("--session"));
}

// The command line owns either an in-process synthetic owner or a real out-of-process
// owner. The in-process owner is honest about its limits: its applied-key memory ends
// with this process, so a later invocation learns nothing from it.
struct Environment {
  std::unique_ptr<ManualClock> clock;
  std::unique_ptr<InProcessSyntheticController> in_process;
  std::unique_ptr<RemoteControllerClient> remote;
  std::unique_ptr<StaticFacilityState> facility;
  AdjacentController* controller = nullptr;
  bool in_process_owner = true;

  [[nodiscard]] Result<Unit> setup(const Options& options, FacilityBinding binding) {
    clock = std::make_unique<ManualClock>(options.get_unsigned("--tick", 1));
    facility = std::make_unique<StaticFacilityState>(std::move(binding));
    if (options.has("--controller")) {
      const std::string endpoint = options.get("--controller");
      if (endpoint.rfind("tcp:", 0) != 0) {
        return Error(ErrorCode::InvalidArgument, "--controller takes the form tcp:PORT");
      }
      RemoteControllerOptions remote_options;
      remote_options.port =
          static_cast<std::uint16_t>(std::stoul(endpoint.substr(4)));
      BSM_TRY_ASSIGN(caller, OwnerId::parse("black-start-manager"));
      remote_options.caller = std::move(caller);
      remote = std::make_unique<RemoteControllerClient>();
      BSM_RETURN_IF_ERROR(remote->connect(remote_options));
      controller = remote.get();
      in_process_owner = false;
      return Unit{};
    }
    SyntheticControllerPolicy policy;
    policy.seed = options.get_unsigned("--seed", 0);
    in_process = std::make_unique<InProcessSyntheticController>(policy);
    controller = in_process.get();
    return Unit{};
  }

  [[nodiscard]] Result<SessionManager> open_manager(const Options& options) {
    ManagerOptions manager_options;
    manager_options.store_root = options.get("--store");
    manager_options.create_store_if_missing = options.has("--create");
    manager_options.clock = clock.get();
    manager_options.controller = controller;
    manager_options.facility = facility.get();
    if (options.has("--compact-bytes")) {
      manager_options.compact_journal_bytes = options.get_unsigned("--compact-bytes", 0);
    }
    if (options.has("--compact-records")) {
      manager_options.compact_journal_records =
          static_cast<std::size_t>(options.get_unsigned("--compact-records", 0));
    }
    return SessionManager::open(manager_options);
  }
};

[[nodiscard]] int fail(const Error& error) {
  JsonObjectBuilder root;
  root.set_bool("ok", false);
  root.set_text("code", to_string(error.code()));
  root.set_text("message", error.message());
  if (!error.detail_json().empty()) {
    const Result<JsonValue> detail = parse_json(error.detail_json());
    if (detail.ok()) {
      root.set("detail", detail.value());
    }
  }
  const Result<JsonValue> value = root.build();
  if (value.ok()) {
    std::cout << canonical_json(value.value()) << '\n';
  } else {
    std::cout << "{\"ok\":false,\"code\":\"internal_error\"}\n";
  }
  return 1;
}

[[nodiscard]] Result<JsonValue> success(JsonValue body) {
  if (!body.is_object()) {
    return Error(ErrorCode::InternalError, "result body is not an object");
  }
  JsonObjectBuilder root;
  root.set_bool("ok", true);
  for (const JsonValue::Field& field : body.as_object()) {
    root.set(field.first, field.second);
  }
  return root.build();
}

[[nodiscard]] int emit(const Options& options, const JsonValue& value) {
  const std::string text = canonical_json(value);
  std::cout << text << '\n';
  if (options.has("--out")) {
    std::ofstream output(options.get("--out"), std::ios::binary | std::ios::trunc);
    if (!output) {
      std::cerr << "bsm: cannot write '" << options.get("--out") << "'\n";
      return 2;
    }
    output << text << '\n';
  }
  return 0;
}

struct Positional {
  std::string command;
  Options options;
};

[[nodiscard]] Result<Positional> parse_arguments(int argc, char** argv) {
  Positional parsed;
  if (argc < 2) {
    return Error(ErrorCode::InvalidArgument, "no command given");
  }
  parsed.command = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument.rfind("--", 0) != 0) {
      return Error(ErrorCode::InvalidArgument, "unexpected argument '" + argument + "'");
    }
    if (index + 1 < argc && std::string(argv[index + 1]).rfind("--", 0) != 0) {
      parsed.options.values[argument] = argv[index + 1];
      parsed.options.lists[argument].push_back(argv[index + 1]);
      ++index;
    } else {
      parsed.options.flags.push_back(argument);
    }
  }
  return parsed;
}

[[nodiscard]] Result<SessionId> resolve_session(SessionManager& manager,
                                                const Options& options) {
  BSM_TRY_ASSIGN(named, parse_session(options));
  if (!named.is_zero()) {
    return named;
  }
  return manager.active_session();
}

}  // namespace

int main(int argc, char** argv) {
  const Result<Positional> parsed = parse_arguments(argc, argv);
  if (!parsed.ok()) {
    usage();
    return 2;
  }
  const std::string command = parsed.value().command;
  const Options& options = parsed.value().options;
  if (command == "help" || command == "--help" || command == "-h") {
    usage();
    return 0;
  }
  if (command == "version") {
    JsonObjectBuilder root;
    root.set_text("version", kVersionString);
    root.set_uint("dccp_boundary", static_cast<std::uint64_t>(kDccpBoundary));
    root.set_uint("state_format_version", static_cast<std::uint64_t>(kStateFormatVersion));
    const Result<JsonValue> value = root.build();
    if (!value.ok()) {
      return fail(value.error());
    }
    return emit(options, value.value());
  }
  if (!options.has("--store")) {
    std::cerr << "bsm: --store is required\n";
    usage();
    return 2;
  }

  // verify and status read the store without needing a facility state document.
  if (command == "verify" || command == "status") {
    ManagerOptions manager_options;
    manager_options.store_root = options.get("--store");
    manager_options.create_store_if_missing = false;
    ManualClock clock(1);
    InProcessSyntheticController controller;
    FacilityBinding empty;
    StaticFacilityState facility(empty);
    manager_options.clock = &clock;
    manager_options.controller = &controller;
    manager_options.facility = &facility;
    Result<SessionManager> manager = SessionManager::open(manager_options);
    if (!manager.ok()) {
      return fail(manager.error());
    }
    const Result<JsonValue> body =
        command == "verify" ? manager.value().verify_store() : manager.value().status();
    if (!body.ok()) {
      return fail(body.error());
    }
    const Result<JsonValue> result = success(body.value());
    return result.ok() ? emit(options, result.value()) : fail(result.error());
  }

  const Result<FacilityBinding> binding = parse_state(options);
  if (!binding.ok()) {
    return fail(binding.error());
  }
  Environment environment;
  const Result<Unit> prepared = environment.setup(options, binding.value());
  if (!prepared.ok()) {
    return fail(prepared.error());
  }
  Result<SessionManager> manager = environment.open_manager(options);
  if (!manager.ok()) {
    return fail(manager.error());
  }
  if (environment.in_process_owner &&
      (command == "resolve" || command == "request")) {
    std::cerr << "bsm: this command uses the in-process synthetic owner; its applied-key "
                 "memory ends when this process exits. Pass --controller tcp:PORT for a "
                 "walkthrough that spans processes.\n";
  }
  SessionManager& session_manager = manager.value();

  const auto finish = [&options](const Result<JsonValue>& body) -> int {
    if (!body.ok()) {
      return fail(body.error());
    }
    const Result<JsonValue> result = success(body.value());
    return result.ok() ? emit(options, result.value()) : fail(result.error());
  };

  if (command == "open") {
    const Result<PlanDocument> plan = parse_plan(options);
    if (!plan.ok()) {
      return fail(plan.error());
    }
    const Result<AuthorityRef> authority = parse_authority(options);
    if (!authority.ok()) {
      return fail(authority.error());
    }
    EstablishSessionRequest request;
    request.plan = plan.value();
    request.binding = binding.value();
    request.authority = authority.value();
    request.reason = options.get("--reason");
    request.supersede_existing = options.has("--supersede");
    const Result<SessionView> view = session_manager.establish_session(request);
    if (!view.ok()) {
      return fail(view.error());
    }
    const Result<JsonValue> body = session_view_to_json(view.value());
    return finish(body);
  }

  const Result<SessionId> session = resolve_session(session_manager, options);
  if (!session.ok()) {
    return fail(session.error());
  }

  // Process authority is not inherited across incarnations: a mutating command that runs
  // in a fresh process must adopt the observed facility state explicitly. --adopt is the
  // operator saying so for this command, and it names the control authority that owns the
  // restoration process.
  if (options.has("--adopt") && command != "reestablish") {
    const Result<JsonValue> adopted_document =
        parse_argument_json("adopt", options.get("--adopt"));
    if (!adopted_document.ok()) {
      return fail(adopted_document.error());
    }
    const Result<AuthorityRef> presented = authority_from_json(adopted_document.value());
    if (!presented.ok()) {
      return fail(presented.error());
    }
    ReestablishRequest adoption;
    adoption.session = session.value();
    adoption.authority = presented.value();
    adoption.reason = options.get("--reason");
    const Result<SessionView> adopted = session_manager.reestablish_authority(adoption);
    if (!adopted.ok()) {
      return fail(adopted.error());
    }
  }
  if (command == "reestablish") {
    const Result<AuthorityRef> presented = parse_authority(options);
    if (!presented.ok()) {
      return fail(presented.error());
    }
    ReestablishRequest adoption;
    adoption.session = session.value();
    adoption.authority = presented.value();
    adoption.reason = options.get("--reason");
    const Result<SessionView> adopted = session_manager.reestablish_authority(adoption);
    if (!adopted.ok()) {
      return fail(adopted.error());
    }
    const Result<JsonValue> body = session_view_to_json(adopted.value());
    return finish(body);
  }
  if (command == "assess") {
    const Result<Assessment> assessment = session_manager.assess(session.value());
    if (!assessment.ok()) {
      return fail(assessment.error());
    }
    const Result<JsonValue> body = assessment_to_json(assessment.value());
    return finish(body);
  }
  if (command == "report") {
    ReportOptions report_options;
    report_options.include_plan = options.has("--plan");
    report_options.include_evidence = !options.has("--no-evidence");
    report_options.include_attempts = !options.has("--no-attempts");
    const Result<JsonValue> body =
        session_manager.report(session.value(), report_options);
    return finish(body);
  }

  if (command == "request") {
    const Result<AuthorityRef> authority = parse_authority(options);
    if (!authority.ok()) {
      return fail(authority.error());
    }
    if (!options.has("--obligation")) {
      return fail(Error(ErrorCode::InvalidArgument, "--obligation is required"));
    }
    const Result<ObligationId> obligation =
        ObligationId::parse(options.get("--obligation"));
    if (!obligation.ok()) {
      return fail(obligation.error());
    }
    EffectRequest request;
    request.session = session.value();
    request.obligation = obligation.value();
    request.authority = authority.value();
    request.deadline_ticks = options.get_unsigned("--deadline", 100);
    const Result<AttemptView> attempt = session_manager.request_effect(request);
    if (!attempt.ok()) {
      return fail(attempt.error());
    }
    const Result<JsonValue> attempt_value = attempt_to_json(attempt.value().attempt);
    if (!attempt_value.ok()) {
      return fail(attempt_value.error());
    }
    JsonObjectBuilder root;
    root.set_bool("replayed", attempt.value().replayed);
    root.set("attempt", attempt_value.value());
    return finish(root.build());
  }
  if (command == "resolve") {
    const Result<AuthorityRef> authority = parse_authority(options);
    if (!authority.ok()) {
      return fail(authority.error());
    }
    if (!options.has("--attempt")) {
      return fail(Error(ErrorCode::InvalidArgument, "--attempt is required"));
    }
    const Result<AttemptId> attempt_id = AttemptId::from_hex(options.get("--attempt"));
    if (!attempt_id.ok()) {
      return fail(attempt_id.error());
    }
    AttemptResolution request;
    request.session = session.value();
    request.attempt = attempt_id.value();
    request.authority = authority.value();
    request.reason = options.get("--reason");
    const Result<AttemptView> attempt = session_manager.resolve_attempt(request);
    if (!attempt.ok()) {
      return fail(attempt.error());
    }
    const Result<JsonValue> attempt_value = attempt_to_json(attempt.value().attempt);
    if (!attempt_value.ok()) {
      return fail(attempt_value.error());
    }
    JsonObjectBuilder root;
    root.set("attempt", attempt_value.value());
    return finish(root.build());
  }
  if (command == "observe") {
    const Result<AuthorityRef> authority = parse_authority(options);
    if (!authority.ok()) {
      return fail(authority.error());
    }
    if (!options.has("--kind") || !options.has("--subject") || !options.has("--observer")) {
      return fail(Error(ErrorCode::InvalidArgument,
                        "--kind, --subject, and --observer are required"));
    }
    ObserveRequest request;
    request.session = session.value();
    const Result<EvidenceKind> kind = EvidenceKind::parse(options.get("--kind"));
    if (!kind.ok()) {
      return fail(kind.error());
    }
    request.kind = kind.value();
    const Result<SubjectId> subject = SubjectId::parse(options.get("--subject"));
    if (!subject.ok()) {
      return fail(subject.error());
    }
    request.subject = subject.value();
    if (options.has("--obligation")) {
      const Result<ObligationId> obligation =
          ObligationId::parse(options.get("--obligation"));
      if (!obligation.ok()) {
        return fail(obligation.error());
      }
      request.has_obligation = true;
      request.obligation = obligation.value();
    }
    if (options.has("--stage")) {
      const Result<StageId> stage = StageId::parse(options.get("--stage"));
      if (!stage.ok()) {
        return fail(stage.error());
      }
      request.has_stage = true;
      request.stage = stage.value();
    }
    const Result<OwnerId> observer = OwnerId::parse(options.get("--observer"));
    if (!observer.ok()) {
      return fail(observer.error());
    }
    request.observer = observer.value();
    request.authority = authority.value();
    request.observed_tick = options.get_unsigned("--observed-tick", 0);
    const Result<EvidenceView> view = session_manager.observe_subject(request);
    if (!view.ok()) {
      return fail(view.error());
    }
    const Result<JsonValue> evidence_value = evidence_to_json(view.value().record);
    if (!evidence_value.ok()) {
      return fail(evidence_value.error());
    }
    JsonObjectBuilder root;
    root.set_bool("replayed", view.value().replayed);
    root.set("evidence", evidence_value.value());
    const Result<Assessment> assessment = session_manager.assess(session.value());
    if (assessment.ok()) {
      const Result<JsonValue> assessment_value = assessment_to_json(assessment.value());
      if (assessment_value.ok()) {
        root.set("assessment", assessment_value.value());
      }
    }
    return finish(root.build());
  }
  if (command == "evidence") {
    if (!options.has("--kind") || !options.has("--subject") || !options.has("--owner") ||
        !options.has("--value")) {
      return fail(Error(ErrorCode::InvalidArgument,
                        "--kind, --subject, --owner, and --value are required"));
    }
    Observation observation;
    observation.session = session.value();
    const Result<EvidenceKind> kind = EvidenceKind::parse(options.get("--kind"));
    if (!kind.ok()) {
      return fail(kind.error());
    }
    observation.kind = kind.value();
    const Result<SubjectId> subject = SubjectId::parse(options.get("--subject"));
    if (!subject.ok()) {
      return fail(subject.error());
    }
    observation.subject = subject.value();
    const Result<std::string> value_text = resolve_value(options.get("--value"));
    if (!value_text.ok()) {
      return fail(value_text.error());
    }
    const Result<JsonValue> value = parse_json(value_text.value());
    if (!value.ok()) {
      return fail(value.error());
    }
    observation.value = value.value();
    const Result<OwnerId> owner = OwnerId::parse(options.get("--owner"));
    if (!owner.ok()) {
      return fail(owner.error());
    }
    observation.source_owner = owner.value();
    const Result<AuthorityDomain> domain =
        parse_authority_domain(options.get("--domain", "control"));
    if (!domain.ok()) {
      return fail(domain.error());
    }
    observation.source_domain = domain.value();
    observation.source_generation = options.get_unsigned("--generation", 0);
    if (options.has("--channel")) {
      const Result<EvidenceChannel> channel =
          parse_evidence_channel(options.get("--channel"));
      if (!channel.ok()) {
        return fail(channel.error());
      }
      observation.channel = channel.value();
    }
    observation.observed_tick = options.get_unsigned("--observed-tick", 0);
    if (options.has("--obligation")) {
      const Result<ObligationId> obligation =
          ObligationId::parse(options.get("--obligation"));
      if (!obligation.ok()) {
        return fail(obligation.error());
      }
      observation.has_obligation = true;
      observation.obligation = obligation.value();
    }
    if (options.has("--stage")) {
      const Result<StageId> stage = StageId::parse(options.get("--stage"));
      if (!stage.ok()) {
        return fail(stage.error());
      }
      observation.has_stage = true;
      observation.stage = stage.value();
    }
    if (options.has("--attempt")) {
      const Result<AttemptId> attempt = AttemptId::from_hex(options.get("--attempt"));
      if (!attempt.ok()) {
        return fail(attempt.error());
      }
      observation.has_attempt = true;
      observation.attempt = attempt.value();
    }
    const Result<EvidenceView> view = session_manager.record_observation(observation);
    if (!view.ok()) {
      return fail(view.error());
    }
    const Result<JsonValue> evidence_value = evidence_to_json(view.value().record);
    if (!evidence_value.ok()) {
      return fail(evidence_value.error());
    }
    JsonObjectBuilder root;
    root.set_bool("replayed", view.value().replayed);
    root.set("evidence", evidence_value.value());
    const Result<Assessment> assessment = session_manager.assess(session.value());
    if (assessment.ok()) {
      const Result<JsonValue> assessment_value = assessment_to_json(assessment.value());
      if (assessment_value.ok()) {
        root.set("assessment", assessment_value.value());
      }
    }
    return finish(root.build());
  }
  if (command == "run") {
    const Result<PlanDocument> plan = parse_plan(options);
    if (!plan.ok()) {
      return fail(plan.error());
    }
    const Result<std::vector<AuthorityRef>> authorities =
        parse_authorities(options, "--authority");
    if (!authorities.ok()) {
      return fail(authorities.error());
    }
    const std::uint64_t maximum = options.get_unsigned("--max-steps", 200);
    // Re-acquires, inside this incarnation, every requirement the caller's owners can
    // report. This is the act that makes recovered evidence current again.
    const auto observe_requirements =
        [&session_manager, &session, &authorities](
            const std::vector<EvidenceRequirement>& requirements, bool has_obligation,
            const ObligationId& obligation) -> Result<Unit> {
      for (const EvidenceRequirement& requirement : requirements) {
        for (const AuthorityRef& authority : authorities.value()) {
          ObserveRequest observation;
          observation.session = session.value();
          observation.kind = requirement.kind;
          observation.subject = requirement.subject;
          observation.has_obligation = has_obligation;
          observation.obligation = obligation;
          observation.observer = authority.owner;
          observation.authority = authority;
          BSM_TRY_ASSIGN(view, session_manager.observe_subject(observation));
          (void)view;
        }
      }
      return Unit{};
    };
    std::vector<JsonValue::Object> journal;
    std::size_t performed = 0;
    for (std::uint64_t step = 0; step < maximum; ++step) {
      const Result<Assessment> assessment = session_manager.assess(session.value());
      if (!assessment.ok()) {
        return fail(assessment.error());
      }
      JsonObjectBuilder entry;
      entry.set_uint("step", step);
      entry.set_text("primary_block", to_string(assessment.value().primary_block));
      entry.set_text("explanation", assessment.value().explanation);
      if (assessment.value().completion_ready) {
        CompletionRequest completion;
        completion.session = session.value();
        completion.authority = authorities.value().front();
        const Result<CompletionView> view =
            session_manager.complete_session(completion);
        if (!view.ok()) {
          return fail(view.error());
        }
        entry.set_text("action", "completed");
        entry.set_text("proof_digest", view.value().proof_digest.hex());
            const Result<JsonValue> entry_value = entry.build();
        if (entry_value.ok()) {
          journal.push_back(entry_value.value().as_object());
        }
        ++performed;
        break;
      }
      if (!assessment.value().has_next_stage && assessment.value().eligible.empty()) {
        // The final stage is complete: acquire the return-to-service proof and let the
        // next iteration decide whether the facility may be handed back.
        const Result<Unit> acquired = observe_requirements(
            plan.value().return_to_service.evidence, false, ObligationId());
        if (!acquired.ok()) {
          return fail(acquired.error());
        }
        entry.set_text("action", "observed_return_to_service");
        const Result<JsonValue> entry_value = entry.build();
        if (entry_value.ok()) {
          journal.push_back(entry_value.value().as_object());
        }
        ++performed;
        continue;
      }
      if (assessment.value().next_stage_ready) {
        StageAdvance advance;
        advance.session = session.value();
        advance.authority = authorities.value().front();
        const Result<StageTransitionView> view = session_manager.advance_stage(advance);
        if (!view.ok()) {
          return fail(view.error());
        }
        entry.set_text("action", "advanced");
        entry.set_text("to_stage", view.value().transition.to.str());
        const Result<JsonValue> entry_value = entry.build();
        if (entry_value.ok()) {
          journal.push_back(entry_value.value().as_object());
        }
        ++performed;
        continue;
      }
      if (assessment.value().eligible.empty()) {
        entry.set_text("action", "stopped");
        const Result<JsonValue> entry_value = entry.build();
        if (entry_value.ok()) {
          journal.push_back(entry_value.value().as_object());
        }
        break;
      }
      const ObligationId obligation = assessment.value().eligible.front();
      const std::size_t index = 0;
      (void)index;
      // The obligation definition comes from the plan the command was given.
      const Result<DerivedPlan> derived = DerivedPlan::compile(plan.value());
      if (!derived.ok()) {
        return fail(derived.error());
      }
      const ObligationDefinition* definition = derived.value().obligation(obligation);
      if (definition == nullptr) {
        return fail(Error(ErrorCode::UnknownIdentity,
                          "the eligible obligation is not part of the plan"));
      }
      const AuthorityRef* owner_authority = nullptr;
      for (const AuthorityRef& authority : authorities.value()) {
        if (authority.domain == definition->owner_domain &&
            authority.owner == definition->owner) {
          owner_authority = &authority;
          break;
        }
      }
      if (owner_authority == nullptr) {
        entry.set_text("action", "stopped");
        entry.set_text("reason", "no presented authority owns the eligible obligation");
        const Result<JsonValue> entry_value = entry.build();
        if (entry_value.ok()) {
          journal.push_back(entry_value.value().as_object());
        }
        break;
      }
      EffectRequest request;
      request.session = session.value();
      request.obligation = obligation;
      request.authority = *owner_authority;
      request.deadline_ticks = options.get_unsigned("--deadline", 100);
      const Result<AttemptView> attempt = session_manager.request_effect(request);
      if (!attempt.ok()) {
        return fail(attempt.error());
      }
      entry.set_text("action", "requested");
      entry.set_text("obligation", obligation.str());
      entry.set_text("attempt", attempt.value().attempt.id.hex());
      entry.set_text("attempt_state", to_string(attempt.value().attempt.state));
      if (attempt.value().attempt.state == AttemptState::Unresolved) {
        AttemptResolution resolution;
        resolution.session = session.value();
        resolution.attempt = attempt.value().attempt.id;
        resolution.authority = authorities.value().front();
        resolution.reason = "restoration driver resolves before continuing";
        const Result<AttemptView> resolved = session_manager.resolve_attempt(resolution);
        if (!resolved.ok()) {
          return fail(resolved.error());
        }
        entry.set_text("resolved_to", to_string(resolved.value().attempt.state));
      }
      const Result<JsonValue> entry_value = entry.build();
      if (entry_value.ok()) {
        journal.push_back(entry_value.value().as_object());
      }
      ++performed;

      // Observe every readiness requirement this obligation carries, from every presented
      // authority that is allowed to report it. This is what makes the evidence current
      // inside this incarnation.
      // Each presented owner reports what it can see. The requirement's own owner has to be
      // among the sources; the further sources are what make a cross-checked requirement
      // independent, so they are taken as well.
      const Result<Unit> observed = observe_requirements(
          definition->required_evidence, true, obligation);
      if (!observed.ok()) {
        return fail(observed.error());
      }
    }
    JsonArrayBuilder steps;
    for (const JsonValue::Object& record : journal) {
        const Result<JsonValue> value = JsonValue::object(record);
      if (!value.ok()) {
        return fail(value.error());
      }
      steps.push(value.value());
    }
    const Result<JsonValue> steps_value = steps.build();
    if (!steps_value.ok()) {
      return fail(steps_value.error());
    }
    const Result<Assessment> final_assessment = session_manager.assess(session.value());
    if (!final_assessment.ok()) {
      return fail(final_assessment.error());
    }
    const Result<JsonValue> assessment_value = assessment_to_json(final_assessment.value());
    if (!assessment_value.ok()) {
      return fail(assessment_value.error());
    }
    JsonObjectBuilder root;
    root.set_uint("actions", static_cast<std::uint64_t>(performed));
    root.set("steps", steps_value.value());
    root.set("assessment", assessment_value.value());
    return finish(root.build());
  }
  if (command == "advance") {
    const Result<AuthorityRef> authority = parse_authority(options);
    if (!authority.ok()) {
      return fail(authority.error());
    }
    StageAdvance request;
    request.session = session.value();
    request.authority = authority.value();
    const Result<StageTransitionView> view = session_manager.advance_stage(request);
    if (!view.ok()) {
      return fail(view.error());
    }
    const StageTransition& moved = view.value().transition;
    JsonObjectBuilder transition;
    transition.set_text("from", moved.from.str());
    transition.set_text("to", moved.to.str());
    transition.set_uint("to_index", static_cast<std::uint64_t>(moved.to_index));
    transition.set_uint("tick", moved.tick);
    transition.set_text("plan_digest", moved.plan_digest.hex());
    transition.set_text("binding_digest", moved.binding_digest.hex());
    transition.set_text("incarnation", moved.incarnation.hex());
    JsonArrayBuilder exit_evidence;
    for (const EvidenceId& id : moved.exit_evidence) {
      exit_evidence.push_text(id.hex());
    }
    const Result<JsonValue> exit_evidence_value = exit_evidence.build();
    if (!exit_evidence_value.ok()) {
      return fail(exit_evidence_value.error());
    }
    transition.set("exit_evidence", exit_evidence_value.value());
    JsonArrayBuilder entry_evidence;
    for (const EvidenceId& id : moved.entry_evidence) {
      entry_evidence.push_text(id.hex());
    }
    const Result<JsonValue> entry_evidence_value = entry_evidence.build();
    if (!entry_evidence_value.ok()) {
      return fail(entry_evidence_value.error());
    }
    transition.set("entry_evidence", entry_evidence_value.value());
    const Result<JsonValue> transition_value = transition.build();
    if (!transition_value.ok()) {
      return fail(transition_value.error());
    }
    const Result<JsonValue> assessment = assessment_to_json(view.value().assessment);
    if (!assessment.ok()) {
      return fail(assessment.error());
    }
    JsonObjectBuilder root;
    root.set("transition", transition_value.value());
    root.set("assessment", assessment.value());
    return finish(root.build());
  }
  if (command == "complete") {
    const Result<AuthorityRef> authority = parse_authority(options);
    if (!authority.ok()) {
      return fail(authority.error());
    }
    CompletionRequest request;
    request.session = session.value();
    request.authority = authority.value();
    const Result<CompletionView> view = session_manager.complete_session(request);
    if (!view.ok()) {
      return fail(view.error());
    }
    JsonArrayBuilder proof;
    for (const EvidenceId& id : view.value().proof_evidence) {
      proof.push_text(id.hex());
    }
    const Result<JsonValue> proof_value = proof.build();
    if (!proof_value.ok()) {
      return fail(proof_value.error());
    }
    JsonObjectBuilder root;
    root.set_text("session", view.value().session.hex());
    root.set_uint("tick", view.value().tick);
    root.set_text("proof_digest", view.value().proof_digest.hex());
    root.set("proof_evidence", proof_value.value());
    return finish(root.build());
  }
  if (command == "hold" || command == "resume" || command == "abort") {
    const Result<AuthorityRef> authority = parse_authority(options);
    if (!authority.ok()) {
      return fail(authority.error());
    }
    Result<SessionView> view = Error(ErrorCode::InternalError, "unreachable");
    if (command == "hold") {
      HoldRequest request;
      request.session = session.value();
      request.authority = authority.value();
      request.reason = options.get("--reason");
      view = session_manager.hold(request);
    } else if (command == "resume") {
      ResumeRequest request;
      request.session = session.value();
      request.authority = authority.value();
      request.reason = options.get("--reason");
      view = session_manager.resume(request);
    } else {
      AbortRequest request;
      request.session = session.value();
      request.authority = authority.value();
      request.reason = options.get("--reason");
      view = session_manager.abort(request);
    }
    if (!view.ok()) {
      return fail(view.error());
    }
    const Result<JsonValue> body = session_view_to_json(view.value());
    return finish(body);
  }
  if (command == "replan") {
    const Result<PlanDocument> plan = parse_plan(options);
    if (!plan.ok()) {
      return fail(plan.error());
    }
    const Result<AuthorityRef> authority = parse_authority(options);
    if (!authority.ok()) {
      return fail(authority.error());
    }
    ReplanRequest request;
    request.session = session.value();
    request.plan = plan.value();
    request.binding = binding.value();
    request.authority = authority.value();
    request.reason = options.get("--reason");
    request.accept_topology_change = options.has("--accept-topology-change");
    const Result<SessionView> view = session_manager.replan(request);
    if (!view.ok()) {
      return fail(view.error());
    }
    const Result<JsonValue> body = session_view_to_json(view.value());
    return finish(body);
  }

  std::cerr << "bsm: unknown command '" << command << "'\n";
  usage();
  return 2;
}
