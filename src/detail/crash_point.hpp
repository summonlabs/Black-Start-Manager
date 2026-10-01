#pragma once

// Crash injection.
//
// A crash point is a named boundary in an operation that must be recoverable. When the
// environment variable BSM_CRASH_AT names a point (or is "*"), the process terminates
// immediately at that boundary without unwinding, without running destructors, and
// without flushing any buffer this code did not already flush. That is the only honest
// way to prove what survives a power loss or a SIGKILL-style abort.
//
// The points are inert unless the variable is set, so a production run pays one
// environment lookup per boundary.

namespace black_start_manager::detail {

inline constexpr int kCrashExitCode = 97;

void crash_point(const char* name) noexcept;

}  // namespace black_start_manager::detail
