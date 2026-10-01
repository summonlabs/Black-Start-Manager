#pragma once

// Out-of-process adjacent owner support.
//
// The plant host owns the synthetic plant and its own durable record of applied
// request keys. The manager talks to it over a loopback socket with a line-delimited
// canonical JSON protocol. Because the host's record is durable and belongs to a
// different process, killing the manager cannot duplicate a consequential request:
// the successor resolves the in-flight key against the host instead of resending it.

#include <cstdint>
#include <string>

#include "black_start_manager/controller.hpp"

namespace black_start_manager {

struct RemoteControllerOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  // Owner name the client presents as the requesting party.
  OwnerId caller;
};

class RemoteControllerClient final : public AdjacentController {
 public:
  RemoteControllerClient();
  RemoteControllerClient(const RemoteControllerClient&) = delete;
  RemoteControllerClient& operator=(const RemoteControllerClient&) = delete;
  RemoteControllerClient(RemoteControllerClient&& other) noexcept;
  RemoteControllerClient& operator=(RemoteControllerClient&& other) noexcept;
  ~RemoteControllerClient() override;

  [[nodiscard]] Result<Unit> connect(const RemoteControllerOptions& options);
  [[nodiscard]] Result<Unit> disconnect();

  [[nodiscard]] Result<ControllerReply> request_effect(
      const RequestEnvelope& envelope) override;
  [[nodiscard]] Result<ControllerReply> query_effect(const RequestKey& key) override;
  [[nodiscard]] Result<ControllerObservation> observe(
      const ObservationRequest& request) override;

  [[nodiscard]] bool connected() const noexcept { return connected_; }

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool connected_ = false;
};

// The host side: a synthetic plant plus its durable applied-key ledger, served over a
// loopback socket. Used by the bsm_plant tool and by the multiprocess tests.
class PlantHost {
 public:
  struct Options {
    std::string host = "127.0.0.1";
    // Zero asks the operating system for an ephemeral port.
    std::uint16_t port = 0;
    std::string store_root;
    bool create_store_if_missing = false;
    SyntheticControllerPolicy policy;
    // Records applied effects durably in the store. Disabling this models an owner
    // that forgets, and is used only to prove that the manager does not depend on a
    // remembered reply for its own safety.
    bool durable_ledger = true;
  };

  PlantHost();
  PlantHost(const PlantHost&) = delete;
  PlantHost& operator=(const PlantHost&) = delete;
  PlantHost(PlantHost&& other) noexcept;
  PlantHost& operator=(PlantHost&& other) noexcept;
  ~PlantHost();

  [[nodiscard]] static Result<PlantHost> start(const Options& options);

  // Blocks until one client disconnects, serving every request it sends.
  [[nodiscard]] Result<Unit> serve_one_client();
  // Blocks until a shutdown request is received. Each client is served on the calling
  // thread; requests are serialized, which is what a single-writer owner does.
  [[nodiscard]] Result<Unit> serve_until_shutdown();
  // Stops the accept loop after the current client disconnects.
  void request_shutdown() noexcept;

  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] const SyntheticPlant& plant() const noexcept;
  [[nodiscard]] std::size_t applied_effects() const noexcept;
  [[nodiscard]] std::size_t duplicate_effects() const noexcept;
  [[nodiscard]] Result<JsonValue> ledger_json() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace black_start_manager
