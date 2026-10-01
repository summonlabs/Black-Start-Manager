#pragma once

// The durable store: one append-only journal of framed, chained records, periodic
// snapshots, and a manifest that names exactly one authoritative generation.
//
// Commit point: a record is committed when its frame is written and flushed; the
// digest chain that links records is what makes a torn tail distinguishable from
// corruption. Publication point: a snapshot generation becomes authoritative when the
// manifest that names it is replaced atomically. Recovery refuses ambiguous, corrupt,
// truncated, incompatible, or partially published state and never guesses.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "black_start_manager/canonical.hpp"
#include "black_start_manager/digest.hpp"
#include "black_start_manager/error.hpp"
#include "black_start_manager/ids.hpp"
#include "black_start_manager/limits.hpp"

namespace black_start_manager {

enum class JournalRecordKind : std::uint16_t {
  SessionEstablished = 1,
  SessionReplanned = 2,
  AuthorityReestablished = 3,
  EvidenceAdmitted = 4,
  AttemptRecorded = 5,
  AttemptSettled = 6,
  AttemptResolved = 7,
  StageAdvanced = 8,
  SessionHeld = 9,
  SessionResumed = 10,
  SessionAborted = 11,
  SessionCompleted = 12,
  SessionSuperseded = 13,
  SessionsDemoted = 14,
  // Used by an adjacent owner that keeps its own durable applied-request ledger in a store
  // of its own. The store engine is generic over record payloads.
  PlantEffectApplied = 15,
};

[[nodiscard]] const char* to_string(JournalRecordKind kind) noexcept;
[[nodiscard]] Result<JournalRecordKind> parse_journal_record_kind(std::uint64_t value);

struct JournalRecord {
  std::uint64_t sequence = 0;
  JournalRecordKind kind = JournalRecordKind::SessionEstablished;
  JsonValue payload;
};

struct StoreOptions {
  std::string root;
  bool create_if_missing = false;
  // Compaction thresholds. A segment is rotated when either bound is reached.
  std::uint64_t compact_journal_bytes = limits::kMaxJournalSegmentBytes;
  std::size_t compact_journal_records = limits::kMaxJournalRecordsPerSegment;
};

struct RecoveryReport {
  bool created_new_store = false;
  std::uint64_t generation = 0;
  std::uint64_t epoch = 0;
  IncarnationId previous_incarnation;
  IncarnationId incarnation;
  std::size_t records_replayed = 0;
  std::uint64_t journal_bytes = 0;
  std::uint64_t tail_bytes_discarded = 0;
  bool tail_truncated = false;
  Digest snapshot_digest;
  Digest chain;
  std::vector<std::string> notes;
  std::vector<std::string> removed_orphans;

  [[nodiscard]] Result<JsonValue> to_json() const;
};

struct StoreVerification {
  std::uint64_t generation = 0;
  std::uint64_t epoch = 0;
  std::size_t journal_records = 0;
  std::uint64_t journal_bytes = 0;
  Digest snapshot_digest;
  Digest chain;
  std::vector<std::string> notes;

  [[nodiscard]] Result<JsonValue> to_json() const;
};

class Store {
 public:
  Store() = default;
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&& other) noexcept;
  Store& operator=(Store&& other) noexcept;
  ~Store();

  // Opens (or creates) a store, takes the exclusive writer lock, recovers the last
  // whole authoritative generation, and records a new incarnation epoch.
  [[nodiscard]] static Result<Store> open(const StoreOptions& options);

  // Appends one record and flushes it. This is the commit point.
  [[nodiscard]] Result<Unit> append(JournalRecordKind kind, JsonValue payload);

  // Publishes a snapshot generation. Staged write, flush, read-back verification,
  // then one atomic manifest replacement. Rotates the journal segment.
  [[nodiscard]] Result<Unit> publish_snapshot(JsonValue snapshot_payload);

  // Publishes a snapshot when a compaction threshold has been reached.
  [[nodiscard]] Result<bool> maybe_compact(const JsonValue& snapshot_payload);

  // Re-reads every artifact from disk and verifies digests, chain, and structure.
  [[nodiscard]] Result<StoreVerification> verify() const;

  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  [[nodiscard]] const JsonValue& snapshot_payload() const noexcept { return snapshot_payload_; }
  [[nodiscard]] const std::vector<JournalRecord>& replayed_records() const noexcept {
    return records_;
  }
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
  [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
  [[nodiscard]] IncarnationId incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_sequence_; }
  [[nodiscard]] bool should_compact() const noexcept;
  [[nodiscard]] const std::string& root() const noexcept { return options_.root; }

  [[nodiscard]] Result<Unit> close();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  StoreOptions options_;
  RecoveryReport recovery_;
  JsonValue snapshot_payload_;
  std::vector<JournalRecord> records_;
  std::uint64_t generation_ = 0;
  std::uint64_t epoch_ = 0;
  std::uint64_t last_sequence_ = 0;
  std::size_t journal_records_ = 0;
  std::uint64_t journal_bytes_ = 0;
  IncarnationId incarnation_;
};

}  // namespace black_start_manager
