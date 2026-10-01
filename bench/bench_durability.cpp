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

// Benchmark: the durable costs this boundary actually pays.
//
// Every measured operation completes and commits: the journal append is followed by a
// real flush, and the stage publication is followed by a real atomic manifest
// replacement. The workload is one synthetic facility (SYNTHETIC) on this host's real
// filesystem (REAL). Values are reported per completed operation, never per submission.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "black_start_manager/controller.hpp"
#include "black_start_manager/manager.hpp"
#include "black_start_manager/store.hpp"

namespace {

using namespace black_start_manager;

struct Measurement {
  std::string name;
  std::uint64_t operations = 0;
  double seconds = 0.0;
  std::uint64_t bytes_per_operation = 0;

  void print() const {
    const double per_second =
        seconds > 0.0 ? static_cast<double>(operations) / seconds : 0.0;
    std::printf("%-46s %10llu ops  %8.1f ops/s  %10llu bytes/op\n", name.c_str(),
                static_cast<unsigned long long>(operations), per_second,
                static_cast<unsigned long long>(bytes_per_operation));
  }
};

[[nodiscard]] std::string scratch(const char* name) {
  const std::filesystem::path path =
      std::filesystem::current_path() / "bsm-bench-state" / name;
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
  return path.string();
}

[[nodiscard]] Measurement measure_journal_append(std::uint64_t operations) {
  Measurement measurement;
  measurement.name = "durable journal append (flush per record)";
  StoreOptions options;
  options.root = scratch("append");
  options.create_if_missing = true;
  Result<Store> store = Store::open(options);
  if (!store.ok()) {
    std::cerr << store.error().message() << '\n';
    return measurement;
  }
  JsonObjectBuilder payload;
  payload.set_text("kind", "benchmark");
  const Result<JsonValue> value = payload.build();
  if (!value.ok()) {
    return measurement;
  }
  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index < operations; ++index) {
    const Result<Unit> appended =
        store.value().append(JournalRecordKind::EvidenceAdmitted, value.value());
    if (!appended.ok()) {
      std::cerr << appended.error().message() << '\n';
      break;
    }
    ++measurement.operations;
  }
  const auto finish = std::chrono::steady_clock::now();
  measurement.seconds = std::chrono::duration<double>(finish - start).count();
  const Result<StoreVerification> verification = store.value().verify();
  if (verification.ok()) {
    measurement.bytes_per_operation = measurement.operations == 0
                                          ? 0
                                          : verification.value().journal_bytes /
                                                measurement.operations;
  }
  const Result<Unit> closed = store.value().close();
  (void)closed;
  return measurement;
}

[[nodiscard]] Measurement measure_snapshot_publication(std::uint64_t operations) {
  Measurement measurement;
  measurement.name = "snapshot publication (flush + atomic replace)";
  StoreOptions options;
  options.root = scratch("snapshot");
  options.create_if_missing = true;
  Result<Store> store = Store::open(options);
  if (!store.ok()) {
    std::cerr << store.error().message() << '\n';
    return measurement;
  }
  JsonObjectBuilder payload;
  payload.set_uint("sessions", operations);
  const Result<JsonValue> value = payload.build();
  if (!value.ok()) {
    return measurement;
  }
  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index < operations; ++index) {
    const Result<Unit> published = store.value().publish_snapshot(value.value());
    if (!published.ok()) {
      std::cerr << published.error().message() << '\n';
      break;
    }
    ++measurement.operations;
  }
  const auto finish = std::chrono::steady_clock::now();
  measurement.seconds = std::chrono::duration<double>(finish - start).count();
  measurement.bytes_per_operation = 4096;
  const Result<Unit> closed = store.value().close();
  (void)closed;
  return measurement;
}

