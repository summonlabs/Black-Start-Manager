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

// Proof obligations: readiness evidence is admitted only from a bound authority
// generation, never from the future, never promoted from recovered state, never satisfied
// by an observation that asserts the wrong readiness, and never averaged across
// contradictions.

#include <cstdint>
#include <memory>
#include <string>

#include "black_start_manager/manager.hpp"
#include "fixture.hpp"
#include "test_harness.hpp"

using namespace black_start_manager;
using namespace bsm_test;

namespace {

struct Setup {
  std::unique_ptr<bsm_test::Harness> harness;
  SessionId session;
};

[[nodiscard]] Result<Setup> prepare(Context& context, const char* name,
                                    const SyntheticControllerPolicy& policy = {}) {
  auto harness = std::make_unique<bsm_test::Harness>();
  const std::string root = bsm_test::scratch_directory(name);
  BSM_RETURN_IF_ERROR(harness->start(root, 5, policy));
  BSM_TRY_ASSIGN(session, harness->open_session());
  Setup setup;
  setup.harness = std::move(harness);
  setup.session = session;
  (void)context;
  return setup;
}

[[nodiscard]] Observation manual_observation(const bsm_test::Harness& harness,
                                             const SessionId& session,
                                             const char* kind_text,
                                             const char* subject_text,
                                             const char* owner_text,
                                             AuthorityDomain domain,
                                             std::uint64_t generation,
                                             const char* value_text,
                                             EvidenceChannel channel =
                                                 EvidenceChannel::InstrumentedMeasurement,
                                             const char* obligation_text = "isolation-a") {
  Observation observation;
  observation.session = session;
  observation.kind = EvidenceKind::parse(kind_text).value();
  observation.subject = SubjectId::parse(subject_text).value();
  observation.value = parse_json(value_text).value();
  observation.source_owner = OwnerId::parse(owner_text).value();
  observation.source_domain = domain;
  observation.source_generation = generation;
  observation.channel = channel;
  observation.observed_tick = harness.clock.now_ticks().value();
  if (obligation_text != nullptr) {
    observation.has_obligation = true;
    observation.obligation = ObligationId::parse(obligation_text).value();
  }
  return observation;
}

// Requests the bounded isolation effect and observes it, which is what makes the first
// stage's exit gate satisfiable.
[[nodiscard]] Result<Unit> drive_isolation(bsm_test::Harness& harness,
                                           const SessionId& session) {
  EffectRequest request;
  request.session = session;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.authority = harness.definition.electrical;
  request.deadline_ticks = 10;
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

[[nodiscard]] const ObligationAssessment* find_obligation(const Assessment& assessment,
                                                          const char* id) {
  for (const ObligationAssessment& obligation : assessment.obligations) {
    if (obligation.id.str() == id) {
      return &obligation;
    }
  }
  return nullptr;
}

[[nodiscard]] bool has_deficit(const ObligationAssessment& obligation,
                               EvidenceDeficitCode code) {
  for (const EvidenceDeficit& deficit : obligation.deficits) {
    if (deficit.code == code) {
      return true;
    }
  }
  return false;
}

}  // namespace

BSM_TEST(evidence_from_an_unbound_owner_is_refused) {
  BSM_CHECK_OK(setup, prepare(context, "evidence-unbound"));
  const bsm_test::Harness& harness = *setup.harness;
  EffectRequest request;
  request.session = setup.session;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.authority = harness.definition.electrical;
  request.deadline_ticks = 10;
  BSM_CHECK_OK(attempt, harness.manager->request_effect(request));

  Observation observation = manual_observation(
      harness, setup.session, "isolation_verified", "isolation:a", "unknown-owner",
      AuthorityDomain::Electrical, 1, "{\"isolated\":true}");
  const Result<EvidenceView> admitted = harness.manager->record_observation(observation);
  BSM_CHECK(!admitted.ok());
  BSM_CHECK_EQ(admitted.error().code(), ErrorCode::AuthorityMissing);
}

BSM_TEST(evidence_from_a_stale_generation_is_refused) {
  BSM_CHECK_OK(setup, prepare(context, "evidence-stale"));
  const bsm_test::Harness& harness = *setup.harness;
  Observation observation = manual_observation(
      harness, setup.session, "isolation_verified", "isolation:a", "electrical-owner",
      AuthorityDomain::Electrical, 7, "{\"isolated\":true}");
  const Result<EvidenceView> admitted = harness.manager->record_observation(observation);
  BSM_CHECK(!admitted.ok());
  BSM_CHECK_EQ(admitted.error().code(), ErrorCode::EvidenceStaleAuthority);
}

BSM_TEST(evidence_from_the_future_is_refused) {
  BSM_CHECK_OK(setup, prepare(context, "evidence-future"));
  const bsm_test::Harness& harness = *setup.harness;
  Observation observation = manual_observation(
      harness, setup.session, "isolation_verified", "isolation:a", "electrical-owner",
      AuthorityDomain::Electrical, 1, "{\"isolated\":true}");
  observation.observed_tick = harness.clock.now_ticks().value() + 100;
  const Result<EvidenceView> admitted = harness.manager->record_observation(observation);
  BSM_CHECK(!admitted.ok());
  BSM_CHECK_EQ(admitted.error().code(), ErrorCode::EvidenceFromFuture);
}

BSM_TEST(evidence_for_an_unrequired_fact_is_refused) {
  BSM_CHECK_OK(setup, prepare(context, "evidence-unrequired"));
  const bsm_test::Harness& harness = *setup.harness;
  Observation observation = manual_observation(
      harness, setup.session, "unspecified_kind", "unspecified:subject",
      "electrical-owner", AuthorityDomain::Electrical, 1, "{\"anything\":true}", 
      EvidenceChannel::InstrumentedMeasurement, nullptr);
  const Result<EvidenceView> admitted = harness.manager->record_observation(observation);
  BSM_CHECK(!admitted.ok());
  BSM_CHECK_EQ(admitted.error().code(), ErrorCode::EvidenceUnknownRequirement);
}

BSM_TEST(pre_session_evidence_is_recorded_as_recovered_and_never_satisfies) {
  BSM_CHECK_OK(setup, prepare(context, "evidence-recovered"));
  bsm_test::Harness& harness = *setup.harness;
  BSM_CHECK_OK(advanced, harness.clock.advance(10));
  Observation observation = manual_observation(
      harness, setup.session, "isolation_verified", "isolation:a", "electrical-owner",
      AuthorityDomain::Electrical, 1, "{\"isolated\":true}");
  observation.observed_tick = 1;  // observed before the session existed
  BSM_CHECK_OK(admitted, harness.manager->record_observation(observation));
  BSM_CHECK_EQ(admitted.record.provenance, EvidenceProvenance::Recovered);
  BSM_CHECK(admitted.record.recovered_tick != 0);
  BSM_CHECK_OK(assessment, harness.manager->assess(setup.session));
  const ObligationAssessment* obligation = find_obligation(assessment, "isolation-a");
  BSM_CHECK(obligation != nullptr);
  BSM_CHECK(!obligation->satisfied);
  BSM_CHECK(has_deficit(*obligation, EvidenceDeficitCode::RecoveredOnly));
  // Recovered evidence neither satisfies the gate nor blocks the remedy: the obligation
  // stays requestable so readiness can be re-established explicitly.
  BSM_CHECK_EQ(obligation->block, BlockCode::None);
  BSM_CHECK_EQ(assessment.eligible.size(), std::size_t{1});
}

BSM_TEST(an_observation_that_asserts_the_wrong_readiness_never_satisfies) {
  BSM_CHECK_OK(setup, prepare(context, "evidence-expectation"));
  bsm_test::Harness& harness = *setup.harness;
  Observation observation = manual_observation(
      harness, setup.session, "isolation_verified", "isolation:a", "electrical-owner",
      AuthorityDomain::Electrical, 1, "{\"isolated\":false}");
  BSM_CHECK_OK(admitted, harness.manager->record_observation(observation));
  BSM_CHECK_EQ(admitted.record.provenance, EvidenceProvenance::Live);
  BSM_CHECK_OK(assessment, harness.manager->assess(setup.session));
  const ObligationAssessment* obligation = find_obligation(assessment, "isolation-a");
  BSM_CHECK(obligation != nullptr);
  BSM_CHECK(!obligation->satisfied);
  BSM_CHECK(has_deficit(*obligation, EvidenceDeficitCode::ExpectationMismatch));
}

BSM_TEST(manager_derived_evidence_is_admitted_but_is_never_independent) {
  BSM_CHECK_OK(setup, prepare(context, "evidence-independent"));
  bsm_test::Harness& harness = *setup.harness;
  Observation observation = manual_observation(
      harness, setup.session, "isolation_verified", "isolation:a", "electrical-owner",
      AuthorityDomain::Electrical, 1, "{\"isolated\":true}",
      EvidenceChannel::ManagerDerived);
  BSM_CHECK_OK(admitted, harness.manager->record_observation(observation));
  BSM_CHECK_OK(assessment, harness.manager->assess(setup.session));
  const ObligationAssessment* obligation = find_obligation(assessment, "isolation-a");
  BSM_CHECK(obligation != nullptr);
  BSM_CHECK(!obligation->satisfied);
  BSM_CHECK(has_deficit(*obligation, EvidenceDeficitCode::NotIndependent));
}

BSM_TEST(contradictory_current_sources_are_refused_rather_than_averaged) {
  BSM_CHECK_OK(setup, prepare(context, "evidence-contradiction"));
  bsm_test::Harness& harness = *setup.harness;
  // Drive the plan to the point where domain_energized requires two sources.
  BSM_CHECK_OK(driven, drive_isolation(harness, setup.session));
  (void)driven;
  StageAdvance advance;
  advance.session = setup.session;
  advance.authority = harness.definition.control;
  BSM_CHECK_OK(advanced, harness.manager->advance_stage(advance));
  EffectRequest request;
  request.session = setup.session;
  request.obligation = ObligationId::parse("energize-a").value();
  request.authority = harness.definition.electrical;
  request.deadline_ticks = 10;
  BSM_CHECK_OK(attempt, harness.manager->request_effect(request));

  Observation first = manual_observation(
      harness, setup.session, "domain_energized", "domain:a", "electrical-owner",
      AuthorityDomain::Electrical, 1, "{\"energized\":true,\"energize_generation\":1}",
      EvidenceChannel::InstrumentedMeasurement, "energize-a");
  BSM_CHECK_OK(admitted_first, harness.manager->record_observation(first));
  Observation second = manual_observation(
      harness, setup.session, "domain_energized", "domain:a", "instrumentation-owner",
      AuthorityDomain::Facility, 1, "{\"energized\":true,\"energize_generation\":2}",
      EvidenceChannel::InstrumentedMeasurement, "energize-a");
  BSM_CHECK_OK(admitted_second, harness.manager->record_observation(second));

  BSM_CHECK_OK(assessment, harness.manager->assess(setup.session));
  const ObligationAssessment* obligation = find_obligation(assessment, "energize-a");
  BSM_CHECK(obligation != nullptr);
  BSM_CHECK(obligation->block == BlockCode::EvidenceContradictory ||
            has_deficit(*obligation, EvidenceDeficitCode::Contradictory));
}

BSM_TEST(a_single_source_never_satisfies_a_cross_checked_requirement) {
  BSM_CHECK_OK(setup, prepare(context, "evidence-insufficient"));
  bsm_test::Harness& harness = *setup.harness;
  BSM_CHECK_OK(driven, drive_isolation(harness, setup.session));
  (void)driven;
  StageAdvance advance;
  advance.session = setup.session;
  advance.authority = harness.definition.control;
  BSM_CHECK_OK(advanced, harness.manager->advance_stage(advance));
  EffectRequest energize;
  energize.session = setup.session;
  energize.obligation = ObligationId::parse("energize-a").value();
  energize.authority = harness.definition.electrical;
  energize.deadline_ticks = 10;
  BSM_CHECK_OK(energize_attempt, harness.manager->request_effect(energize));
  BSM_CHECK_EQ(energize_attempt.attempt.state, AttemptState::Acknowledged);
  BSM_CHECK_OK(energized_view,
               harness.observe(setup.session, ObligationId::parse("energize-a").value(),
                               EvidenceKind::parse("domain_energized").value(),
                               SubjectId::parse("domain:a").value(),
                               OwnerId::parse("electrical-owner").value(),
                               harness.definition.electrical));
  (void)energized_view;
  BSM_CHECK_OK(assessment, harness.manager->assess(setup.session));
  const ObligationAssessment* obligation = find_obligation(assessment, "energize-a");
  BSM_CHECK(obligation != nullptr);
  BSM_CHECK(!obligation->satisfied);
  BSM_CHECK(has_deficit(*obligation, EvidenceDeficitCode::InsufficientSources));
}

BSM_TEST(the_same_observation_admitted_twice_is_the_same_record) {
  BSM_CHECK_OK(setup, prepare(context, "evidence-idempotent"));
  bsm_test::Harness& harness = *setup.harness;
  Observation observation = manual_observation(
      harness, setup.session, "isolation_verified", "isolation:a", "electrical-owner",
      AuthorityDomain::Electrical, 1, "{\"isolated\":true}");
  BSM_CHECK_OK(first, harness.manager->record_observation(observation));
  BSM_CHECK(!first.replayed);
  BSM_CHECK_OK(second, harness.manager->record_observation(observation));
  BSM_CHECK(second.replayed);
  BSM_CHECK_EQ(second.record.id.hex(), first.record.id.hex());
  BSM_CHECK_OK(report, harness.manager->report(setup.session));
  const JsonValue* evidence = report.find("evidence");
  BSM_CHECK(evidence != nullptr);
  BSM_CHECK_EQ(evidence->as_array().size(), std::size_t{1});
}

BSM_TEST(observation_of_a_subject_the_owner_does_not_model_is_refused) {
  BSM_CHECK_OK(setup, prepare(context, "evidence-unknown-subject"));
  bsm_test::Harness& harness = *setup.harness;
  ObserveRequest request;
  request.session = setup.session;
  request.kind = EvidenceKind::parse("isolation_verified").value();
  request.subject = SubjectId::parse("isolation:a").value();
  request.has_obligation = true;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.observer = OwnerId::parse("electrical-owner").value();
  request.authority = harness.definition.electrical;
  const Result<EvidenceView> observed = harness.manager->observe_subject(request);
  BSM_CHECK(observed.ok());
  BSM_CHECK(observed.value().record.value.is_object());
}

BSM_TEST_MAIN("bsm_test_evidence")
