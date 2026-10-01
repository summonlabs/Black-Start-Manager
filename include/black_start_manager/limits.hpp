#pragma once

// Fixed bounds for every externally influenced size. Checked at the boundary where
// the value enters the library, so no later code has to defend against an absurd
// length, count, or depth.

#include <cstddef>
#include <cstdint>

namespace black_start_manager::limits {

// Identifier and text bounds.
inline constexpr std::size_t kMaxIdentifierLength = 128;
inline constexpr std::size_t kMaxOwnerLength = 128;
inline constexpr std::size_t kMaxTextLength = 4096;
inline constexpr std::size_t kMaxReasonLength = 1024;

// Canonical encoding bounds.
inline constexpr std::size_t kMaxJsonDepth = 64;
inline constexpr std::size_t kMaxJsonBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kMaxStringBytes = 1024u * 1024u;
inline constexpr std::size_t kMaxArrayElements = 65536;
inline constexpr std::size_t kMaxObjectFields = 65536;

// Restoration plan bounds.
inline constexpr std::size_t kMaxStages = 256;
inline constexpr std::size_t kMaxObligations = 4096;
inline constexpr std::size_t kMaxDependenciesPerObligation = 1024;
inline constexpr std::size_t kMaxEvidenceRequirements = 64;
inline constexpr std::size_t kMaxRequiredSources = 8;
inline constexpr std::uint64_t kMaxAttemptBudget = 64;

// Session bounds.
inline constexpr std::size_t kMaxSessions = 4096;
inline constexpr std::size_t kMaxEvidencePerSession = 16384;
inline constexpr std::size_t kMaxAttemptsPerSession = 8192;
inline constexpr std::size_t kMaxHoldsPerSession = 1024;
inline constexpr std::size_t kMaxStageTransitions = 4096;

// Time and counter bounds. Ticks are logical units supplied by a clock; they are
// never wall-clock values inside canonical output.
inline constexpr std::uint64_t kMaxTick = 1ull << 62;
inline constexpr std::uint64_t kMaxDeadlineTicks = 1ull << 40;
inline constexpr std::uint64_t kMaxEvidenceAgeTicks = 1ull << 40;
inline constexpr std::uint64_t kMaxGeneration = 1ull << 62;
inline constexpr std::uint64_t kMaxPolicyRevision = 1ull << 62;

// Durable store bounds.
inline constexpr std::size_t kMaxJournalFramePayload = 4u * 1024u * 1024u;
inline constexpr std::size_t kMaxSnapshotPayload = 256u * 1024u * 1024u;
inline constexpr std::size_t kMaxJournalRecordsPerSegment = 1u << 20;
inline constexpr std::uint64_t kMaxJournalSegmentBytes = 64ull * 1024ull * 1024ull;
inline constexpr std::uint16_t kStoreFormatVersion = 1;
inline constexpr std::uint16_t kJournalFormatVersion = 1;
inline constexpr std::uint16_t kStateFormatVersion = 1;
inline constexpr std::uint16_t kPlanFormatVersion = 1;

// Filesystem bounds. Store root paths are validated against this before any file is
// created: Windows path handling, long-path prefixes, and reserved names are
// normalized by the path layer, and anything longer than this is refused.
inline constexpr std::size_t kMaxPathLength = 4096;
inline constexpr std::size_t kMaxStoreRootLength = 200;
inline constexpr std::size_t kMaxStoreArtifactNameLength = 128;

// Controller bounds.
inline constexpr std::size_t kMaxControllers = 64;
inline constexpr std::size_t kMaxControllerReplyBytes = 64u * 1024u;
inline constexpr std::uint32_t kMaxControllerRetries = 4;

}  // namespace black_start_manager::limits
