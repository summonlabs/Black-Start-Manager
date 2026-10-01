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

#include "black_start_manager/session.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "detail/assessment.hpp"

namespace black_start_manager {
namespace {

[[nodiscard]] std::uint64_t effective_max_age(const EvidenceRequirement& requirement,
                                              const PlanPolicy& policy) noexcept {
  return requirement.max_age_ticks == 0 ? policy.default_max_age_ticks
                                        : requirement.max_age_ticks;
}

[[nodiscard]] bool same_kind_and_subject(const EvidenceRecord& record,
                                         const EvidenceRequirement& requirement) noexcept {
  return record.kind == requirement.kind && record.subject == requirement.subject;
}

// Partial match: every field the expectation names must be present and equal. Arrays and
// scalars must match exactly. This is what lets an observation carry extra provenance
// fields without weakening the readiness claim.
[[nodiscard]] bool matches_expectation(const JsonValue& value, const JsonValue& expect) {
  if (expect.is_object()) {
    if (!value.is_object()) {
      return false;
    }
    for (const JsonValue::Field& field : expect.as_object()) {
      const JsonValue* observed = value.find(field.first);
      if (observed == nullptr || !matches_expectation(*observed, field.second)) {
        return false;
      }
    }
    return true;
  }
  if (expect.is_array()) {
    if (!value.is_array() || value.as_array().size() != expect.as_array().size()) {
      return false;
    }
    for (std::size_t index = 0; index < expect.as_array().size(); ++index) {
      if (!matches_expectation(value.as_array()[index], expect.as_array()[index])) {
        return false;
      }
    }
    return true;
  }
  return value == expect;
}

[[nodiscard]] FenceCode fence_code_for(BindingChangeCode code) noexcept {
  switch (code) {
    case BindingChangeCode::FacilityChanged:
    case BindingChangeCode::EpochChanged:
      return FenceCode::EpochChanged;
    case BindingChangeCode::TopologyChanged:
      return FenceCode::TopologyChanged;
    case BindingChangeCode::PolicyChanged:
      return FenceCode::PolicyChanged;
    case BindingChangeCode::IncidentChanged:
    case BindingChangeCode::IncidentGenerationChanged:
      return FenceCode::IncidentChanged;
    case BindingChangeCode::AuthorityRemoved:
      return FenceCode::AuthorityRemoved;
    case BindingChangeCode::AuthorityOwnerChanged:
      return FenceCode::AuthorityOwnerChanged;
    case BindingChangeCode::AuthorityGenerationStale:
      return FenceCode::AuthorityGenerationStale;
    case BindingChangeCode::AuthorityAttestationChanged:
      return FenceCode::AuthorityAttestationChanged;
    case BindingChangeCode::PrerequisiteRemoved:
    case BindingChangeCode::PrerequisiteGenerationStale:
      return FenceCode::PrerequisiteGenerationStale;
    case BindingChangeCode::AuthorityAdded:
    case BindingChangeCode::AuthorityGenerationAdvanced:
    case BindingChangeCode::PrerequisiteAdded:
    case BindingChangeCode::PrerequisiteGenerationAdvanced:
      // Advances do not fence the session; they require explicit re-establishment,
      // which is reported as AuthorityNotReestablished.
      return FenceCode::IncidentChanged;
  }
  return FenceCode::IncidentChanged;
}

[[nodiscard]] bool is_fence_causing(BindingChangeCode code) noexcept {
  switch (code) {
    case BindingChangeCode::AuthorityAdded:
    case BindingChangeCode::AuthorityGenerationAdvanced:
    case BindingChangeCode::PrerequisiteAdded:
    case BindingChangeCode::PrerequisiteGenerationAdvanced:
    case BindingChangeCode::IncidentGenerationChanged:
      return false;
    default:
      return true;
  }
}

[[nodiscard]] BlockCode block_for_deficits(const std::vector<EvidenceDeficit>& deficits,
                                           BlockCode stale_code,
                                           BlockCode missing_code) noexcept {
  BlockCode result = BlockCode::None;
  int best = 1000;
  for (const EvidenceDeficit& deficit : deficits) {
    BlockCode candidate = BlockCode::None;
    switch (deficit.code) {
      case EvidenceDeficitCode::Missing:
        candidate = missing_code;
        break;
      case EvidenceDeficitCode::ExpectationMismatch:
        candidate = BlockCode::EvidenceContradictory;
        break;
      case EvidenceDeficitCode::RecoveredOnly:
        candidate = BlockCode::EvidenceRecovered;
        break;
      case EvidenceDeficitCode::Expired:
      case EvidenceDeficitCode::ForeignGeneration:
        candidate = stale_code;
        break;
      case EvidenceDeficitCode::ForeignOwner:
      case EvidenceDeficitCode::NotIndependent:
        candidate = BlockCode::EvidenceNotIndependent;
        break;
      case EvidenceDeficitCode::Contradictory:
        candidate = BlockCode::EvidenceContradictory;
        break;
      case EvidenceDeficitCode::InsufficientSources:
      case EvidenceDeficitCode::KindMismatch:
        candidate = BlockCode::EvidenceInsufficient;
        break;
    }
    const int precedence = block_precedence(candidate);
    if (candidate != BlockCode::None && precedence < best) {
      best = precedence;
      result = candidate;
    }
  }
  return result;
}

void note(std::vector<std::string>& parts, const std::string& text) {
  if (!text.empty()) {
    parts.push_back(text);
  }
}

[[nodiscard]] std::string join(const std::vector<std::string>& parts) {
  std::string out;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index > 0) {
      out.append("; ");
    }
    out.append(parts[index]);
  }
  return out;
}

}  // namespace

