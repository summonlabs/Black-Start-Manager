#pragma once

// Canonical, deterministic reporting. Everything a report contains is derived from
// committed records and the plan, so two independent processes produce byte-identical
// reports for the same store.

#include "black_start_manager/canonical.hpp"
#include "black_start_manager/session.hpp"
#include "black_start_manager/store.hpp"

namespace black_start_manager {

struct ReportOptions {
  bool include_assessment = true;
  bool include_evidence = true;
  bool include_attempts = true;
  bool include_plan = false;
};

// Full audit lineage for one session: identity, binding, lifecycle, stage history,
// evidence ledger with provenance, attempt history, holds, replans, fences, terminal
// record, and the derived assessment.
[[nodiscard]] Result<JsonValue> session_report(const DerivedPlan& plan,
                                               const SessionRecord& session,
                                               const Assessment& assessment,
                                               const ReportOptions& options = {});

// Compact deterministic view of the durable store for status output.
[[nodiscard]] Result<JsonValue> store_status_json(const RecoveryReport& recovery,
                                                  const StoreVerification& verification,
                                                  std::size_t session_count);

}  // namespace black_start_manager
