#pragma once

// Durable attempt identity and the bounded request envelope.
//
// The manager records the intent to send a request before it sends it, so a crash
// between the durable record and the controller's answer leaves an attempt that is
// explicitly unresolved rather than silently repeated. Every request carries a
// deterministic idempotency key derived from the plan, the obligation, the attempt
// sequence, and the bound binding, so a lost response is resolved by querying the
// owner instead of mutating the plant a second time.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "black_start_manager/authority.hpp"
#include "black_start_manager/canonical.hpp"
#include "black_start_manager/digest.hpp"
#include "black_start_manager/ids.hpp"

namespace black_start_manager {

enum class AttemptState : std::uint8_t {
  // Intent is durable; the outcome is not yet known. A crash here recovers as
  // Unresolved.
  Requested = 0,
  // The owner confirmed the effect was applied (possibly by replaying an earlier
  // identical request).
  Acknowledged = 1,
  // The owner did not give a definitive answer, or the previous incarnation died
  // with the attempt in flight. Blocks further requests for the obligation until it
  // is resolved.
  Unresolved = 2,
  // Resolution proved the owner never applied the effect. A further attempt is
  // permitted and does not consume the attempt budget.
  NotApplied = 3,
  // The owner refused the request.
  Refused = 4,
  // The owner reported a failure, or the request could not be delivered.
  Failed = 5,
  // The attempt belongs to a superseded plan, binding, or session and must never be
  // repeated.
  Fenced = 6,
};

[[nodiscard]] const char* to_string(AttemptState state) noexcept;
[[nodiscard]] bool attempt_is_in_flight(AttemptState state) noexcept;
[[nodiscard]] bool attempt_blocks_requests(AttemptState state) noexcept;
[[nodiscard]] bool attempt_consumes_budget(AttemptState state) noexcept;

struct RequestEnvelope {
  AttemptId attempt;
  RequestKey key;
  SessionId session;
  ObligationId obligation;
  Digest plan_digest;
  Digest binding_digest;
  CapabilityId capability;
  JsonValue parameters;
  AuthorityRef authority;
  std::uint64_t attempt_sequence = 0;
  std::uint64_t created_tick = 0;
  std::uint64_t deadline_tick = 0;
  Digest envelope_digest;
};

[[nodiscard]] Result<Digest> request_envelope_digest(const RequestEnvelope& envelope);
[[nodiscard]] Result<RequestKey> request_key_for(const SessionId& session,
                                                 const ObligationId& obligation,
                                                 std::uint64_t attempt_sequence,
                                                 const Digest& plan_digest,
                                                 const Digest& binding_digest,
                                                 const CapabilityId& capability,
                                                 const JsonValue& parameters);
[[nodiscard]] Result<AttemptId> attempt_id_for(const SessionId& session,
                                               const ObligationId& obligation,
                                               std::uint64_t attempt_sequence,
                                               const Digest& plan_digest,
                                               const Digest& binding_digest);
[[nodiscard]] Result<JsonValue> request_envelope_to_json(const RequestEnvelope& envelope);

enum class ControllerReplyKind : std::uint8_t {
  Applied = 0,
  AlreadyApplied = 1,
  Refused = 2,
  Failed = 3,
  Unknown = 4,
  Unavailable = 5,
  Deferred = 6,
};

[[nodiscard]] const char* to_string(ControllerReplyKind kind) noexcept;

struct ControllerReply {
  ControllerReplyKind kind = ControllerReplyKind::Unknown;
  RequestKey key;
  std::string detail;
  // Canonical observation the owner returned with the reply, or null when the owner
  // returned none. A reply never satisfies a readiness gate by itself.
  JsonValue observed;
  // Owner-side generation after the effect, when the owner reports one.
  std::uint64_t effect_generation = 0;
};

struct AttemptRecord {
  AttemptId id;
  SessionId session;
  ObligationId obligation;
  std::uint64_t sequence = 0;
  AttemptState state = AttemptState::Requested;
  RequestKey key;
  Digest request_digest;
  Digest plan_digest;
  Digest binding_digest;
  // Incarnation that recorded the intent. An attempt whose incarnation is not the
  // current one can never be repeated by the current incarnation.
  IncarnationId incarnation;
  std::uint64_t requested_tick = 0;
  std::uint64_t settled_tick = 0;
  ControllerReply reply;
  std::string resolution_note;
  // Evidence admitted against this attempt's effect, in admission order.
  std::vector<EvidenceId> evidence;
};

[[nodiscard]] Result<JsonValue> attempt_to_json(const AttemptRecord& attempt);
[[nodiscard]] Result<AttemptRecord> attempt_from_json(const JsonValue& value);
[[nodiscard]] Result<Unit> validate_attempt_record(const AttemptRecord& attempt);

}  // namespace black_start_manager
