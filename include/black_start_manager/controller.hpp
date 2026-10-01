#pragma once

// The adjacent owners Black Start Manager may request effects from and accept
// readiness evidence from.
//
// The interface is deliberately narrow: a bounded request with a deterministic
// idempotency key, a query by that key, and a non-consequential observation. The
// manager never treats a reply as proof of a physical effect, and never infers
// authority from an owner's response.

#include <cstdint>
#include <string>
#include <vector>

#include "black_start_manager/attempt.hpp"
#include "black_start_manager/authority.hpp"
#include "black_start_manager/canonical.hpp"
#include "black_start_manager/error.hpp"
#include "black_start_manager/ids.hpp"

namespace black_start_manager {

struct ObservationRequest {
  CapabilityId capability;
  JsonValue parameters;
  SubjectId subject;
  OwnerId observer;
  AuthorityRef authority;
};

struct ControllerObservation {
  OwnerId owner;
  AuthorityDomain domain = AuthorityDomain::Control;
  std::uint64_t generation = 0;
  JsonValue value;
  std::string detail;
};

class AdjacentController {
 public:
  AdjacentController() = default;
  AdjacentController(const AdjacentController&) = delete;
  AdjacentController& operator=(const AdjacentController&) = delete;
  virtual ~AdjacentController() = default;

  // Sends a bounded request. Implementations must be idempotent on the request key:
  // a repeated key returns AlreadyApplied with the original outcome and performs no
  // second effect.
  [[nodiscard]] virtual Result<ControllerReply> request_effect(
      const RequestEnvelope& envelope) = 0;

  // Resolves an in-flight request without sending a new one.
  [[nodiscard]] virtual Result<ControllerReply> query_effect(const RequestKey& key) = 0;

  // Reads the owner's current view of a subject. Non-consequential.
  [[nodiscard]] virtual Result<ControllerObservation> observe(
      const ObservationRequest& request) = 0;
};

// The synthetic plant: a deterministic model of the facility effects a black-start
// boundary may request, and of the observations adjacent owners report about them.
// SYNTHETIC provenance: it models plant behaviour, it is not plant hardware.
class SyntheticPlant {
 public:
  SyntheticPlant();

  // Applies a capability. Refuses unknown capabilities, malformed parameters, and
  // values outside the modelled range. The effect is deterministic in the capability
  // and its canonical parameters.
  [[nodiscard]] Result<Unit> apply(const CapabilityId& capability,
                                   const JsonValue& parameters);

  [[nodiscard]] Result<JsonValue> observe(const SubjectId& subject) const;
  [[nodiscard]] Result<JsonValue> observe_text(const std::string& subject) const;
  // Replaces the modelled state. Used when an owner replays its durable ledger, because the
  // facility it models outlives the process that observed it.
  [[nodiscard]] Result<Unit> restore(const JsonValue& state);
  [[nodiscard]] Result<JsonValue> state_json() const;
  [[nodiscard]] bool has_subject(const SubjectId& subject) const noexcept;

  // Subject touched by the most recent successful apply, empty when none.
  [[nodiscard]] const std::string& last_subject() const noexcept { return last_subject_; }

  // Deterministic effect identity of a capability and its parameters: two distinct
  // requests with the same effect identity are a duplicate consequential mutation.
  [[nodiscard]] static Result<Digest> effect_identity(const CapabilityId& capability,
                                                      const JsonValue& parameters);

