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

#include "detail/paths.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "black_start_manager/canonical.hpp"
#include "black_start_manager/limits.hpp"

namespace black_start_manager::detail {
namespace {

[[nodiscard]] bool is_reserved_device_name(const std::string& name) noexcept {
  std::string stem = name;
  const std::size_t dot = stem.find('.');
  if (dot != std::string::npos) {
    stem = stem.substr(0, dot);
  }
  for (char& character : stem) {
    character = static_cast<char>(
        std::toupper(static_cast<unsigned char>(character)));
  }
  if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") {
    return true;
  }
  if (stem.size() == 4) {
    const bool prefix = stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0;
    if (prefix && stem[3] >= '1' && stem[3] <= '9') {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool has_separator(const std::string& text) noexcept {
  return text.find('/') != std::string::npos || text.find('\\') != std::string::npos;
}

[[nodiscard]] bool is_artifact_char(char character) noexcept {
  const unsigned char byte = static_cast<unsigned char>(character);
  if (byte >= 'a' && byte <= 'z') {
    return true;
  }
  if (byte >= 'A' && byte <= 'Z') {
    return true;
  }
  if (byte >= '0' && byte <= '9') {
    return true;
  }
  return byte == '.' || byte == '_' || byte == '-';
}

[[nodiscard]] bool matches(const std::string& name, const char* prefix,
                           const char* suffix) noexcept {
  const std::string prefix_text(prefix);
  const std::string suffix_text(suffix);
  if (name.size() <= prefix_text.size() + suffix_text.size()) {
    return false;
  }
  if (name.compare(0, prefix_text.size(), prefix_text) != 0) {
    return false;
  }
  if (name.compare(name.size() - suffix_text.size(), suffix_text.size(), suffix_text) != 0) {
    return false;
  }
  const std::string digits =
      name.substr(prefix_text.size(), name.size() - prefix_text.size() - suffix_text.size());
  return !digits.empty() &&
         std::all_of(digits.begin(), digits.end(),
                     [](char character) { return character >= '0' && character <= '9'; });
}

}  // namespace

Result<std::string> validate_store_root(const std::string& root) {
  if (root.empty()) {
    return Error(ErrorCode::InvalidArgument, "store root is empty");
  }
  if (root.size() > limits::kMaxStoreRootLength) {
    return Error(ErrorCode::PathTooLong,
                 "store root exceeds the supported length of " +
                     std::to_string(limits::kMaxStoreRootLength) + " characters");
  }
  if (root.find('\0') != std::string::npos) {
    return Error(ErrorCode::PathUnsafe, "store root contains a NUL character");
  }
  for (const char character : root) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x20u || byte == 0x7fu) {
      return Error(ErrorCode::PathUnsafe,
                   "store root contains a control character");
    }
  }
  if (!is_valid_utf8(root)) {
    return Error(ErrorCode::InvalidUtf8, "store root is not valid UTF-8");
  }
  if (root.size() > 2 && (root.back() == ' ' || root.back() == '.')) {
    // A Windows path that ends in a space or a dot silently collapses to a different
    // path; refusing is the only honest answer.
    return Error(ErrorCode::PathUnsafe,
                 "store root ends with a space or a dot, which Windows would collapse");
  }
  std::filesystem::path as_path(root);
  const std::string leaf = as_path.filename().string();
  if (!leaf.empty() && is_reserved_device_name(leaf)) {
    return Error(ErrorCode::PathUnsafe,
                 "store root names a reserved Windows device");
  }
  return root;
}

Result<std::string> absolute_path(const std::string& path) {
  std::error_code error;
  std::filesystem::path candidate(path);
  std::filesystem::path absolute = std::filesystem::absolute(candidate, error);
  if (error) {
    return Error(ErrorCode::IoFailure, "cannot resolve '" + path + "' to an absolute path");
  }
  std::filesystem::path normalized = absolute.lexically_normal();
  const std::string text = normalized.string();
  if (text.size() > limits::kMaxPathLength) {
    return Error(ErrorCode::PathTooLong, "resolved path exceeds the supported length");
  }
  return text;
}

std::string join_path(const std::string& root, const std::string& name) {
  if (root.empty()) {
    return name;
  }
  const char last = root.back();
  if (last == '/' || last == '\\') {
    return root + name;
  }
  return root + "/" + name;
}

Result<std::string> validate_artifact_name(const std::string& name) {
  if (name.empty()) {
    return Error(ErrorCode::InvalidArgument, "store artifact name is empty");
  }
  if (name.size() > limits::kMaxStoreArtifactNameLength) {
    return Error(ErrorCode::PathTooLong, "store artifact name exceeds the supported length");
  }
  if (name.front() == '.') {
    return Error(ErrorCode::PathUnsafe, "store artifact name starts with a dot");
  }
  if (has_separator(name)) {
    return Error(ErrorCode::PathUnsafe,
                 "store artifact name contains a path separator");
  }
  if (name == "." || name == "..") {
    return Error(ErrorCode::PathUnsafe, "store artifact name is a relative path");
  }
  if (!std::all_of(name.begin(), name.end(),
                   [](char character) { return is_artifact_char(character); })) {
    return Error(ErrorCode::PathUnsafe,
                 "store artifact name contains a character outside [A-Za-z0-9._-]");
  }
  if (is_reserved_device_name(name)) {
    return Error(ErrorCode::PathUnsafe, "store artifact name is a reserved device name");
  }
  const bool known = name == kManifestName || name == kLockName ||
                     is_snapshot_name(name) || is_journal_name(name);
  if (!known) {
    return Error(ErrorCode::PathUnsafe,
                 "store artifact name '" + name + "' is not a store artifact");
  }
  return name;
}

bool has_temporary_suffix(const std::string& name) noexcept {
  const std::string suffix(kTemporarySuffix);
  return name.size() > suffix.size() &&
         name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

Result<std::string> validate_store_file(const std::string& name) {
  if (has_temporary_suffix(name)) {
    const std::string base =
        name.substr(0, name.size() - std::string(kTemporarySuffix).size());
    BSM_RETURN_IF_ERROR(validate_artifact_name(base));
    return name;
  }
  return validate_artifact_name(name);
}

bool is_snapshot_name(const std::string& name) noexcept {
  return matches(name, kSnapshotPrefix, kSnapshotSuffix);
}

bool is_journal_name(const std::string& name) noexcept {
  return matches(name, kJournalPrefix, kJournalSuffix);
}

std::string snapshot_name(std::uint64_t generation) {
  std::string digits = std::to_string(generation);
  if (digits.size() < 16) {
    digits.insert(0, 16 - digits.size(), '0');
  }
  return std::string(kSnapshotPrefix) + digits + kSnapshotSuffix;
}

std::string journal_name(std::uint64_t generation) {
  std::string digits = std::to_string(generation);
  if (digits.size() < 16) {
    digits.insert(0, 16 - digits.size(), '0');
  }
  return std::string(kJournalPrefix) + digits + kJournalSuffix;
}

}  // namespace black_start_manager::detail
