#pragma once

// Store path handling.
//
// Every path the store touches is either a caller-supplied root that is validated once
// or an artifact name this library generated. Names recovered from a manifest are
// re-validated against the exact pattern before they are used, so a tampered manifest
// cannot address a file outside the store root.

#include <string>

#include "black_start_manager/error.hpp"

namespace black_start_manager::detail {

inline constexpr const char* kManifestName = "store.manifest";
inline constexpr const char* kLockName = "store.lock";
inline constexpr const char* kSnapshotPrefix = "snapshot-";
inline constexpr const char* kSnapshotSuffix = ".bsm";
inline constexpr const char* kJournalPrefix = "journal-";
inline constexpr const char* kJournalSuffix = ".log";
inline constexpr const char* kTemporarySuffix = ".tmp";

// Validates a caller-supplied store root: present, within the length bound, valid
// UTF-8, no embedded NUL, and not a Windows reserved device name.
[[nodiscard]] Result<std::string> validate_store_root(const std::string& root);

// Lexically normalizes a path and makes it absolute against the current working
// directory. Never resolves links: the store owns the directory it was given.
[[nodiscard]] Result<std::string> absolute_path(const std::string& path);

[[nodiscard]] std::string join_path(const std::string& root, const std::string& name);

// Validates an artifact file name: a single path component from the allowed character
// set, within bounds, not a reserved device name, and a recognized store artifact.
[[nodiscard]] Result<std::string> validate_artifact_name(const std::string& name);

// True when the name ends in the staged-publication suffix.
[[nodiscard]] bool has_temporary_suffix(const std::string& name) noexcept;

// Validates a name found in the store directory: either a store artifact or a staged
// artifact awaiting publication. Anything else is refused.
[[nodiscard]] Result<std::string> validate_store_file(const std::string& name);

[[nodiscard]] bool is_snapshot_name(const std::string& name) noexcept;
[[nodiscard]] bool is_journal_name(const std::string& name) noexcept;

[[nodiscard]] std::string snapshot_name(std::uint64_t generation);
[[nodiscard]] std::string journal_name(std::uint64_t generation);

}  // namespace black_start_manager::detail
