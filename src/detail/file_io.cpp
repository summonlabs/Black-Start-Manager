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

#include "detail/file_io.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace black_start_manager::detail {
namespace {

[[nodiscard]] std::string describe_last_error() {
#if defined(_WIN32)
  return "windows error " +
         std::to_string(static_cast<unsigned long long>(::GetLastError()));
#else
  return std::string("errno ") + std::to_string(errno);
#endif
}

}  // namespace

#if defined(_WIN32)
Result<std::filesystem::path> to_native_path(const std::string& utf8) {
  if (utf8.empty()) {
    return Error(ErrorCode::InvalidArgument, "path is empty");
  }
  const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) {
    return Error(ErrorCode::InvalidUtf8, "path is not valid UTF-8");
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                            static_cast<int>(utf8.size()), wide.data(),
                                            needed);
  if (written != needed) {
    return Error(ErrorCode::InvalidUtf8, "path is not valid UTF-8");
  }
  // The extended-length prefix requires backslash separators; a forward slash reaching
  // CreateFileW with that prefix fails with ERROR_INVALID_NAME.
  for (wchar_t& character : wide) {
    if (character == L'/') {
      character = L'\\';
    }
  }
  const bool absolute =
      (wide.size() >= 2 && wide[1] == L':') || (wide.size() >= 2 && wide[0] == L'\\' &&
                                                wide[1] == L'\\');
  if (absolute && wide.compare(0, 4, L"\\\\?\\") != 0) {
    if (wide[0] == L'\\') {
      wide = L"\\\\?\\UNC\\" + wide.substr(2);
    } else {
      wide = L"\\\\?\\" + wide;
    }
  }
  return std::filesystem::path(wide);
}
#else
Result<std::filesystem::path> to_native_path(const std::string& utf8) {
  if (utf8.empty()) {
    return Error(ErrorCode::InvalidArgument, "path is empty");
  }
  return std::filesystem::path(utf8);
}
#endif

FileHandle::FileHandle(FileHandle&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

FileHandle::~FileHandle() { close(); }

bool FileHandle::valid() const noexcept {
#if defined(_WIN32)
  return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
#else
  return handle_ != nullptr;
#endif
}

void FileHandle::close() noexcept {
#if defined(_WIN32)
  if (valid()) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
  }
  handle_ = nullptr;
#else
  if (handle_ != nullptr) {
    const int descriptor = *static_cast<int*>(handle_);
    ::close(descriptor);
    delete static_cast<int*>(handle_);
    handle_ = nullptr;
  }
#endif
}

Result<FileHandle> FileHandle::create(const std::string& path) {
  BSM_TRY_ASSIGN(native, to_native_path(path));
#if defined(_WIN32)
  HANDLE handle = ::CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::IoFailure,
                 "cannot create '" + path + "' (" + describe_last_error() + ")");
  }
  FileHandle result;
  result.handle_ = handle;
  return result;
#else
  const int descriptor =
      ::open(native.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (descriptor < 0) {
    return Error(ErrorCode::IoFailure,
                 "cannot create '" + path + "' (" + describe_last_error() + ")");
  }
  FileHandle result;
  result.handle_ = new int(descriptor);
  return result;
#endif
}

Result<FileHandle> FileHandle::open_read(const std::string& path) {
  BSM_TRY_ASSIGN(native, to_native_path(path));
#if defined(_WIN32)
  HANDLE handle = ::CreateFileW(native.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::IoFailure,
                 "cannot open '" + path + "' for reading (" + describe_last_error() + ")");
  }
  FileHandle result;
  result.handle_ = handle;
  return result;
#else
  const int descriptor = ::open(native.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return Error(ErrorCode::IoFailure,
                 "cannot open '" + path + "' for reading (" + describe_last_error() + ")");
  }
  FileHandle result;
  result.handle_ = new int(descriptor);
  return result;
#endif
}

Result<FileHandle> FileHandle::open_append(const std::string& path) {
  BSM_TRY_ASSIGN(native, to_native_path(path));
#if defined(_WIN32)
  HANDLE handle = ::CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::IoFailure,
                 "cannot open '" + path + "' for appending (" + describe_last_error() + ")");
  }
  LARGE_INTEGER distance{};
  distance.QuadPart = 0;
  if (::SetFilePointerEx(handle, distance, nullptr, FILE_END) == 0) {
    ::CloseHandle(handle);
    return Error(ErrorCode::IoFailure,
                 "cannot position '" + path + "' at its end (" + describe_last_error() +
                     ")");
  }
  FileHandle result;
  result.handle_ = handle;
  return result;
