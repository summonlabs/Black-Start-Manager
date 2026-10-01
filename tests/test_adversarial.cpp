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

// Proof obligations: malformed, truncated, corrupt, contradictory, replayed, and
// boundary-shaped input is refused with a specific code, and a refusal never leaves a
// half-applied change. The frames for these tests are encoded here, independently of the
// library's own encoder, so a decoder defect cannot hide behind a matching encoder.

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "black_start_manager/manager.hpp"
#include "black_start_manager/store.hpp"
#include "src/detail/file_io.hpp"
#include "src/detail/paths.hpp"
#include "fixture.hpp"
#include "test_harness.hpp"

using namespace black_start_manager;
using namespace bsm_test;
using namespace black_start_manager::detail;

namespace {

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xffu));
  out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xffu));
  }
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xffu));
  }
}

// An independent encoder for one journal frame.
[[nodiscard]] std::vector<std::uint8_t> frame(std::uint64_t sequence,
                                              std::uint16_t kind,
                                              const std::string& payload_text,
                                              const Digest& previous_chain,
                                              Digest& chain_out,
                                              std::uint32_t declared_length = 0,
                                              std::uint16_t version = 1,
                                              const std::string& magic = "BSMJ") {
  std::vector<std::uint8_t> header;
  header.insert(header.end(), magic.begin(), magic.end());
  append_u16(header, version);
  append_u16(header, 0);
  append_u64(header, sequence);
  append_u16(header, kind);
  append_u16(header, 0);
  const std::uint32_t length = declared_length == 0
                                   ? static_cast<std::uint32_t>(payload_text.size())
                                   : declared_length;
  append_u32(header, length);
  const std::span<const std::uint8_t> payload_bytes(
      reinterpret_cast<const std::uint8_t*>(payload_text.data()), payload_text.size());
  append_u32(header, crc32c(payload_bytes));
  append_u32(header, crc32c(std::span<const std::uint8_t>(header.data(), header.size())));

  Sha256 hasher;
  hasher.update(previous_chain.span());
  hasher.update(std::span<const std::uint8_t>(header.data(), header.size()));
  hasher.update(payload_bytes);
  chain_out = hasher.finish();

  std::vector<std::uint8_t> result = header;
  result.insert(result.end(), payload_bytes.begin(), payload_bytes.end());
  const std::span<const std::uint8_t> chain_bytes = chain_out.span();
  result.insert(result.end(), chain_bytes.begin(), chain_bytes.end());
  return result;
}

[[nodiscard]] Result<Unit> write_bytes(const std::string& path,
                                       const std::vector<std::uint8_t>& bytes) {
  return write_file_durable(path, bytes);
}

[[nodiscard]] std::string artifact(const std::string& root, const char* prefix) {
  const Result<std::vector<std::string>> names = list_directory(root);
  if (!names.ok()) {
    return std::string();
  }
  for (const std::string& name : names.value()) {
    if (name.rfind(prefix, 0) == 0) {
      return join_path(root, name);
    }
  }
  return std::string();
}

// A controller that calls back into the manager from inside the callback. This is the
// positive half of the lock-reentrancy audit: a read-only callback must succeed, and a
// nested mutation must be refused instead of deadlocking.
class ReentrantController final : public AdjacentController {
 public:
  SessionManager* manager = nullptr;
  SessionId session;
  bool nested_assess_succeeded = false;
  bool nested_assess_rejected = false;
  bool nested_request_refused = false;
  bool invoked = false;

  [[nodiscard]] Result<ControllerReply> request_effect(
      const RequestEnvelope& envelope) override {
    if (!invoked && manager != nullptr) {
      invoked = true;
      const Result<Assessment> assessment = manager->assess(session);
      nested_assess_succeeded = assessment.ok();
      nested_assess_rejected = !assessment.ok();
      EffectRequest request;
      request.session = session;
      request.obligation = envelope.obligation;
      request.authority = envelope.authority;
      request.deadline_ticks = 10;
      const Result<AttemptView> nested = manager->request_effect(request);
      nested_request_refused =
          !nested.ok() && nested.error().code() == ErrorCode::ReentrantOperation;
    }
    ControllerReply reply;
    reply.kind = ControllerReplyKind::Applied;
    reply.key = envelope.key;
    reply.detail = "reentrancy probe";
    return reply;
  }

