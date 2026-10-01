#pragma once

// Shared JSON codecs for model pieces that several record types embed.

#include "black_start_manager/attempt.hpp"
#include "black_start_manager/authority.hpp"
#include "black_start_manager/canonical.hpp"

namespace black_start_manager::detail {

[[nodiscard]] Result<JsonValue> authority_to_json(const AuthorityRef& authority);
[[nodiscard]] Result<AuthorityRef> authority_from_json(const JsonValue& value);

[[nodiscard]] Result<JsonValue> controller_reply_to_json(const ControllerReply& reply);
[[nodiscard]] Result<ControllerReply> controller_reply_from_json(const JsonValue& value);
[[nodiscard]] Result<RequestEnvelope> request_envelope_from_json(const JsonValue& value);

}  // namespace black_start_manager::detail
