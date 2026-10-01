#pragma once

// Session sub-record codecs. The manager replays durable records one part at a time, so
// the individual codecs are shared with the session codec in codec_model.cpp.

#include "black_start_manager/canonical.hpp"
#include "black_start_manager/session.hpp"

namespace black_start_manager::detail {

[[nodiscard]] Result<JsonValue> hold_to_json(const HoldRecord& hold);
[[nodiscard]] Result<HoldRecord> hold_from_json(const JsonValue& value);
[[nodiscard]] Result<JsonValue> transition_to_json(const StageTransition& transition);
[[nodiscard]] Result<StageTransition> transition_from_json(const JsonValue& value);
[[nodiscard]] Result<JsonValue> replan_to_json(const ReplanRecord& replan);
[[nodiscard]] Result<ReplanRecord> replan_from_json(const JsonValue& value);
[[nodiscard]] Result<JsonValue> fence_to_json(const FenceRecord& fence);
[[nodiscard]] Result<FenceRecord> fence_from_json(const JsonValue& value);
[[nodiscard]] Result<JsonValue> terminal_to_json(const TerminalRecord& terminal);
[[nodiscard]] Result<TerminalRecord> terminal_from_json(const JsonValue& value);

}  // namespace black_start_manager::detail
