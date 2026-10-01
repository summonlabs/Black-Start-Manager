#pragma once

// Operating-system single-writer exclusion.
//
// Authority to mutate a store is the successful acquisition of an exclusive operating
// system lock, never the existence of a file. The kernel releases the lock when the
// holding process dies, so an abruptly killed writer never leaves a stale claim behind,
// and a successor must acquire the lock again before it may write.

#include <string>

#include "black_start_manager/error.hpp"

namespace black_start_manager::detail {

class ProcessLock {
 public:
  ProcessLock() = default;
  ProcessLock(const ProcessLock&) = delete;
  ProcessLock& operator=(const ProcessLock&) = delete;
  ProcessLock(ProcessLock&& other) noexcept;
  ProcessLock& operator=(ProcessLock&& other) noexcept;
  ~ProcessLock();

  // Acquires the exclusive lock, creating the lock file when necessary. Refuses with
  // StoreLocked when another process holds it.
  [[nodiscard]] static Result<ProcessLock> acquire(const std::string& path);

  [[nodiscard]] bool held() const noexcept;
  void release() noexcept;

 private:
  void* handle_ = nullptr;
};

}  // namespace black_start_manager::detail
