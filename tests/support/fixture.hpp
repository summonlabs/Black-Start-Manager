#pragma once

// Deterministic fixtures: one synthetic facility, one restoration plan, and one scripted
// adjacent-owner policy that the tests drive. The facility is SYNTHETIC: it models
// isolation, energization, control power, cooling, capacity, rack admission, and
// return-to-service readiness. Nothing here is facility hardware.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "black_start_manager/controller.hpp"
#include "black_start_manager/manager.hpp"
#include "black_start_manager/plan.hpp"
#include "test_harness.hpp"

namespace bsm_test {

struct Facility {
  black_start_manager::FacilityBinding binding;
  black_start_manager::PlanDocument plan;
  black_start_manager::AuthorityRef control;
  black_start_manager::AuthorityRef electrical;
  black_start_manager::AuthorityRef cooling;
  black_start_manager::AuthorityRef facility_owner;
  black_start_manager::AuthorityRef instrumentation;
};

// Builds the standard synthetic facility binding at the requested epoch, authority
// generation, and incident generation.
[[nodiscard]] black_start_manager::Result<Facility> make_facility(
    std::uint64_t epoch = 1, std::uint64_t generation = 1,
    std::uint64_t incident_generation = 3);

// The standard three-stage restoration plan: isolation, energization with control power
// and cooling, then capacity and rack admission, with a return-to-service proof.
[[nodiscard]] std::string standard_plan_text();
[[nodiscard]] black_start_manager::Result<black_start_manager::PlanDocument> standard_plan();

// One live manager over a fresh store inside the test's working directory.
struct Harness {
  std::string root;
  black_start_manager::ManualClock clock{1};
  black_start_manager::InProcessSyntheticController controller;
  std::unique_ptr<black_start_manager::StaticFacilityState> facility;
  std::unique_ptr<black_start_manager::SessionManager> manager;
  Facility definition;

  Harness() = default;
  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;

  [[nodiscard]] black_start_manager::Result<black_start_manager::Unit> start(
      const std::string& directory, std::uint64_t tick = 1,
      const black_start_manager::SyntheticControllerPolicy& policy = {},
      std::uint64_t compact_bytes = black_start_manager::limits::kMaxJournalSegmentBytes,
      std::size_t compact_records =
          black_start_manager::limits::kMaxJournalRecordsPerSegment);

  [[nodiscard]] black_start_manager::Result<black_start_manager::Unit> reopen(
      std::uint64_t tick);

  [[nodiscard]] black_start_manager::Result<black_start_manager::SessionId> open_session();
  [[nodiscard]] black_start_manager::SessionManager& session() { return *manager; }
  [[nodiscard]] const black_start_manager::FacilityBinding& binding() const {
    return facility->binding();
  }

  // Scripted progression helpers. Each returns the library's own refusal unchanged, so a
  // test can assert on the exact error code.
  [[nodiscard]] black_start_manager::Result<black_start_manager::EvidenceView> observe(
      const black_start_manager::SessionId& session,
      const black_start_manager::ObligationId& obligation,
      const black_start_manager::EvidenceKind& kind,
      const black_start_manager::SubjectId& subject,
      const black_start_manager::OwnerId& observer,
      const black_start_manager::AuthorityRef& authority);
};

// A unique scratch directory under the test's working directory.
[[nodiscard]] std::string scratch_directory(const std::string& name);

}  // namespace bsm_test
