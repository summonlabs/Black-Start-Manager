// Copyright 2026 Summon Software Ltd
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

#include "scenario.hpp"

#include <memory>
#include <string>
#include <vector>

#include "black_start_manager/remote_controller.hpp"
#include "fixture.hpp"

namespace bsm_test {

using namespace black_start_manager;

namespace {

[[nodiscard]] Result<ObligationId> obligation(const char* text) {
  return ObligationId::parse(text);
}

[[nodiscard]] Result<EvidenceKind> kind(const char* text) {
  return EvidenceKind::parse(text);
}

[[nodiscard]] Result<SubjectId> subject(const char* text) {
  return SubjectId::parse(text);
}

[[nodiscard]] Result<OwnerId> owner(const char* text) { return OwnerId::parse(text); }

struct Environment {
  Facility definition;
  ManualClock clock;
  InProcessSyntheticController in_process;
  std::unique_ptr<RemoteControllerClient> remote;
  std::unique_ptr<StaticFacilityState> facility;
  std::unique_ptr<SessionManager> manager;

  explicit Environment(std::uint64_t tick) : clock(tick) {}

  [[nodiscard]] Result<Unit> setup(const StepOptions& options) {
    BSM_TRY_ASSIGN(value, make_facility());
    definition = value;
    facility = std::make_unique<StaticFacilityState>(definition.binding);
    if (!options.controller.empty()) {
      if (options.controller.rfind("tcp:", 0) != 0) {
        return Error(ErrorCode::InvalidArgument, "controller endpoint must be tcp:PORT");
      }
      RemoteControllerOptions remote_options;
      remote_options.port =
          static_cast<std::uint16_t>(std::stoul(options.controller.substr(4)));
      BSM_TRY_ASSIGN(caller, OwnerId::parse("black-start-manager"));
      remote_options.caller = std::move(caller);
      remote = std::make_unique<RemoteControllerClient>();
      BSM_RETURN_IF_ERROR(remote->connect(remote_options));
    }
    ManagerOptions manager_options;
    manager_options.store_root = options.store;
    manager_options.create_store_if_missing = options.create_store;
    manager_options.clock = &clock;
    manager_options.controller =
        remote != nullptr ? static_cast<AdjacentController*>(remote.get()) : &in_process;
    manager_options.facility = facility.get();
    manager_options.compact_journal_records = options.compact_records;
    BSM_TRY_ASSIGN(opened, SessionManager::open(manager_options));
    manager = std::make_unique<SessionManager>(std::move(opened));
    return Unit{};
  }

  [[nodiscard]] Result<SessionId> active() {
    return manager->active_session();
  }

  [[nodiscard]] Result<AttemptView> request(const SessionId& session,
                                            const char* obligation_text,
                                            const AuthorityRef& authority) {
    BSM_TRY_ASSIGN(id, obligation(obligation_text));
    EffectRequest request;
    request.session = session;
    request.obligation = id;
    request.authority = authority;
    request.deadline_ticks = 100;
    return manager->request_effect(request);
  }

  [[nodiscard]] Result<EvidenceView> observe(const SessionId& session,
                                             const char* obligation_text,
                                             const char* kind_text,
                                             const char* subject_text,
                                             const char* owner_text,
                                             const AuthorityRef& authority) {
    BSM_TRY_ASSIGN(id, obligation(obligation_text));
    BSM_TRY_ASSIGN(evidence_kind, kind(kind_text));
    BSM_TRY_ASSIGN(subject_id, subject(subject_text));
    BSM_TRY_ASSIGN(observer, owner(owner_text));
    ObserveRequest request;
    request.session = session;
    request.kind = evidence_kind;
    request.subject = subject_id;
    request.has_obligation = true;
    request.obligation = id;
    request.observer = observer;
    request.authority = authority;
    BSM_TRY_ASSIGN(tick, clock.now_ticks());
    request.observed_tick = tick;
    return manager->observe_subject(request);
  }

