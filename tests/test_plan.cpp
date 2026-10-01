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

// Proof obligations: plan compilation is deterministic, refuses self-contradictory
// declarations instead of reordering them, and round-trips through its canonical form
// without losing or silently repairing anything.

#include <cstdint>
#include <string>
#include <vector>

#include "black_start_manager/plan.hpp"
#include "fixture.hpp"
#include "test_harness.hpp"

using namespace black_start_manager;
using namespace bsm_test;

namespace {

[[nodiscard]] std::string plan_with(const std::string& stages,
                                    const std::string& obligations) {
  return std::string("{\"format_version\":1,\"facility\":\"facility-1\",") +
         "\"policy\":{\"id\":\"p\",\"revision\":1,\"default_max_age_ticks\":10," +
         "\"default_attempt_budget\":2,\"strict_within_stage_order\":true}," +
         "\"stages\":" + stages + ",\"obligations\":" + obligations + "," +
         "\"return_to_service\":{\"evidence\":[],\"required_obligations\":[\"ob-0\"]}}";
}

const char* kOneStage =
    "[{\"id\":\"s0\",\"rank\":0,\"title\":\"s\"}]";

[[nodiscard]] std::string obligation(const std::string& id, const std::string& stage,
                                     const std::string& extra) {
  return "{\"id\":\"" + id + "\",\"title\":\"t\",\"stage\":\"" + stage +
         "\",\"priority\":\"protected\",\"owner_domain\":\"electrical\"," +
         "\"owner\":\"electrical-owner\",\"requires\":[{\"kind\":\"probe_ready\"," +
         "\"subject\":\"probe:1\",\"min_sources\":1," +
         "\"required_owner\":\"electrical-owner\",\"required_domain\":\"electrical\"," +
         "\"expect\":{\"ready\":true}}]," +
         "\"request\":{\"capability\":\"verify-isolation\",\"parameters\":{}}" +
         extra + "}";
}

}  // namespace

BSM_TEST(standard_plan_compiles_with_the_derived_order) {
  BSM_CHECK_OK(plan, standard_plan());
  BSM_CHECK_OK(derived, DerivedPlan::compile(plan));
  BSM_CHECK_EQ(derived.stages().size(), std::size_t{3});
  BSM_CHECK_EQ(derived.stages()[0].id.str(), std::string("stage-isolation"));
  BSM_CHECK_EQ(derived.stages()[1].id.str(), std::string("stage-energize"));
  BSM_CHECK_EQ(derived.stages()[2].id.str(), std::string("stage-admit"));
  BSM_CHECK_EQ(derived.obligations().size(), std::size_t{6});
  // Stage order first, then priority, then declaration order.
  BSM_CHECK_EQ(derived.obligations()[0].id.str(), std::string("isolation-a"));
  BSM_CHECK_EQ(derived.obligations()[1].id.str(), std::string("energize-a"));
  BSM_CHECK_EQ(derived.obligations()[2].id.str(), std::string("control-power"));
  BSM_CHECK_EQ(derived.obligations()[3].id.str(), std::string("cooling-primary"));
  BSM_CHECK_EQ(derived.obligations()[4].id.str(), std::string("admit-r1"));
  BSM_CHECK(!derived.digest().is_zero());
  // Every obligation belongs to exactly one stage, and the stage lists partition them.
  std::size_t assigned = 0;
  for (const CompiledStage& stage : derived.stages()) {
    assigned += stage.obligation_indices.size();
    for (const std::size_t index : stage.obligation_indices) {
      BSM_CHECK_EQ(derived.obligations()[index].stage.str(), stage.id.str());
    }
  }
  BSM_CHECK_EQ(assigned, derived.obligations().size());
  BSM_CHECK_EQ(derived.stages()[0].obligation_indices.size(), std::size_t{1});
  BSM_CHECK_EQ(derived.obligations()[derived.stages()[0].obligation_indices[0]].id.str(),
               std::string("isolation-a"));
  BSM_CHECK_EQ(derived.stages()[1].obligation_indices.size(), std::size_t{3});
  BSM_CHECK_EQ(derived.obligations()[5].id.str(), std::string("return-to-service"));
  BSM_CHECK_EQ(derived.stages()[2].obligation_indices.size(), std::size_t{2});
  // Stage dependency edges are derived from the obligation dependency graph.
  const std::size_t admit = derived.stage_index(StageId::parse("stage-admit").value());
  // Stage precedence is derived from direct obligation dependencies, so the admit stage
  // depends on the stage that owns the obligations it depends on.
  BSM_CHECK_EQ(derived.stages()[admit].depends_on_stages.size(), std::size_t{1});
  BSM_CHECK_EQ(derived.stages()[admit].depends_on_stages[0].str(),
               std::string("stage-energize"));
}

