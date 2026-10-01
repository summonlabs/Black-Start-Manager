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

#include "black_start_manager/remote_controller.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "black_start_manager/limits.hpp"
#include "black_start_manager/store.hpp"
#include "detail/model_json.hpp"
#include "detail/socket.hpp"

namespace black_start_manager {
namespace {

[[nodiscard]] Error protocol_error(const std::string& message) {
  return Error(ErrorCode::ControllerProtocolViolation, message);
}

// Sends one request line and reads one response line.
[[nodiscard]] Result<JsonValue> exchange(detail::Socket& socket, const JsonValue& request) {
  BSM_RETURN_IF_ERROR(socket.send_line(canonical_json(request)));
  BSM_TRY_ASSIGN(line, socket.receive_line(limits::kMaxControllerReplyBytes));
  BSM_TRY_ASSIGN(value, parse_json(line));
  if (!value.is_object()) {
    return protocol_error("the adjacent owner answered with a non-object");
  }
  BSM_TRY_ASSIGN(ok, json::require_bool(value, "ok"));
  if (!ok) {
    BSM_TRY_ASSIGN(error_value, json::require_object_member(value, "error"));
    BSM_TRY_ASSIGN(code_text, json::require_string(*error_value, "code"));
    BSM_TRY_ASSIGN(message_text, json::require_string(*error_value, "message"));
    ErrorCode code = ErrorCode::ControllerRejected;
    if (code_text == "controller_unavailable") {
      code = ErrorCode::ControllerUnavailable;
    } else if (code_text == "limit_exceeded") {
      code = ErrorCode::LimitExceeded;
    } else if (code_text == "schema_violation" || code_text == "invalid_encoding") {
      code = ErrorCode::SchemaViolation;
    }
    return Error(code, "the adjacent owner refused the protocol request: " + message_text);
  }
  return value;
}

[[nodiscard]] Result<JsonValue> observe_request_json(const ObservationRequest& request) {
  JsonObjectBuilder root;
  root.set_text("capability", request.capability.str());
  root.set("parameters", request.parameters);
  root.set_text("subject", request.subject.str());
  root.set_text("observer", request.observer.str());
  BSM_TRY_ASSIGN(authority, detail::authority_to_json(request.authority));
  root.set("authority", authority);
  return root.build();
}

[[nodiscard]] Result<ObservationRequest> observe_request_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "observation request must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"capability", "parameters", "subject", "observer", "authority"}));
  ObservationRequest request;
  BSM_TRY_ASSIGN(capability_text, json::require_string(value, "capability"));
  if (!capability_text.empty()) {
    BSM_TRY_ASSIGN(capability, CapabilityId::parse(capability_text));
    request.capability = std::move(capability);
  }
  BSM_TRY_ASSIGN(parameters, json::require_field(value, "parameters"));
  if (!parameters->is_object()) {
    return Error(ErrorCode::TypeMismatch, "observation parameters must be an object");
  }
  request.parameters = *parameters;
  BSM_TRY_ASSIGN(subject_text, json::require_string(value, "subject"));
  BSM_TRY_ASSIGN(subject, SubjectId::parse(subject_text));
  request.subject = std::move(subject);
  BSM_TRY_ASSIGN(observer_text, json::require_string(value, "observer"));
  BSM_TRY_ASSIGN(observer, OwnerId::parse(observer_text));
  request.observer = std::move(observer);
  BSM_TRY_ASSIGN(authority_value, json::require_object_member(value, "authority"));
  BSM_TRY_ASSIGN(authority, detail::authority_from_json(*authority_value));
  request.authority = std::move(authority);
  return request;
}

[[nodiscard]] Result<JsonValue> observation_to_json(const ControllerObservation& observation) {
  JsonObjectBuilder root;
  root.set_text("owner", observation.owner.str());
  root.set_text("domain", to_string(observation.domain));
  root.set_uint("generation", observation.generation);
  root.set("value", observation.value);
  root.set_text("detail", observation.detail);
  return root.build();
}

[[nodiscard]] Result<ControllerObservation> observation_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "controller observation must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"owner", "domain", "generation", "value", "detail"}));
  ControllerObservation observation;
  BSM_TRY_ASSIGN(owner_text, json::require_string(value, "owner"));
  BSM_TRY_ASSIGN(owner, OwnerId::parse(owner_text));
  observation.owner = std::move(owner);
  BSM_TRY_ASSIGN(domain_text, json::require_string(value, "domain"));
  BSM_TRY_ASSIGN(domain, parse_authority_domain(domain_text));
  observation.domain = domain;
  BSM_TRY_ASSIGN(generation, json::require_unsigned(value, "generation"));
  observation.generation = generation;
  BSM_TRY_ASSIGN(observed, json::require_field(value, "value"));
  observation.value = *observed;
  BSM_TRY_ASSIGN(detail_text, json::require_string(value, "detail"));
  observation.detail = std::move(detail_text);
  return observation;
}

}  // namespace

