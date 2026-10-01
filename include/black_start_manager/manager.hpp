#pragma once

// SessionManager: the boundary's public control surface.
//
// Every mutating operation follows the same discipline:
//   1. validate against the committed session record, the compiled plan, the bound
//      binding, and the current facility binding;
//   2. write the durable record that makes the operation recoverable, and flush it;
//   3. perform the external interaction (if any) without holding internal state locks;
//   4. record the outcome durably.
// A crash between any two steps leaves a state that recovery can explain and that the
// next incarnation refuses to repeat blindly.

#include <cstdint>
#include <string>
#include <vector>

#include "black_start_manager/attempt.hpp"
#include "black_start_manager/authority.hpp"
#include "black_start_manager/canonical.hpp"
#include "black_start_manager/clock.hpp"
#include "black_start_manager/controller.hpp"
#include "black_start_manager/evidence.hpp"
#include "black_start_manager/plan.hpp"
#include "black_start_manager/report.hpp"
#include "black_start_manager/session.hpp"
#include "black_start_manager/store.hpp"

namespace black_start_manager {

struct ManagerOptions {
  std::string store_root;
  bool create_store_if_missing = false;
  // Borrowed for the lifetime of the manager.
  const Clock* clock = nullptr;
  AdjacentController* controller = nullptr;
  FacilityStateProvider* facility = nullptr;
  std::uint64_t compact_journal_bytes = limits::kMaxJournalSegmentBytes;
  std::size_t compact_journal_records = limits::kMaxJournalRecordsPerSegment;
};

struct EstablishSessionRequest {
  PlanDocument plan;
  FacilityBinding binding;
  // Must be the Control authority of the binding: only an authority that owns the
  // restoration process may open a restoration session.
  AuthorityRef authority;
  std::string reason;
  // Establishes a new session while a previous non-terminal session exists for the
  // same facility. The previous session becomes Superseded and its in-flight attempts
  // are fenced.
  bool supersede_existing = false;
};

struct ReestablishRequest {
  SessionId session;
  AuthorityRef authority;
  std::string reason;
};

struct EffectRequest {
  SessionId session;
  ObligationId obligation;
  AuthorityRef authority;
  // Bounded request window in ticks, counted from the current tick. Zero is refused.
  std::uint64_t deadline_ticks = 0;
};

struct AttemptResolution {
  SessionId session;
  AttemptId attempt;
  AuthorityRef authority;
  std::string reason;
};

struct Observation {
  SessionId session;
  EvidenceKind kind;
  SubjectId subject;
  bool has_obligation = false;
  ObligationId obligation;
  bool has_stage = false;
  StageId stage;
  JsonValue value;
  OwnerId source_owner;
  AuthorityDomain source_domain = AuthorityDomain::Control;
  std::uint64_t source_generation = 0;
  EvidenceChannel channel = EvidenceChannel::OwnerReported;
  std::uint64_t observed_tick = 0;
  bool has_attempt = false;
  AttemptId attempt;
};

struct ObserveRequest {
  SessionId session;
  EvidenceKind kind;
  SubjectId subject;
  bool has_obligation = false;
  ObligationId obligation;
  bool has_stage = false;
  StageId stage;
  OwnerId observer;
  AuthorityRef authority;
  std::uint64_t observed_tick = 0;
};

struct StageAdvance {
  SessionId session;
  AuthorityRef authority;
};

struct CompletionRequest {
  SessionId session;
  AuthorityRef authority;
};

struct HoldRequest {
  SessionId session;
  AuthorityRef authority;
  std::string reason;
};

struct ResumeRequest {
  SessionId session;
  AuthorityRef authority;
  std::string reason;
};

struct ReplanRequest {
  SessionId session;
  PlanDocument plan;
  FacilityBinding binding;
  AuthorityRef authority;
  std::string reason;
  bool accept_topology_change = false;
};

struct AbortRequest {
  SessionId session;
  AuthorityRef authority;
  std::string reason;
};

class SessionManager {
 public:
  SessionManager() = default;
  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;
  SessionManager(SessionManager&& other) noexcept;
  SessionManager& operator=(SessionManager&& other) noexcept;
  ~SessionManager();

  [[nodiscard]] static Result<SessionManager> open(const ManagerOptions& options);

  [[nodiscard]] const RecoveryReport& recovery() const noexcept;
  [[nodiscard]] IncarnationId incarnation() const noexcept;
  [[nodiscard]] std::uint64_t epoch() const noexcept;
  [[nodiscard]] Result<JsonValue> verify_store() const;

  // The only non-terminal session, or an error when there is none or more than one.
  [[nodiscard]] Result<SessionId> active_session() const;
  [[nodiscard]] Result<std::vector<SessionId>> sessions() const;

  [[nodiscard]] Result<SessionView> establish_session(const EstablishSessionRequest& request);
  [[nodiscard]] Result<SessionView> reestablish_authority(const ReestablishRequest& request);
  [[nodiscard]] Result<SessionView> hold(const HoldRequest& request);
  [[nodiscard]] Result<SessionView> resume(const ResumeRequest& request);
  [[nodiscard]] Result<SessionView> replan(const ReplanRequest& request);
  [[nodiscard]] Result<SessionView> abort(const AbortRequest& request);

  [[nodiscard]] Result<AttemptView> request_effect(const EffectRequest& request);
  [[nodiscard]] Result<AttemptView> resolve_attempt(const AttemptResolution& request);
  [[nodiscard]] Result<EvidenceView> record_observation(const Observation& observation);
  [[nodiscard]] Result<EvidenceView> observe_subject(const ObserveRequest& request);
  [[nodiscard]] Result<StageTransitionView> advance_stage(const StageAdvance& request);
  [[nodiscard]] Result<CompletionView> complete_session(const CompletionRequest& request);

  [[nodiscard]] Result<Assessment> assess() const;
  [[nodiscard]] Result<Assessment> assess(const SessionId& session) const;
  [[nodiscard]] Result<JsonValue> report() const;
  [[nodiscard]] Result<JsonValue> report(const SessionId& session,
                                         const ReportOptions& options = {}) const;
  [[nodiscard]] Result<JsonValue> status() const;

  [[nodiscard]] Result<Unit> close();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace black_start_manager