 private:
  // Keyed by subject text, sorted: canonical output never depends on insertion order.
  std::vector<std::pair<std::string, JsonValue>> subjects_;
  std::string last_subject_;
  JsonValue* find_subject(const SubjectId& subject);
};

// Deterministic scripted behaviour for the synthetic controller. Every field has a
// default that models a healthy owner.
struct SyntheticControllerPolicy {
  std::uint64_t seed = 0;
  // Capabilities the owner refuses outright.
  std::vector<CapabilityId> refuse;
  // Capabilities that report a failure after the effect is applied.
  std::vector<CapabilityId> fail_after_apply;
  // Capabilities whose first request is reported Unavailable although the effect was
  // applied and recorded: models a lost response.
  std::vector<CapabilityId> lose_first_reply;
  // Capabilities whose first request is reported Unavailable without applying.
  std::vector<CapabilityId> unavailable_once;
  // Capabilities whose requests are always reported Deferred.
  std::vector<CapabilityId> defer;
  // Observers that fail to observe.
  std::vector<OwnerId> blind_observers;
  // Capabilities whose observation reports a different value than the applied effect,
  // used to prove that a contradiction is refused rather than averaged away.
  std::vector<CapabilityId> contradictory_observation;
};

// One recorded outcome: the request key the owner remembers, the outcome it will report
// for that key, and the deterministic identity of the effect it applied.
struct AppliedOutcome {
  RequestKey key;
  ControllerReply reply;
  Digest effect;
};

// In-process synthetic controller: the deterministic adjacent owner used by unit,
// integration, and property tests. It keeps applied request keys in memory, exactly
// like a real owner keeps them durably; the out-of-process plant host adds real
// durability and real process death.
class InProcessSyntheticController final : public AdjacentController {
 public:
  explicit InProcessSyntheticController(SyntheticControllerPolicy policy = {});

  [[nodiscard]] Result<ControllerReply> request_effect(
      const RequestEnvelope& envelope) override;
  [[nodiscard]] Result<ControllerReply> query_effect(const RequestKey& key) override;
  [[nodiscard]] Result<ControllerObservation> observe(
      const ObservationRequest& request) override;

  [[nodiscard]] const SyntheticPlant& plant() const noexcept { return plant_; }
  [[nodiscard]] const std::vector<RequestEnvelope>& received() const noexcept {
    return received_;
  }
  [[nodiscard]] const std::vector<Digest>& applied_effects() const noexcept {
    return applied_effects_;
  }
  // Test affordance: changes the scripted behaviour between operations.
  void set_policy(SyntheticControllerPolicy policy) { policy_ = std::move(policy); }
  [[nodiscard]] const SyntheticControllerPolicy& policy() const noexcept { return policy_; }

  // Recorded outcomes in key order. The plant host replays these from its durable ledger
  // and appends new ones before it answers a request.
  [[nodiscard]] const std::vector<AppliedOutcome>& applied() const noexcept {
    return applied_;
  }
  // Restores one recorded outcome, exactly as replaying a durable ledger would.
  [[nodiscard]] Result<Unit> restore_outcome(const AppliedOutcome& outcome);
  // Restores the modelled plant state that accompanied an applied outcome.
  [[nodiscard]] Result<Unit> plant_restore(const JsonValue& state) {
    return plant_.restore(state);
  }
  [[nodiscard]] std::size_t applied_effect_count() const noexcept {
    return applied_effects_.size();
  }
  [[nodiscard]] std::size_t duplicate_effect_count() const noexcept {
    return duplicate_effects_;
  }

 private:
  SyntheticControllerPolicy policy_;
  SyntheticPlant plant_;
  std::vector<AppliedOutcome> applied_;  // sorted by key
  std::vector<RequestEnvelope> received_;
  std::vector<Digest> applied_effects_;
  std::size_t duplicate_effects_ = 0;
  std::vector<RequestKey> lost_replies_;
  std::vector<RequestKey> unavailable_;
  std::vector<CapabilityId> unavailable_capabilities_;
  std::vector<RequestKey> deferred_;
};

// A facility state provider answers "what binding is in force now". The deployment
// reads it from the adjacent owners; tests and the command line tool set it
// explicitly.
class FacilityStateProvider {
 public:
  FacilityStateProvider() = default;
  FacilityStateProvider(const FacilityStateProvider&) = delete;
  FacilityStateProvider& operator=(const FacilityStateProvider&) = delete;
  virtual ~FacilityStateProvider() = default;

  [[nodiscard]] virtual Result<FacilityBinding> current_binding() const = 0;
};

class StaticFacilityState final : public FacilityStateProvider {
 public:
  explicit StaticFacilityState(FacilityBinding binding) : binding_(std::move(binding)) {}

  [[nodiscard]] Result<FacilityBinding> current_binding() const override {
    return binding_;
  }
  void set(FacilityBinding binding) { binding_ = std::move(binding); }
  [[nodiscard]] const FacilityBinding& binding() const noexcept { return binding_; }

 private:
  FacilityBinding binding_;
};

}  // namespace black_start_manager
