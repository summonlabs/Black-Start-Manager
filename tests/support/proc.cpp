// Implementation of the independent-process test harness (see proc.hpp).
//
// Design
// ------
//  * One anonymous pipe carries the child's stdout and stderr. The write end is
//    inheritable and the read end is not, and the parent closes its own copy of
//    the write end immediately after the spawn, so the pipe reports end of file
//    exactly when the last writer (the child, or a grandchild that inherited the
//    handle) has gone away.
//  * No timeouts, no watchdog threads, no polling or sleeping loops: read_line()
//    blocks in ReadFile()/read() until a newline or end of file arrives,
//    child_running() is a single non-blocking kernel query, and wait_child()
//    blocks until the child has actually exited.
//  * No hidden state. Every handle lives in the ChildProcess value the caller
//    owns, so a test that dies cannot leave a process or a pipe behind in a
//    global.
//  * No exception crosses the API boundary: each entry point funnels through
//    Guarded(), which turns an escaping exception into InternalError.
//
// Windows notes
// -------------
//  * CreateProcessW receives the executable through lpApplicationName and the
//    same path quoted at the head of the command line, so a path with spaces
//    works and the child's argv[0] survives intact. Every argument is quoted
//    with the standard backslash rules the CRT and CommandLineToArgvW use.
//  * The environment block is built from GetEnvironmentStringsW() plus the
//    caller's additions and sorted case-insensitively by name, which is the
//    order CreateProcessW documents for lpEnvironment.
//
// POSIX notes
// -----------
//  * pipe()/fork()/execve(); the child closes the read end, dup2()s the write end
//    onto stdout and stderr, and reports a failed exec by writing the reason to
//    the pipe (which is its stderr) before _exit(127).

#include "proc.hpp"

#include <cstddef>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwchar>

#else  // POSIX

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#endif

namespace bsm_test {
namespace {

using black_start_manager::Error;
using black_start_manager::ErrorCode;
using black_start_manager::Result;

// Runs one implementation function and converts any exception that escapes it
// into an InternalError. The public API must not throw: a test that is verifying
// crash recovery cannot also be expected to catch std::bad_alloc. Should the
// conversion itself run out of memory the process terminates, which is the same
// policy the library's own Result preconditions follow.
template <class T, class Function>
[[nodiscard]] Result<T> Guarded(const char* operation, Function&& function) noexcept {
  try {
    return function();
  } catch (const std::exception& error) {
    return Error(ErrorCode::InternalError,
                 std::string(operation) + ": escaped exception: " + error.what());
  } catch (...) {
    return Error(ErrorCode::InternalError,
                 std::string(operation) + ": escaped a non-standard exception");
  }
}

#if defined(_WIN32)

[[nodiscard]] HANDLE AsHandle(void* value) noexcept { return static_cast<HANDLE>(value); }

[[nodiscard]] bool IsUsableHandle(HANDLE handle) noexcept {
  return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

void CloseHandleSlot(void*& slot) noexcept {
  const HANDLE handle = AsHandle(slot);
  if (IsUsableHandle(handle)) {
    ::CloseHandle(handle);
  }
  slot = nullptr;
}

[[nodiscard]] std::string WideToUtf8(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return std::string();
  }
  const int length = static_cast<int>(text.size());
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return std::string();
  }
  std::string out(static_cast<std::size_t>(size), '\0');
  const int written = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), length, out.data(), size, nullptr, nullptr);
  if (written != size) {
    return std::string();
  }
  return out;
}

[[nodiscard]] bool Utf8ToWide(const std::string& text, std::wstring& out) {
  out.clear();
  if (text.empty()) {
    return true;
  }
  if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  const int length = static_cast<int>(text.size());
  const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
  if (size <= 0) {
    return false;
  }
  out.resize(static_cast<std::size_t>(size));
  const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, out.data(), size);
  return written == size;
}

// Human readable text for a Win32 error code, so an Error message says what the
// operating system actually refused instead of only a number.
[[nodiscard]] std::string DescribeWindowsError(DWORD code) {
  wchar_t buffer[512] = {};
  const DWORD length = ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, buffer,
                                        static_cast<DWORD>(std::size(buffer)), nullptr);
  std::wstring text;
  if (length > 0) {
    text.assign(buffer, static_cast<std::size_t>(length));
  }
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
    text.pop_back();
  }
  const std::string description = WideToUtf8(text);
  if (description.empty()) {
    return "error " + std::to_string(code);
  }
  return description + " (error " + std::to_string(code) + ")";
}

