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

// Proof obligations: deterministic seeded randomized state machines. A model that is
// written independently of the library states what the plan allows; after every mutation
// the library's assessment must agree with the model, a refusal must change nothing, and
// state must never move backwards. Failing runs print the seed that reproduces them.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "black_start_manager/manager.hpp"
#include "fixture.hpp"
#include "test_harness.hpp"

using namespace black_start_manager;
using namespace bsm_test;

namespace {

// An independent model of the documented eligibility rules.
struct Model {
  std::vector<std::string> ids;
  std::vector<std::size_t> stage_of;
  std::vector<std::vector<std::size_t>> depends_on;
  std::vector<std::uint32_t> budget;
  std::vector<bool> satisfied;
  std::vector<bool> acknowledged;
  std::vector<std::uint32_t> required_sources;
  std::vector<std::uint32_t> observed_sources;
  std::vector<std::uint32_t> attempts_used;
  std::vector<std::uint32_t> unresolved;
  std::size_t stage_count = 0;
  std::size_t stage = 0;

  [[nodiscard]] std::vector<std::string> eligible() const {
    std::vector<std::string> result;
    for (std::size_t index = 0; index < ids.size(); ++index) {
      if (satisfied[index] || stage_of[index] != stage || unresolved[index] != 0) {
        continue;
      }
      // An acknowledged effect is waiting for its readiness observation, not for another
      // consequential request.
      if (acknowledged[index]) {
        continue;
      }
      if (attempts_used[index] >= budget[index]) {
        continue;
      }
      bool prerequisites = true;
      for (const std::size_t dependency : depends_on[index]) {
        if (!satisfied[dependency]) {
          prerequisites = false;
        }
      }
      if (!prerequisites) {
        continue;
      }
      // Strict in-stage precedence: every earlier required obligation is satisfied.
      for (std::size_t other = 0; other < index; ++other) {
        if (stage_of[other] != stage_of[index]) {
          continue;
        }
        if (!satisfied[other]) {
          prerequisites = false;
        }
      }
      if (prerequisites) {
        result.push_back(ids[index]);
      }
    }
    return result;
  }

  [[nodiscard]] bool ready_to_advance() const {
    for (std::size_t index = 0; index < ids.size(); ++index) {
      if (stage_of[index] == stage && !satisfied[index]) {
        return false;
      }
    }
    return stage + 1 < stage_count;
  }
};

[[nodiscard]] Model build_model(const DerivedPlan& plan) {
  Model model;
  model.stage_count = plan.stages().size();
  for (const ObligationDefinition& obligation : plan.obligations()) {
    model.ids.push_back(obligation.id.str());
    model.stage_of.push_back(plan.stage_index(obligation.stage));
    model.budget.push_back(obligation.attempt_budget);
    model.satisfied.push_back(false);
    model.acknowledged.push_back(false);
    std::uint32_t sources = 1;
    for (const EvidenceRequirement& requirement : obligation.required_evidence) {
      sources = std::max(sources, requirement.min_sources);
    }
    model.required_sources.push_back(sources);
    model.observed_sources.push_back(0);
    model.attempts_used.push_back(0);
    model.unresolved.push_back(0);
    std::vector<std::size_t> dependencies;
    for (const ObligationId& dependency : obligation.depends_on) {
      dependencies.push_back(plan.obligation_index(dependency));
    }
    model.depends_on.push_back(std::move(dependencies));
  }
  return model;
}

[[nodiscard]] std::vector<std::string> assessment_eligible(const Assessment& assessment) {
  std::vector<std::string> result;
  for (const ObligationId& id : assessment.eligible) {
    result.push_back(id.str());
  }
  return result;
}

[[nodiscard]] bool same(const std::vector<std::string>& left,
                        const std::vector<std::string>& right) {
  return left == right;
}

// Two assessments of an unchanged session must be byte-identical.
[[nodiscard]] std::string canonical_assessment(const Assessment& assessment) {
  const Result<JsonValue> value = assessment_to_json(assessment);
  return value.ok() ? canonical_json(value.value()) : std::string("<unencodable>");
}

}  // namespace