namespace detail {

EvidenceDeficitCode classify_record(const EvidenceRecord& record,
                                    const SessionRecord& session,
                                    std::uint64_t effective_max_age,
                                    std::uint64_t now_tick) noexcept {
  if (record.provenance != EvidenceProvenance::Live) {
    return EvidenceDeficitCode::RecoveredOnly;
  }
  if (!(record.plan_digest == session.plan_digest) ||
      !(record.binding_digest == session.binding_digest) ||
      record.facility_epoch != session.binding.facility_epoch) {
    return EvidenceDeficitCode::ForeignGeneration;
  }
  if (record.observed_tick > now_tick) {
    return EvidenceDeficitCode::Expired;
  }
  if (now_tick - record.observed_tick > effective_max_age) {
    return EvidenceDeficitCode::Expired;
  }
  if (!channel_is_independent(record.channel)) {
    return EvidenceDeficitCode::NotIndependent;
  }
  // The record must be attested by the authority generation the session is bound to.
  const AuthorityRef* authority =
      find_authority(session.binding, record.source_domain, record.source_owner);
  if (authority == nullptr || authority->generation != record.source_generation) {
    return EvidenceDeficitCode::ForeignGeneration;
  }
  return EvidenceDeficitCode::Missing;  // no deficit: the record is usable
}

RequirementEvaluation evaluate_requirement(const EvidenceRequirement& requirement,
                                           const std::vector<EvidenceRecord>& evidence,
                                           const SessionRecord& session,
                                           const PlanPolicy& policy,
                                           std::uint64_t now_tick) {
  RequirementEvaluation evaluation;
  const std::uint64_t max_age = effective_max_age(requirement, policy);

  std::vector<const EvidenceRecord*> usable;
  std::vector<const EvidenceRecord*> off_owner;
  std::size_t from_required_owner = 0;
  std::size_t in_required_domain = 0;
  bool saw_recovered = false;
  bool saw_expired = false;
  bool saw_foreign_generation = false;
  bool saw_foreign_owner = false;
  bool saw_not_independent = false;
  bool saw_expectation = false;

  for (const EvidenceRecord& record : evidence) {
    if (!same_kind_and_subject(record, requirement)) {
      continue;
    }
    const EvidenceDeficitCode code = classify_record(record, session, max_age, now_tick);
    if (code == EvidenceDeficitCode::Missing && requirement.has_expect &&
        !matches_expectation(record.value, requirement.expect)) {
      // The observation is current and independent but does not assert the required
      // readiness, so it never satisfies the requirement.
      evaluation.rejected.push_back(record.id);
      saw_expectation = true;
      continue;
    }
    if (code == EvidenceDeficitCode::Missing) {
      usable.push_back(&record);
      if (requirement.has_required_owner &&
          record.source_owner == requirement.required_owner) {
        ++from_required_owner;
      }
      if (requirement.has_required_domain &&
          record.source_domain == requirement.required_domain) {
        ++in_required_domain;
      }
      if ((requirement.has_required_owner &&
           !(record.source_owner == requirement.required_owner)) ||
          (requirement.has_required_domain &&
           record.source_domain != requirement.required_domain)) {
        off_owner.push_back(&record);
      }
      evaluation.usable.push_back(record.id);
      continue;
    }
    evaluation.rejected.push_back(record.id);
    switch (code) {
      case EvidenceDeficitCode::RecoveredOnly:
        saw_recovered = true;
        break;
      case EvidenceDeficitCode::Expired:
        saw_expired = true;
        break;
      case EvidenceDeficitCode::ForeignGeneration:
        saw_foreign_generation = true;
        break;
      case EvidenceDeficitCode::ForeignOwner:
        saw_foreign_owner = true;
        break;
      case EvidenceDeficitCode::NotIndependent:
        saw_not_independent = true;
        break;
      case EvidenceDeficitCode::ExpectationMismatch:
        saw_expectation = true;
        break;
      default:
        break;
    }
  }

  if (usable.empty()) {
    evaluation.current_sources = 0;
    evaluation.satisfied = false;
    if (saw_expectation) {
      evaluation.code = EvidenceDeficitCode::ExpectationMismatch;
    } else if (saw_recovered) {
      evaluation.code = EvidenceDeficitCode::RecoveredOnly;
    } else if (saw_expired) {
      evaluation.code = EvidenceDeficitCode::Expired;
    } else if (saw_foreign_generation) {
      evaluation.code = EvidenceDeficitCode::ForeignGeneration;
    } else if (saw_foreign_owner) {
      evaluation.code = EvidenceDeficitCode::ForeignOwner;
    } else if (saw_not_independent) {
      evaluation.code = EvidenceDeficitCode::NotIndependent;
    } else {
      evaluation.code = EvidenceDeficitCode::Missing;
    }
    return evaluation;
  }

  if (requirement.has_required_owner && from_required_owner == 0) {
    evaluation.code = EvidenceDeficitCode::ForeignOwner;
    evaluation.current_sources = static_cast<std::uint32_t>(usable.size());
    evaluation.satisfied = false;
    for (const EvidenceRecord* record : off_owner) {
      evaluation.rejected.push_back(record->id);
    }
    return evaluation;
  }
  if (requirement.has_required_domain && in_required_domain == 0) {
    evaluation.code = EvidenceDeficitCode::ForeignOwner;
    evaluation.current_sources = static_cast<std::uint32_t>(usable.size());
    evaluation.satisfied = false;
    for (const EvidenceRecord* record : off_owner) {
      evaluation.rejected.push_back(record->id);
    }
    return evaluation;
  }

  // Contradiction: current, usable sources that disagree. Averaging is never allowed.
  const JsonValue& reference = usable.front()->value;
  for (const EvidenceRecord* record : usable) {
    if (!(record->value == reference)) {
      evaluation.code = EvidenceDeficitCode::Contradictory;
      evaluation.current_sources = static_cast<std::uint32_t>(usable.size());
      evaluation.satisfied = false;
      return evaluation;
    }
  }

  evaluation.current_sources = static_cast<std::uint32_t>(usable.size());
  if (evaluation.current_sources < requirement.min_sources) {
    evaluation.code = EvidenceDeficitCode::InsufficientSources;
    evaluation.satisfied = false;
    return evaluation;
  }
  evaluation.satisfied = true;
  evaluation.code = EvidenceDeficitCode::Missing;
  return evaluation;
}

std::vector<EvidenceId> satisfied_evidence_ids(
    const std::vector<EvidenceRequirement>& requirements,
    const std::vector<EvidenceRecord>& evidence, const SessionRecord& session,
    const PlanPolicy& policy, std::uint64_t now_tick) {
  std::vector<EvidenceId> ids;
  for (const EvidenceRequirement& requirement : requirements) {
    const RequirementEvaluation evaluation =
        evaluate_requirement(requirement, evidence, session, policy, now_tick);
    if (!evaluation.satisfied) {
      continue;
    }
    for (const EvidenceId& id : evaluation.usable) {
      if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
        ids.push_back(id);
      }
    }
  }
  return ids;
}