  [[nodiscard]] Result<ControllerReply> query_effect(const RequestKey& key) override {
    ControllerReply reply;
    reply.kind = ControllerReplyKind::Unknown;
    reply.key = key;
    reply.detail = "no record";
    return reply;
  }

  [[nodiscard]] Result<ControllerObservation> observe(
      const ObservationRequest& request) override {
    ControllerObservation observation;
    observation.owner = request.observer;
    observation.domain = request.authority.domain;
    observation.generation = request.authority.generation;
    observation.value = parse_json("{\"isolated\":true}").value();
    observation.detail = "reentrancy probe";
    return observation;
  }
};

struct Prepared {
  std::unique_ptr<bsm_test::Harness> harness;
  SessionId session;
};

[[nodiscard]] Result<Prepared> prepare(const char* name) {
  auto harness = std::make_unique<bsm_test::Harness>();
  BSM_RETURN_IF_ERROR(harness->start(bsm_test::scratch_directory(name), 5));
  BSM_TRY_ASSIGN(session, harness->open_session());
  Prepared prepared;
  prepared.harness = std::move(harness);
  prepared.session = session;
  return prepared;
}

}  // namespace

BSM_TEST(a_journal_frame_with_an_unknown_version_is_refused) {
  const std::string root = bsm_test::scratch_directory("adversarial-version");
  StoreOptions options;
  options.root = root;
  options.create_if_missing = true;
  BSM_CHECK_OK(store, Store::open(options));
  BSM_CHECK_OK(closed, store.close());

  const std::string journal = artifact(root, kJournalPrefix);
  Digest chain;
  const std::vector<std::uint8_t> bytes =
      frame(1, 4, "{\"session\":\"x\"}", Digest(), chain, 0, 9);
  BSM_CHECK_OK(written, write_bytes(journal, bytes));
  const Result<Store> reopened = Store::open(options);
  BSM_CHECK(!reopened.ok());
  BSM_CHECK(reopened.error().code() == ErrorCode::UnsupportedFormatVersion ||
            reopened.error().code() == ErrorCode::StoreCorrupt);
}

BSM_TEST(a_journal_frame_with_an_absurd_length_is_treated_as_a_torn_tail) {
  const std::string root = bsm_test::scratch_directory("adversarial-length");
  StoreOptions options;
  options.root = root;
  options.create_if_missing = true;
  BSM_CHECK_OK(store, Store::open(options));
  BSM_CHECK_OK(closed, store.close());
  const std::string journal = artifact(root, kJournalPrefix);
  Digest chain;
  const std::vector<std::uint8_t> bytes =
      frame(1, 4, "{\"session\":\"x\"}", Digest(), chain, 0xFFFFFFF0u);
  BSM_CHECK_OK(written, write_bytes(journal, bytes));
  BSM_CHECK_OK(reopened, Store::open(options));
  BSM_CHECK(reopened.recovery().tail_truncated);
  BSM_CHECK_EQ(reopened.replayed_records().size(), std::size_t{0});
  BSM_CHECK_OK(closed_reopened, reopened.close());
}

BSM_TEST(a_journal_record_whose_payload_is_not_an_object_is_refused) {
  const std::string root = bsm_test::scratch_directory("adversarial-payload");
  StoreOptions options;
  options.root = root;
  options.create_if_missing = true;
  BSM_CHECK_OK(store, Store::open(options));
  BSM_CHECK_OK(closed, store.close());
  const std::string journal = artifact(root, kJournalPrefix);
  Digest chain;
  const std::vector<std::uint8_t> bytes = frame(1, 4, "[]", Digest(), chain);
  BSM_CHECK_OK(written, write_bytes(journal, bytes));
  const Result<Store> reopened = Store::open(options);
  BSM_CHECK(!reopened.ok());
  BSM_CHECK(reopened.error().code() == ErrorCode::SchemaViolation ||
            reopened.error().code() == ErrorCode::StoreCorrupt);
}