BSM_TEST(compilation_is_deterministic_and_round_trips) {
  BSM_CHECK_OK(plan, standard_plan());
  BSM_CHECK_OK(first, DerivedPlan::compile(plan));
  BSM_CHECK_OK(second, DerivedPlan::compile(plan));
  BSM_CHECK_EQ(first.digest().hex(), second.digest().hex());
  BSM_CHECK_OK(encoded, first.to_json());
  BSM_CHECK_OK(decoded, DerivedPlan::from_json(encoded));
  BSM_CHECK_EQ(decoded.digest().hex(), first.digest().hex());
  BSM_CHECK_OK(reencoded, decoded.to_json());
  BSM_CHECK_EQ(canonical_json(reencoded), canonical_json(encoded));
}

BSM_TEST(derived_form_refuses_tampering) {
  BSM_CHECK_OK(plan, standard_plan());
  BSM_CHECK_OK(derived, DerivedPlan::compile(plan));
  BSM_CHECK_OK(encoded, derived.to_json());
  // Changing a stored obligation digest is a real tamper: the decoder recomputes it.
  JsonValue::Object fields = encoded.as_object();
  for (JsonValue::Field& field : fields) {
    if (field.first != "obligations") {
      continue;
    }
    JsonValue::Array obligations = field.second.as_array();
    JsonValue::Object entry = obligations[0].as_object();
    for (JsonValue::Field& item : entry) {
      if (item.first == "digest") {
        item.second = JsonValue::string(std::string(64, 'a'));
      }
    }
    BSM_CHECK_OK(replaced, JsonValue::object(std::move(entry)));
    obligations[0] = replaced;
    BSM_CHECK_OK(reordered, JsonValue::array(std::move(obligations)));
    field.second = reordered;
  }
  BSM_CHECK_OK(tampered, JsonValue::object(std::move(fields)));
  const Result<DerivedPlan> rejected = DerivedPlan::from_json(tampered);
  BSM_CHECK(!rejected.ok());
  BSM_CHECK_EQ(rejected.error().code(), ErrorCode::IntegrityMismatch);

  // Reordering the derived list is not a tamper: declaration indices define the document,
  // and recompiling reproduces the same plan and the same digest.
  JsonValue::Object shuffled_fields = encoded.as_object();
  for (JsonValue::Field& field : shuffled_fields) {
    if (field.first != "obligations") {
      continue;
    }
    JsonValue::Array obligations = field.second.as_array();
    std::swap(obligations[0], obligations[1]);
    BSM_CHECK_OK(shuffled, JsonValue::array(std::move(obligations)));
    field.second = shuffled;
  }
  BSM_CHECK_OK(shuffled_document, JsonValue::object(std::move(shuffled_fields)));
  BSM_CHECK_OK(recompiled, DerivedPlan::from_json(shuffled_document));
  BSM_CHECK_EQ(recompiled.digest().hex(), derived.digest().hex());
}