BlockCode block_code_for(EvidenceDeficitCode code) noexcept {
  switch (code) {
    case EvidenceDeficitCode::Missing:
    case EvidenceDeficitCode::KindMismatch:
      return BlockCode::EvidenceMissing;
    case EvidenceDeficitCode::ExpectationMismatch:
      return BlockCode::EvidenceContradictory;
    case EvidenceDeficitCode::RecoveredOnly:
      return BlockCode::EvidenceRecovered;
    case EvidenceDeficitCode::Expired:
    case EvidenceDeficitCode::ForeignGeneration:
      return BlockCode::EvidenceStale;
    case EvidenceDeficitCode::ForeignOwner:
    case EvidenceDeficitCode::NotIndependent:
      return BlockCode::EvidenceNotIndependent;
    case EvidenceDeficitCode::Contradictory:
      return BlockCode::EvidenceContradictory;
    case EvidenceDeficitCode::InsufficientSources:
      return BlockCode::EvidenceInsufficient;
  }
  return BlockCode::EvidenceMissing;
}

ObligationState obligation_state_for(const ObligationDefinition& obligation,
                                     const std::vector<AttemptRecord>& attempts,
                                     bool satisfied) {
  bool has_fenced = false;
  bool has_in_flight = false;
  bool has_acknowledged = false;
  std::uint64_t consumed = 0;
  for (const AttemptRecord& attempt : attempts) {
    if (attempt.state == AttemptState::Fenced) {
      has_fenced = true;
    }
    if (attempt_is_in_flight(attempt.state)) {
      has_in_flight = true;
    }
    if (attempt.state == AttemptState::Acknowledged) {
      has_acknowledged = true;
    }
    if (attempt_consumes_budget(attempt.state)) {
      ++consumed;
    }
  }
  if (satisfied) {
    return ObligationState::Satisfied;
  }
  if (has_fenced) {
    return ObligationState::Fenced;
  }
  if (has_in_flight) {
    return ObligationState::InFlight;
  }
  if (has_acknowledged) {
    return ObligationState::Acknowledged;
  }
  if (consumed >= obligation.attempt_budget && !attempts.empty()) {
    return ObligationState::Failed;
  }
  return ObligationState::Pending;
}

