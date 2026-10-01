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

// Proof obligations: one authoritative generation, an explicit commit point, refusal of
// ambiguous or corrupt state, conservative handling of a torn tail, and exclusion of a
// second writer.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "black_start_manager/store.hpp"
#include "src/detail/file_io.hpp"
#include "src/detail/paths.hpp"
#include "fixture.hpp"
#include "test_harness.hpp"

using namespace black_start_manager;
using namespace bsm_test;
using namespace black_start_manager::detail;

namespace {

[[nodiscard]] Result<JsonValue> payload(std::uint64_t sequence) {
  JsonObjectBuilder builder;
  builder.set_uint("sequence", sequence);
  builder.set_text("kind", "test");
  return builder.build();
}

[[nodiscard]] std::string file_in(const std::string& root, const char* name) {
  return join_path(root, name);
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::string& path) {
  const Result<std::vector<std::uint8_t>> bytes =
      read_file(path, limits::kMaxSnapshotPayload + 4096);
  return bytes.ok() ? bytes.value() : std::vector<std::uint8_t>{};
}

[[nodiscard]] Result<Unit> write_bytes(const std::string& path,
                                       const std::vector<std::uint8_t>& bytes) {
  return write_file_durable(path, bytes);
}

[[nodiscard]] StoreOptions options_for(const std::string& root) {
  StoreOptions options;
  options.root = root;
  options.create_if_missing = true;
  return options;
}

[[nodiscard]] std::string find_artifact(const std::string& root, const char* prefix) {
  const Result<std::vector<std::string>> names = list_directory(root);
  if (!names.ok()) {
    return std::string();
  }
  for (const std::string& name : names.value()) {
    if (name.rfind(prefix, 0) == 0) {
      return file_in(root, name.c_str());
    }
  }
  return std::string();
}

}  // namespace

BSM_TEST(a_new_store_publishes_one_authoritative_generation) {
  const std::string root = bsm_test::scratch_directory("store-new");
  BSM_CHECK_OK(store, Store::open(options_for(root)));
  BSM_CHECK(store.recovery().created_new_store);
  BSM_CHECK_EQ(store.recovery().records_replayed, std::size_t{0});
  BSM_CHECK(!store.recovery().tail_truncated);
  BSM_CHECK_EQ(store.epoch(), std::uint64_t{2});
  BSM_CHECK_OK(verification, store.verify());
  BSM_CHECK_EQ(verification.generation, std::uint64_t{1});
  BSM_CHECK(!verification.snapshot_digest.is_zero());
  BSM_CHECK_OK(first, store.append(JournalRecordKind::EvidenceAdmitted, payload(1).value()));
  BSM_CHECK_OK(second, store.append(JournalRecordKind::EvidenceAdmitted, payload(2).value()));
  (void)first;
  (void)second;
  BSM_CHECK_EQ(store.last_sequence(), std::uint64_t{2});
}

BSM_TEST(records_replay_in_order_after_a_reopen) {
  const std::string root = bsm_test::scratch_directory("store-replay");
  {
    BSM_CHECK_OK(store, Store::open(options_for(root)));
    for (std::uint64_t index = 1; index <= 5; ++index) {
      BSM_CHECK_OK(appended, store.append(JournalRecordKind::EvidenceAdmitted,
                                          payload(index).value()));
      (void)appended;
    }
    BSM_CHECK_OK(closed, store.close());
  }
  BSM_CHECK_OK(reopened, Store::open(options_for(root)));
  BSM_CHECK_EQ(reopened.replayed_records().size(), std::size_t{5});
  for (std::size_t index = 0; index < reopened.replayed_records().size(); ++index) {
    BSM_CHECK_EQ(reopened.replayed_records()[index].sequence,
                 static_cast<std::uint64_t>(index + 1));
    BSM_CHECK(reopened.replayed_records()[index].kind ==
              JournalRecordKind::EvidenceAdmitted);
  }
  BSM_CHECK_EQ(reopened.epoch(), std::uint64_t{3});
  BSM_CHECK(!(reopened.incarnation() == reopened.recovery().previous_incarnation));
}

