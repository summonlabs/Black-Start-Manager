#pragma once

// The restoration plan: stages, the dependency graph over restoration obligations,
// readiness evidence requirements, and the final return-to-service proof.
//
// A plan is a declaration, not an actuation authority. Compilation turns the
// declaration into a deterministic execution order and refuses a plan whose declared
// stage sequence contradicts its own dependency graph. Nothing in the derived plan
// depends on declaration order, map iteration order, or any host default.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "black_start_manager/authority.hpp"
#include "black_start_manager/canonical.hpp"
#include "black_start_manager/digest.hpp"
#include "black_start_manager/evidence.hpp"
#include "black_start_manager/ids.hpp"

namespace black_start_manager {

enum class Priority : std::uint8_t {
  // Protected and critical services are ordered ahead of standard workload in the
  // eligibility order of every stage.
  Protected = 0,
  Critical = 1,
  Standard = 2,
};

[[nodiscard]] const char* to_string(Priority priority) noexcept;
[[nodiscard]] Result<Priority> parse_priority(std::string_view text);

// A bounded request to an adjacent owner. The manager may only ever send a
// capability that the plan declares, with the parameters the plan fixes.
struct RequestSpec {
  CapabilityId capability;
  JsonValue parameters;  // canonical object; empty object when the capability takes none
};

struct ObligationDefinition {
  ObligationId id;
  std::string title;
  StageId stage;
  Priority priority = Priority::Standard;
  // Obligations that must be satisfied and verified before this one may be requested.
  std::vector<ObligationId> depends_on;  // sorted, unique
  // The adjacent authority domain and owner that perform the capability.
  AuthorityDomain owner_domain = AuthorityDomain::Control;
  OwnerId owner;
  // Readiness evidence that proves the obligation's objective is met.
  std::vector<EvidenceRequirement> required_evidence;
  // False for an evidence-only obligation: no request is ever sent for it.
  bool consequential = true;
  RequestSpec request;
  // Bounded number of attempts. Refused attempts count; a resolution that proves the
  // controller never applied the effect does not consume the budget.
  std::uint32_t attempt_budget = 1;
  // False for an optional obligation: it does not gate its stage or completion.
  bool required = true;

  friend bool operator==(const ObligationDefinition& left,
                         const ObligationDefinition& right) noexcept;
  friend bool operator!=(const ObligationDefinition& left,
                         const ObligationDefinition& right) noexcept {
    return !(left == right);
  }
};

struct StageDefinition {
  StageId id;
  // Declared rank. The derived order must agree with the dependency graph; a
  // contradiction is refused at compile time rather than silently reordered.
  std::uint64_t rank = 0;
  std::string title;
  // Evidence required before the stage may be entered.
  std::vector<EvidenceRequirement> entry_evidence;
  // Evidence required before the stage may be left.
  std::vector<EvidenceRequirement> exit_evidence;
};

struct ReturnToServiceRequirement {
  std::vector<EvidenceRequirement> evidence;
  // Obligations that must be satisfied at completion. Empty means every required
  // obligation in the plan.
  std::vector<ObligationId> required_obligations;
};

struct PlanPolicy {
  PolicyTagId id;
  std::uint64_t revision = 0;
  std::uint64_t default_max_age_ticks = 1000;
  std::uint32_t default_attempt_budget = 2;
  // When true, an obligation inside a stage may only be requested after every
  // earlier-ordered obligation of that stage (protected and critical first) is
  // satisfied.
  bool strict_within_stage_order = true;
};

struct PlanDocument {
  std::uint16_t format_version = 0;
  FacilityId facility;
  PlanPolicy policy;
  std::vector<StageDefinition> stages;
  std::vector<ObligationDefinition> obligations;
  ReturnToServiceRequirement return_to_service;

  [[nodiscard]] static Result<PlanDocument> parse(std::string_view text);
  [[nodiscard]] Result<JsonValue> to_json() const;
};

struct CompiledStage {
  StageId id;
  std::uint64_t rank = 0;
  std::size_t index = 0;  // position in the derived execution order
  std::string title;
  std::vector<std::size_t> obligation_indices;  // into DerivedPlan::obligations()
  // Stages that must complete before this stage may be entered, derived from
  // obligation dependencies. Sorted, unique.
  std::vector<StageId> depends_on_stages;
};

// A plan that has been validated and given a deterministic execution order.
class DerivedPlan {
 public:
  DerivedPlan() = default;

  [[nodiscard]] static Result<DerivedPlan> compile(PlanDocument document);
  // Strictly decodes the canonical derived form, rejects tampering, and recompiles
  // the declared document so the derived order and digests are reproduced exactly.
  [[nodiscard]] static Result<DerivedPlan> from_json(const JsonValue& value);

  [[nodiscard]] const PlanDocument& document() const noexcept { return document_; }
  [[nodiscard]] const std::vector<CompiledStage>& stages() const noexcept { return stages_; }
  [[nodiscard]] const std::vector<ObligationDefinition>& obligations() const noexcept {
    return obligations_;
  }
  [[nodiscard]] Digest digest() const noexcept { return digest_; }
  [[nodiscard]] bool empty() const noexcept { return stages_.empty(); }

  // Index of a stage in the derived order; stages().size() when absent.
  [[nodiscard]] std::size_t stage_index(const StageId& id) const noexcept;
  [[nodiscard]] std::size_t obligation_index(const ObligationId& id) const noexcept;
  [[nodiscard]] bool has_obligation(const ObligationId& id) const noexcept;
  [[nodiscard]] Digest obligation_digest(const ObligationId& id) const;
  [[nodiscard]] const ObligationDefinition* obligation(const ObligationId& id) const noexcept;

  [[nodiscard]] Result<JsonValue> to_json() const;

 private:
  PlanDocument document_;
  std::vector<CompiledStage> stages_;
  std::vector<ObligationDefinition> obligations_;
  std::vector<Digest> obligation_digests_;
  Digest digest_;
};

}  // namespace black_start_manager