// Quotes one command line argument exactly the way the Visual C++ runtime and
// CommandLineToArgvW take it apart again:
//   * an argument without space, tab, newline or quote is passed through as is;
//   * a run of N backslashes in front of a quote becomes 2N+1 backslashes and
//     the quote is escaped;
//   * a run of N backslashes at the end of the argument is doubled so it cannot
//     escape the closing quote.
[[nodiscard]] std::wstring QuoteArgument(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring quoted;
  quoted.push_back(L'"');
  std::size_t index = 0;
  while (index < argument.size()) {
    std::size_t backslashes = 0;
    while (index < argument.size() && argument[index] == L'\\') {
      ++index;
      ++backslashes;
    }
    if (index == argument.size()) {
      quoted.append(backslashes * 2, L'\\');
      break;
    }
    if (argument[index] == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
    } else {
      quoted.append(backslashes, L'\\');
    }
    quoted.push_back(argument[index]);
    ++index;
  }
  quoted.push_back(L'"');
  return quoted;
}

// A single NAME=VALUE entry of the environment block being assembled.
struct EnvironmentEntry {
  std::wstring name;
  std::wstring text;
};

// CreateProcessW requires the environment block to be sorted case-insensitively
// by variable name. CompareStringOrdinal(..., TRUE) is the case-insensitive
// ordinal comparison Windows itself uses, so the resulting order is the one the
// CRT and cmd.exe expect and does not depend on the current locale. A failed
// comparison (which cannot happen for valid strings) falls back to the ordinal
// order so the comparator stays a strict weak ordering.
[[nodiscard]] bool EnvironmentEntryLess(const EnvironmentEntry& left, const EnvironmentEntry& right) {
  const int order = ::CompareStringOrdinal(left.name.c_str(), -1, right.name.c_str(), -1, TRUE);
  if (order == 0) {
    return left.name < right.name;
  }
  return order == CSTR_LESS_THAN;
}

