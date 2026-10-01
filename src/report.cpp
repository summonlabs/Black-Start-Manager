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

#include "black_start_manager/report.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace black_start_manager {

Result<JsonValue> session_report(const DerivedPlan& plan, const SessionRecord& session,
                                 const Assessment& assessment,
                                 const ReportOptions& options) {
  BSM_TRY_ASSIGN(base, session_to_json(session));

  JsonObjectBuilder root;
  for (const JsonValue::Field& field : base.as_object()) {
    if (!options.include_evidence && field.first == "evidence") {
      continue;
    }
    if (!options.include_attempts && field.first == "attempts") {
      continue;
    }
    root.set(field.first, field.second);
  }
  root.set_uint("format_version", limits::kStateFormatVersion);
  if (options.include_assessment) {
    BSM_TRY_ASSIGN(assessment_value, assessment_to_json(assessment));
    root.set("assessment", assessment_value);
  }
  if (options.include_plan) {
    BSM_TRY_ASSIGN(plan_value, plan.to_json());
    root.set("plan", plan_value);
  }
  return root.build();
}

Result<JsonValue> store_status_json(const RecoveryReport& recovery,
                                    const StoreVerification& verification,
                                    std::size_t session_count) {
  JsonObjectBuilder root;
  root.set_uint("format_version", limits::kStateFormatVersion);
  root.set_uint("session_count", static_cast<std::uint64_t>(session_count));
  BSM_TRY_ASSIGN(recovery_value, recovery.to_json());
  root.set("recovery", recovery_value);
  BSM_TRY_ASSIGN(verification_value, verification.to_json());
  root.set("verification", verification_value);
  return root.build();
}

}  // namespace black_start_manager
