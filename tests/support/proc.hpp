#pragma once

// Real independent-operating-system-process control for the crash and
// multiprocess tests.
//
// Design
// ------
// The crash-injection, lock-contention and controller hand-off tests need a
// genuine second process: an in-process fault injector cannot prove that a
// durable store survives the death of the process that owned it. This header
// exposes the smallest surface that makes such a test possible - spawn, read the
// child's output through a blocking anonymous pipe, terminate, wait - and
// nothing else.
//
// Every operation blocks in the kernel until it has its answer. There are no
// timeouts, no watchdog threads and no polling or sleeping loops anywhere in the
// implementation, so a wedged child shows up as a wedged test instead of a flaky
// one. The ChildProcess value owns every handle; nothing is hidden in global
// state, and no exception ever crosses this API: failures are reported as
// black_start_manager::Error.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "black_start_manager/error.hpp"  // Result<T>, Unit, BSM_TRY_ASSIGN

namespace bsm_test {

// A running child process with its stdout connected to a blocking pipe.
struct ChildProcess {
  void* process = nullptr;   // Windows: HANDLE. POSIX: unused (nullptr).
  void* pipe_read = nullptr; // Windows: HANDLE. POSIX: not used, see fd field.
  int pipe_fd = -1;          // POSIX: read end of the stdout pipe.
  std::int64_t pid = 0;
  bool valid = false;
};

// Starts a child process. args excludes the executable name. environment_extra
// entries are added to the inherited environment (name, value). stdout and stderr
// of the child are both connected to the returned pipe. working_directory may be
// empty, meaning inherit the parent's.
[[nodiscard]] black_start_manager::Result<ChildProcess> spawn_child(
    const std::string& executable,
    const std::vector<std::string>& arguments,
    const std::string& working_directory,
    const std::vector<std::pair<std::string, std::string>>& environment_extra);

// Blocks until one line (terminated by '\n') has been read, or the pipe reaches
// end of file. Returns the line without the terminator. A return value with
// ok()==true and an empty string means end of file. A carriage return written by
// the Windows CRT in text mode is removed as well, so a line reads identically on
// every platform; bytes without a terminating newline are returned as they were
// read when the pipe reaches end of file.
[[nodiscard]] black_start_manager::Result<std::string> read_line(ChildProcess& child);

// Blocks until the pipe reaches end of file and returns everything read.
[[nodiscard]] black_start_manager::Result<std::string> read_until_eof(ChildProcess& child);

// True while the child has not exited. Never blocks.
[[nodiscard]] bool child_running(ChildProcess& child);

// Terminates the child immediately (Windows TerminateProcess, POSIX SIGKILL) and
// releases nothing else. Safe to call on an already exited child.
void terminate_child(ChildProcess& child);

// Waits for the child to exit and returns its exit code. Windows: the process exit
// code as an int. POSIX: 128 + signal number when the child was signalled, else the
// exit status. Returns -1 when the child handle is unusable.
[[nodiscard]] int wait_child(ChildProcess& child);

// Closes the pipe and process handles without waiting.
void close_child(ChildProcess& child);

}  // namespace bsm_test
