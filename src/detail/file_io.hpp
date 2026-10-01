#pragma once

// Durable file primitives.
//
// The store's durability claims rest on three operating system facilities and nothing
// else: an exclusive lock owned by the kernel, a flush that forces bytes to stable
// storage, and an atomic replace of a single file. This header wraps exactly those,
// plus bounded reads, so every claim in the store can be traced to a real syscall.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "black_start_manager/error.hpp"

namespace black_start_manager::detail {

// Converts a validated UTF-8 path into the platform's native form. On Windows the
// conversion is wide and an absolute path longer than the classic limit is expressed
// with the extended-length prefix, so long store roots work without a manifest change.
[[nodiscard]] Result<std::filesystem::path> to_native_path(const std::string& utf8);

class FileHandle {
 public:
  FileHandle() = default;
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  FileHandle(FileHandle&& other) noexcept;
  FileHandle& operator=(FileHandle&& other) noexcept;
  ~FileHandle();

  // Creates or truncates the file.
  [[nodiscard]] static Result<FileHandle> create(const std::string& path);
  // Opens an existing file for reading.
  [[nodiscard]] static Result<FileHandle> open_read(const std::string& path);
  // Opens for appending, creating the file when it does not exist, positioned at the
  // end of the current content.
  [[nodiscard]] static Result<FileHandle> open_append(const std::string& path);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Result<Unit> write_all(std::span<const std::uint8_t> bytes);
  // Forces the written bytes to stable storage. This is the commit point of an append.
  [[nodiscard]] Result<Unit> flush_durable();
  [[nodiscard]] Result<std::uint64_t> size() const;
  [[nodiscard]] Result<std::vector<std::uint8_t>> read_all(std::size_t max_bytes) const;
  [[nodiscard]] Result<std::vector<std::uint8_t>> read_exact(std::uint64_t offset,
                                                             std::size_t length) const;
  void close() noexcept;

 private:
  void* handle_ = nullptr;
};

[[nodiscard]] Result<bool> file_exists(const std::string& path);
[[nodiscard]] Result<std::uint64_t> file_size(const std::string& path);
[[nodiscard]] Result<Unit> create_directories(const std::string& path);
[[nodiscard]] Result<std::vector<std::string>> list_directory(const std::string& path);
[[nodiscard]] Result<Unit> remove_file(const std::string& path);
[[nodiscard]] Result<std::vector<std::uint8_t>> read_file(const std::string& path,
                                                          std::size_t max_bytes);
[[nodiscard]] Result<Unit> write_file_durable(const std::string& path,
                                              std::span<const std::uint8_t> bytes);
// Atomically replaces the destination with the source. On success the source no longer
// exists. The replaced content is either wholly the old file or wholly the new one.
[[nodiscard]] Result<Unit> replace_file(const std::string& source,
                                        const std::string& destination);

}  // namespace black_start_manager::detail
