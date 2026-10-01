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

// Proof obligations: the restoration session is a dependency-gated authority protocol.
// Prerequisites, stage entry and exit evidence, in-stage precedence, holds, fencing,
// re-establishment, replanning, and return-to-service proof all decide what may happen
// next, and every refusal names the gate that stopped it.

#include <cstdint>
#include <memory>
#include <string>

#include "black_start_manager/manager.hpp"
#include "fixture.hpp"
#include "test_harness.hpp"

using namespace black_start_manager;
using namespace bsm_test;

namespace {

[[nodiscard]] Result<std::unique_ptr<bsm_test::Harness>> start(const char* name,
                                                              std::uint64_t tick = 5) {
  auto harness = std::make_unique<bsm_test::Harness>();
  BSM_RETURN_IF_ERROR(harness->start(bsm_test::scratch_directory(name), tick));
  return harness;
}

[[nodiscard]] const ObligationAssessment* obligation_of(const Assessment& assessment,
                                                        const char* id) {
  for (const ObligationAssessment& obligation : assessment.obligations) {
    if (obligation.id.str() == id) {
      return &obligation;
    }
  }
  return nullptr;
}

// Drives stage-isolation to a satisfied state.
[[nodiscard]] Result<Unit> satisfy_isolation(bsm_test::Harness& harness,
                                             const SessionId& session) {
  EffectRequest request;
  request.session = session;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.authority = harness.definition.electrical;
  request.deadline_ticks = 20;
  BSM_RETURN_IF_ERROR(harness.manager->request_effect(request));
  ObserveRequest observation;
  observation.session = session;
  observation.kind = EvidenceKind::parse("isolation_verified").value();
  observation.subject = SubjectId::parse("isolation:a").value();
  observation.has_obligation = true;
  observation.obligation = request.obligation;
  observation.observer = OwnerId::parse("electrical-owner").value();
  observation.authority = harness.definition.electrical;
  BSM_TRY_ASSIGN(tick, harness.clock.now_ticks());
  observation.observed_tick = tick;
  BSM_TRY_ASSIGN(view, harness.manager->observe_subject(observation));
  (void)view;
  return Unit{};
}

}  // namespace

BSM_TEST(a_session_opens_only_with_a_current_control_authority) {
  BSM_CHECK_OK(harness, start("session-authority"));
  EstablishSessionRequest request;
  request.plan = harness->definition.plan;
  request.binding = harness->definition.binding;
  request.authority = harness->definition.electrical;  // wrong domain
  BSM_CHECK_EQ(harness->manager->establish_session(request).error().code(),
               ErrorCode::AuthorityMismatch);

  AuthorityRef stale = harness->definition.control;
  stale.generation = 9;
  request.authority = stale;
  BSM_CHECK_EQ(harness->manager->establish_session(request).error().code(),
               ErrorCode::AuthorityStale);

  request.authority = harness->definition.control;
  BSM_CHECK_OK(view, harness->manager->establish_session(request));
  BSM_CHECK_EQ(view.state, SessionState::Active);
  BSM_CHECK_EQ(view.stage_index, std::size_t{0});
  // A second session for the same facility is refused without an explicit supersede.
  BSM_CHECK_EQ(harness->manager->establish_session(request).error().code(),
               ErrorCode::SessionExists);
}

BSM_TEST(a_prerequisite_cannot_be_skipped) {
  BSM_CHECK_OK(harness, start("session-prerequisite"));
  BSM_CHECK_OK(session, harness->open_session());
  EffectRequest request;
  request.session = session;
  request.obligation = ObligationId::parse("energize-a").value();
  request.authority = harness->definition.electrical;
  request.deadline_ticks = 20;
  // energize-a lives in a later stage than the session's current stage.
  BSM_CHECK_EQ(harness->manager->request_effect(request).error().code(),
               ErrorCode::GatePrerequisiteUnsatisfied);
  BSM_CHECK_OK(assessment, harness->manager->assess(session));
  BSM_CHECK_EQ(assessment.eligible.size(), std::size_t{1});
  BSM_CHECK_EQ(assessment.eligible[0].str(), std::string("isolation-a"));
}

BSM_TEST(in_stage_precedence_blocks_a_later_obligation) {
  BSM_CHECK_OK(harness, start("session-precedence"));
  BSM_CHECK_OK(session, harness->open_session());
  BSM_CHECK_OK(satisfied, satisfy_isolation(*harness, session));
  (void)satisfied;
  StageAdvance advance;
  advance.session = session;
  advance.authority = harness->definition.control;
  BSM_CHECK_OK(advanced, harness->manager->advance_stage(advance));
  BSM_CHECK_OK(after_first, harness->manager->assess(session));
  BSM_CHECK_EQ(after_first.eligible.size(), std::size_t{1});
  BSM_CHECK_EQ(after_first.eligible[0].str(), std::string("energize-a"));
  // control-power is declared later in the same stage and is not yet eligible.
  EffectRequest request;
  request.session = session;
  request.obligation = ObligationId::parse("control-power").value();
  request.authority = harness->definition.facility_owner;
  request.deadline_ticks = 20;
  BSM_CHECK_EQ(harness->manager->request_effect(request).error().code(),
               ErrorCode::GatePrecedenceBlocked);
}