Result<Assessment> assess_session(const DerivedPlan& plan, const SessionRecord& session,
                                   const FacilityBinding& observed_binding,
                                   std::uint64_t now_tick) {
  Assessment assessment;
  assessment.session = session.id;
  BSM_TRY_ASSIGN(digest, session_digest(session));
  assessment.session_digest = digest;
  assessment.state = session.state;
  assessment.plan_digest = session.plan_digest;
  assessment.binding_digest = session.binding_digest;
  BSM_TRY_ASSIGN(observed_digest, binding_digest(observed_binding));
  assessment.observed_binding_digest = observed_digest;
  assessment.authority_reestablished = session.authority_reestablished;
  assessment.stage_index = session.stage_index;
  assessment.stage_count = plan.stages().size();

  BSM_TRY_ASSIGN(delta, compare_bindings(session.binding, observed_binding));
  assessment.delta = delta;
  for (const BindingChange& change : delta.changes) {
    if (!is_fence_causing(change.code)) {
      continue;
    }
    const FenceCode code = fence_code_for(change.code);
    if (std::find(assessment.fence_codes.begin(), assessment.fence_codes.end(), code) ==
        assessment.fence_codes.end()) {
      assessment.fence_codes.push_back(code);
    }
  }
  assessment.fenced = !assessment.fence_codes.empty();

  if (session.stage_index < plan.stages().size()) {
    assessment.current_stage = plan.stages()[session.stage_index].id;
  }

  // Obligations.
  assessment.obligations.reserve(plan.obligations().size());
  std::vector<bool> satisfied_flags;
  satisfied_flags.reserve(plan.obligations().size());
  for (std::size_t index = 0; index < plan.obligations().size(); ++index) {
    const ObligationDefinition& obligation = plan.obligations()[index];
    ObligationAssessment entry;
    entry.id = obligation.id;
    entry.stage = obligation.stage;
    entry.priority = obligation.priority;
    entry.index = index;
    entry.required = obligation.required;
    entry.attempt_budget = obligation.attempt_budget;

    std::vector<AttemptRecord> attempts;
    for (const AttemptRecord& attempt : session.attempts) {
      if (attempt.obligation == obligation.id) {
        attempts.push_back(attempt);
      }
    }
    std::sort(attempts.begin(), attempts.end(),
              [](const AttemptRecord& left, const AttemptRecord& right) {
                return left.sequence < right.sequence;
              });
    std::uint64_t consumed = 0;
    for (const AttemptRecord& attempt : attempts) {
      entry.attempts.push_back(attempt.id);
      if (attempt_consumes_budget(attempt.state)) {
        ++consumed;
      }
      if (attempt_blocks_requests(attempt.state)) {
        entry.unresolved_attempts.push_back(attempt.id);
      }
    }
    entry.attempts_used = consumed;

    bool satisfied = true;
    for (const EvidenceRequirement& requirement : obligation.required_evidence) {
      const RequirementEvaluation evaluation = evaluate_requirement(
          requirement, session.evidence, session, plan.document().policy, now_tick);
      if (!evaluation.satisfied) {
        satisfied = false;
        EvidenceDeficit deficit;
        deficit.kind = requirement.kind;
        deficit.subject = requirement.subject;
        deficit.code = evaluation.code;
        deficit.required_sources = requirement.min_sources;
        deficit.current_sources = evaluation.current_sources;
        deficit.usable = evaluation.usable;
        deficit.rejected = evaluation.rejected;
        entry.deficits.push_back(std::move(deficit));
        continue;
      }
      for (const EvidenceId& id : evaluation.usable) {
        if (std::find(entry.satisfied_by.begin(), entry.satisfied_by.end(), id) ==
            entry.satisfied_by.end()) {
          entry.satisfied_by.push_back(id);
        }
      }
    }
    entry.satisfied = satisfied;
    satisfied_flags.push_back(satisfied);
    entry.state = obligation_state_for(obligation, attempts, satisfied);
    assessment.obligations.push_back(std::move(entry));
  }

  // Stage gates.
  assessment.stages.reserve(plan.stages().size());
  for (std::size_t index = 0; index < plan.stages().size(); ++index) {
    const CompiledStage& stage = plan.stages()[index];
    const StageDefinition* definition = nullptr;
    for (const StageDefinition& candidate : plan.document().stages) {
      if (candidate.id == stage.id) {
        definition = &candidate;
        break;
      }
    }
    if (definition == nullptr) {
      return Error(ErrorCode::InternalError, "compiled stage is missing from the document");
    }
    StageAssessment stage_assessment;
    stage_assessment.id = stage.id;
    stage_assessment.index = index;
    stage_assessment.current = index == session.stage_index;
    stage_assessment.entered = index <= session.stage_index;
    for (const EvidenceRequirement& requirement : definition->entry_evidence) {
      const RequirementEvaluation evaluation = evaluate_requirement(
          requirement, session.evidence, session, plan.document().policy, now_tick);
      stage_assessment.entry_satisfied = stage_assessment.entry_satisfied || evaluation.satisfied;
      if (!evaluation.satisfied) {
        EvidenceDeficit deficit;
        deficit.kind = requirement.kind;
        deficit.subject = requirement.subject;
        deficit.code = evaluation.code;
        deficit.required_sources = requirement.min_sources;
        deficit.current_sources = evaluation.current_sources;
        deficit.usable = evaluation.usable;
        deficit.rejected = evaluation.rejected;
        stage_assessment.entry_deficits.push_back(std::move(deficit));
      }
    }
    if (definition->entry_evidence.empty()) {
      stage_assessment.entry_satisfied = true;
    }
    bool obligations_satisfied = true;
    for (const std::size_t obligation_index : stage.obligation_indices) {
      if (!plan.obligations()[obligation_index].required) {
        continue;
      }
      if (!satisfied_flags[obligation_index]) {
        obligations_satisfied = false;
        break;
      }
    }
    stage_assessment.obligations_satisfied = obligations_satisfied;
    for (const EvidenceRequirement& requirement : definition->exit_evidence) {
      const RequirementEvaluation evaluation = evaluate_requirement(
          requirement, session.evidence, session, plan.document().policy, now_tick);
      if (!evaluation.satisfied) {
        EvidenceDeficit deficit;
        deficit.kind = requirement.kind;
        deficit.subject = requirement.subject;
        deficit.code = evaluation.code;
        deficit.required_sources = requirement.min_sources;
        deficit.current_sources = evaluation.current_sources;
        deficit.usable = evaluation.usable;
        deficit.rejected = evaluation.rejected;
        stage_assessment.exit_deficits.push_back(std::move(deficit));
      }
    }
    assessment.stages.push_back(std::move(stage_assessment));
  }

  const bool terminal = session_is_terminal(session.state);
  const bool held = session.state == SessionState::Held;
  const bool gate_open = !terminal && !assessment.fenced &&
                         session.authority_reestablished && !held;

  const StageAssessment* current_stage_assessment =
      session.stage_index < assessment.stages.size() ? &assessment.stages[session.stage_index]
                                                     : nullptr;
  const bool stage_entry_satisfied =
      current_stage_assessment == nullptr || current_stage_assessment->entry_satisfied;

  // Eligibility: dependencies, precedence, budget, unresolved attempts, and the stage
  // entry gate all have to hold before an obligation may be requested.
  for (std::size_t index = 0; index < assessment.obligations.size(); ++index) {
    ObligationAssessment& entry = assessment.obligations[index];
    const ObligationDefinition& obligation = plan.obligations()[index];
    BlockCode block = BlockCode::None;

    if (assessment.fenced) {
      block = BlockCode::SessionFenced;
    } else if (terminal) {
      block = session.state == SessionState::Aborted
                  ? BlockCode::SessionAborted
                  : (session.state == SessionState::Completed ? BlockCode::SessionCompleted
                                                              : BlockCode::SessionSuperseded);
    } else if (!session.authority_reestablished) {
      block = BlockCode::AuthorityNotReestablished;
    } else if (held) {
      block = BlockCode::HoldActive;
    } else if (index < plan.stages().size() &&
               obligation.stage != assessment.current_stage) {
      block = BlockCode::PrerequisiteUnsatisfied;
    } else if (!entry.unresolved_attempts.empty()) {
      block = BlockCode::UnresolvedAttempt;
    } else if (!stage_entry_satisfied) {
      block = block_for_deficits(current_stage_assessment->entry_deficits,
                                 BlockCode::StageEntryEvidenceStale,
                                 BlockCode::StageEntryEvidenceMissing);
    } else {
      bool prerequisites_ok = true;
      ObligationId first_missing;
      for (const ObligationId& dependency : obligation.depends_on) {
        const std::size_t dependency_index = plan.obligation_index(dependency);
        if (dependency_index >= satisfied_flags.size()) {
          continue;
        }
        if (!satisfied_flags[dependency_index]) {
          prerequisites_ok = false;
          if (first_missing.empty()) {
            first_missing = dependency;
          }
        }
      }
      if (!prerequisites_ok) {
        block = BlockCode::PrerequisiteUnsatisfied;
      } else if (plan.document().policy.strict_within_stage_order) {
        bool precedence_ok = true;
        for (std::size_t other = 0; other < index; ++other) {
          if (plan.obligations()[other].stage != obligation.stage) {
            continue;
          }
          if (!plan.obligations()[other].required) {
            continue;
          }
          if (!satisfied_flags[other]) {
            precedence_ok = false;
            break;
          }
        }
        if (!precedence_ok) {
          block = BlockCode::PrecedenceBlocked;
        }
      }

      if (block == BlockCode::None) {
        bool contradictory = false;
        for (const EvidenceDeficit& deficit : entry.deficits) {
          if (deficit.code == EvidenceDeficitCode::Contradictory) {
            contradictory = true;
          }
        }
        if (entry.satisfied) {
          block = BlockCode::None;
        } else if (entry.attempts_used >= obligation.attempt_budget) {
          block = BlockCode::AttemptBudgetExhausted;
        } else if (entry.state == ObligationState::Acknowledged) {
          block = BlockCode::EffectNotVerified;
        } else if (contradictory) {
          block = BlockCode::EvidenceContradictory;
        }
      }
    }

    entry.block = block;
    const bool requestable_here =
        obligation.consequential && !entry.satisfied && block == BlockCode::None;
    entry.eligible = requestable_here;
    if (requestable_here) {
      assessment.eligible.push_back(obligation.id);
    }
  }

  // Next stage and completion.
  if (session.stage_index + 1 < plan.stages().size()) {
    assessment.has_next_stage = true;
    assessment.next_stage = plan.stages()[session.stage_index + 1].id;
    assessment.next_stage_entry_deficits = assessment.stages[session.stage_index + 1].entry_deficits;
    const StageAssessment& current = assessment.stages[session.stage_index];
    assessment.next_stage_ready = current.exit_deficits.empty() &&
                                  current.obligations_satisfied &&
                                  assessment.next_stage_entry_deficits.empty();
  } else if (session.stage_index < plan.stages().size()) {
    const StageAssessment& current = assessment.stages[session.stage_index];
    assessment.next_stage_ready = current.exit_deficits.empty() && current.obligations_satisfied;
  }

  std::vector<ObligationId> required_obligations =
      plan.document().return_to_service.required_obligations;
  if (required_obligations.empty()) {
    for (const ObligationDefinition& obligation : plan.obligations()) {
      if (obligation.required) {
        required_obligations.push_back(obligation.id);
      }
    }
  }
  for (const ObligationId& id : required_obligations) {
    const std::size_t index = plan.obligation_index(id);
    if (index >= satisfied_flags.size() || !satisfied_flags[index]) {
      assessment.completion_missing_obligations.push_back(id);
    }
  }
  for (const EvidenceRequirement& requirement : plan.document().return_to_service.evidence) {
    const RequirementEvaluation evaluation = evaluate_requirement(
        requirement, session.evidence, session, plan.document().policy, now_tick);
    if (!evaluation.satisfied) {
      EvidenceDeficit deficit;
      deficit.kind = requirement.kind;
      deficit.subject = requirement.subject;
      deficit.code = evaluation.code;
      deficit.required_sources = requirement.min_sources;
      deficit.current_sources = evaluation.current_sources;
      deficit.usable = evaluation.usable;
      deficit.rejected = evaluation.rejected;
      assessment.completion_deficits.push_back(std::move(deficit));
    }
  }
  assessment.completion_ready = gate_open && assessment.completion_deficits.empty() &&
                                assessment.completion_missing_obligations.empty() &&
                                assessment.next_stage_ready;

  // Primary explanation, by fixed precedence.
  std::vector<std::string> parts;
  BlockCode primary = BlockCode::None;
  if (terminal) {
    primary = session.state == SessionState::Aborted
                  ? BlockCode::SessionAborted
                  : (session.state == SessionState::Completed ? BlockCode::SessionCompleted
                                                              : BlockCode::SessionSuperseded);
    note(parts, std::string("session is ") + to_string(session.state));
  } else if (assessment.fenced) {
    primary = BlockCode::SessionFenced;
    note(parts, std::string("session is fenced: ") +
                    to_string(assessment.fence_codes.front()));
    for (std::size_t index = 1; index < assessment.fence_codes.size(); ++index) {
      parts.push_back(to_string(assessment.fence_codes[index]));
    }
  } else if (!session.authority_reestablished) {
    primary = BlockCode::AuthorityNotReestablished;
    note(parts, "process authority has not been re-established by this incarnation");
  } else if (held) {
    primary = BlockCode::HoldActive;
    note(parts, "session is held");
    if (!session.holds.empty()) {
      parts.push_back(session.holds.back().reason);
    }
  } else {
    // Unresolved attempts first: they are the only condition that can lose a
    // consequential request if ignored.
    for (const ObligationAssessment& entry : assessment.obligations) {
      if (!entry.unresolved_attempts.empty()) {
        primary = BlockCode::UnresolvedAttempt;
        note(parts, "attempt " + entry.unresolved_attempts.front().hex() +
                        " for obligation " + entry.id.str() + " is unresolved");
        break;
      }
    }
    if (primary == BlockCode::None &&
        !current_stage_assessment->entry_deficits.empty()) {
      primary = block_for_deficits(current_stage_assessment->entry_deficits,
                                   BlockCode::StageEntryEvidenceStale,
                                   BlockCode::StageEntryEvidenceMissing);
      note(parts, std::string("stage ") + assessment.current_stage.str() +
                      " entry evidence is not satisfied");
    }
    if (primary == BlockCode::None) {
      for (const ObligationAssessment& entry : assessment.obligations) {
        if (entry.block != BlockCode::None && !entry.satisfied) {
          primary = entry.block;
          note(parts, std::string("obligation ") + entry.id.str() + " is blocked (" +
                          to_string(entry.block) + ")");
          if (!entry.deficits.empty()) {
            parts.push_back(std::string("missing evidence ") +
                            entry.deficits.front().kind.str() + "/" +
                            entry.deficits.front().subject.str() + " (" +
                            to_string(entry.deficits.front().code) + ")");
          }
          break;
        }
      }
    }
    if (primary == BlockCode::None && assessment.eligible.empty() &&
        !assessment.completion_ready) {
      primary = BlockCode::NoEligibleObligation;
      note(parts, "no obligation is eligible for a bounded request");
    }
    if (primary == BlockCode::None && assessment.completion_ready) {
      note(parts, "session may advance to return-to-service proof");
    }
  }
  assessment.primary_block = primary;
  assessment.explanation = join(parts);
  return assessment;
}

}  // namespace detail