BSM_TEST(dependency_cycles_are_refused_with_their_path) {
  const std::string stages = kOneStage;
  const std::string obligations =
      "[" + obligation("ob-0", "s0", ",\"depends_on\":[\"ob-1\"]") + "," +
      obligation("ob-1", "s0", ",\"depends_on\":[\"ob-0\"]") + "]";
  BSM_CHECK_OK(plan, PlanDocument::parse(plan_with(stages, obligations)));
  const Result<DerivedPlan> compiled = DerivedPlan::compile(plan);
  BSM_CHECK(!compiled.ok());
  BSM_CHECK_EQ(compiled.error().code(), ErrorCode::PlanCycle);
  BSM_CHECK(compiled.error().detail_json().find("cycle") != std::string::npos);
}

BSM_TEST(self_dependency_is_refused) {
  const std::string obligations =
      "[" + obligation("ob-0", "s0", ",\"depends_on\":[\"ob-0\"]") + "]";
  BSM_CHECK_OK(plan, PlanDocument::parse(plan_with(kOneStage, obligations)));
  const Result<DerivedPlan> compiled = DerivedPlan::compile(plan);
  BSM_CHECK(!compiled.ok());
  BSM_CHECK_EQ(compiled.error().code(), ErrorCode::PlanCycle);
}

BSM_TEST(a_dependency_placed_in_a_later_stage_is_refused) {
  const std::string stages =
      std::string("[{\"id\":\"s0\",\"rank\":0,\"title\":\"first\"},") +
      "{\"id\":\"s1\",\"rank\":1,\"title\":\"second\"}]";
  const std::string obligations =
      "[" + obligation("ob-0", "s0", ",\"depends_on\":[\"ob-1\"]") + "," +
      obligation("ob-1", "s1", "") + "]";
  BSM_CHECK_OK(plan, PlanDocument::parse(plan_with(stages, obligations)));
  const Result<DerivedPlan> compiled = DerivedPlan::compile(plan);
  BSM_CHECK(!compiled.ok());
  BSM_CHECK_EQ(compiled.error().code(), ErrorCode::PlanStageOrderConflict);
}

BSM_TEST(an_in_stage_dependency_ordered_after_its_dependent_is_refused) {
  // ob-early is protected and therefore ordered first, but it depends on ob-late.
  const std::string obligations =
      "[{\"id\":\"ob-early\",\"title\":\"t\",\"stage\":\"s0\"," +
      std::string("\"priority\":\"protected\",\"owner_domain\":\"electrical\",") +
      "\"owner\":\"electrical-owner\",\"depends_on\":[\"ob-late\"]," +
      "\"requires\":[{\"kind\":\"probe_ready\",\"subject\":\"probe:1\"," +
      "\"min_sources\":1,\"required_owner\":\"electrical-owner\"," +
      "\"required_domain\":\"electrical\"}]," +
      "\"request\":{\"capability\":\"verify-isolation\",\"parameters\":{}}}," +
      "{\"id\":\"ob-late\",\"title\":\"t\",\"stage\":\"s0\"," +
      "\"priority\":\"standard\",\"owner_domain\":\"electrical\"," +
      "\"owner\":\"electrical-owner\",\"requires\":[{\"kind\":\"probe_ready\"," +
      "\"subject\":\"probe:1\",\"min_sources\":1," +
      "\"required_owner\":\"electrical-owner\"," +
      "\"required_domain\":\"electrical\"}]," +
      "\"request\":{\"capability\":\"verify-isolation\",\"parameters\":{}}}]";
  BSM_CHECK_OK(plan, PlanDocument::parse(plan_with(kOneStage, obligations)));
  const Result<DerivedPlan> compiled = DerivedPlan::compile(plan);
  BSM_CHECK(!compiled.ok());
  BSM_CHECK_EQ(compiled.error().code(), ErrorCode::PlanStageOrderConflict);
}