[[nodiscard]] bool SameEnvironmentName(const std::wstring& left, const std::wstring& right) {
  return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// Builds the Unicode environment block: the parent's variables, with the
// caller's additions replacing same-named entries, sorted case-insensitively.
[[nodiscard]] Result<std::wstring> BuildEnvironmentBlock(
    const std::vector<std::pair<std::string, std::string>>& environment_extra) {
  const LPWCH raw = ::GetEnvironmentStringsW();
  if (raw == nullptr) {
    return Error(ErrorCode::IoFailure,
                 "spawn_child: GetEnvironmentStringsW failed: " + DescribeWindowsError(::GetLastError()));
  }

  std::vector<EnvironmentEntry> entries;
  for (const wchar_t* cursor = raw; *cursor != L'\0'; cursor += std::wcslen(cursor) + 1) {
    const std::wstring text(cursor);
    if (text.front() == L'=') {
      // "=C:=C:\dir" entries describe per-drive current directories rather than
      // real variables. They may not be sorted together with the rest, and the
      // child starts in a directory named by lpCurrentDirectory anyway, so they
      // are dropped deliberately.
      continue;
    }
    const std::size_t separator = text.find(L'=');
    if (separator == std::wstring::npos || separator == 0) {
      continue;  // Not a NAME=VALUE entry: nothing the child could ever look up.
    }
    entries.push_back(EnvironmentEntry{text.substr(0, separator), text});
  }
  ::FreeEnvironmentStringsW(raw);

  for (const std::pair<std::string, std::string>& extra : environment_extra) {
    if (extra.first.empty()) {
      return Error(ErrorCode::InvalidArgument,
                   "spawn_child: environment_extra contains an entry with an empty name");
    }
    std::wstring name;
    std::wstring value;
    if (!Utf8ToWide(extra.first, name) || !Utf8ToWide(extra.second, value)) {
      return Error(ErrorCode::InvalidArgument,
                   "spawn_child: environment entry '" + extra.first + "' is not valid UTF-8");
    }
    const std::wstring text = name + L"=" + value;
    bool replaced = false;
    for (EnvironmentEntry& entry : entries) {
      if (SameEnvironmentName(entry.name, name)) {
        entry = EnvironmentEntry{name, text};
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      entries.push_back(EnvironmentEntry{name, text});
    }
  }

  std::sort(entries.begin(), entries.end(), EnvironmentEntryLess);

  std::wstring block;
  for (const EnvironmentEntry& entry : entries) {
    block.append(entry.text);
    block.push_back(L'\0');
  }
  block.push_back(L'\0');  // The block ends with a second, empty entry.
  return block;
}

[[nodiscard]] Result<ChildProcess> SpawnChild(
    const std::string& executable,
    const std::vector<std::string>& arguments,
    const std::string& working_directory,
    const std::vector<std::pair<std::string, std::string>>& environment_extra) {
  if (executable.empty()) {
    return Error(ErrorCode::InvalidArgument, "spawn_child: the executable path is empty");
  }

  std::wstring wide_executable;
  if (!Utf8ToWide(executable, wide_executable)) {
    return Error(ErrorCode::InvalidArgument,
                 "spawn_child: the executable path '" + executable + "' is not valid UTF-8");
  }
  std::wstring wide_working_directory;
  if (!working_directory.empty() && !Utf8ToWide(working_directory, wide_working_directory)) {
    return Error(ErrorCode::InvalidArgument,
                 "spawn_child: the working directory '" + working_directory + "' is not valid UTF-8");
  }
  std::vector<std::wstring> wide_arguments;
  wide_arguments.reserve(arguments.size());
  for (const std::string& argument : arguments) {
    std::wstring wide;
    if (!Utf8ToWide(argument, wide)) {
      return Error(ErrorCode::InvalidArgument,
                   "spawn_child: the argument '" + argument + "' is not valid UTF-8");
    }
    wide_arguments.push_back(std::move(wide));
  }

  BSM_TRY_ASSIGN(environment_block, BuildEnvironmentBlock(environment_extra));

  SECURITY_ATTRIBUTES attributes = {};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  attributes.lpSecurityDescriptor = nullptr;

  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!::CreatePipe(&read_end, &write_end, &attributes, 0)) {
    return Error(ErrorCode::IoFailure,
                 "spawn_child: CreatePipe failed: " + DescribeWindowsError(::GetLastError()));
  }
  // The child must not inherit the read end. Otherwise a grandchild could hold
  // the pipe open and the parent would never observe end of file.
  if (!::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)) {
    const std::string description = DescribeWindowsError(::GetLastError());
    ::CloseHandle(read_end);
    ::CloseHandle(write_end);
    return Error(ErrorCode::IoFailure,
                 "spawn_child: SetHandleInformation on the pipe read end failed: " + description);
  }

  // Windows paths cannot contain a double quote, so wrapping the application path
  // in quotes preserves it exactly, spaces included. Arguments use the full
  // backslash quoting rules.
  std::wstring command_line = L"\"";
  command_line.append(wide_executable);
  command_line.push_back(L'"');
  for (const std::wstring& argument : wide_arguments) {
    command_line.push_back(L' ');
    command_line.append(QuoteArgument(argument));
  }

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  // stdin is deliberately left as the parent's own handle: this harness only
  // needs to observe a child, never to talk to one.
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  if (startup.hStdInput == INVALID_HANDLE_VALUE) {
    startup.hStdInput = nullptr;
  }
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;

  // The error mode is inherited by the child at creation. A critical-error or
  // GP-fault dialog in an unattended crash test would hang until someone clicks
  // it, so both are suppressed for the instant of the spawn and the parent's own
  // mode is restored immediately afterwards.
  const UINT previous_error_mode =
      ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

  PROCESS_INFORMATION process_info = {};
  const BOOL created = ::CreateProcessW(wide_executable.c_str(),
                                        command_line.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,  // bInheritHandles: the pipe must cross the boundary.
                                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                        environment_block.data(),
                                        wide_working_directory.empty() ? nullptr : wide_working_directory.c_str(),
                                        &startup,
                                        &process_info);
  const DWORD create_error = created ? ERROR_SUCCESS : ::GetLastError();
  ::SetErrorMode(previous_error_mode);

  // The parent's copy of the write end must go away in every case: while any copy
  // is open the read end cannot report end of file.
  ::CloseHandle(write_end);
  if (!created) {
    ::CloseHandle(read_end);
    return Error(ErrorCode::IoFailure,
                 "spawn_child: CreateProcessW failed for '" + executable + "': " +
                     DescribeWindowsError(create_error));
  }
  ::CloseHandle(process_info.hThread);

  ChildProcess child;
  child.process = static_cast<void*>(process_info.hProcess);
  child.pipe_read = static_cast<void*>(read_end);
  child.pipe_fd = -1;
  child.pid = static_cast<std::int64_t>(process_info.dwProcessId);
  child.valid = true;
  return child;
}

