#pragma once

// Restoration session state: the durable record of one black-start attempt and the
// derived assessment that explains, deterministically, what may happen next.
//
// The persisted record contains only committed facts (identity, binding, plan digest,
// lifecycle state, stage publications, admitted evidence, attempts, holds, replans,
// and the terminal record). Everything that can be recomputed — obligation
// satisfaction, readiness gates, eligibility, block reasons — is derived, so two
// processes that replay the same records always agree on the same explanation.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "black_start_manager/attempt.hpp"
#include "black_start_manager/authority.hpp"
#include "black_start_manager/canonical.hpp"
#include "black_start_manager/digest.hpp"
#include "black_start_manager/evidence.hpp"
#include "black_start_manager/ids.hpp"
#include "black_start_manager/plan.hpp"

namespace black_start_manager {

enum class SessionState : std::uint8_t {
  Active = 0,
  Held = 1,
  Aborted = 2,
  Completed = 3,
  Superseded = 4,
};

[[nodiscard]] const char* to_string(SessionState state) noexcept;
[[nodiscard]] bool session_is_terminal(SessionState state) noexcept;

enum class ObligationState : std::uint8_t {
  Pending = 0,
  InFlight = 1,
  Acknowledged = 2,
  Satisfied = 3,
  Failed = 4,
  Fenced = 5,
};

[[nodiscard]] const char* to_string(ObligationState state) noexcept;

// Why a session is fenced: it may report and explain, but it may not progress until
// the operator replans or re-establishes authority explicitly.
enum class FenceCode : std::uint8_t {
  EpochChanged = 0,
  TopologyChanged,
  PolicyChanged,
  IncidentChanged,
  AuthorityRemoved,
  AuthorityOwnerChanged,
  AuthorityGenerationStale,
  AuthorityAttestationChanged,
  PrerequisiteGenerationStale,
  IncarnationSuperseded,
};

[[nodiscard]] const char* to_string(FenceCode code) noexcept;

enum class BlockCode : std::uint8_t {
  // Declaration order is precedence order: the first applicable code in this list is
  // the primary explanation for a session that cannot progress.
  None = 0,
  SessionSuperseded,
  SessionAborted,
  SessionCompleted,
  SessionFenced,
  AuthorityNotReestablished,
  HoldActive,
  UnresolvedAttempt,
  // The owner acknowledged the effect, so the readiness evidence is what is missing. A
  // second request would be a duplicate consequential mutation.
  EffectNotVerified,
  AttemptBudgetExhausted,
  StageEntryEvidenceMissing,
  StageEntryEvidenceStale,
  PrecedenceBlocked,
  PrerequisiteUnsatisfied,
  EvidenceMissing,
  EvidenceStale,
  EvidenceRecovered,
  EvidenceNotIndependent,
  EvidenceContradictory,
  EvidenceInsufficient,
  CompletionEvidenceMissing,
  NoEligibleObligation,
};

[[nodiscard]] const char* to_string(BlockCode code) noexcept;
[[nodiscard]] int block_precedence(BlockCode code) noexcept;

struct HoldRecord {
  std::string reason;
  bool has_authority = false;
  AuthorityRef authority;
  std::uint64_t tick = 0;
  bool released = false;
  std::uint64_t released_tick = 0;
  std::string release_reason;
};

struct StageTransition {
  StageId from;
  StageId to;
  std::size_t to_index = 0;
  std::uint64_t tick = 0;
  // Evidence that satisfied the exit gate of the stage that was left, and the entry
  // gate of the stage that was entered, in deterministic order.
  std::vector<EvidenceId> exit_evidence;
  std::vector<EvidenceId> entry_evidence;
  Digest plan_digest;
  Digest binding_digest;
  IncarnationId incarnation;
};

struct ReplanRecord {
  Digest previous_plan_digest;
  Digest new_plan_digest;
  std::string reason;
  AuthorityRef authority;
  std::uint64_t tick = 0;
  std::size_t carried_obligations = 0;
  std::size_t reset_obligations = 0;
  bool accepted_topology_change = false;
};

struct FenceRecord {
  FenceCode code = FenceCode::EpochChanged;
  std::string detail;
  std::uint64_t tick = 0;
  Digest observed_binding_digest;
};

struct TerminalRecord {
  SessionState state = SessionState::Aborted;
  std::string reason;
  bool has_authority = false;
  AuthorityRef authority;
  std::uint64_t tick = 0;
  std::vector<EvidenceId> proof_evidence;
  Digest proof_digest;
};

struct SessionRecord {
  SessionId id;
  Digest plan_digest;
  FacilityBinding binding;
  Digest binding_digest;
  SessionState state = SessionState::Active;
  IncarnationId opened_by;
  // Incarnation that last explicitly re-established process authority. A session
  // recovered by a new incarnation has authority_reestablished = false until the
  // operator re-establishes it.
  IncarnationId authority_incarnation;
  bool authority_reestablished = false;
  std::uint64_t opened_tick = 0;
  std::uint64_t updated_tick = 0;
  std::uint64_t revision = 0;
  std::size_t stage_index = 0;
  std::vector<StageTransition> stage_history;
  std::vector<EvidenceRecord> evidence;
  std::vector<AttemptRecord> attempts;
  std::vector<HoldRecord> holds;
  std::vector<ReplanRecord> replans;
  std::vector<FenceRecord> fences;
  bool has_terminal = false;
  TerminalRecord terminal;
};

[[nodiscard]] Result<JsonValue> session_to_json(const SessionRecord& session);
[[nodiscard]] Result<SessionRecord> session_from_json(const JsonValue& value);
[[nodiscard]] Result<Unit> validate_session_record(const SessionRecord& session);
[[nodiscard]] Result<Digest> session_digest(const SessionRecord& session);

// Derived, read-only explanation of a session against a plan and an observed binding.
struct ObligationAssessment {
  ObligationId id;
  StageId stage;
  Priority priority = Priority::Standard;
  std::size_t index = 0;  // position in the derived plan order
  ObligationState state = ObligationState::Pending;
  bool required = true;
  bool satisfied = false;
  bool eligible = false;
  std::uint64_t attempts_used = 0;
  std::uint64_t attempt_budget = 0;
  std::vector<AttemptId> unresolved_attempts;
  std::vector<AttemptId> attempts;  // in sequence order
  std::vector<EvidenceId> satisfied_by;
  std::vector<EvidenceDeficit> deficits;
  BlockCode block = BlockCode::None;
};

struct StageAssessment {
  StageId id;
  std::size_t index = 0;
  bool current = false;
  bool entered = false;
  bool entry_satisfied = false;
  bool obligations_satisfied = false;
  std::vector<EvidenceDeficit> entry_deficits;
  std::vector<EvidenceDeficit> exit_deficits;
};

struct Assessment {
  SessionId session;
  Digest session_digest;
  SessionState state = SessionState::Active;
  Digest plan_digest;
  Digest binding_digest;
  Digest observed_binding_digest;
  bool authority_reestablished = false;
  bool fenced = false;
  std::vector<FenceCode> fence_codes;
  BindingDelta delta;
  std::size_t stage_index = 0;
  std::size_t stage_count = 0;
  StageId current_stage;
  std::vector<StageAssessment> stages;
  std::vector<ObligationAssessment> obligations;
  std::vector<ObligationId> eligible;
  bool has_next_stage = false;
  StageId next_stage;
  bool next_stage_ready = false;
  std::vector<EvidenceDeficit> next_stage_entry_deficits;
  bool completion_ready = false;
  std::vector<EvidenceDeficit> completion_deficits;
  std::vector<ObligationId> completion_missing_obligations;
  BlockCode primary_block = BlockCode::None;
  std::string explanation;

  [[nodiscard]] Result<JsonValue> to_json() const;
};

struct SessionView {
  SessionId id;
  SessionState state = SessionState::Active;
  Digest plan_digest;
  Digest binding_digest;
  std::uint64_t revision = 0;
  std::uint64_t updated_tick = 0;
  std::size_t stage_index = 0;
  StageId stage;
};

struct AttemptView {
  AttemptRecord attempt;
  // True when the call replayed an already committed outcome instead of mutating
  // anything.
  bool replayed = false;
};

struct EvidenceView {
  EvidenceRecord record;
  bool replayed = false;
};

struct StageTransitionView {
  StageTransition transition;
  Assessment assessment;
};

struct CompletionView {
  SessionId session;
  std::uint64_t tick = 0;
  std::vector<EvidenceId> proof_evidence;
  Digest proof_digest;
};

[[nodiscard]] Result<JsonValue> assessment_to_json(const Assessment& assessment);
[[nodiscard]] Result<JsonValue> session_view_to_json(const SessionView& view);

}  // namespace black_start_manager