struct RemoteControllerClient::Impl {
  detail::Socket socket;
  OwnerId caller;
};

RemoteControllerClient::RemoteControllerClient() : impl_(new Impl()) {}

RemoteControllerClient::RemoteControllerClient(RemoteControllerClient&& other) noexcept
    : impl_(other.impl_), connected_(other.connected_) {
  other.impl_ = nullptr;
  other.connected_ = false;
}

RemoteControllerClient& RemoteControllerClient::operator=(
    RemoteControllerClient&& other) noexcept {
  if (this != &other) {
    delete impl_;
    impl_ = other.impl_;
    connected_ = other.connected_;
    other.impl_ = nullptr;
    other.connected_ = false;
  }
  return *this;
}

RemoteControllerClient::~RemoteControllerClient() { delete impl_; }

Result<Unit> RemoteControllerClient::connect(const RemoteControllerOptions& options) {
  if (options.port == 0) {
    return Error(ErrorCode::InvalidArgument, "a remote controller needs a TCP port");
  }
  if (connected_) {
    return Error(ErrorCode::InvalidArgument, "the controller client is already connected");
  }
  BSM_TRY_ASSIGN(socket, detail::connect_loopback(options.port));
  impl_->socket = std::move(socket);
  impl_->caller = options.caller;
  connected_ = true;
  return Unit{};
}

Result<Unit> RemoteControllerClient::disconnect() {
  impl_->socket.close();
  connected_ = false;
  return Unit{};
}

Result<ControllerReply> RemoteControllerClient::request_effect(
    const RequestEnvelope& envelope) {
  if (!connected_) {
    return Error(ErrorCode::ControllerUnavailable, "the controller client is not connected");
  }
  BSM_TRY_ASSIGN(envelope_value, request_envelope_to_json(envelope));
  JsonObjectBuilder root;
  root.set_text("op", "request");
  root.set("envelope", envelope_value);
  BSM_TRY_ASSIGN(request, root.build());
  BSM_TRY_ASSIGN(response, exchange(impl_->socket, request));
  BSM_TRY_ASSIGN(reply_value, json::require_object_member(response, "reply"));
  return detail::controller_reply_from_json(*reply_value);
}

Result<ControllerReply> RemoteControllerClient::query_effect(const RequestKey& key) {
  if (!connected_) {
    return Error(ErrorCode::ControllerUnavailable, "the controller client is not connected");
  }
  JsonObjectBuilder root;
  root.set_text("op", "query");
  root.set_text("key", key.hex());
  BSM_TRY_ASSIGN(request, root.build());
  BSM_TRY_ASSIGN(response, exchange(impl_->socket, request));
  BSM_TRY_ASSIGN(reply_value, json::require_object_member(response, "reply"));
  return detail::controller_reply_from_json(*reply_value);
}

