#pragma once

// Readiness evidence.
//
// An observation is admitted as a record with an explicit provenance, source,
// authority generation, and observation tick. Only live, current, independently
// sourced evidence can satisfy a readiness gate; recovered evidence is retained for
// audit lineage and never promoted to current. A record's identity is the digest of
// its immutable content, so the same observation admitted twice is the same record.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "black_start_manager/authority.hpp"
#include "black_start_manager/canonical.hpp"
#include "black_start_manager/digest.hpp"
#include "black_start_manager/ids.hpp"

namespace black_start_manager {

enum class EvidenceProvenance : std::uint8_t {
  // Observed during this session by a source bound to the session's binding.
  Live = 0,
  // Recovered from durable state, or observed before the session existed. Retained
  // for lineage; never satisfies a gate.
  Recovered = 1,
};

enum class EvidenceChannel : std::uint8_t {
  OwnerReported = 0,
  InstrumentedMeasurement = 1,
  SyntheticModel = 2,
  ManagerDerived = 3,
  OperatorAsserted = 4,
};

[[nodiscard]] const char* to_string(EvidenceProvenance provenance) noexcept;
[[nodiscard]] const char* to_string(EvidenceChannel channel) noexcept;
[[nodiscard]] Result<EvidenceChannel> parse_evidence_channel(std::string_view text);

// A channel is independent unless the value was derived by the manager itself. A
// manager-derived value can never satisfy a readiness gate.
[[nodiscard]] bool channel_is_independent(EvidenceChannel channel) noexcept;

// What a plan requires before an obligation or stage may be considered ready.
struct EvidenceRequirement {
  EvidenceKind kind;
  SubjectId subject;
  // Maximum age in ticks. Zero means "use the plan policy default".
  std::uint64_t max_age_ticks = 0;
  // Number of independent current sources that must agree on the value. One source
  // is an observation; two or more are a cross-checked verification.
  std::uint32_t min_sources = 1;
  // The readiness the observation must assert. An object expectation matches when every
  // field it names is present and equal in the observation; arrays and scalars must match
  // exactly. An observation that does not assert the required readiness never satisfies
  // the requirement, so "not ready" can never be mistaken for "ready".
  bool has_expect = false;
  JsonValue expect;
  // When set, at least one current source must be this owner; further sources may come
  // from other bound owners, which is what makes a cross-check independent.
  bool has_required_owner = false;
  OwnerId required_owner;
  // When set, at least one current source must belong to this authority domain.
  bool has_required_domain = false;
  AuthorityDomain required_domain = AuthorityDomain::Control;

  friend bool operator==(const EvidenceRequirement& left,
                         const EvidenceRequirement& right) noexcept;
  friend bool operator!=(const EvidenceRequirement& left,
                         const EvidenceRequirement& right) noexcept {
    return !(left == right);
  }
};

struct EvidenceRecord {
  EvidenceId id;
  SessionId session;
  EvidenceKind kind;
  SubjectId subject;
  // Set when the observation supports a specific obligation; empty for stage
  // evidence.
  bool has_obligation = false;
  ObligationId obligation;
  bool has_stage = false;
  StageId stage;
  // The observation itself: a canonical scalar or object.
  JsonValue value;
  OwnerId source_owner;
  AuthorityDomain source_domain = AuthorityDomain::Control;
  std::uint64_t source_generation = 0;
  EvidenceChannel channel = EvidenceChannel::OwnerReported;
  EvidenceProvenance provenance = EvidenceProvenance::Live;
  std::uint64_t observed_tick = 0;
  // Tick at which the record was demoted to recovered evidence; zero while live.
  std::uint64_t recovered_tick = 0;
  Digest plan_digest;
  Digest binding_digest;
  std::uint64_t facility_epoch = 0;
  // Identity digest over the immutable content above (excluding provenance and
  // recovered_tick, which are session-local state).
  Digest content_digest;
};

[[nodiscard]] Result<Digest> evidence_content_digest(const EvidenceRecord& record);
[[nodiscard]] Result<EvidenceId> evidence_id_for(const EvidenceRecord& record);
[[nodiscard]] Result<JsonValue> evidence_to_json(const EvidenceRecord& record);
[[nodiscard]] Result<EvidenceRecord> evidence_from_json(const JsonValue& value);
[[nodiscard]] Result<Unit> validate_evidence_record(const EvidenceRecord& record);

// Why a requirement is not satisfied by the evidence currently on record.
enum class EvidenceDeficitCode : std::uint8_t {
  Missing = 0,
  ExpectationMismatch,
  RecoveredOnly,
  Expired,
  ForeignGeneration,
  ForeignOwner,
  NotIndependent,
  Contradictory,
  InsufficientSources,
  KindMismatch,
};

[[nodiscard]] const char* to_string(EvidenceDeficitCode code) noexcept;

struct EvidenceDeficit {
  EvidenceKind kind;
  SubjectId subject;
  EvidenceDeficitCode code = EvidenceDeficitCode::Missing;
  std::uint32_t required_sources = 1;
  std::uint32_t current_sources = 0;
  // Evidence considered and why it was rejected, in deterministic order.
  std::vector<EvidenceId> usable;
  std::vector<EvidenceId> rejected;
};

}  // namespace black_start_manager
