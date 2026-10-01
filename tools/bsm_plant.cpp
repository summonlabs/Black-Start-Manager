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

// bsm_plant: the out-of-process synthetic adjacent owner.
//
// It owns a deterministic synthetic plant and a durable ledger of applied request keys,
// and it serves the bsm command line tool over a loopback socket. Running the owner in
// its own process is what makes the duplicate-effect proofs real: the manager can be
// killed at any point without the owner forgetting what it applied.

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "black_start_manager/remote_controller.hpp"

namespace {

using black_start_manager::CapabilityId;
using black_start_manager::OwnerId;
using black_start_manager::PlantHost;
using black_start_manager::Result;
using black_start_manager::SyntheticControllerPolicy;

struct Options {
  std::string store;
  std::uint16_t port = 0;
  bool create_store = true;
  bool durable_ledger = true;
  SyntheticControllerPolicy policy;
};

void usage() {
  std::cout <<
      "usage: bsm_plant --store DIR [--port N] [--no-durable-ledger]\n"
      "                 [--refuse CAP,...] [--fail CAP,...] [--lose-reply CAP,...]\n"
      "                 [--unavailable-once CAP,...] [--defer CAP,...]\n"
      "                 [--blind OBSERVER,...] [--contradict CAP,...]\n"
      "\n"
      "Serves the synthetic adjacent owner protocol on the loopback interface.\n"
      "Prints one canonical JSON ready line, then serves until a shutdown request.\n";
}

[[nodiscard]] Result<std::vector<CapabilityId>> capability_list(const std::string& text) {
  std::vector<CapabilityId> capabilities;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::size_t end = comma == std::string::npos ? text.size() : comma;
    if (end > start) {
      BSM_TRY_ASSIGN(capability, CapabilityId::parse(text.substr(start, end - start)));
      capabilities.push_back(std::move(capability));
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return capabilities;
}

[[nodiscard]] Result<std::vector<OwnerId>> owner_list(const std::string& text) {
  std::vector<OwnerId> owners;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::size_t end = comma == std::string::npos ? text.size() : comma;
    if (end > start) {
      BSM_TRY_ASSIGN(owner, OwnerId::parse(text.substr(start, end - start)));
      owners.push_back(std::move(owner));
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return owners;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto value = [&argc, &argv, &index]() -> std::string {
      return index + 1 < argc ? std::string(argv[index + 1]) : std::string();
    };
    if (argument == "--help" || argument == "-h") {
      usage();
      return 0;
    }
    if (argument == "--store") {
      options.store = value();
      ++index;
      continue;
    }
    if (argument == "--port") {
      options.port = static_cast<std::uint16_t>(std::stoul(value()));
      ++index;
      continue;
    }
    if (argument == "--no-durable-ledger") {
      options.durable_ledger = false;
      continue;
    }
    if (argument == "--no-create") {
      options.create_store = false;
      continue;
    }
    if (argument == "--refuse" || argument == "--fail" || argument == "--lose-reply" ||
        argument == "--unavailable-once" || argument == "--defer" ||
        argument == "--contradict" || argument == "--blind") {
      const std::string list = value();
      ++index;
      Result<std::vector<CapabilityId>> capabilities =
          capability_list(list);
      Result<std::vector<OwnerId>> owners = owner_list(list);
      if (argument == "--blind") {
        if (!owners.ok()) {
          std::cerr << "bsm_plant: " << owners.error().message() << "\n";
          return 2;
        }
        for (const OwnerId& owner : owners.value()) {
          options.policy.blind_observers.push_back(owner);
        }
        continue;
      }
      if (!capabilities.ok()) {
        std::cerr << "bsm_plant: " << capabilities.error().message() << "\n";
        return 2;
      }
      std::vector<CapabilityId>& destination =
          argument == "--refuse"            ? options.policy.refuse
          : argument == "--fail"            ? options.policy.fail_after_apply
          : argument == "--lose-reply"      ? options.policy.lose_first_reply
          : argument == "--unavailable-once" ? options.policy.unavailable_once
          : argument == "--contradict"      ? options.policy.contradictory_observation
                                            : options.policy.defer;
      for (const CapabilityId& capability : capabilities.value()) {
        destination.push_back(capability);
      }
      continue;
    }
    std::cerr << "bsm_plant: unknown argument '" << argument << "'\n";
    usage();
    return 2;
  }
  if (options.store.empty() && options.durable_ledger) {
    std::cerr << "bsm_plant: --store is required unless --no-durable-ledger is given\n";
    return 2;
  }

  PlantHost::Options host_options;
  host_options.port = options.port;
  host_options.store_root = options.store;
  host_options.create_store_if_missing = options.create_store;
  host_options.policy = options.policy;
  host_options.durable_ledger = options.durable_ledger;
  Result<PlantHost> host = PlantHost::start(host_options);
  if (!host.ok()) {
    std::cerr << "bsm_plant: " << host.error().message() << "\n";
    return 1;
  }
  std::cout << "{\"ready\":true,\"port\":" << host.value().port() << "}" << std::endl;
  const Result<black_start_manager::Unit> served = host.value().serve_until_shutdown();
  if (!served.ok()) {
    std::cerr << "bsm_plant: " << served.error().message() << "\n";
    return 1;
  }
  return 0;
}