Result<ControllerObservation> RemoteControllerClient::observe(
    const ObservationRequest& request) {
  if (!connected_) {
    return Error(ErrorCode::ControllerUnavailable, "the controller client is not connected");
  }
  JsonObjectBuilder root;
  root.set_text("op", "observe");
  BSM_TRY_ASSIGN(request_value, observe_request_json(request));
  root.set("request", request_value);
  BSM_TRY_ASSIGN(message, root.build());
  BSM_TRY_ASSIGN(response, exchange(impl_->socket, message));
  BSM_TRY_ASSIGN(observation_value, json::require_object_member(response, "observation"));
  return observation_from_json(*observation_value);
}

struct PlantHost::Impl {
  Options options;
  InProcessSyntheticController controller;
  detail::Listener listener;
  std::unique_ptr<Store> store;
  std::vector<RequestKey> persisted_keys;
  bool shutdown = false;
  std::uint64_t commands = 0;

  [[nodiscard]] Result<Unit> persist_new_outcomes() {
    if (store == nullptr) {
      return Unit{};
    }
    for (const AppliedOutcome& outcome : controller.applied()) {
      if (std::find(persisted_keys.begin(), persisted_keys.end(), outcome.key) !=
          persisted_keys.end()) {
        continue;
      }
      JsonObjectBuilder root;
      root.set_text("key", outcome.key.hex());
      root.set_text("effect", outcome.effect.hex());
      BSM_TRY_ASSIGN(reply, detail::controller_reply_to_json(outcome.reply));
      root.set("reply", reply);
      // The model is the facility, so it survives the owner process that observed it.
      BSM_TRY_ASSIGN(plant_state, controller.plant().state_json());
      root.set("plant", plant_state);
      BSM_TRY_ASSIGN(payload, root.build());
      // The owner's record is durable before it answers: a lost reply must never lose
      // the fact that the effect was applied.
      BSM_RETURN_IF_ERROR(store->append(JournalRecordKind::PlantEffectApplied, payload));
      persisted_keys.push_back(outcome.key);
      std::sort(persisted_keys.begin(), persisted_keys.end());
    }
    return Unit{};
  }

  [[nodiscard]] Result<JsonValue> ledger_json() const {
    JsonObjectBuilder root;
    root.set_uint("applied", static_cast<std::uint64_t>(controller.applied().size()));
    root.set_uint("effects", static_cast<std::uint64_t>(controller.applied_effect_count()));
    root.set_uint("duplicates", static_cast<std::uint64_t>(controller.duplicate_effect_count()));
    root.set_uint("commands", commands);
    JsonArrayBuilder keys;
    for (const AppliedOutcome& outcome : controller.applied()) {
      keys.push_text(outcome.key.hex());
    }
    BSM_TRY_ASSIGN(keys_value, keys.build());
    root.set("keys", keys_value);
    BSM_TRY_ASSIGN(state, controller.plant().state_json());
    root.set("plant", state);
    return root.build();
  }

  [[nodiscard]] Result<JsonValue> failure(ErrorCode code, const std::string& message) {
    JsonObjectBuilder root;
    root.set_bool("ok", false);
    JsonObjectBuilder error;
    error.set_text("code", to_string(code));
    error.set_text("message", message);
    BSM_TRY_ASSIGN(error_value, error.build());
    root.set("error", error_value);
    return root.build();
  }