BSM_TEST(a_seeded_state_machine_agrees_with_an_independent_model) {
  const std::uint64_t seed = 0x5EED1234ull;
  context.note("seed: " + std::to_string(seed));
  bsm_test::SeededRandom random(seed);

  bsm_test::Harness harness;
  BSM_CHECK_OK(started, harness.start(bsm_test::scratch_directory("property"), 1));
  BSM_CHECK_OK(session, harness.open_session());
  BSM_CHECK_OK(plan_derived, DerivedPlan::compile(harness.definition.plan));
  Model model = build_model(plan_derived);

  std::uint64_t tick = 1;
  std::uint64_t previous_revision = 0;
  std::size_t previous_stage = 0;

  for (int iteration = 0; iteration < 24; ++iteration) {
    tick += 1;
    BSM_CHECK_OK(clock_set, harness.clock.set(tick));
    BSM_CHECK_OK(before, harness.manager->assess(session));
    if (!same(assessment_eligible(before), model.eligible())) {
      context.note("model mismatch at iteration " + std::to_string(iteration));
      for (const std::string& id : assessment_eligible(before)) {
        context.note("  library eligible: " + id);
      }
      for (const std::string& id : model.eligible()) {
        context.note("  model eligible: " + id);
      }
      BSM_CHECK(false);
    }

    const std::uint64_t choice = random.below(100);
    if (choice < 45) {
      // Request the first eligible obligation, or an ineligible one to prove that a
      // refusal changes nothing.
      const std::vector<std::string> eligible = model.eligible();
      const bool legal = !eligible.empty() && random.chance(4, 5);
      std::string target;
      if (legal) {
        target = eligible[random.below(eligible.size())];
      } else {
        target = model.ids[random.below(model.ids.size())];
      }
      const std::size_t index = static_cast<std::size_t>(
          std::find(model.ids.begin(), model.ids.end(), target) - model.ids.begin());
      EffectRequest request;
      request.session = session;
      request.obligation = ObligationId::parse(target).value();
      request.authority = harness.definition.electrical;
      request.deadline_ticks = 30;
      const std::size_t stage_before = model.stage;
      const Result<AttemptView> attempt = harness.manager->request_effect(request);
      if (!legal) {
        BSM_CHECK(!attempt.ok());
        BSM_CHECK_OK(after_refusal, harness.manager->assess(session));
        BSM_CHECK_EQ(canonical_assessment(before), canonical_assessment(after_refusal));
        BSM_CHECK_EQ(model.stage, stage_before);
        continue;
      }
      BSM_CHECK(attempt.ok());
      ++model.attempts_used[index];
      if (attempt.value().attempt.state == AttemptState::Unresolved) {
        ++model.unresolved[index];
        continue;
      }
      BSM_CHECK(attempt.value().attempt.state == AttemptState::Acknowledged);
      model.acknowledged[index] = true;
    } else if (choice < 75) {
      // Observe every obligation whose effect was applied but that is not satisfied yet.
      bool observed_any = false;
      for (std::size_t index = 0; index < model.ids.size(); ++index) {
        if (model.satisfied[index] || model.attempts_used[index] == 0 ||
            model.unresolved[index] != 0) {
          continue;
        }
        const ObligationDefinition& obligation = *plan_derived.obligation(
            ObligationId::parse(model.ids[index]).value());
        if (!obligation.consequential) {
          continue;
        }
        ObserveRequest observation;
        observation.session = session;
        observation.kind = obligation.required_evidence.front().kind;
        observation.subject = obligation.required_evidence.front().subject;
        observation.has_obligation = true;
        observation.obligation = obligation.id;
        observation.observer = obligation.owner;
        observation.authority = harness.definition.electrical;
        observation.observed_tick = tick;
        const Result<EvidenceView> viewed = harness.manager->observe_subject(observation);
        if (!viewed.ok()) {
          continue;
        }
        observed_any = true;
        model.observed_sources[index] += 1;
        if (model.observed_sources[index] >= model.required_sources[index]) {
          model.satisfied[index] = true;
          model.acknowledged[index] = false;
        }
        break;
      }
      if (!observed_any) {
        continue;
      }
    } else if (choice < 90) {
      // Advance when the model says the stage is complete, otherwise prove the refusal.
      StageAdvance advance;
      advance.session = session;
      advance.authority = harness.definition.control;
      const bool ready = model.ready_to_advance();
      const Result<StageTransitionView> transition = harness.manager->advance_stage(advance);
      if (ready) {
        if (!transition.ok()) {
          context.note("advance refused: " + transition.error().message());
          const Result<Assessment> moment = harness.manager->assess(session);
          if (moment.ok()) {
            for (const ObligationAssessment& obligation : moment.value().obligations) {
              context.note("  obligation " + obligation.id.str() + " satisfied=" +
                           (obligation.satisfied ? "true" : "false") +
                           " block=" + to_string(obligation.block) +
                           " attempts=" + std::to_string(obligation.attempts_used));
              for (const EvidenceDeficit& deficit : obligation.deficits) {
                context.note("    deficit " + deficit.kind.str() + "/" +
                             deficit.subject.str() + " " + to_string(deficit.code));
              }
            }
            for (const StageAssessment& stage : moment.value().stages) {
              context.note("  stage " + stage.id.str() + " entry=" +
                           (stage.entry_satisfied ? "true" : "false") +
                           " obligations=" +
                           (stage.obligations_satisfied ? "true" : "false"));
            }
          }
          BSM_CHECK(false);
        }
        ++model.stage;
      } else {
        BSM_CHECK(!transition.ok());
        continue;
      }
    } else {
      // Holds and resumes must not disturb satisfaction or stage progress.
      HoldRequest hold;
      hold.session = session;
      hold.authority = harness.definition.control;
      hold.reason = "randomized hold";
      BSM_CHECK_OK(held, harness.manager->hold(hold));
      ResumeRequest resume;
      resume.session = session;
      resume.authority = harness.definition.control;
      resume.reason = "randomized resume";
      BSM_CHECK_OK(resumed, harness.manager->resume(resume));
      BSM_CHECK_EQ(resumed.stage_index, held.stage_index);
    }

    BSM_CHECK_OK(after, harness.manager->assess(session));
    // Monotonic invariants.
    BSM_CHECK(after.session_digest.is_zero() == false);
    BSM_CHECK(after.stage_index >= previous_stage);
    previous_stage = after.stage_index;
    BSM_CHECK_EQ(after.stage_index, model.stage);
    for (const ObligationAssessment& obligation : after.obligations) {
      const std::size_t index = static_cast<std::size_t>(
          std::find(model.ids.begin(), model.ids.end(), obligation.id.str()) -
          model.ids.begin());
      BSM_CHECK_EQ(obligation.satisfied, model.satisfied[index]);
    }
    BSM_CHECK(same(assessment_eligible(after), model.eligible()));
    // Two consecutive assessments of an unchanged session are byte-identical.
    BSM_CHECK_OK(again, harness.manager->assess(session));
    BSM_CHECK_EQ(canonical_assessment(after), canonical_assessment(again));
    previous_revision = after.session_digest == before.session_digest ? previous_revision : 1;
  }
}