// End of file is reported by a broken pipe, but a zero-length read or the
// handle-EOF status mean the same thing and are treated alike.
[[nodiscard]] bool IsWindowsEndOfFile(DWORD code) noexcept {
  return code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF || code == ERROR_PIPE_NOT_CONNECTED ||
         code == ERROR_NO_DATA;
}

[[nodiscard]] Result<std::string> ReadLine(ChildProcess& child) {
  const HANDLE pipe = AsHandle(child.pipe_read);
  if (!child.valid || !IsUsableHandle(pipe)) {
    return Error(ErrorCode::InvalidArgument, "read_line: the child has no open stdout pipe");
  }
  // The ChildProcess value carries no read buffer, so the line is collected one
  // byte at a time straight from the blocking pipe. That keeps the API free of
  // hidden state and cannot over-read past the newline, which matters because the
  // caller may switch to read_until_eof() between two calls.
  std::string line;
  for (;;) {
    char byte = '\0';
    DWORD read = 0;
    if (::ReadFile(pipe, &byte, 1, &read, nullptr)) {
      if (read == 0) {
        break;  // Defensive: a synchronous pipe normally reports end of file as an error.
      }
      if (byte == '\n') {
        // The Windows CRT writes "\r\n" when stdout is a text-mode pipe. The
        // carriage return belongs to the terminator, not to the line.
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        return line;
      }
      line.push_back(byte);
      continue;
    }
    const DWORD code = ::GetLastError();
    if (IsWindowsEndOfFile(code)) {
      break;
    }
    return Error(ErrorCode::IoFailure, "read_line: ReadFile failed: " + DescribeWindowsError(code));
  }
  return line;  // End of file; a last line without a terminator is returned as read.
}

[[nodiscard]] Result<std::string> ReadUntilEof(ChildProcess& child) {
  const HANDLE pipe = AsHandle(child.pipe_read);
  if (!child.valid || !IsUsableHandle(pipe)) {
    return Error(ErrorCode::InvalidArgument, "read_until_eof: the child has no open stdout pipe");
  }
  char buffer[4096];
  std::string text;
  for (;;) {
    DWORD read = 0;
    if (::ReadFile(pipe, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr)) {
      if (read == 0) {
        break;
      }
      text.append(buffer, static_cast<std::size_t>(read));
      continue;
    }
    const DWORD code = ::GetLastError();
    if (IsWindowsEndOfFile(code)) {
      break;
    }
    return Error(ErrorCode::IoFailure, "read_until_eof: ReadFile failed: " + DescribeWindowsError(code));
  }
  return text;
}