  [[nodiscard]] Result<JsonValue> handle(const JsonValue& message) {
    ++commands;
    if (!message.is_object()) {
      return failure(ErrorCode::SchemaViolation, "protocol message must be an object");
    }
    const Result<std::string> op = json::require_string(message, "op");
    if (!op.ok()) {
      return failure(ErrorCode::MissingField, "protocol message has no operation");
    }
    if (op.value() == "request") {
      const Result<const JsonValue*> envelope_value =
          json::require_object_member(message, "envelope");
      if (!envelope_value.ok()) {
        return failure(ErrorCode::MissingField, "a request message needs an envelope");
      }
      const Result<RequestEnvelope> envelope =
          detail::request_envelope_from_json(*envelope_value.value());
      if (!envelope.ok()) {
        return failure(envelope.error().code(), envelope.error().message());
      }
      Result<ControllerReply> reply = controller.request_effect(envelope.value());
      if (!reply.ok()) {
        return failure(reply.error().code(), reply.error().message());
      }
      const Result<Unit> persisted = persist_new_outcomes();
      if (!persisted.ok()) {
        return failure(persisted.error().code(), persisted.error().message());
      }
      JsonObjectBuilder root;
      root.set_bool("ok", true);
      BSM_TRY_ASSIGN(reply_value, detail::controller_reply_to_json(reply.value()));
      root.set("reply", reply_value);
      return root.build();
    }
    if (op.value() == "query") {
      const Result<std::string> key_text = json::require_string(message, "key");
      if (!key_text.ok()) {
        return failure(ErrorCode::MissingField, "a query message needs a request key");
      }
      const Result<RequestKey> key = RequestKey::from_hex(key_text.value());
      if (!key.ok()) {
        return failure(key.error().code(), key.error().message());
      }
      BSM_TRY_ASSIGN(reply, controller.query_effect(key.value()));
      JsonObjectBuilder root;
      root.set_bool("ok", true);
      BSM_TRY_ASSIGN(reply_value, detail::controller_reply_to_json(reply));
      root.set("reply", reply_value);
      return root.build();
    }
    if (op.value() == "observe") {
      const Result<const JsonValue*> request_value =
          json::require_object_member(message, "request");
      if (!request_value.ok()) {
        return failure(ErrorCode::MissingField, "an observe message needs a request");
      }
      const Result<ObservationRequest> request =
          observe_request_from_json(*request_value.value());
      if (!request.ok()) {
        return failure(request.error().code(), request.error().message());
      }
      const Result<ControllerObservation> observation = controller.observe(request.value());
      if (!observation.ok()) {
        return failure(observation.error().code(), observation.error().message());
      }
      JsonObjectBuilder root;
      root.set_bool("ok", true);
      BSM_TRY_ASSIGN(observation_value, observation_to_json(observation.value()));
      root.set("observation", observation_value);
      return root.build();
    }
    if (op.value() == "ledger") {
      BSM_TRY_ASSIGN(ledger, ledger_json());
      JsonObjectBuilder root;
      root.set_bool("ok", true);
      root.set("ledger", ledger);
      return root.build();
    }
    if (op.value() == "ping") {
      JsonObjectBuilder root;
      root.set_bool("ok", true);
      root.set_bool("pong", true);
      return root.build();
    }
    if (op.value() == "shutdown") {
      shutdown = true;
      JsonObjectBuilder root;
      root.set_bool("ok", true);
      root.set_bool("stopping", true);
      return root.build();
    }
    return failure(ErrorCode::InvalidArgument,
                   "unknown protocol operation '" + op.value() + "'");
  }
};

PlantHost::PlantHost() = default;

PlantHost::PlantHost(PlantHost&& other) noexcept : impl_(other.impl_) {
  other.impl_ = nullptr;
}