BSM_TEST(a_manifest_that_names_a_path_outside_the_store_is_refused) {
  const std::string root = bsm_test::scratch_directory("adversarial-manifest");
  StoreOptions options;
  options.root = root;
  options.create_if_missing = true;
  BSM_CHECK_OK(store, Store::open(options));
  BSM_CHECK_OK(closed, store.close());

  const std::string manifest_path = join_path(root, kManifestName);
  const Result<std::vector<std::uint8_t>> bytes = read_file(manifest_path, 1024 * 1024);
  BSM_CHECK(bytes.ok());
  const std::string text(reinterpret_cast<const char*>(bytes.value().data()),
                         bytes.value().size());
  std::string modified = text;
  const std::size_t position = modified.find("snapshot-");
  BSM_CHECK(position != std::string::npos);
  modified.replace(position, 9, "../escape");
  BSM_CHECK_OK(written,
               write_bytes(manifest_path, std::vector<std::uint8_t>(modified.begin(),
                                                                    modified.end())));
  const Result<Store> reopened = Store::open(options);
  BSM_CHECK(!reopened.ok());
  BSM_CHECK(reopened.error().code() == ErrorCode::PathUnsafe ||
            reopened.error().code() == ErrorCode::IntegrityMismatch ||
            reopened.error().code() == ErrorCode::StoreCorrupt);
}

BSM_TEST(a_truncated_snapshot_is_refused) {
  const std::string root = bsm_test::scratch_directory("adversarial-snapshot");
  StoreOptions options;
  options.root = root;
  options.create_if_missing = true;
  BSM_CHECK_OK(store, Store::open(options));
  BSM_CHECK_OK(closed, store.close());
  const std::string snapshot = artifact(root, kSnapshotPrefix);
  BSM_CHECK(!snapshot.empty());
  BSM_CHECK_OK(bytes, read_file(snapshot, 1024 * 1024));
  std::vector<std::uint8_t> truncated = bytes;
  truncated.resize(truncated.size() / 2);
  BSM_CHECK_OK(written, write_bytes(snapshot, truncated));
  const Result<Store> reopened = Store::open(options);
  BSM_CHECK(!reopened.ok());
  BSM_CHECK(reopened.error().code() == ErrorCode::StoreCorrupt ||
            reopened.error().code() == ErrorCode::TruncatedInput ||
            reopened.error().code() == ErrorCode::IntegrityMismatch);
}

BSM_TEST(a_tick_beyond_the_supported_range_is_refused) {
  BSM_CHECK_OK(prepared, prepare("adversarial-tick"));
  bsm_test::Harness& harness = *prepared.harness;
  // Checked arithmetic reaches the clock: a tick beyond the supported range is refused
  // rather than wrapped.
  const Result<Unit> set_beyond = harness.clock.set(limits::kMaxTick + 1);
  BSM_CHECK(!set_beyond.ok());
  BSM_CHECK_EQ(set_beyond.error().code(), ErrorCode::NumberOutOfRange);
  BSM_CHECK_OK(advance_beyond, harness.clock.set(limits::kMaxTick));
  const Result<Unit> too_far = harness.clock.advance(1);
  BSM_CHECK(!too_far.ok());
  BSM_CHECK_EQ(too_far.error().code(), ErrorCode::NumberOutOfRange);
}

BSM_TEST(a_deadline_window_of_zero_is_refused) {
  BSM_CHECK_OK(prepared, prepare("adversarial-deadline"));
  bsm_test::Harness& harness = *prepared.harness;
  EffectRequest request;
  request.session = prepared.session;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.authority = harness.definition.electrical;
  request.deadline_ticks = 0;
  BSM_CHECK_EQ(harness.manager->request_effect(request).error().code(),
               ErrorCode::InvalidArgument);
  request.deadline_ticks = limits::kMaxDeadlineTicks + 1;
  BSM_CHECK_EQ(harness.manager->request_effect(request).error().code(),
               ErrorCode::InvalidArgument);
}

