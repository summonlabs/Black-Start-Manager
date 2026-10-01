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

// Proof obligations: a consequential request has one durable identity, a lost answer
// becomes an explicit unresolved attempt instead of a silent retry, resolution decides
// what actually happened, and an attempt that was never applied does not consume the
// bounded budget.

#include <cstdint>
#include <memory>
#include <string>

#include "black_start_manager/manager.hpp"
#include "fixture.hpp"
#include "test_harness.hpp"

using namespace black_start_manager;
using namespace bsm_test;

namespace {

[[nodiscard]] Result<std::unique_ptr<bsm_test::Harness>> start(
    const char* name, const SyntheticControllerPolicy& policy = {},
    std::uint64_t tick = 5) {
  auto harness = std::make_unique<bsm_test::Harness>();
  BSM_RETURN_IF_ERROR(harness->start(bsm_test::scratch_directory(name), tick, policy));
  return harness;
}

[[nodiscard]] Result<AttemptView> request_isolation(bsm_test::Harness& harness,
                                                    const SessionId& session) {
  EffectRequest request;
  request.session = session;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.authority = harness.definition.electrical;
  request.deadline_ticks = 20;
  return harness.manager->request_effect(request);
}

[[nodiscard]] Result<AttemptView> resolve(bsm_test::Harness& harness,
                                          const SessionId& session,
                                          const AttemptId& attempt) {
  AttemptResolution resolution;
  resolution.session = session;
  resolution.attempt = attempt;
  resolution.authority = harness.definition.control;
  resolution.reason = "operator resolution";
  return harness.manager->resolve_attempt(resolution);
}

}  // namespace

BSM_TEST(a_lost_answer_becomes_an_explicit_unresolved_attempt) {
  BSM_CHECK_OK(capability, CapabilityId::parse("verify-isolation"));
  SyntheticControllerPolicy policy;
  policy.lose_first_reply.push_back(capability);
  BSM_CHECK_OK(harness, start("attempt-lost-reply", policy));
  BSM_CHECK_OK(session, harness->open_session());
  BSM_CHECK_OK(attempt, request_isolation(*harness, session));
  BSM_CHECK_EQ(attempt.attempt.state, AttemptState::Unresolved);
  BSM_CHECK(!attempt.replayed);
  // The owner applied the effect, and no second request may be sent while it is unresolved.
  BSM_CHECK_EQ(harness->controller.applied_effects().size(), std::size_t{1});
  const Result<AttemptView> repeated = request_isolation(*harness, session);
  BSM_CHECK(!repeated.ok());
  BSM_CHECK_EQ(repeated.error().code(), ErrorCode::AttemptUnresolved);

  // Resolution asks the owner; the owner remembers the key, so no second effect happens.
  BSM_CHECK_OK(resolved, resolve(*harness, session, attempt.attempt.id));
  BSM_CHECK_EQ(resolved.attempt.state, AttemptState::Acknowledged);
  BSM_CHECK_EQ(harness->controller.applied_effects().size(), std::size_t{1});
  BSM_CHECK_EQ(harness->controller.duplicate_effect_count(), std::size_t{0});
  BSM_CHECK_EQ(resolved.attempt.key.hex(), attempt.attempt.key.hex());
}

BSM_TEST(an_attempt_that_was_never_applied_does_not_consume_the_budget) {
  BSM_CHECK_OK(capability, CapabilityId::parse("verify-isolation"));
  SyntheticControllerPolicy policy;
  policy.unavailable_once.push_back(capability);
  BSM_CHECK_OK(harness, start("attempt-not-applied", policy));
  BSM_CHECK_OK(session, harness->open_session());
  BSM_CHECK_OK(first, request_isolation(*harness, session));
  BSM_CHECK_EQ(first.attempt.state, AttemptState::Unresolved);
  BSM_CHECK_EQ(harness->controller.applied_effects().size(), std::size_t{0});
  BSM_CHECK_OK(resolved, resolve(*harness, session, first.attempt.id));
  BSM_CHECK_EQ(resolved.attempt.state, AttemptState::NotApplied);
  BSM_CHECK_OK(assessment, harness->manager->assess(session));
  for (const ObligationAssessment& obligation : assessment.obligations) {
    if (obligation.id.str() == "isolation-a") {
      BSM_CHECK_EQ(obligation.attempts_used, std::uint64_t{0});
      BSM_CHECK_EQ(obligation.state, ObligationState::Pending);
    }
  }
  // The budget is intact, so a second attempt is allowed and consumes exactly one.
  BSM_CHECK_OK(second, request_isolation(*harness, session));
  BSM_CHECK_EQ(second.attempt.state, AttemptState::Acknowledged);
  BSM_CHECK(!(second.attempt.key == first.attempt.key));
  BSM_CHECK_EQ(harness->controller.applied_effects().size(), std::size_t{1});
  BSM_CHECK_OK(after_second, harness->manager->assess(session));
  for (const ObligationAssessment& obligation : after_second.obligations) {
    if (obligation.id.str() == "isolation-a") {
      BSM_CHECK_EQ(obligation.attempts_used, std::uint64_t{1});
    }
  }
}