PlantHost& PlantHost::operator=(PlantHost&& other) noexcept {
  if (this != &other) {
    delete impl_;
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

PlantHost::~PlantHost() { delete impl_; }

Result<PlantHost> PlantHost::start(const Options& options) {
  auto impl = std::make_unique<Impl>();
  impl->options = options;
  impl->controller.set_policy(options.policy);
  if (options.durable_ledger) {
    if (options.store_root.empty()) {
      return Error(ErrorCode::InvalidArgument,
                   "a durable ledger needs a store root for the adjacent owner");
    }
    StoreOptions store_options;
    store_options.root = options.store_root;
    store_options.create_if_missing = options.create_store_if_missing;
    BSM_TRY_ASSIGN(store, Store::open(store_options));
    for (const JournalRecord& record : store.replayed_records()) {
      if (record.kind != JournalRecordKind::PlantEffectApplied) {
        return Error(ErrorCode::SchemaViolation,
                     "the adjacent owner ledger contains a record it does not own");
      }
      BSM_RETURN_IF_ERROR(
          json::reject_unknown_fields(record.payload, {"key", "effect", "reply", "plant"}));
      const JsonValue* plant_state = record.payload.find("plant");
      if (plant_state != nullptr) {
        BSM_RETURN_IF_ERROR(impl->controller.plant_restore(*plant_state));
      }
      BSM_TRY_ASSIGN(key_text, json::require_string(record.payload, "key"));
      BSM_TRY_ASSIGN(key, RequestKey::from_hex(key_text));
      BSM_TRY_ASSIGN(effect_text, json::require_string(record.payload, "effect"));
      BSM_TRY_ASSIGN(effect, Digest::from_hex(effect_text));
      BSM_TRY_ASSIGN(reply_value, json::require_object_member(record.payload, "reply"));
      BSM_TRY_ASSIGN(reply, detail::controller_reply_from_json(*reply_value));
      AppliedOutcome outcome;
      outcome.key = key;
      outcome.reply = reply;
      outcome.effect = effect;
      BSM_RETURN_IF_ERROR(impl->controller.restore_outcome(outcome));
      impl->persisted_keys.push_back(key);
    }
    std::sort(impl->persisted_keys.begin(), impl->persisted_keys.end());
    impl->store = std::make_unique<Store>(std::move(store));
  }
  BSM_TRY_ASSIGN(listener, detail::Listener::bind_loopback(options.port));
  impl->listener = std::move(listener);

  PlantHost host;
  host.impl_ = impl.release();
  return host;
}

Result<Unit> PlantHost::serve_one_client() {
  if (impl_ == nullptr) {
    return Error(ErrorCode::ControllerUnavailable, "the plant host is not running");
  }
  BSM_TRY_ASSIGN(socket, impl_->listener.accept_one());
  while (true) {
    const Result<std::string> line =
        socket.receive_line(limits::kMaxControllerReplyBytes);
    if (!line.ok()) {
      // The client closed the connection or was killed: the session ends, the listener
      // stays open, and the next client is served normally. A manager that dies with a
      // request in flight must not be able to stop the owner.
      socket.close();
      return Unit{};
    }
    BSM_TRY_ASSIGN(message, parse_json(line.value()));
    BSM_TRY_ASSIGN(response, impl_->handle(message));
    const Result<Unit> sent = socket.send_line(canonical_json(response));
    if (!sent.ok()) {
      socket.close();
      return Unit{};
    }
    if (impl_->shutdown) {
      socket.close();
      return Unit{};
    }
  }
}

Result<Unit> PlantHost::serve_until_shutdown() {
  while (impl_ != nullptr && !impl_->shutdown) {
    BSM_RETURN_IF_ERROR(serve_one_client());
  }
  return Unit{};
}

void PlantHost::request_shutdown() noexcept {
  if (impl_ != nullptr) {
    impl_->shutdown = true;
    impl_->listener.close();
  }
}

std::uint16_t PlantHost::port() const noexcept {
  return impl_ == nullptr ? 0 : impl_->listener.port();
}

const SyntheticPlant& PlantHost::plant() const noexcept {
  static const SyntheticPlant kEmpty;
  return impl_ == nullptr ? kEmpty : impl_->controller.plant();
}

std::size_t PlantHost::applied_effects() const noexcept {
  return impl_ == nullptr ? 0 : impl_->controller.applied().size();
}

std::size_t PlantHost::duplicate_effects() const noexcept {
  return impl_ == nullptr ? 0 : impl_->controller.duplicate_effect_count();
}

Result<JsonValue> PlantHost::ledger_json() const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::ControllerUnavailable, "the plant host is not running");
  }
  return impl_->ledger_json();
}

}  // namespace black_start_manager