BSM_TEST(stage_entry_evidence_gates_the_next_stage) {
  BSM_CHECK_OK(harness, start("session-stage-entry"));
  BSM_CHECK_OK(session, harness->open_session());
  BSM_CHECK_OK(satisfied, satisfy_isolation(*harness, session));
  (void)satisfied;
  StageAdvance advance;
  advance.session = session;
  advance.authority = harness->definition.control;
  BSM_CHECK_OK(advanced, harness->manager->advance_stage(advance));

  // Age the isolation evidence beyond its window: the next stage's entry gate fails even
  // though the obligation that produced the evidence was satisfied earlier.
  BSM_CHECK_OK(future, harness->clock.set(1000));
  BSM_CHECK_OK(aged, harness->manager->assess(session));
  const ObligationAssessment* isolation = obligation_of(aged, "isolation-a");
  BSM_CHECK(isolation != nullptr);
  BSM_CHECK(!isolation->satisfied);
  EffectRequest request;
  request.session = session;
  request.obligation = ObligationId::parse("energize-a").value();
  request.authority = harness->definition.electrical;
  request.deadline_ticks = 20;
  const Result<AttemptView> refused = harness->manager->request_effect(request);
  BSM_CHECK(!refused.ok());
  BSM_CHECK(refused.error().code() == ErrorCode::GateStageEvidenceMissing ||
            refused.error().code() == ErrorCode::GateStageEvidenceStale);
}

BSM_TEST(a_hold_stops_progress_and_resume_restores_it) {
  BSM_CHECK_OK(harness, start("session-hold"));
  BSM_CHECK_OK(session, harness->open_session());
  HoldRequest hold;
  hold.session = session;
  hold.authority = harness->definition.control;
  hold.reason = "operator inspection";
  BSM_CHECK_OK(held, harness->manager->hold(hold));
  BSM_CHECK_EQ(held.state, SessionState::Held);
  EffectRequest request;
  request.session = session;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.authority = harness->definition.electrical;
  request.deadline_ticks = 20;
  BSM_CHECK_EQ(harness->manager->request_effect(request).error().code(),
               ErrorCode::SessionHeld);
  BSM_CHECK_EQ(harness->manager->hold(hold).error().code(), ErrorCode::SessionHeld);

  ResumeRequest resume;
  resume.session = session;
  resume.authority = harness->definition.control;
  resume.reason = "inspection complete";
  BSM_CHECK_OK(resumed, harness->manager->resume(resume));
  BSM_CHECK_EQ(resumed.state, SessionState::Active);
  BSM_CHECK_OK(attempt, harness->manager->request_effect(request));
  BSM_CHECK_EQ(attempt.attempt.state, AttemptState::Acknowledged);
}

BSM_TEST(an_abort_is_terminal_and_requires_a_reason) {
  BSM_CHECK_OK(harness, start("session-abort"));
  BSM_CHECK_OK(session, harness->open_session());
  AbortRequest abort;
  abort.session = session;
  abort.authority = harness->definition.control;
  BSM_CHECK_EQ(harness->manager->abort(abort).error().code(),
               ErrorCode::InvalidArgument);
  abort.reason = "facility declared unsafe";
  BSM_CHECK_OK(aborted, harness->manager->abort(abort));
  BSM_CHECK_EQ(aborted.state, SessionState::Aborted);
  EffectRequest request;
  request.session = session;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.authority = harness->definition.electrical;
  request.deadline_ticks = 20;
  BSM_CHECK_EQ(harness->manager->request_effect(request).error().code(),
               ErrorCode::SessionTerminal);
  BSM_CHECK_EQ(harness->manager->abort(abort).error().code(),
               ErrorCode::SessionTerminal);
}

BSM_TEST(a_stale_binding_fences_the_session_and_reestablishment_is_explicit) {
  BSM_CHECK_OK(harness, start("session-fencing"));
  BSM_CHECK_OK(session, harness->open_session());
  BSM_CHECK_OK(satisfied, satisfy_isolation(*harness, session));
  (void)satisfied;

  // The electrical authority is replaced: same owner, higher generation.
  BSM_CHECK_OK(advanced_facility, bsm_test::make_facility(1, 2));
  harness->facility->set(advanced_facility.binding);

  BSM_CHECK_OK(assessment, harness->manager->assess(session));
  // An authority advance is not a contradiction: the session is not fenced, but the
  // binding has moved and every action now requires explicit re-establishment.
  BSM_CHECK(!assessment.fenced);
  BSM_CHECK(!assessment.delta.empty());
  BSM_CHECK(assessment.fence_codes.empty());
  BSM_CHECK(!assessment.delta.empty());

  EffectRequest request;
  request.session = session;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.authority = harness->definition.electrical;
  request.deadline_ticks = 20;
  // The session is stale rather than fenced, and every action is refused until the
  // operator re-establishes authority explicitly.
  BSM_CHECK_EQ(harness->manager->request_effect(request).error().code(),
               ErrorCode::AuthorityStale);

  // Re-establishment must present the *current* control authority.
  ReestablishRequest reestablish;
  reestablish.session = session;
  reestablish.authority = harness->definition.control;
  BSM_CHECK_EQ(harness->manager->reestablish_authority(reestablish).error().code(),
               ErrorCode::AuthorityStale);
  reestablish.authority = advanced_facility.control;
  reestablish.reason = "operations control restarted";
  BSM_CHECK_OK(reestablished, harness->manager->reestablish_authority(reestablish));
  BSM_CHECK_EQ(reestablished.binding_digest.hex(),
               binding_digest(advanced_facility.binding).value().hex());

  // The evidence admitted under the old binding is not current any more.
  BSM_CHECK_OK(after_reestablish, harness->manager->assess(session));
  const ObligationAssessment* isolation = obligation_of(after_reestablish, "isolation-a");
  BSM_CHECK(isolation != nullptr);
  BSM_CHECK(!isolation->satisfied);
}