#else
  const int descriptor = ::open(native.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (descriptor < 0) {
    return Error(ErrorCode::IoFailure,
                 "cannot open '" + path + "' for appending (" + describe_last_error() + ")");
  }
  FileHandle result;
  result.handle_ = new int(descriptor);
  return result;
#endif
}

Result<Unit> FileHandle::write_all(std::span<const std::uint8_t> bytes) {
  if (!valid()) {
    return Error(ErrorCode::StoreNotOpen, "file handle is not open");
  }
  std::size_t written = 0;
  while (written < bytes.size()) {
#if defined(_WIN32)
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size() - written, 1u << 20));
    DWORD produced = 0;
    if (::WriteFile(static_cast<HANDLE>(handle_), bytes.data() + written, chunk, &produced,
                    nullptr) == 0) {
      return Error(ErrorCode::IoFailure,
                   "write failed (" + describe_last_error() + ")");
    }
    if (produced == 0) {
      return Error(ErrorCode::IoFailure, "write produced no bytes");
    }
    written += produced;
#else
    const std::size_t chunk =
        std::min<std::size_t>(bytes.size() - written, 1u << 20);
    const ssize_t produced =
        ::write(*static_cast<int*>(handle_), bytes.data() + written, chunk);
    if (produced < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Error(ErrorCode::IoFailure, "write failed (" + describe_last_error() + ")");
    }
    written += static_cast<std::size_t>(produced);
#endif
  }
  return Unit{};
}

Result<Unit> FileHandle::flush_durable() {
  if (!valid()) {
    return Error(ErrorCode::StoreNotOpen, "file handle is not open");
  }
#if defined(_WIN32)
  if (::FlushFileBuffers(static_cast<HANDLE>(handle_)) == 0) {
    return Error(ErrorCode::IoFailure,
                 "flush failed (" + describe_last_error() + ")");
  }
#else
  if (::fsync(*static_cast<int*>(handle_)) != 0) {
    return Error(ErrorCode::IoFailure, "flush failed (" + describe_last_error() + ")");
  }
#endif
  return Unit{};
}

Result<std::uint64_t> FileHandle::size() const {
  if (!valid()) {
    return Error(ErrorCode::StoreNotOpen, "file handle is not open");
  }
#if defined(_WIN32)
  LARGE_INTEGER length{};
  if (::GetFileSizeEx(static_cast<HANDLE>(handle_), &length) == 0) {
    return Error(ErrorCode::IoFailure,
                 "cannot read the file size (" + describe_last_error() + ")");
  }
  return static_cast<std::uint64_t>(length.QuadPart);
#else
  struct stat information {};
  if (::fstat(*static_cast<int*>(handle_), &information) != 0) {
    return Error(ErrorCode::IoFailure, "cannot read the file size");
  }
  return static_cast<std::uint64_t>(information.st_size);
#endif
}

Result<std::vector<std::uint8_t>> FileHandle::read_exact(std::uint64_t offset,
                                                         std::size_t length) const {
  if (!valid()) {
    return Error(ErrorCode::StoreNotOpen, "file handle is not open");
  }
  std::vector<std::uint8_t> buffer(length);
  std::size_t read = 0;
  while (read < length) {
#if defined(_WIN32)
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset + read);
    if (::SetFilePointerEx(static_cast<HANDLE>(handle_), position, nullptr, FILE_BEGIN) == 0) {
      return Error(ErrorCode::IoFailure,
                   "cannot seek in the file (" + describe_last_error() + ")");
    }
    const DWORD chunk =
        static_cast<DWORD>(std::min<std::size_t>(length - read, 1u << 20));
    DWORD produced = 0;
    if (::ReadFile(static_cast<HANDLE>(handle_), buffer.data() + read, chunk, &produced,
                   nullptr) == 0) {
      return Error(ErrorCode::IoFailure, "read failed (" + describe_last_error() + ")");
    }
    if (produced == 0) {
      return Error(ErrorCode::TruncatedInput, "file ended before the requested range");
    }
    read += produced;
#else
    const std::size_t chunk = std::min<std::size_t>(length - read, 1u << 20);
    const ssize_t produced = ::pread(*static_cast<int*>(handle_), buffer.data() + read, chunk,
                                     static_cast<off_t>(offset + read));
    if (produced < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Error(ErrorCode::IoFailure, "read failed (" + describe_last_error() + ")");
    }
    if (produced == 0) {
      return Error(ErrorCode::TruncatedInput, "file ended before the requested range");
    }
    read += static_cast<std::size_t>(produced);
#endif
  }
  return buffer;
}