[[nodiscard]] Measurement measure_session_requests(std::uint64_t operations) {
  Measurement measurement;
  measurement.name = "evidence admission round trip (durable)";
  const std::string root = scratch("session");
  ManualClock clock(1);
  InProcessSyntheticController owner;
  Result<PlanDocument> plan = PlanDocument::parse(R"JSON({
    "format_version": 1,
    "facility": "bench-facility",
    "policy": {"id": "bench-policy", "revision": 1, "default_max_age_ticks": 1000000,
               "default_attempt_budget": 2, "strict_within_stage_order": true},
    "stages": [
      {"id": "stage-0", "rank": 0, "title": "stage"},
      {"id": "stage-1", "rank": 1, "title": "next"}
    ],
    "obligations": [
      {"id": "ob-0", "title": "evidence only", "stage": "stage-0",
       "priority": "standard", "owner_domain": "electrical", "owner": "electrical-owner",
       "consequential": false,
       "requires": [{"kind": "probe_ready", "subject": "probe:1", "min_sources": 1,
                     "required_owner": "electrical-owner",
                     "required_domain": "electrical", "expect": {"ready": true}}]}
    ],
    "return_to_service": {"evidence": [], "required_obligations": ["ob-0"]}
  })JSON");
  if (!plan.ok()) {
    std::cerr << plan.error().message() << '\n';
    return measurement;
  }
  const Result<JsonValue> control_document = parse_json(R"JSON({
    "id": "authority-control", "domain": "control", "owner": "ops-control",
    "generation": 1, "attestation":
    "3333333333333333333333333333333333333333333333333333333333333333"
  })JSON");
  if (!control_document.ok()) {
    return measurement;
  }
  const Result<AuthorityRef> control = authority_from_json(control_document.value());
  if (!control.ok()) {
    return measurement;
  }
  const Result<JsonValue> authority_document = parse_json(R"JSON({
    "id": "authority-electrical", "domain": "electrical", "owner": "electrical-owner",
    "generation": 1, "attestation":
    "1111111111111111111111111111111111111111111111111111111111111111"
  })JSON");
  if (!authority_document.ok()) {
    return measurement;
  }
  const Result<AuthorityRef> authority = authority_from_json(authority_document.value());
  if (!authority.ok()) {
    return measurement;
  }
  const Result<JsonValue> binding_document = parse_json(R"JSON({
    "facility": "bench-facility", "facility_epoch": 1,
    "topology_digest": "2222222222222222222222222222222222222222222222222222222222222222",
    "policy": "bench-policy", "policy_revision": 1,
    "incident": {"id": "bench-incident", "generation": 1},
    "authorities": [{"id": "authority-control", "domain": "control", "owner": "ops-control",
                     "generation": 1,
                     "attestation":
                     "3333333333333333333333333333333333333333333333333333333333333333"},
                    {"id": "authority-electrical", "domain": "electrical",
                     "owner": "electrical-owner", "generation": 1,
                     "attestation":
                     "1111111111111111111111111111111111111111111111111111111111111111"}],
    "prerequisite_generations": []
  })JSON");
  if (!binding_document.ok()) {
    std::cerr << binding_document.error().message() << '\n';
    return measurement;
  }
  const Result<FacilityBinding> binding = binding_from_json(binding_document.value());
  if (!binding.ok()) {
    std::cerr << binding.error().message() << '\n';
    return measurement;
  }
  StaticFacilityState state(binding.value());
  ManagerOptions options;
  options.store_root = root;
  options.create_store_if_missing = true;
  options.clock = &clock;
  options.controller = &owner;
  options.facility = &state;
  Result<SessionManager> manager = SessionManager::open(options);
  if (!manager.ok()) {
    std::cerr << manager.error().message() << '\n';
    return measurement;
  }
  EstablishSessionRequest establish;
  establish.plan = plan.value();
  establish.binding = binding.value();
  establish.authority = control.value();
  Result<SessionView> session = manager.value().establish_session(establish);
  if (!session.ok()) {
    std::cerr << session.error().message() << '\n';
    return measurement;
  }

  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index < operations; ++index) {
    Result<Unit> tick = clock.advance(1);
    if (!tick.ok()) {
      break;
    }
    JsonObjectBuilder value;
    value.set_bool("ready", true);
    value.set_uint("sequence", index);
    const Result<JsonValue> value_json = value.build();
    if (!value_json.ok()) {
      break;
    }
    Observation observation;
    observation.session = session.value().id;
    observation.kind = EvidenceKind::parse("probe_ready").value();
    observation.subject = SubjectId::parse("probe:1").value();
    observation.value = value_json.value();
    observation.source_owner = OwnerId::parse("electrical-owner").value();
    observation.source_domain = AuthorityDomain::Electrical;
    observation.source_generation = 1;
    observation.observed_tick = clock.now_ticks().value();
    const Result<EvidenceView> admitted = manager.value().record_observation(observation);
    if (!admitted.ok()) {
      std::cerr << admitted.error().message() << '\n';
      break;
    }
    ++measurement.operations;
  }
  const auto finish = std::chrono::steady_clock::now();
  measurement.seconds = std::chrono::duration<double>(finish - start).count();
  const Result<JsonValue> verification = manager.value().verify_store();
  if (verification.ok() && measurement.operations > 0) {
    const JsonValue* bytes = verification.value().find("journal_bytes");
    if (bytes != nullptr && bytes->is_integer()) {
      measurement.bytes_per_operation =
          static_cast<std::uint64_t>(bytes->as_integer()) / measurement.operations;
    }
  }
  const Result<Unit> closed = manager.value().close();
  (void)closed;
  return measurement;
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t operations =
      argc > 1 ? static_cast<std::uint64_t>(std::stoull(argv[1])) : 2000;
  std::cout << "Black Start Manager durability benchmark\n"
            << "provenance: REAL filesystem durability (flush + atomic replace), "
               "SYNTHETIC facility and owner\n"
            << "scale: " << operations << " completed operations per measurement\n\n";
  measure_journal_append(operations).print();
  measure_snapshot_publication(operations / 20 == 0 ? 1 : operations / 20).print();
  measure_session_requests(operations / 4 == 0 ? 1 : operations / 4).print();
  return 0;
}