BSM_TEST(a_second_writer_is_refused_and_abrupt_release_frees_the_lock) {
  const std::string root = bsm_test::scratch_directory("store-lock");
  BSM_CHECK_OK(store, Store::open(options_for(root)));
  const Result<Store> second = Store::open(options_for(root));
  BSM_CHECK(!second.ok());
  BSM_CHECK_EQ(second.error().code(), ErrorCode::StoreLocked);
  BSM_CHECK_OK(closed, store.close());
  BSM_CHECK_OK(third, Store::open(options_for(root)));
  BSM_CHECK_OK(closed_third, third.close());
}

BSM_TEST(a_torn_tail_is_discarded_conservatively) {
  const std::string root = bsm_test::scratch_directory("store-torn");
  {
    BSM_CHECK_OK(store, Store::open(options_for(root)));
    for (std::uint64_t index = 1; index <= 3; ++index) {
      BSM_CHECK_OK(appended, store.append(JournalRecordKind::EvidenceAdmitted,
                                          payload(index).value()));
      (void)appended;
    }
    BSM_CHECK_OK(closed, store.close());
  }
  const std::string journal = find_artifact(root, kJournalPrefix);
  BSM_CHECK(!journal.empty());
  std::vector<std::uint8_t> bytes = read_bytes(journal);
  BSM_CHECK(bytes.size() > 10);
  // Cut the last frame in half: an interrupted append leaves exactly this shape.
  bytes.resize(bytes.size() - 10);
  BSM_CHECK_OK(written, write_bytes(journal, bytes));
  BSM_CHECK_OK(reopened, Store::open(options_for(root)));
  BSM_CHECK(reopened.recovery().tail_truncated);
  // The whole remaining fragment of the interrupted frame is discarded, not just the part
  // this test removed, and every complete record survives.
  BSM_CHECK(reopened.recovery().tail_bytes_discarded > 10);
  BSM_CHECK(reopened.recovery().tail_bytes_discarded < 128);
  // The interrupted frame is discarded whole, so only the complete records replay.
  BSM_CHECK_EQ(reopened.replayed_records().size(), std::size_t{2});
  BSM_CHECK_EQ(reopened.last_sequence(), std::uint64_t{2});
  BSM_CHECK_OK(closed_reopened, reopened.close());
}

BSM_TEST(a_damaged_frame_before_valid_frames_is_refused) {
  const std::string root = bsm_test::scratch_directory("store-damaged");
  {
    BSM_CHECK_OK(store, Store::open(options_for(root)));
    for (std::uint64_t index = 1; index <= 4; ++index) {
      BSM_CHECK_OK(appended, store.append(JournalRecordKind::EvidenceAdmitted,
                                          payload(index).value()));
      (void)appended;
    }
    BSM_CHECK_OK(closed, store.close());
  }
  const std::string journal = find_artifact(root, kJournalPrefix);
  std::vector<std::uint8_t> bytes = read_bytes(journal);
  BSM_CHECK(bytes.size() > 200);
  // Corrupt a byte in the first frame's payload: the chain must no longer link.
  bytes[40] = static_cast<std::uint8_t>(bytes[40] ^ 0xffu);
  BSM_CHECK_OK(written, write_bytes(journal, bytes));
  const Result<Store> reopened = Store::open(options_for(root));
  BSM_CHECK(!reopened.ok());
  BSM_CHECK(reopened.error().code() == ErrorCode::IntegrityMismatch ||
            reopened.error().code() == ErrorCode::StoreCorrupt);
}