Result<std::vector<std::uint8_t>> FileHandle::read_all(std::size_t max_bytes) const {
  BSM_TRY_ASSIGN(length, size());
  if (length > max_bytes) {
    return Error(ErrorCode::LimitExceeded, "file exceeds the supported size for reading");
  }
  if (length == 0) {
    return std::vector<std::uint8_t>{};
  }
  return read_exact(0, static_cast<std::size_t>(length));
}

Result<bool> file_exists(const std::string& path) {
  BSM_TRY_ASSIGN(native, to_native_path(path));
  std::error_code error;
  const bool exists = std::filesystem::exists(native, error);
  if (error) {
    return Error(ErrorCode::IoFailure, "cannot inspect '" + path + "'");
  }
  return exists;
}

Result<std::uint64_t> file_size(const std::string& path) {
  BSM_TRY_ASSIGN(handle, FileHandle::open_read(path));
  return handle.size();
}

Result<Unit> create_directories(const std::string& path) {
  BSM_TRY_ASSIGN(native, to_native_path(path));
  std::error_code error;
  std::filesystem::create_directories(native, error);
  if (error && !std::filesystem::is_directory(native)) {
    return Error(ErrorCode::IoFailure, "cannot create the directory '" + path + "'");
  }
  return Unit{};
}

Result<std::vector<std::string>> list_directory(const std::string& path) {
  BSM_TRY_ASSIGN(native, to_native_path(path));
  std::error_code error;
  std::filesystem::directory_iterator iterator(native, error);
  if (error) {
    return Error(ErrorCode::IoFailure, "cannot list the directory '" + path + "'");
  }
  std::vector<std::string> names;
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const std::filesystem::directory_entry& entry = *iterator;
    const std::string name = entry.path().filename().string();
    names.push_back(name);
    iterator.increment(error);
    if (error) {
      return Error(ErrorCode::IoFailure, "cannot list the directory '" + path + "'");
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

Result<Unit> remove_file(const std::string& path) {
  BSM_TRY_ASSIGN(native, to_native_path(path));
  std::error_code error;
  if (!std::filesystem::remove(native, error) && error) {
    return Error(ErrorCode::IoFailure, "cannot remove '" + path + "'");
  }
  return Unit{};
}

Result<std::vector<std::uint8_t>> read_file(const std::string& path,
                                            std::size_t max_bytes) {
  BSM_TRY_ASSIGN(handle, FileHandle::open_read(path));
  return handle.read_all(max_bytes);
}

Result<Unit> write_file_durable(const std::string& path,
                                std::span<const std::uint8_t> bytes) {
  BSM_TRY_ASSIGN(handle, FileHandle::create(path));
  BSM_RETURN_IF_ERROR(handle.write_all(bytes));
  return handle.flush_durable();
}

Result<Unit> replace_file(const std::string& source, const std::string& destination) {
  BSM_TRY_ASSIGN(source_native, to_native_path(source));
  BSM_TRY_ASSIGN(destination_native, to_native_path(destination));
#if defined(_WIN32)
  if (::MoveFileExW(source_native.c_str(), destination_native.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return Error(ErrorCode::IoFailure,
                 "cannot publish '" + destination + "' (" + describe_last_error() + ")");
  }
#else
  if (::rename(source_native.c_str(), destination_native.c_str()) != 0) {
    return Error(ErrorCode::IoFailure,
                 "cannot publish '" + destination + "' (" + describe_last_error() + ")");
  }
  // Flush the containing directory so the rename itself is durable.
  const std::filesystem::path parent = destination_native.parent_path();
  const int directory =
      ::open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY);
  if (directory >= 0) {
    ::fsync(directory);
    ::close(directory);
  }
#endif
  return Unit{};
}

}  // namespace black_start_manager::detail