BSM_TEST(a_refusal_settles_the_attempt_without_an_effect) {
  BSM_CHECK_OK(capability, CapabilityId::parse("verify-isolation"));
  SyntheticControllerPolicy policy;
  policy.refuse.push_back(capability);
  BSM_CHECK_OK(harness, start("attempt-refused", policy));
  BSM_CHECK_OK(session, harness->open_session());
  BSM_CHECK_OK(attempt, request_isolation(*harness, session));
  BSM_CHECK_EQ(attempt.attempt.state, AttemptState::Refused);
  BSM_CHECK_EQ(harness->controller.applied_effects().size(), std::size_t{0});
  // A refused attempt is already settled, so resolution refuses to re-examine it.
  BSM_CHECK_ERR(resolve(*harness, session, attempt.attempt.id),
                ErrorCode::AttemptStateInvalid);
  BSM_CHECK_OK(assessment, harness->manager->assess(session));
  for (const ObligationAssessment& obligation : assessment.obligations) {
    if (obligation.id.str() == "isolation-a") {
      BSM_CHECK_EQ(obligation.satisfied, false);
      // One attempt was consumed and nothing was applied, so the obligation is retryable
      // while the budget lasts.
      BSM_CHECK_EQ(obligation.attempts_used, std::uint64_t{1});
      BSM_CHECK_EQ(obligation.block, BlockCode::None);
    }
  }
  BSM_CHECK_OK(retry, request_isolation(*harness, session));
  BSM_CHECK_EQ(retry.attempt.state, AttemptState::Refused);
  BSM_CHECK_OK(spent, harness->manager->assess(session));
  for (const ObligationAssessment& obligation : spent.obligations) {
    if (obligation.id.str() == "isolation-a") {
      BSM_CHECK_EQ(obligation.attempts_used, std::uint64_t{2});
      BSM_CHECK_EQ(obligation.block, BlockCode::AttemptBudgetExhausted);
    }
  }
  BSM_CHECK_ERR(request_isolation(*harness, session), ErrorCode::AttemptBudgetExhausted);
}