BSM_TEST(a_contradictory_binding_cannot_be_reestablished_but_can_be_replanned) {
  BSM_CHECK_OK(harness, start("session-contradiction"));
  BSM_CHECK_OK(session, harness->open_session());
  // The observed epoch goes backwards, which is contradictory rather than advanced.
  BSM_CHECK_OK(earlier, bsm_test::make_facility(0, 1));
  harness->facility->set(earlier.binding);
  ReestablishRequest reestablish;
  reestablish.session = session;
  reestablish.authority = earlier.control;
  reestablish.reason = "should be refused";
  BSM_CHECK_EQ(harness->manager->reestablish_authority(reestablish).error().code(),
               ErrorCode::SessionFenced);

  // A replan may not silently move to a different de-energization epoch either.
  ReplanRequest replan;
  replan.session = session;
  replan.plan = harness->definition.plan;
  replan.binding = earlier.binding;
  replan.authority = earlier.control;
  replan.reason = "same epoch required";
  BSM_CHECK_EQ(harness->manager->replan(replan).error().code(),
               ErrorCode::AuthorityMismatch);
}

BSM_TEST(a_replan_binds_a_new_plan_and_keeps_in_flight_attempts_resolvable) {
  BSM_CHECK_OK(harness, start("session-replan"));
  BSM_CHECK_OK(session, harness->open_session());
  // An in-flight attempt whose reply is lost: it must stay resolvable across a replan.
  SyntheticControllerPolicy policy;
  BSM_CHECK_OK(capability, CapabilityId::parse("verify-isolation"));
  policy.lose_first_reply.push_back(capability);
  harness->controller.set_policy(policy);
  EffectRequest request;
  request.session = session;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.authority = harness->definition.electrical;
  request.deadline_ticks = 20;
  BSM_CHECK_OK(attempt, harness->manager->request_effect(request));
  BSM_CHECK_EQ(attempt.attempt.state, AttemptState::Unresolved);

  ReplanRequest replan;
  replan.session = session;
  replan.plan = harness->definition.plan;
  replan.binding = harness->definition.binding;
  replan.authority = harness->definition.control;
  replan.reason = "adopt the corrected declaration";
  // The same declaration compiles to the same digest, so a no-op replan is refused.
  BSM_CHECK_EQ(harness->manager->replan(replan).error().code(), ErrorCode::PlanUnchanged);

  BSM_CHECK_OK(modified_plan, bsm_test::standard_plan());
  modified_plan.policy.revision = 2;
  modified_plan.obligations[0].title = "Verify that domain A is isolated (revised)";
  FacilityBinding modified_binding = harness->definition.binding;
  modified_binding.policy_revision = 2;
  replan.plan = modified_plan;
  replan.binding = modified_binding;
  BSM_CHECK_OK(replanned, harness->manager->replan(replan));
  harness->facility->set(modified_binding);

  // The in-flight attempt remains unresolved and still blocks its obligation, because the
  // owner may have applied the effect.
  BSM_CHECK_OK(after_replan, harness->manager->assess(session));
  const ObligationAssessment* isolation = obligation_of(after_replan, "isolation-a");
  BSM_CHECK(isolation != nullptr);
  BSM_CHECK_EQ(isolation->unresolved_attempts.size(), std::size_t{1});
  BSM_CHECK_EQ(harness->manager->request_effect(request).error().code(),
               ErrorCode::AttemptUnresolved);
}

BSM_TEST(completion_requires_current_proof_for_every_required_obligation) {
  BSM_CHECK_OK(harness, start("session-completion"));
  BSM_CHECK_OK(session, harness->open_session());
  CompletionRequest completion;
  completion.session = session;
  completion.authority = harness->definition.control;
  const Result<CompletionView> early = harness->manager->complete_session(completion);
  BSM_CHECK(!early.ok());
  BSM_CHECK_EQ(early.error().code(), ErrorCode::GateCompletionUnsatisfied);
  BSM_CHECK_OK(assessment, harness->manager->assess(session));
  BSM_CHECK(!assessment.completion_ready);
  BSM_CHECK_EQ(assessment.completion_missing_obligations.size(), std::size_t{6});
}

BSM_TEST_MAIN("bsm_test_session")