Result<JsonValue> assessment_to_json(const Assessment& assessment) {
  JsonObjectBuilder root;
  root.set_text("session", assessment.session.hex());
  root.set_text("session_digest", assessment.session_digest.hex());
  root.set_text("state", to_string(assessment.state));
  root.set_text("plan_digest", assessment.plan_digest.hex());
  root.set_text("binding_digest", assessment.binding_digest.hex());
  root.set_text("observed_binding_digest", assessment.observed_binding_digest.hex());
  root.set_bool("authority_reestablished", assessment.authority_reestablished);
  root.set_bool("fenced", assessment.fenced);
  JsonArrayBuilder fences;
  for (const FenceCode code : assessment.fence_codes) {
    fences.push_text(to_string(code));
  }
  BSM_TRY_ASSIGN(fences_value, fences.build());
  root.set("fence_codes", fences_value);

  JsonArrayBuilder binding_changes;
  for (const BindingChange& change : assessment.delta.changes) {
    JsonObjectBuilder entry;
    entry.set_text("code", to_string(change.code));
    entry.set_text("domain", to_string(change.domain));
    if (change.has_owner) {
      entry.set_text("owner", change.owner.str());
    }
    if (change.has_obligation) {
      entry.set_text("obligation", change.obligation.str());
    }
    entry.set_uint("expected", change.expected);
    entry.set_uint("current", change.current);
    BSM_TRY_ASSIGN(entry_value, entry.build());
    binding_changes.push(entry_value);
  }
  BSM_TRY_ASSIGN(binding_changes_value, binding_changes.build());
  root.set("binding_changes", binding_changes_value);

  root.set_uint("stage_index", static_cast<std::uint64_t>(assessment.stage_index));
  root.set_uint("stage_count", static_cast<std::uint64_t>(assessment.stage_count));
  if (!assessment.current_stage.empty()) {
    root.set_text("current_stage", assessment.current_stage.str());
  }

  JsonArrayBuilder stages;
  for (const StageAssessment& stage : assessment.stages) {
    JsonObjectBuilder entry;
    entry.set_text("id", stage.id.str());
    entry.set_uint("index", static_cast<std::uint64_t>(stage.index));
    entry.set_bool("current", stage.current);
    entry.set_bool("entered", stage.entered);
    entry.set_bool("entry_satisfied", stage.entry_satisfied);
    entry.set_bool("obligations_satisfied", stage.obligations_satisfied);
    JsonArrayBuilder entry_deficits;
    for (const EvidenceDeficit& deficit : stage.entry_deficits) {
      JsonObjectBuilder item;
      item.set_text("kind", deficit.kind.str());
      item.set_text("subject", deficit.subject.str());
      item.set_text("deficit", to_string(deficit.code));
      item.set_uint("required_sources", deficit.required_sources);
      item.set_uint("current_sources", deficit.current_sources);
      BSM_TRY_ASSIGN(item_value, item.build());
      entry_deficits.push(item_value);
    }
    BSM_TRY_ASSIGN(entry_deficits_value, entry_deficits.build());
    entry.set("entry_deficits", entry_deficits_value);
    JsonArrayBuilder exit_deficits;
    for (const EvidenceDeficit& deficit : stage.exit_deficits) {
      JsonObjectBuilder item;
      item.set_text("kind", deficit.kind.str());
      item.set_text("subject", deficit.subject.str());
      item.set_text("deficit", to_string(deficit.code));
      item.set_uint("required_sources", deficit.required_sources);
      item.set_uint("current_sources", deficit.current_sources);
      BSM_TRY_ASSIGN(item_value, item.build());
      exit_deficits.push(item_value);
    }
    BSM_TRY_ASSIGN(exit_deficits_value, exit_deficits.build());
    entry.set("exit_deficits", exit_deficits_value);
    BSM_TRY_ASSIGN(entry_value, entry.build());
    stages.push(entry_value);
  }
  BSM_TRY_ASSIGN(stages_value, stages.build());
  root.set("stages", stages_value);

  JsonArrayBuilder obligations;
  for (const ObligationAssessment& obligation : assessment.obligations) {
    JsonObjectBuilder entry;
    entry.set_text("id", obligation.id.str());
    entry.set_text("stage", obligation.stage.str());
    entry.set_text("priority", to_string(obligation.priority));
    entry.set_uint("index", static_cast<std::uint64_t>(obligation.index));
    entry.set_text("state", to_string(obligation.state));
    entry.set_bool("required", obligation.required);
    entry.set_bool("satisfied", obligation.satisfied);
    entry.set_bool("eligible", obligation.eligible);
    entry.set_uint("attempts_used", obligation.attempts_used);
    entry.set_uint("attempt_budget", obligation.attempt_budget);
    entry.set_text("block", to_string(obligation.block));
    JsonArrayBuilder unresolved;
    for (const AttemptId& id : obligation.unresolved_attempts) {
      unresolved.push_text(id.hex());
    }
    BSM_TRY_ASSIGN(unresolved_value, unresolved.build());
    entry.set("unresolved_attempts", unresolved_value);
    JsonArrayBuilder attempts;
    for (const AttemptId& id : obligation.attempts) {
      attempts.push_text(id.hex());
    }
    BSM_TRY_ASSIGN(attempts_value, attempts.build());
    entry.set("attempts", attempts_value);
    JsonArrayBuilder satisfied_by;
    for (const EvidenceId& id : obligation.satisfied_by) {
      satisfied_by.push_text(id.hex());
    }
    BSM_TRY_ASSIGN(satisfied_by_value, satisfied_by.build());
    entry.set("satisfied_by", satisfied_by_value);
    JsonArrayBuilder deficits;
    for (const EvidenceDeficit& deficit : obligation.deficits) {
      JsonObjectBuilder item;
      item.set_text("kind", deficit.kind.str());
      item.set_text("subject", deficit.subject.str());
      item.set_text("deficit", to_string(deficit.code));
      item.set_uint("required_sources", deficit.required_sources);
      item.set_uint("current_sources", deficit.current_sources);
      JsonArrayBuilder usable;
      for (const EvidenceId& id : deficit.usable) {
        usable.push_text(id.hex());
      }
      BSM_TRY_ASSIGN(usable_value, usable.build());
      item.set("usable", usable_value);
      JsonArrayBuilder rejected;
      for (const EvidenceId& id : deficit.rejected) {
        rejected.push_text(id.hex());
      }
      BSM_TRY_ASSIGN(rejected_value, rejected.build());
      item.set("rejected", rejected_value);
      BSM_TRY_ASSIGN(item_value, item.build());
      deficits.push(item_value);
    }
    BSM_TRY_ASSIGN(deficits_value, deficits.build());
    entry.set("deficits", deficits_value);
    BSM_TRY_ASSIGN(entry_value, entry.build());
    obligations.push(entry_value);
  }
  BSM_TRY_ASSIGN(obligations_value, obligations.build());
  root.set("obligations", obligations_value);

  JsonArrayBuilder eligible;
  for (const ObligationId& id : assessment.eligible) {
    eligible.push_text(id.str());
  }
  BSM_TRY_ASSIGN(eligible_value, eligible.build());
  root.set("eligible", eligible_value);

  root.set_bool("has_next_stage", assessment.has_next_stage);
  if (assessment.has_next_stage) {
    root.set_text("next_stage", assessment.next_stage.str());
  }
  root.set_bool("next_stage_ready", assessment.next_stage_ready);
  JsonArrayBuilder next_deficits;
  for (const EvidenceDeficit& deficit : assessment.next_stage_entry_deficits) {
    JsonObjectBuilder item;
    item.set_text("kind", deficit.kind.str());
    item.set_text("subject", deficit.subject.str());
    item.set_text("deficit", to_string(deficit.code));
    BSM_TRY_ASSIGN(item_value, item.build());
    next_deficits.push(item_value);
  }
  BSM_TRY_ASSIGN(next_deficits_value, next_deficits.build());
  root.set("next_stage_entry_deficits", next_deficits_value);

  root.set_bool("completion_ready", assessment.completion_ready);
  JsonArrayBuilder completion_missing;
  for (const ObligationId& id : assessment.completion_missing_obligations) {
    completion_missing.push_text(id.str());
  }
  BSM_TRY_ASSIGN(completion_missing_value, completion_missing.build());
  root.set("completion_missing_obligations", completion_missing_value);
  JsonArrayBuilder completion_deficits;
  for (const EvidenceDeficit& deficit : assessment.completion_deficits) {
    JsonObjectBuilder item;
    item.set_text("kind", deficit.kind.str());
    item.set_text("subject", deficit.subject.str());
    item.set_text("deficit", to_string(deficit.code));
    BSM_TRY_ASSIGN(item_value, item.build());
    completion_deficits.push(item_value);
  }
  BSM_TRY_ASSIGN(completion_deficits_value, completion_deficits.build());
  root.set("completion_deficits", completion_deficits_value);

  root.set_text("primary_block", to_string(assessment.primary_block));
  root.set_text("explanation", assessment.explanation);
  return root.build();
}

Result<JsonValue> session_view_to_json(const SessionView& view) {
  JsonObjectBuilder root;
  root.set_text("id", view.id.hex());
  root.set_text("state", to_string(view.state));
  root.set_text("plan_digest", view.plan_digest.hex());
  root.set_text("binding_digest", view.binding_digest.hex());
  root.set_uint("revision", view.revision);
  root.set_uint("updated_tick", view.updated_tick);
  root.set_uint("stage_index", static_cast<std::uint64_t>(view.stage_index));
  if (!view.stage.empty()) {
    root.set_text("stage", view.stage.str());
  }
  return root.build();
}

}  // namespace black_start_manager