BSM_TEST(an_observation_naming_a_foreign_obligation_is_refused) {
  BSM_CHECK_OK(prepared, prepare("adversarial-foreign"));
  bsm_test::Harness& harness = *prepared.harness;
  Observation observation;
  observation.session = prepared.session;
  observation.kind = EvidenceKind::parse("isolation_verified").value();
  observation.subject = SubjectId::parse("isolation:a").value();
  observation.value = parse_json("{\"isolated\":true}").value();
  observation.source_owner = OwnerId::parse("electrical-owner").value();
  observation.source_domain = AuthorityDomain::Electrical;
  observation.source_generation = 1;
  observation.observed_tick = harness.clock.now_ticks().value();
  observation.has_obligation = true;
  observation.obligation = ObligationId::parse("energize-a").value();
  const Result<EvidenceView> admitted = harness.manager->record_observation(observation);
  BSM_CHECK(!admitted.ok());
  BSM_CHECK_EQ(admitted.error().code(), ErrorCode::EvidenceUnknownRequirement);
}

BSM_TEST(a_controller_that_calls_back_into_the_manager_does_not_deadlock) {
  const std::string root = bsm_test::scratch_directory("adversarial-reentrancy");
  BSM_CHECK_OK(definition, bsm_test::make_facility());
  ReentrantController controller;
  StaticFacilityState facility(definition.binding);
  ManualClock clock(5);
  ManagerOptions options;
  options.store_root = root;
  options.create_store_if_missing = true;
  options.clock = &clock;
  options.controller = &controller;
  options.facility = &facility;
  BSM_CHECK_OK(opened, SessionManager::open(options));
  std::unique_ptr<SessionManager> manager =
      std::make_unique<SessionManager>(std::move(opened));
  EstablishSessionRequest establish;
  establish.plan = definition.plan;
  establish.binding = definition.binding;
  establish.authority = definition.control;
  BSM_CHECK_OK(session, manager->establish_session(establish));
  controller.manager = manager.get();
  controller.session = session.id;

  EffectRequest request;
  request.session = session.id;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.authority = definition.electrical;
  request.deadline_ticks = 20;
  BSM_CHECK_OK(attempt, manager->request_effect(request));
  BSM_CHECK(controller.invoked);
  BSM_CHECK(controller.nested_assess_succeeded || controller.nested_assess_rejected);
  BSM_CHECK(!controller.nested_assess_rejected);
  BSM_CHECK(controller.nested_request_refused);
  BSM_CHECK_EQ(attempt.attempt.state, AttemptState::Acknowledged);
}

BSM_TEST(a_refusal_from_the_controller_leaves_no_half_applied_state) {
  BSM_CHECK_OK(prepared, prepare("adversarial-refusal"));
  bsm_test::Harness& harness = *prepared.harness;
  BSM_CHECK_OK(capability, CapabilityId::parse("verify-isolation"));
  SyntheticControllerPolicy policy;
  policy.refuse.push_back(capability);
  harness.controller.set_policy(policy);
  BSM_CHECK_OK(before, harness.manager->report(prepared.session));
  EffectRequest request;
  request.session = prepared.session;
  request.obligation = ObligationId::parse("isolation-a").value();
  request.authority = harness.definition.electrical;
  request.deadline_ticks = 20;
  BSM_CHECK_OK(attempt, harness.manager->request_effect(request));
  BSM_CHECK_EQ(attempt.attempt.state, AttemptState::Refused);
  BSM_CHECK_OK(after, harness.manager->report(prepared.session));
  const JsonValue* before_attempts = before.find("attempts");
  const JsonValue* after_attempts = after.find("attempts");
  BSM_CHECK(before_attempts != nullptr && after_attempts != nullptr);
  BSM_CHECK_EQ(after_attempts->as_array().size(),
               before_attempts->as_array().size() + 1);
  BSM_CHECK_OK(evidence_report, harness.manager->report(prepared.session));
  const JsonValue* evidence = evidence_report.find("evidence");
  BSM_CHECK(evidence != nullptr);
  BSM_CHECK_EQ(evidence->as_array().size(), std::size_t{0});
}

BSM_TEST(json_boundaries_are_refused_without_partial_parsing) {
  BSM_CHECK(!parse_json(std::string(limits::kMaxJsonBytes + 1, ' ')).ok());
  BSM_CHECK(!parse_json("{\"a\":" + std::string(limits::kMaxStringBytes + 1, 'a') + "}").ok());
  std::string deep;
  for (int index = 0; index < 200; ++index) {
    deep += "[";
  }
  BSM_CHECK(!parse_json(deep).ok());
}

BSM_TEST_MAIN("bsm_test_adversarial")