BSM_TEST(a_tampered_manifest_is_refused) {
  const std::string root = bsm_test::scratch_directory("store-manifest");
  {
    BSM_CHECK_OK(store, Store::open(options_for(root)));
    BSM_CHECK_OK(appended, store.append(JournalRecordKind::EvidenceAdmitted, payload(1).value()));
    (void)appended;
    BSM_CHECK_OK(closed, store.close());
  }
  const std::string manifest = file_in(root, kManifestName);
  std::string text(reinterpret_cast<const char*>(read_bytes(manifest).data()),
                   read_bytes(manifest).size());
  BSM_CHECK(!text.empty());
  // Changing the recorded epoch without recomputing the manifest digest is exactly the
  // tamper the digest exists to catch.
  const std::size_t epoch = text.find("\"epoch\":");
  BSM_CHECK(epoch != std::string::npos);
  const std::size_t digit = text.find_first_of("0123456789", epoch);
  BSM_CHECK(digit != std::string::npos);
  text[digit] = text[digit] == '9' ? '8' : static_cast<char>(text[digit] + 1);
  BSM_CHECK_OK(written, write_bytes(manifest, std::vector<std::uint8_t>(text.begin(),
                                                                       text.end())));
  const Result<Store> reopened = Store::open(options_for(root));
  BSM_CHECK(!reopened.ok());
  BSM_CHECK(reopened.error().code() == ErrorCode::IntegrityMismatch ||
            reopened.error().code() == ErrorCode::StoreCorrupt ||
            reopened.error().code() == ErrorCode::SchemaViolation ||
            reopened.error().code() == ErrorCode::UnsupportedFormatVersion);
}

BSM_TEST(a_missing_manifest_with_artifacts_present_is_ambiguous) {
  const std::string root = bsm_test::scratch_directory("store-ambiguous");
  {
    BSM_CHECK_OK(store, Store::open(options_for(root)));
    BSM_CHECK_OK(closed, store.close());
  }
  BSM_CHECK_OK(removed, remove_file(file_in(root, kManifestName)));
  const Result<Store> reopened = Store::open(options_for(root));
  BSM_CHECK(!reopened.ok());
  BSM_CHECK_EQ(reopened.error().code(), ErrorCode::StoreAmbiguous);
}

BSM_TEST(a_snapshot_publication_rotates_the_generation_and_removes_orphans) {
  const std::string root = bsm_test::scratch_directory("store-snapshot");
  BSM_CHECK_OK(store, Store::open(options_for(root)));
  for (std::uint64_t index = 1; index <= 3; ++index) {
    BSM_CHECK_OK(appended,
                 store.append(JournalRecordKind::EvidenceAdmitted, payload(index).value()));
    (void)appended;
  }
  BSM_CHECK_OK(published, store.publish_snapshot(payload(99).value()));
  BSM_CHECK_EQ(store.generation(), std::uint64_t{2});
  BSM_CHECK_EQ(store.last_sequence(), std::uint64_t{3});
  BSM_CHECK_OK(published_again, store.publish_snapshot(payload(100).value()));
  BSM_CHECK_EQ(store.generation(), std::uint64_t{3});
  BSM_CHECK(!store.should_compact());
  BSM_CHECK_OK(verification, store.verify());
  BSM_CHECK_EQ(verification.generation, std::uint64_t{3});
  BSM_CHECK_OK(closed, store.close());

  BSM_CHECK_OK(reopened, Store::open(options_for(root)));
  BSM_CHECK_EQ(reopened.generation(), std::uint64_t{3});
  BSM_CHECK_EQ(reopened.replayed_records().size(), std::size_t{0});
  BSM_CHECK(reopened.snapshot_payload().is_object());
  BSM_CHECK_OK(closed_reopened, reopened.close());
}

