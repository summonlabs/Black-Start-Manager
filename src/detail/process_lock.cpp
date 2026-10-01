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

#include "detail/process_lock.hpp"

#include <string>
#include <utility>

#include "detail/file_io.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace black_start_manager::detail {

ProcessLock::ProcessLock(ProcessLock&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

ProcessLock& ProcessLock::operator=(ProcessLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

ProcessLock::~ProcessLock() { release(); }

bool ProcessLock::held() const noexcept { return handle_ != nullptr; }

void ProcessLock::release() noexcept {
#if defined(_WIN32)
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (handle_ != nullptr) {
    const int descriptor = *static_cast<int*>(handle_);
    ::close(descriptor);
    delete static_cast<int*>(handle_);
    handle_ = nullptr;
  }
#endif
}

Result<ProcessLock> ProcessLock::acquire(const std::string& path) {
#if defined(_WIN32)
  BSM_TRY_ASSIGN(native, to_native_path(path));
  // Share mode zero: the second opener fails with a sharing violation, and the kernel
  // releases the claim when this process dies for any reason.
  HANDLE handle = ::CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
      return Error(ErrorCode::StoreLocked,
                   "another process holds the exclusive store lock");
    }
    return Error(ErrorCode::IoFailure,
                 "cannot open the store lock file (windows error " +
                     std::to_string(static_cast<unsigned long long>(error)) + ")");
  }
  ProcessLock lock;
  lock.handle_ = handle;
  return lock;
#else
  BSM_TRY_ASSIGN(native, to_native_path(path));
  const int descriptor = ::open(native.c_str(), O_CREAT | O_RDWR, 0600);
  if (descriptor < 0) {
    return Error(ErrorCode::IoFailure, "cannot open the store lock file");
  }
  struct flock claim {};
  claim.l_type = F_WRLCK;
  claim.l_whence = SEEK_SET;
  claim.l_start = 0;
  claim.l_len = 0;
  if (::fcntl(descriptor, F_SETLK, &claim) != 0) {
    ::close(descriptor);
    if (errno == EACCES || errno == EAGAIN) {
      return Error(ErrorCode::StoreLocked,
                   "another process holds the exclusive store lock");
    }
    return Error(ErrorCode::IoFailure, "cannot claim the store lock file");
  }
  ProcessLock lock;
  lock.handle_ = new int(descriptor);
  return lock;
#endif
}

}  // namespace black_start_manager::detail
