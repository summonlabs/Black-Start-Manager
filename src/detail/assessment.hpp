#pragma once

// Internal assessment engine: the deterministic function from (plan, committed session
// record, observed binding, current tick) to the explanation of what may happen next.
//
// This header is private to the implementation. It is deliberately separate from the
// manager so the gate rules can be exercised directly by tests and reused by the
// report writer.

#include <cstdint>
#include <vector>

#include "black_start_manager/authority.hpp"
#include "black_start_manager/evidence.hpp"
#include "black_start_manager/plan.hpp"
#include "black_start_manager/session.hpp"

namespace black_start_manager::detail {

struct RequirementEvaluation {
  bool satisfied = false;
  EvidenceDeficitCode code = EvidenceDeficitCode::Missing;
  std::uint32_t current_sources = 0;
  std::vector<EvidenceId> usable;
  std::vector<EvidenceId> rejected;
};

// Evaluates one readiness evidence requirement against the session's evidence ledger.
// A record can only count when it is live, current for the session's plan, binding, and
// facility epoch, independent, sourced from the bound authority generation, and inside
// its age window. Contradictory current sources fail the requirement rather than being
// averaged away.
[[nodiscard]] RequirementEvaluation evaluate_requirement(
    const EvidenceRequirement& requirement, const std::vector<EvidenceRecord>& evidence,
    const SessionRecord& session, const PlanPolicy& policy, std::uint64_t now_tick);

[[nodiscard]] std::vector<EvidenceId> satisfied_evidence_ids(
    const std::vector<EvidenceRequirement>& requirements,
    const std::vector<EvidenceRecord>& evidence, const SessionRecord& session,
    const PlanPolicy& policy, std::uint64_t now_tick);

[[nodiscard]] BlockCode block_code_for(EvidenceDeficitCode code) noexcept;

// Full derived assessment. Pure: it reads only its arguments and never mutates state.
[[nodiscard]] Result<Assessment> assess_session(const DerivedPlan& plan,
                                               const SessionRecord& session,
                                               const FacilityBinding& observed_binding,
                                               std::uint64_t now_tick);

// Obligation state derived from the committed attempt history and current readiness.
[[nodiscard]] ObligationState obligation_state_for(const ObligationDefinition& obligation,
                                                   const std::vector<AttemptRecord>& attempts,
                                                   bool satisfied);

// Currentness classification of a single record against one requirement.
[[nodiscard]] EvidenceDeficitCode classify_record(const EvidenceRecord& record,
                                                  const SessionRecord& session,
                                                  std::uint64_t effective_max_age,
                                                  std::uint64_t now_tick) noexcept;

}  // namespace black_start_manager::detail