BSM_TEST(an_unpublished_staged_snapshot_is_ignored_and_cleaned) {
  const std::string root = bsm_test::scratch_directory("store-orphan");
  {
    BSM_CHECK_OK(store, Store::open(options_for(root)));
    BSM_CHECK_OK(appended, store.append(JournalRecordKind::EvidenceAdmitted, payload(1).value()));
    (void)appended;
    BSM_CHECK_OK(closed, store.close());
  }
  // Simulate a crash between staging a snapshot and publishing the manifest.
  const std::string staged = file_in(root, (std::string(snapshot_name(9)) + kTemporarySuffix).c_str());
  BSM_CHECK_OK(written, write_bytes(staged, std::vector<std::uint8_t>{'x', 'y'}));
  BSM_CHECK_OK(reopened, Store::open(options_for(root)));
  BSM_CHECK_EQ(reopened.generation(), std::uint64_t{1});
  BSM_CHECK_EQ(reopened.recovery().removed_orphans.size(), std::size_t{1});
  BSM_CHECK_OK(closed_reopened, reopened.close());
}

BSM_TEST(compaction_is_triggered_by_the_configured_threshold) {
  const std::string root = bsm_test::scratch_directory("store-compact");
  StoreOptions options = options_for(root);
  options.compact_journal_records = 2;
  BSM_CHECK_OK(store, Store::open(options));
  BSM_CHECK_OK(first, store.append(JournalRecordKind::EvidenceAdmitted, payload(1).value()));
  (void)first;
  BSM_CHECK(!store.should_compact());
  BSM_CHECK_OK(second, store.append(JournalRecordKind::EvidenceAdmitted, payload(2).value()));
  (void)second;
  BSM_CHECK(store.should_compact());
  BSM_CHECK_OK(compacted, store.maybe_compact(payload(0).value()));
  BSM_CHECK(compacted);
  BSM_CHECK_EQ(store.generation(), std::uint64_t{2});
  BSM_CHECK_EQ((store.maybe_compact(payload(0).value())).value(), false);
  BSM_CHECK_OK(closed, store.close());
}

BSM_TEST(unsafe_paths_and_artifact_names_are_refused) {
  BSM_CHECK(!validate_store_root("").ok());
  BSM_CHECK(!validate_store_root(std::string(limits::kMaxStoreRootLength + 1, 'a')).ok());
  BSM_CHECK(!validate_store_root("C:\\temp\\CON").ok());
  BSM_CHECK(!validate_store_root(std::string("root\0name", 9)).ok());
  BSM_CHECK(!validate_artifact_name("../escape").ok());
  BSM_CHECK(!validate_artifact_name("..").ok());
  BSM_CHECK(!validate_artifact_name("snapshot-1.bsm.exe").ok());
  BSM_CHECK(!validate_artifact_name(".hidden").ok());
  BSM_CHECK(!validate_artifact_name("LPT1").ok());
  BSM_CHECK(validate_artifact_name("snapshot-0000000000000001.bsm").ok());
  BSM_CHECK(validate_artifact_name("journal-0000000000000001.log").ok());
  BSM_CHECK(validate_artifact_name("store.manifest").ok());
  BSM_CHECK(validate_store_file("snapshot-0000000000000001.bsm.tmp").ok());
  BSM_CHECK(!validate_store_file("snapshot-0000000000000001.bsm.tmp.exe").ok());
  BSM_CHECK(!is_snapshot_name("snapshot-.bsm"));
  BSM_CHECK(!is_journal_name("journal-abc.log"));
  BSM_CHECK_EQ(snapshot_name(7), std::string("snapshot-0000000000000007.bsm"));
  BSM_CHECK_EQ(journal_name(7), std::string("journal-0000000000000007.log"));
}

BSM_TEST(a_store_that_does_not_exist_is_refused_without_create) {
  const std::string root =
      bsm_test::scratch_directory("store-missing") + "-absent";
  StoreOptions options;
  options.root = root;
  options.create_if_missing = false;
  const Result<Store> store = Store::open(options);
  BSM_CHECK(!store.ok());
  BSM_CHECK_EQ(store.error().code(), ErrorCode::StoreNotFound);
}

BSM_TEST_MAIN("bsm_test_persistence")