BSM_TEST(unknown_references_and_structural_defects_are_refused) {
  BSM_CHECK_OK(plan, PlanDocument::parse(plan_with(
                          kOneStage,
                          "[" + obligation("ob-0", "s0", ",\"depends_on\":[\"missing\"]") +
                              "]")));
  BSM_CHECK_EQ(DerivedPlan::compile(plan).error().code(), ErrorCode::PlanUnknownReference);

  // A plan whose return-to-service proof names an obligation it does not declare is
  // refused, and a plan that proves nothing at all is refused too.
  BSM_CHECK_OK(no_obligations, PlanDocument::parse(plan_with(kOneStage, "[]")));
  BSM_CHECK_EQ(DerivedPlan::compile(no_obligations).error().code(),
               ErrorCode::PlanUnknownReference);
  BSM_CHECK_OK(proof_less,
               PlanDocument::parse("{\"format_version\":1,\"facility\":\"f\","
                                   "\"policy\":{\"id\":\"p\",\"revision\":1,"
                                   "\"default_max_age_ticks\":1,"
                                   "\"default_attempt_budget\":1,"
                                   "\"strict_within_stage_order\":true},"
                                   "\"stages\":[{\"id\":\"s0\",\"rank\":0}],"
                                   "\"obligations\":[],"
                                   "\"return_to_service\":{\"evidence\":[],"
                                   "\"required_obligations\":[]}}"));
  BSM_CHECK_EQ(DerivedPlan::compile(proof_less).error().code(), ErrorCode::PlanInvalid);

  // A plan with no stage is refused while decoding.
  const Result<PlanDocument> empty_stages = PlanDocument::parse(plan_with("[]", "[]"));
  BSM_CHECK(!empty_stages.ok());
  BSM_CHECK_EQ(empty_stages.error().code(), ErrorCode::PlanEmpty);

  BSM_CHECK_OK(duplicate, PlanDocument::parse(plan_with(
                              kOneStage, "[" + obligation("ob-0", "s0", "") + "," +
                                             obligation("ob-0", "s0", "") + "]")));
  BSM_CHECK_EQ(DerivedPlan::compile(duplicate).error().code(),
               ErrorCode::DuplicateIdentity);

  BSM_CHECK_OK(unknown_stage,
               PlanDocument::parse(plan_with(kOneStage, "[" + obligation("ob-0", "s9", "") + "]")));
  BSM_CHECK_EQ(DerivedPlan::compile(unknown_stage).error().code(),
               ErrorCode::PlanUnknownReference);
}

BSM_TEST(obligation_defects_are_refused) {
  // Evidence-only obligations must not carry a request, and the decoder refuses the
  // contradiction before compilation ever sees it.
  BSM_CHECK(!PlanDocument::parse(plan_with(
                                  kOneStage,
                                  "[{\"id\":\"ob-0\",\"title\":\"t\"," +
                                      std::string("\"stage\":\"s0\",") +
                                      "\"owner_domain\":\"electrical\"," +
                                      "\"owner\":\"electrical-owner\"," +
                                      "\"consequential\":false," +
                                      "\"requires\":[{\"kind\":\"probe_ready\"," +
                                      "\"subject\":\"probe:1\",\"min_sources\":1}]," +
                                      "\"request\":{\"capability\":\"verify-isolation\"," +
                                      "\"parameters\":{}}}]"))
                 .ok());

  // An obligation with no readiness evidence is a checklist step, not a gate.
  BSM_CHECK_OK(no_evidence, PlanDocument::parse(plan_with(
                                kOneStage,
                                "[{\"id\":\"ob-0\",\"title\":\"t\",\"stage\":\"s0\"," +
                                    std::string("\"owner_domain\":\"electrical\",") +
                                    "\"owner\":\"electrical-owner\"," +
                                    "\"requires\":[]," +
                                    "\"request\":{\"capability\":\"verify-isolation\"," +
                                    "\"parameters\":{}}}]")));
  BSM_CHECK_EQ(DerivedPlan::compile(no_evidence).error().code(), ErrorCode::PlanInvalid);

  // A required owner without its authority domain is ambiguous.
  BSM_CHECK_OK(owner_only, PlanDocument::parse(plan_with(
                               kOneStage,
                               "[{\"id\":\"ob-0\",\"title\":\"t\",\"stage\":\"s0\"," +
                                   std::string("\"owner_domain\":\"electrical\",") +
                                   "\"owner\":\"electrical-owner\"," +
                                   "\"requires\":[{\"kind\":\"probe_ready\"," +
                                   "\"subject\":\"probe:1\",\"min_sources\":1," +
                                   "\"required_owner\":\"electrical-owner\"}]," +
                                   "\"request\":{\"capability\":\"verify-isolation\"," +
                                   "\"parameters\":{}}}]")));
  BSM_CHECK(!DerivedPlan::compile(owner_only).ok());

  // An attempt budget of zero would mean an obligation that can never be attempted.
  BSM_CHECK_OK(no_budget,
               PlanDocument::parse(plan_with(kOneStage, "[" + obligation(
                                                            "ob-0", "s0",
                                                            ",\"attempt_budget\":0") +
                                                            "]")));
  BSM_CHECK(!DerivedPlan::compile(no_budget).ok());
}