BSM_TEST(the_attempt_budget_is_bounded_and_exhaustion_is_explained) {
  BSM_CHECK_OK(capability, CapabilityId::parse("verify-isolation"));
  SyntheticControllerPolicy policy;
  policy.unavailable_once.push_back(capability);
  BSM_CHECK_OK(harness, start("attempt-budget", policy));
  BSM_CHECK_OK(session, harness->open_session());
  // Attempt one: the owner applies nothing, and resolution proves it, so the bounded
  // budget is not consumed.
  BSM_CHECK_OK(first, request_isolation(*harness, session));
  BSM_CHECK_EQ(first.attempt.state, AttemptState::Unresolved);
  BSM_CHECK_OK(resolved_first, resolve(*harness, session, first.attempt.id));
  BSM_CHECK_EQ(resolved_first.attempt.state, AttemptState::NotApplied);
  BSM_CHECK_OK(after_first, harness->manager->assess(session));
  for (const ObligationAssessment& obligation : after_first.obligations) {
    if (obligation.id.str() == "isolation-a") {
      BSM_CHECK_EQ(obligation.attempts_used, std::uint64_t{0});
    }
  }

  // Attempt two: the owner applies the effect and then reports a failure. That consumes
  // one attempt and leaves the obligation unsatisfied.
  SyntheticControllerPolicy failing;
  failing.fail_after_apply.push_back(capability);
  harness->controller.set_policy(failing);
  BSM_CHECK_OK(second, request_isolation(*harness, session));
  BSM_CHECK_EQ(second.attempt.state, AttemptState::Failed);
  BSM_CHECK_OK(after_second, harness->manager->assess(session));
  for (const ObligationAssessment& obligation : after_second.obligations) {
    if (obligation.id.str() == "isolation-a") {
      BSM_CHECK_EQ(obligation.attempts_used, std::uint64_t{1});
      BSM_CHECK_EQ(obligation.block, BlockCode::None);
      // A failed attempt with budget left leaves the obligation retryable, which the
      // derived state reports as pending rather than failed.
      BSM_CHECK_EQ(obligation.state, ObligationState::Pending);
    }
  }

  // Attempt three: the owner acknowledges, which consumes the second and final attempt.
  harness->controller.set_policy(SyntheticControllerPolicy{});
  BSM_CHECK_OK(third, request_isolation(*harness, session));
  BSM_CHECK_EQ(third.attempt.state, AttemptState::Acknowledged);
  BSM_CHECK_OK(after_third, harness->manager->assess(session));
  for (const ObligationAssessment& obligation : after_third.obligations) {
    if (obligation.id.str() == "isolation-a") {
      BSM_CHECK_EQ(obligation.attempts_used, std::uint64_t{2});
      BSM_CHECK_EQ(obligation.block, BlockCode::AttemptBudgetExhausted);
    }
  }
  BSM_CHECK_ERR(request_isolation(*harness, session), ErrorCode::AttemptBudgetExhausted);
}

BSM_TEST(a_replayed_outcome_is_reported_as_already_applied) {
  BSM_CHECK_OK(harness, start("attempt-replay"));
  BSM_CHECK_OK(session, harness->open_session());
  BSM_CHECK_OK(attempt, request_isolation(*harness, session));
  BSM_CHECK_EQ(attempt.attempt.state, AttemptState::Acknowledged);
  // Asking the owner again with the same key returns the recorded outcome and performs no
  // second effect.
  BSM_CHECK_OK(reply, harness->controller.query_effect(attempt.attempt.key));
  BSM_CHECK(reply.kind == ControllerReplyKind::Applied ||
            reply.kind == ControllerReplyKind::AlreadyApplied);
  BSM_CHECK_EQ(harness->controller.applied_effects().size(), std::size_t{1});
  BSM_CHECK_EQ(harness->controller.duplicate_effect_count(), std::size_t{0});
  BSM_CHECK_EQ(harness->controller.received().size(), std::size_t{1});
}

BSM_TEST(a_deferred_request_resolves_as_never_applied) {
  BSM_CHECK_OK(capability, CapabilityId::parse("verify-isolation"));
  SyntheticControllerPolicy policy;
  policy.defer.push_back(capability);
  BSM_CHECK_OK(harness, start("attempt-deferred", policy));
  BSM_CHECK_OK(session, harness->open_session());
  BSM_CHECK_OK(attempt, request_isolation(*harness, session));
  BSM_CHECK_EQ(attempt.attempt.state, AttemptState::Unresolved);
  // The owner deferred the request and holds no record of the key, so resolution proves
  // that nothing consequential happened and the budget stays intact.
  BSM_CHECK_OK(resolved, resolve(*harness, session, attempt.attempt.id));
  BSM_CHECK_EQ(resolved.attempt.state, AttemptState::NotApplied);
  BSM_CHECK_EQ(harness->controller.applied_effects().size(), std::size_t{0});
  BSM_CHECK_OK(second, request_isolation(*harness, session));
  BSM_CHECK_EQ(second.attempt.state, AttemptState::Unresolved);
}

BSM_TEST(a_settled_attempt_cannot_be_resolved_again) {
  BSM_CHECK_OK(harness, start("attempt-settled"));
  BSM_CHECK_OK(session, harness->open_session());
  BSM_CHECK_OK(attempt, request_isolation(*harness, session));
  const Result<AttemptView> again = resolve(*harness, session, attempt.attempt.id);
  BSM_CHECK(!again.ok());
  BSM_CHECK_EQ(again.error().code(), ErrorCode::AttemptStateInvalid);
  BSM_CHECK(!resolve(*harness, session, AttemptId()).ok());
}

BSM_TEST_MAIN("bsm_test_attempts")