[[nodiscard]] bool ChildRunning(ChildProcess& child) {
  const HANDLE process = AsHandle(child.process);
  if (!child.valid || !IsUsableHandle(process)) {
    return false;
  }
  // A zero timeout is not a timeout: the call only asks the kernel whether the
  // process object is still unsignalled.
  return ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

void TerminateChild(ChildProcess& child) {
  const HANDLE process = AsHandle(child.process);
  if (!child.valid || !IsUsableHandle(process)) {
    return;
  }
  if (::WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
    // A failure here means the child exited between the check and the call, which
    // is exactly the "already exited" case the API promises to tolerate.
    (void)::TerminateProcess(process, 1);
  }
}

[[nodiscard]] int WaitChild(ChildProcess& child) {
  const HANDLE process = AsHandle(child.process);
  if (!IsUsableHandle(process)) {
    return -1;
  }
  if (::WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) {
    return -1;
  }
  DWORD exit_code = 0;
  if (!::GetExitCodeProcess(process, &exit_code)) {
    return -1;
  }
  // The exit code is a DWORD; the API promises an int, and a value above
  // INT_MAX is not something a test child produces.
  return static_cast<int>(exit_code);
}

void CloseChild(ChildProcess& child) {
  CloseHandleSlot(child.pipe_read);
  CloseHandleSlot(child.process);
  child.pipe_fd = -1;
  child.valid = false;
}

#else  // POSIX

[[nodiscard]] std::string DescribeErrno(int code) {
  const char* text = std::strerror(code);
  const std::string description = (text != nullptr) ? text : "unknown error";
  return description + " (errno " + std::to_string(code) + ")";
}

// Writes the reason for a failed exec or chdir to the pipe and leaves the child.
// This runs after fork(), before exec: the child is expected to be effectively
// single-threaded in a test harness, and staying silent would hide why a spawn
// failed.
[[noreturn]] void FailInChild(int fd, const std::string& message) {
  const std::string line = message + "\n";
  ssize_t written = 0;
  while (static_cast<std::size_t>(written) < line.size()) {
    const ssize_t step = ::write(fd, line.data() + written, line.size() - static_cast<std::size_t>(written));
    if (step <= 0) {
      break;
    }
    written += step;
  }
  ::_exit(127);
}

[[nodiscard]] Result<ChildProcess> SpawnChild(
    const std::string& executable,
    const std::vector<std::string>& arguments,
    const std::string& working_directory,
    const std::vector<std::pair<std::string, std::string>>& environment_extra) {
  if (executable.empty()) {
    return Error(ErrorCode::InvalidArgument, "spawn_child: the executable path is empty");
  }

  // argv[0] is the executable itself; the caller supplies everything after it.
  std::vector<std::string> argument_storage;
  argument_storage.reserve(arguments.size() + 1);
  argument_storage.push_back(executable);
  for (const std::string& argument : arguments) {
    argument_storage.push_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(argument_storage.size() + 1);
  for (std::string& argument : argument_storage) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  std::vector<std::string> environment_storage;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    environment_storage.emplace_back(*entry);
  }
  for (const std::pair<std::string, std::string>& extra : environment_extra) {
    if (extra.first.empty()) {
      return Error(ErrorCode::InvalidArgument,
                   "spawn_child: environment_extra contains an entry with an empty name");
    }
    if (extra.first.find('=') != std::string::npos) {
      return Error(ErrorCode::InvalidArgument,
                   "spawn_child: the environment name '" + extra.first + "' contains '='");
    }
    const std::string text = extra.first + "=" + extra.second;
    bool replaced = false;
    for (std::string& entry : environment_storage) {
      const std::size_t separator = entry.find('=');
      if (separator == extra.first.size() && entry.compare(0, separator, extra.first) == 0) {
        entry = text;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      environment_storage.push_back(text);
    }
  }
  std::vector<char*> envp;
  envp.reserve(environment_storage.size() + 1);
  for (std::string& entry : environment_storage) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);

  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    return Error(ErrorCode::IoFailure, "spawn_child: pipe failed: " + DescribeErrno(errno));
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    const std::string description = DescribeErrno(errno);
    ::close(fds[0]);
    ::close(fds[1]);
    return Error(ErrorCode::IoFailure, "spawn_child: fork failed: " + description);
  }

  if (pid == 0) {
    // Child: the only ways out of this block are execve() or _exit().
    ::close(fds[0]);
    if (!working_directory.empty() && ::chdir(working_directory.c_str()) != 0) {
      FailInChild(fds[1], "spawn_child: chdir to '" + working_directory + "' failed: " + DescribeErrno(errno));
    }
    if (::dup2(fds[1], STDOUT_FILENO) < 0 || ::dup2(fds[1], STDERR_FILENO) < 0) {
      FailInChild(fds[1], "spawn_child: dup2 onto stdout/stderr failed: " + DescribeErrno(errno));
    }
    if (fds[1] != STDOUT_FILENO && fds[1] != STDERR_FILENO) {
      ::close(fds[1]);
    }
    ::execve(executable.c_str(), argv.data(), envp.data());
    // execve() replaces the image or returns; there is no third possibility.
    FailInChild(STDERR_FILENO, "spawn_child: execve failed for '" + executable + "': " + DescribeErrno(errno));
  }

  // Parent: drop the write end so end of file is reported when the child exits.
  ::close(fds[1]);

  ChildProcess child;
  child.process = nullptr;
  child.pipe_read = nullptr;
  child.pipe_fd = fds[0];
  child.pid = static_cast<std::int64_t>(pid);
  child.valid = true;
  return child;
}