BSM_TEST(plan_documents_reject_unknown_fields_and_bad_versions) {
  BSM_CHECK(!PlanDocument::parse("{\"format_version\":1,\"facility\":\"f\","
                                 "\"policy\":{},\"stages\":[],\"obligations\":[],"
                                 "\"return_to_service\":{},\"extra\":1}")
                 .ok());
  BSM_CHECK(!PlanDocument::parse("{\"format_version\":9,\"facility\":\"f\","
                                 "\"policy\":{\"id\":\"p\",\"revision\":1,"
                                 "\"default_max_age_ticks\":1,"
                                 "\"default_attempt_budget\":1,"
                                 "\"strict_within_stage_order\":true},"
                                 "\"stages\":[],\"obligations\":[],"
                                 "\"return_to_service\":{\"evidence\":[],"
                                 "\"required_obligations\":[]}}")
                 .ok());
  BSM_CHECK(!PlanDocument::parse("[]").ok());
}

BSM_TEST(expectations_participate_in_the_plan_digest) {
  const std::string with_expectation =
      "[" + obligation("ob-0", "s0", "") + "]";
  const std::string without_expectation =
      "[" +
      std::string("{\"id\":\"ob-0\",\"title\":\"t\",\"stage\":\"s0\",") +
      "\"priority\":\"protected\",\"owner_domain\":\"electrical\"," +
      "\"owner\":\"electrical-owner\",\"requires\":[{\"kind\":\"probe_ready\"," +
      "\"subject\":\"probe:1\",\"min_sources\":1," +
      "\"required_owner\":\"electrical-owner\"," +
      "\"required_domain\":\"electrical\"}]," +
      "\"request\":{\"capability\":\"verify-isolation\",\"parameters\":{}}}]";
  BSM_CHECK_OK(first, PlanDocument::parse(plan_with(kOneStage, with_expectation)));
  BSM_CHECK_OK(second, PlanDocument::parse(plan_with(kOneStage, without_expectation)));
  BSM_CHECK_OK(first_plan, DerivedPlan::compile(first));
  BSM_CHECK_OK(second_plan, DerivedPlan::compile(second));
  BSM_CHECK(!(first_plan.digest() == second_plan.digest()));
  BSM_CHECK_OK(obligation_id, ObligationId::parse("ob-0"));
  BSM_CHECK(!(first_plan.obligation_digest(obligation_id) ==
              second_plan.obligation_digest(obligation_id)));
}

BSM_TEST(plan_bounds_are_enforced) {
  std::string obligations = "[";
  for (int index = 0; index < 5000; ++index) {
    if (index > 0) {
      obligations += ",";
    }
    obligations += obligation("ob-" + std::to_string(index), "s0", "");
  }
  obligations += "]";
  // The bound is enforced while decoding, before a plan of that size can be compiled.
  const Result<PlanDocument> plan = PlanDocument::parse(plan_with(kOneStage, obligations));
  BSM_CHECK(!plan.ok());
  BSM_CHECK_EQ(plan.error().code(), ErrorCode::LimitExceeded);
}

BSM_TEST_MAIN("bsm_test_plan")