BSM_TEST(a_seeded_random_document_stream_never_produces_an_invalid_plan_state) {
  bsm_test::SeededRandom random(0xC0FFEEull);
  context.note("seed: 12648430");
  for (int iteration = 0; iteration < 40; ++iteration) {
    JsonObjectBuilder root;
    root.set_uint("format_version", 1);
    root.set_text("facility", "facility-1");
    JsonObjectBuilder policy;
    policy.set_text("id", "p");
    policy.set_uint("revision", random.below(3));
    policy.set_uint("default_max_age_ticks", random.below(100));
    policy.set_uint("default_attempt_budget", 1 + random.below(3));
    policy.set_bool("strict_within_stage_order", random.chance(1, 2));
    BSM_CHECK_OK(policy_value, policy.build());
    root.set("policy", policy_value);
    JsonArrayBuilder stages;
    JsonObjectBuilder stage;
    stage.set_text("id", "s0");
    stage.set_uint("rank", 0);
    stage.set_text("title", "stage");
    BSM_CHECK_OK(stage_value, stage.build());
    stages.push(stage_value);
    BSM_CHECK_OK(stages_value, stages.build());
    root.set("stages", stages_value);
    JsonArrayBuilder obligations;
    const int count = static_cast<int>(random.below(4));
    for (int index = 0; index < count; ++index) {
      JsonObjectBuilder obligation;
      obligation.set_text("id", "ob-" + std::to_string(index));
      obligation.set_text("title", "t");
      obligation.set_text("stage", "s0");
      obligation.set_text("owner_domain", "electrical");
      obligation.set_text("owner", "electrical-owner");
      JsonArrayBuilder requires_array;
      JsonObjectBuilder requirement;
      requirement.set_text("kind", "probe_ready");
      requirement.set_text("subject", "probe:1");
      requirement.set_uint("min_sources", 1 + random.below(3));
      BSM_CHECK_OK(requirement_value, requirement.build());
      requires_array.push(requirement_value);
      BSM_CHECK_OK(requires_value, requires_array.build());
      obligation.set("requires", requires_value);
      if (random.chance(1, 2)) {
        JsonArrayBuilder depends;
        depends.push_text("ob-" + std::to_string(random.below(4)));
        BSM_CHECK_OK(depends_value, depends.build());
        obligation.set("depends_on", depends_value);
      }
      JsonObjectBuilder request;
      request.set_text("capability", "verify-isolation");
      BSM_CHECK_OK(parameters, JsonValue::object(JsonValue::Object{}));
      request.set("parameters", parameters);
      BSM_CHECK_OK(request_value, request.build());
      obligation.set("request", request_value);
      BSM_CHECK_OK(obligation_value, obligation.build());
      obligations.push(obligation_value);
    }
    BSM_CHECK_OK(obligations_value, obligations.build());
    root.set("obligations", obligations_value);
    JsonObjectBuilder rts;
    BSM_CHECK_OK(empty, JsonValue::array(JsonValue::Array{}));
    rts.set("evidence", empty);
    JsonArrayBuilder required;
    for (int index = 0; index < count; ++index) {
      required.push_text("ob-" + std::to_string(index));
    }
    BSM_CHECK_OK(required_value, required.build());
    rts.set("required_obligations", required_value);
    BSM_CHECK_OK(rts_value, rts.build());
    root.set("return_to_service", rts_value);
    BSM_CHECK_OK(document, root.build());
    const std::string text = canonical_json(document);
    const Result<PlanDocument> plan = PlanDocument::parse(text);
    if (plan.ok()) {
      const Result<DerivedPlan> compiled = DerivedPlan::compile(plan.value());
      if (compiled.ok()) {
        // A compiled plan must round-trip through its canonical form unchanged.
        BSM_CHECK_OK(encoded, compiled.value().to_json());
        BSM_CHECK_OK(decoded, DerivedPlan::from_json(encoded));
        BSM_CHECK_EQ(decoded.digest().hex(), compiled.value().digest().hex());
      }
    }
  }
}

BSM_TEST_MAIN("bsm_test_property")