  [[nodiscard]] Result<EvidenceView> observe_facility(const SessionId& session,
                                                      const char* kind_text,
                                                      const char* subject_text,
                                                      const char* owner_text,
                                                      const AuthorityRef& authority) {
    BSM_TRY_ASSIGN(evidence_kind, kind(kind_text));
    BSM_TRY_ASSIGN(subject_id, subject(subject_text));
    BSM_TRY_ASSIGN(observer, owner(owner_text));
    ObserveRequest request;
    request.session = session;
    request.kind = evidence_kind;
    request.subject = subject_id;
    request.observer = observer;
    request.authority = authority;
    BSM_TRY_ASSIGN(tick, clock.now_ticks());
    request.observed_tick = tick;
    return manager->observe_subject(request);
  }
};

}  // namespace

bool is_known_step(const std::string& step) {
  static const std::vector<std::string> steps = {
      "open",         "request-isolation", "observe-isolation",
      "advance-energize", "request-energize", "observe-energize-electrical",
      "observe-energize-instrumentation", "request-control-power",
      "observe-control-power", "request-cooling", "observe-cooling", "advance-admit",
      "request-admit", "observe-admit", "observe-rts", "complete",
      "resolve-isolation"};
  for (const std::string& candidate : steps) {
    if (candidate == step) {
      return true;
    }
  }
  return false;
}

Result<std::string> run_step(const StepOptions& options) {
  Environment environment(options.tick);
  BSM_RETURN_IF_ERROR(environment.setup(options));

  if (options.step == "open") {
    EstablishSessionRequest request;
    request.plan = environment.definition.plan;
    request.binding = environment.definition.binding;
    request.authority = environment.definition.control;
    BSM_TRY_ASSIGN(view, environment.manager->establish_session(request));
    return view.id.hex();
  }

  BSM_TRY_ASSIGN(session, environment.active());
  const Facility& facility = environment.definition;

  // Process authority is not inherited: each invocation is a new incarnation, and it
  // adopts the observed facility state explicitly before it may act. This is the step a
  // real operator performs once after a manager restart.
  {
    ReestablishRequest reestablish;
    reestablish.session = session;
    reestablish.authority = facility.control;
    reestablish.reason = "scripted operator re-establishes authority";
    BSM_RETURN_IF_ERROR(environment.manager->reestablish_authority(reestablish));
  }

  if (options.step == "resolve-isolation") {
    BSM_TRY_ASSIGN(assessment, environment.manager->assess(session));
    for (const ObligationAssessment& obligation : assessment.obligations) {
      if (obligation.id.str() != "isolation-a") {
        continue;
      }
      if (obligation.unresolved_attempts.empty()) {
        return Error(ErrorCode::AttemptNotFound,
                     "no unresolved attempt exists for isolation-a");
      }
      AttemptResolution resolution;
      resolution.session = session;
      resolution.attempt = obligation.unresolved_attempts.front();
      resolution.authority = facility.control;
      resolution.reason = "recovered after an incarnation change";
      BSM_RETURN_IF_ERROR(environment.manager->resolve_attempt(resolution));
      return session.hex();
    }
    return Error(ErrorCode::UnknownIdentity, "the plan does not declare isolation-a");
  } else if (options.step == "request-isolation") {
    BSM_RETURN_IF_ERROR(environment.request(session, "isolation-a", facility.electrical));
  } else if (options.step == "observe-isolation") {
    BSM_RETURN_IF_ERROR(environment.observe(session, "isolation-a", "isolation_verified",
                                            "isolation:a", "electrical-owner",
                                            facility.electrical));
  } else if (options.step == "advance-energize") {
    StageAdvance request;
    request.session = session;
    request.authority = facility.control;
    BSM_RETURN_IF_ERROR(environment.manager->advance_stage(request));
  } else if (options.step == "request-energize") {
    BSM_RETURN_IF_ERROR(environment.request(session, "energize-a", facility.electrical));
  } else if (options.step == "observe-energize-electrical") {
    BSM_RETURN_IF_ERROR(environment.observe(session, "energize-a", "domain_energized",
                                            "domain:a", "electrical-owner",
                                            facility.electrical));
  } else if (options.step == "observe-energize-instrumentation") {
    BSM_RETURN_IF_ERROR(environment.observe(session, "energize-a", "domain_energized",
                                            "domain:a", "instrumentation-owner",
                                            facility.instrumentation));
  } else if (options.step == "request-control-power") {
    BSM_RETURN_IF_ERROR(
        environment.request(session, "control-power", facility.facility_owner));
  } else if (options.step == "observe-control-power") {
    BSM_RETURN_IF_ERROR(environment.observe(session, "control-power",
                                            "control_power_available", "control-power:main",
                                            "facility-owner", facility.facility_owner));
  } else if (options.step == "request-cooling") {
    BSM_RETURN_IF_ERROR(
        environment.request(session, "cooling-primary", facility.cooling));
  } else if (options.step == "observe-cooling") {
    BSM_RETURN_IF_ERROR(environment.observe(session, "cooling-primary", "cooling_ready",
                                            "cooling:primary", "cooling-owner",
                                            facility.cooling));
  } else if (options.step == "advance-admit") {
    StageAdvance request;
    request.session = session;
    request.authority = facility.control;
    BSM_RETURN_IF_ERROR(environment.manager->advance_stage(request));
  } else if (options.step == "request-admit") {
    BSM_RETURN_IF_ERROR(environment.request(session, "admit-r1", facility.facility_owner));
  } else if (options.step == "observe-admit") {
    BSM_RETURN_IF_ERROR(environment.observe(session, "admit-r1", "rack_admitted", "rack:r1",
                                            "facility-owner", facility.facility_owner));
  } else if (options.step == "observe-rts") {
    BSM_RETURN_IF_ERROR(environment.observe_facility(session, "return_to_service",
                                                     "facility", "facility-owner",
                                                     facility.facility_owner));
  } else if (options.step == "complete") {
    CompletionRequest request;
    request.session = session;
    request.authority = facility.control;
    BSM_RETURN_IF_ERROR(environment.manager->complete_session(request));
  } else {
    return Error(ErrorCode::InvalidArgument, "unknown scenario step '" + options.step + "'");
  }
  return session.hex();
}

}  // namespace bsm_test