[[nodiscard]] Result<std::string> ReadLine(ChildProcess& child) {
  if (!child.valid || child.pipe_fd < 0) {
    return Error(ErrorCode::InvalidArgument, "read_line: the child has no open stdout pipe");
  }
  std::string line;
  for (;;) {
    char byte = '\0';
    const ssize_t read = ::read(child.pipe_fd, &byte, 1);
    if (read == 1) {
      if (byte == '\n') {
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        return line;
      }
      line.push_back(byte);
      continue;
    }
    if (read == 0) {
      break;  // End of file.
    }
    if (errno == EINTR) {
      continue;  // A signal interrupted the read; the same read is retried.
    }
    return Error(ErrorCode::IoFailure, "read_line: read failed: " + DescribeErrno(errno));
  }
  return line;
}

[[nodiscard]] Result<std::string> ReadUntilEof(ChildProcess& child) {
  if (!child.valid || child.pipe_fd < 0) {
    return Error(ErrorCode::InvalidArgument, "read_until_eof: the child has no open stdout pipe");
  }
  char buffer[4096];
  std::string text;
  for (;;) {
    const ssize_t read = ::read(child.pipe_fd, buffer, sizeof(buffer));
    if (read > 0) {
      text.append(buffer, static_cast<std::size_t>(read));
      continue;
    }
    if (read == 0) {
      break;  // End of file.
    }
    if (errno == EINTR) {
      continue;
    }
    return Error(ErrorCode::IoFailure, "read_until_eof: read failed: " + DescribeErrno(errno));
  }
  return text;
}

[[nodiscard]] bool ChildRunning(ChildProcess& child) {
  if (!child.valid || child.pid <= 0) {
    return false;
  }
#if defined(WNOWAIT)
  // waitid() with WNOWAIT observes the child without reaping it, so a later
  // wait_child() still reports the real exit status instead of ECHILD.
  siginfo_t info = {};
  if (::waitid(P_PID, static_cast<id_t>(child.pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
    return false;
  }
  return info.si_pid == 0;
#else
  int status = 0;
  return ::waitpid(static_cast<pid_t>(child.pid), &status, WNOHANG) == 0;
#endif
}

void TerminateChild(ChildProcess& child) {
  if (!child.valid || child.pid <= 0) {
    return;
  }
  // A failure means the child is already gone, which the API tolerates.
  (void)::kill(static_cast<pid_t>(child.pid), SIGKILL);
}

[[nodiscard]] int WaitChild(ChildProcess& child) {
  if (child.pid <= 0) {
    return -1;
  }
  for (;;) {
    int status = 0;
    const pid_t reaped = ::waitpid(static_cast<pid_t>(child.pid), &status, 0);
    if (reaped < 0) {
      if (errno == EINTR) {
        continue;  // Blocking retry, not a poll: the child is still being awaited.
      }
      return -1;
    }
    if (reaped != static_cast<pid_t>(child.pid)) {
      return -1;
    }
    if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
      return 128 + WTERMSIG(status);
    }
    return -1;
  }
}

void CloseChild(ChildProcess& child) {
  if (child.pipe_fd >= 0) {
    ::close(child.pipe_fd);
    child.pipe_fd = -1;
  }
  child.process = nullptr;
  child.pipe_read = nullptr;
  child.valid = false;
}

#endif  // _WIN32

}  // namespace

Result<ChildProcess> spawn_child(const std::string& executable,
                                 const std::vector<std::string>& arguments,
                                 const std::string& working_directory,
                                 const std::vector<std::pair<std::string, std::string>>& environment_extra) {
  return Guarded<ChildProcess>("spawn_child", [&]() -> Result<ChildProcess> {
    return SpawnChild(executable, arguments, working_directory, environment_extra);
  });
}

Result<std::string> read_line(ChildProcess& child) {
  return Guarded<std::string>("read_line", [&]() -> Result<std::string> { return ReadLine(child); });
}

Result<std::string> read_until_eof(ChildProcess& child) {
  return Guarded<std::string>("read_until_eof", [&]() -> Result<std::string> { return ReadUntilEof(child); });
}

bool child_running(ChildProcess& child) { return ChildRunning(child); }

void terminate_child(ChildProcess& child) { TerminateChild(child); }

int wait_child(ChildProcess& child) { return WaitChild(child); }

void close_child(ChildProcess& child) { CloseChild(child); }

}  // namespace bsm_test
