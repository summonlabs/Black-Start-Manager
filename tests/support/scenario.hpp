#pragma once

// The standard scripted progression, shared by the multiprocess tests and the probe
// executable. Every step goes through the public API, so a child process and the test
// process perform byte-identical operations.

#include <cstdint>
#include <string>

#include "black_start_manager/error.hpp"
#include "black_start_manager/limits.hpp"

namespace bsm_test {

struct StepOptions {
  std::string store;
  std::string step;
  std::uint64_t tick = 1;
  // Empty means an in-process synthetic owner; otherwise "tcp:PORT".
  std::string controller;
  bool create_store = true;
  // Journal record threshold that triggers compaction, so a crash can be injected into a
  // real snapshot publication.
  std::size_t compact_records =
      black_start_manager::limits::kMaxJournalRecordsPerSegment;
};

// Runs one named step of the standard progression. Returns the session identity of the
// session the step acted on.
[[nodiscard]] black_start_manager::Result<std::string> run_step(const StepOptions& options);

[[nodiscard]] bool is_known_step(const std::string& step);

}  // namespace bsm_test
