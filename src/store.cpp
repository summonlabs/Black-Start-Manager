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

#include "black_start_manager/store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "black_start_manager/checked.hpp"
#include "detail/crash_point.hpp"
#include "detail/file_io.hpp"
#include "detail/paths.hpp"
#include "detail/process_lock.hpp"

namespace black_start_manager {
namespace {

constexpr std::size_t kFrameHeaderBytes = 32;
constexpr std::size_t kDigestBytes = 32;
constexpr std::size_t kSnapshotHeaderBytes = 32;
constexpr std::uint64_t kMaxManifestBytes = 512u * 1024u;
constexpr std::uint8_t kJournalMagic[4] = {'B', 'S', 'M', 'J'};
constexpr std::uint8_t kSnapshotMagic[4] = {'B', 'S', 'M', 'S'};

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xffu));
  out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xffu));
  }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xffu));
  }
}

[[nodiscard]] std::uint16_t get_u16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                    (static_cast<std::uint16_t>(bytes[1]) << 8u));
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (int index = 3; index >= 0; --index) {
    value = (value << 8u) | static_cast<std::uint32_t>(bytes[index]);
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (int index = 7; index >= 0; --index) {
    value = (value << 8u) | static_cast<std::uint64_t>(bytes[index]);
  }
  return value;
}

[[nodiscard]] Digest chain_of(const Digest& previous,
                              std::span<const std::uint8_t> header,
                              std::span<const std::uint8_t> payload) {
  Sha256 hasher;
  hasher.update(previous.span());
  hasher.update(header);
  hasher.update(payload);
  return hasher.finish();
}

struct Manifest {
  std::uint16_t format_version = limits::kStoreFormatVersion;
  std::uint64_t generation = 0;
  std::uint64_t epoch = 0;
  IncarnationId last_incarnation;
  std::string snapshot_name;
  Digest snapshot_digest;
  std::uint64_t snapshot_sequence = 0;
  Digest snapshot_chain;
  std::string journal_name;
  std::uint64_t journal_base_sequence = 0;
};

[[nodiscard]] Result<JsonValue> manifest_body(const Manifest& manifest) {
  JsonObjectBuilder root;
  root.set_uint("format_version", manifest.format_version);
  root.set_uint("generation", manifest.generation);
  root.set_uint("epoch", manifest.epoch);
  root.set_text("last_incarnation", manifest.last_incarnation.hex());
  JsonObjectBuilder snapshot;
  snapshot.set_text("name", manifest.snapshot_name);
  snapshot.set_text("digest", manifest.snapshot_digest.hex());
  snapshot.set_uint("sequence", manifest.snapshot_sequence);
  snapshot.set_text("chain", manifest.snapshot_chain.hex());
  BSM_TRY_ASSIGN(snapshot_value, snapshot.build());
  root.set("snapshot", snapshot_value);
  JsonObjectBuilder journal;
  journal.set_text("name", manifest.journal_name);
  journal.set_uint("base_sequence", manifest.journal_base_sequence);
  BSM_TRY_ASSIGN(journal_value, journal.build());
  root.set("journal", journal_value);
  return root.build();
}

[[nodiscard]] Result<Digest> manifest_digest_of(const JsonValue& body) {
  return Digest::of(canonical_json(body));
}

[[nodiscard]] Result<Manifest> manifest_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::TypeMismatch, "store manifest must be an object");
  }
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(
      value, {"format_version", "generation", "epoch", "last_incarnation", "snapshot",
              "journal", "manifest_digest"}));
  BSM_TRY_ASSIGN(version, json::require_unsigned(value, "format_version"));
  if (version != limits::kStoreFormatVersion) {
    return Error(ErrorCode::UnsupportedFormatVersion,
                 "store format version " + std::to_string(version) +
                     " is not supported by this build");
  }
  BSM_TRY_ASSIGN(digest_text, json::require_string(value, "manifest_digest"));
  BSM_TRY_ASSIGN(stored_digest, Digest::from_hex(digest_text));

  JsonValue::Object body_fields;
  for (const JsonValue::Field& field : value.as_object()) {
    if (field.first == "manifest_digest") {
      continue;
    }
    body_fields.push_back(field);
  }
  BSM_TRY_ASSIGN(body, JsonValue::object(std::move(body_fields)));
  BSM_TRY_ASSIGN(computed, manifest_digest_of(body));
  if (!(computed == stored_digest)) {
    return Error(ErrorCode::IntegrityMismatch,
                 "store manifest digest does not match its content");
  }

  Manifest manifest;
  manifest.format_version = static_cast<std::uint16_t>(version);
  BSM_TRY_ASSIGN(generation, json::require_unsigned(body, "generation"));
  manifest.generation = generation;
  BSM_TRY_ASSIGN(epoch, json::require_unsigned(body, "epoch"));
  manifest.epoch = epoch;
  BSM_TRY_ASSIGN(incarnation_text, json::require_string(body, "last_incarnation"));
  BSM_TRY_ASSIGN(incarnation, IncarnationId::from_hex(incarnation_text));
  manifest.last_incarnation = incarnation;

  BSM_TRY_ASSIGN(snapshot, json::require_object_member(body, "snapshot"));
  BSM_RETURN_IF_ERROR(
      json::reject_unknown_fields(*snapshot, {"name", "digest", "sequence", "chain"}));
  BSM_TRY_ASSIGN(snapshot_name_text, json::require_string(*snapshot, "name"));
  BSM_TRY_ASSIGN(snapshot_name_valid, detail::validate_artifact_name(snapshot_name_text));
  if (!detail::is_snapshot_name(snapshot_name_valid)) {
    return Error(ErrorCode::StoreCorrupt,
                 "manifest names '" + snapshot_name_text + "', which is not a snapshot");
  }
  manifest.snapshot_name = snapshot_name_valid;
  BSM_TRY_ASSIGN(snapshot_digest_text, json::require_string(*snapshot, "digest"));
  BSM_TRY_ASSIGN(snapshot_digest_value, Digest::from_hex(snapshot_digest_text));
  manifest.snapshot_digest = snapshot_digest_value;
  BSM_TRY_ASSIGN(snapshot_sequence, json::require_unsigned(*snapshot, "sequence"));
  manifest.snapshot_sequence = snapshot_sequence;
  BSM_TRY_ASSIGN(chain_text, json::require_string(*snapshot, "chain"));
  BSM_TRY_ASSIGN(chain_value, Digest::from_hex(chain_text));
  manifest.snapshot_chain = chain_value;

  BSM_TRY_ASSIGN(journal, json::require_object_member(body, "journal"));
  BSM_RETURN_IF_ERROR(json::reject_unknown_fields(*journal, {"name", "base_sequence"}));
  BSM_TRY_ASSIGN(journal_name_text, json::require_string(*journal, "name"));
  BSM_TRY_ASSIGN(journal_name_valid, detail::validate_artifact_name(journal_name_text));
  if (!detail::is_journal_name(journal_name_valid)) {
    return Error(ErrorCode::StoreCorrupt,
                 "manifest names '" + journal_name_text + "', which is not a journal");
  }
  manifest.journal_name = journal_name_valid;
  BSM_TRY_ASSIGN(base_sequence, json::require_unsigned(*journal, "base_sequence"));
  manifest.journal_base_sequence = base_sequence;
  if (manifest.journal_base_sequence != manifest.snapshot_sequence) {
    return Error(ErrorCode::StoreCorrupt,
                 "manifest journal base sequence does not match the snapshot sequence");
  }
  if (manifest.generation == 0 || manifest.epoch == 0) {
    return Error(ErrorCode::StoreCorrupt, "manifest generation and epoch start at one");
  }
  if (manifest.snapshot_sequence > limits::kMaxJournalRecordsPerSegment *
                                       static_cast<std::uint64_t>(1u << 20)) {
    return Error(ErrorCode::StoreCorrupt, "manifest snapshot sequence is out of range");
  }
  return manifest;
}

[[nodiscard]] Result<Unit> write_manifest(const std::string& root,
                                          const Manifest& manifest) {
  BSM_TRY_ASSIGN(body, manifest_body(manifest));
  BSM_TRY_ASSIGN(digest, manifest_digest_of(body));
  JsonObjectBuilder root_builder;
  for (const JsonValue::Field& field : body.as_object()) {
    root_builder.set(field.first, field.second);
  }
  root_builder.set_text("manifest_digest", digest.hex());
  BSM_TRY_ASSIGN(document, root_builder.build());
  const std::string text = canonical_json(document);
  const std::span<const std::uint8_t> bytes(
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size());

  const std::string target = detail::join_path(root, detail::kManifestName);
  const std::string temporary = target + detail::kTemporarySuffix;
  BSM_RETURN_IF_ERROR(detail::write_file_durable(temporary, bytes));
  // Read the staged bytes back and verify them before they can become authoritative.
  BSM_TRY_ASSIGN(staged, detail::read_file(temporary, kMaxManifestBytes));
  if (staged.size() != bytes.size() ||
      !std::equal(staged.begin(), staged.end(), bytes.begin())) {
    return Error(ErrorCode::IoFailure,
                 "staged manifest did not read back byte for byte");
  }
  BSM_TRY_ASSIGN(staged_value, parse_json(std::string_view(
                                   reinterpret_cast<const char*>(staged.data()),
                                   staged.size())));
  BSM_TRY_ASSIGN(staged_manifest, manifest_from_json(staged_value));
  if (staged_manifest.generation != manifest.generation) {
    return Error(ErrorCode::IoFailure, "staged manifest did not decode to itself");
  }
  return detail::replace_file(temporary, target);
}

struct FrameHeader {
  std::uint64_t sequence = 0;
  JournalRecordKind kind = JournalRecordKind::SessionEstablished;
  std::uint32_t payload_length = 0;
  std::uint32_t payload_crc = 0;
  std::uint32_t header_crc = 0;
};

[[nodiscard]] bool header_is_valid(const std::uint8_t* header, FrameHeader& out,
                                   Error& error) {
  if (std::memcmp(header, kJournalMagic, 4) != 0) {
    error = Error(ErrorCode::InvalidEncoding, "journal frame magic is not present");
    return false;
  }
  const std::uint16_t version = get_u16(header + 4);
  if (version != limits::kJournalFormatVersion) {
    error = Error(ErrorCode::UnsupportedFormatVersion,
                  "journal frame format version is not supported");
    return false;
  }
  const std::uint32_t header_crc = get_u32(header + 28);
  if (crc32c(std::span<const std::uint8_t>(header, 28)) != header_crc) {
    error = Error(ErrorCode::IntegrityMismatch,
                  "journal frame header checksum does not match");
    return false;
  }
  out.sequence = get_u64(header + 8);
  const std::uint16_t kind = get_u16(header + 16);
  const Result<JournalRecordKind> kind_value = parse_journal_record_kind(kind);
  if (!kind_value.ok()) {
    error = kind_value.error();
    return false;
  }
  out.kind = kind_value.value();
  out.payload_length = get_u32(header + 20);
  out.payload_crc = get_u32(header + 24);
  out.header_crc = header_crc;
  return true;
}

[[nodiscard]] Result<std::vector<std::uint8_t>> encode_frame(
    std::uint64_t sequence, JournalRecordKind kind, const JsonValue& payload,
    const Digest& previous_chain, Digest& chain_out) {
  const std::string payload_text = canonical_json(payload);
  if (payload_text.size() > limits::kMaxJournalFramePayload) {
    return Error(ErrorCode::LimitExceeded,
                 "journal record payload exceeds the supported size");
  }
  std::vector<std::uint8_t> header;
  header.reserve(kFrameHeaderBytes);
  header.insert(header.end(), std::begin(kJournalMagic), std::end(kJournalMagic));
  put_u16(header, limits::kJournalFormatVersion);
  put_u16(header, 0);
  put_u64(header, sequence);
  put_u16(header, static_cast<std::uint16_t>(kind));
  put_u16(header, 0);
  put_u32(header, static_cast<std::uint32_t>(payload_text.size()));
  const std::span<const std::uint8_t> payload_bytes(
      reinterpret_cast<const std::uint8_t*>(payload_text.data()), payload_text.size());
  put_u32(header, crc32c(payload_bytes));
  put_u32(header, crc32c(std::span<const std::uint8_t>(header.data(), header.size())));

  chain_out = chain_of(previous_chain, header, payload_bytes);

  std::vector<std::uint8_t> frame;
  frame.reserve(kFrameHeaderBytes + payload_bytes.size() + kDigestBytes);
  frame.insert(frame.end(), header.begin(), header.end());
  frame.insert(frame.end(), payload_bytes.begin(), payload_bytes.end());
  const std::span<const std::uint8_t> chain_bytes = chain_out.span();
  frame.insert(frame.end(), chain_bytes.begin(), chain_bytes.end());
  return frame;
}

[[nodiscard]] Result<std::vector<std::uint8_t>> encode_snapshot(const JsonValue& payload,
                                                                std::uint64_t sequence) {
  const std::string payload_text = canonical_json(payload);
  if (payload_text.size() > limits::kMaxSnapshotPayload) {
    return Error(ErrorCode::LimitExceeded, "snapshot payload exceeds the supported size");
  }
  std::vector<std::uint8_t> header;
  header.reserve(kSnapshotHeaderBytes);
  header.insert(header.end(), std::begin(kSnapshotMagic), std::end(kSnapshotMagic));
  put_u16(header, limits::kStoreFormatVersion);
  put_u16(header, 0);
  put_u64(header, sequence);
  put_u64(header, static_cast<std::uint64_t>(payload_text.size()));
  const std::span<const std::uint8_t> payload_bytes(
      reinterpret_cast<const std::uint8_t*>(payload_text.data()), payload_text.size());
  put_u32(header, crc32c(payload_bytes));
  put_u32(header, crc32c(std::span<const std::uint8_t>(header.data(), header.size())));

  std::vector<std::uint8_t> file;
  file.reserve(kSnapshotHeaderBytes + payload_bytes.size() + kDigestBytes);
  file.insert(file.end(), header.begin(), header.end());
  file.insert(file.end(), payload_bytes.begin(), payload_bytes.end());
  const Digest digest = sha256(std::span<const std::uint8_t>(file.data(), file.size()));
  const std::span<const std::uint8_t> digest_bytes = digest.span();
  file.insert(file.end(), digest_bytes.begin(), digest_bytes.end());
  return file;
}

struct SnapshotImage {
  JsonValue payload;
  std::uint64_t sequence = 0;
  Digest digest;
};

[[nodiscard]] Result<SnapshotImage> decode_snapshot(const std::vector<std::uint8_t>& file) {
  if (file.size() < kSnapshotHeaderBytes + kDigestBytes) {
    return Error(ErrorCode::TruncatedInput, "snapshot file is shorter than its header");
  }
  if (std::memcmp(file.data(), kSnapshotMagic, 4) != 0) {
    return Error(ErrorCode::StoreCorrupt, "snapshot magic is not present");
  }
  const std::uint16_t version = get_u16(file.data() + 4);
  if (version != limits::kStoreFormatVersion) {
    return Error(ErrorCode::UnsupportedFormatVersion,
                 "snapshot format version " + std::to_string(version) +
                     " is not supported by this build");
  }
  const std::uint64_t sequence = get_u64(file.data() + 8);
  const std::uint64_t payload_length = get_u64(file.data() + 16);
  if (payload_length > limits::kMaxSnapshotPayload ||
      payload_length > file.size()) {
    return Error(ErrorCode::StoreCorrupt, "snapshot payload length is not plausible");
  }
  if (file.size() != kSnapshotHeaderBytes + static_cast<std::size_t>(payload_length) +
                          kDigestBytes) {
    return Error(ErrorCode::StoreCorrupt,
                 "snapshot length does not match its declared payload length");
  }
  const std::uint32_t payload_crc = get_u32(file.data() + 24);
  const std::uint32_t header_crc = get_u32(file.data() + 28);
  if (crc32c(std::span<const std::uint8_t>(file.data(), 28)) != header_crc) {
    return Error(ErrorCode::IntegrityMismatch, "snapshot header checksum does not match");
  }
  const std::span<const std::uint8_t> payload_bytes(file.data() + kSnapshotHeaderBytes,
                                                    static_cast<std::size_t>(payload_length));
  if (crc32c(payload_bytes) != payload_crc) {
    return Error(ErrorCode::IntegrityMismatch, "snapshot payload checksum does not match");
  }
  const std::span<const std::uint8_t> digest_bytes(
      file.data() + kSnapshotHeaderBytes + static_cast<std::size_t>(payload_length),
      kDigestBytes);
  const Digest computed =
      sha256(std::span<const std::uint8_t>(file.data(), file.size() - kDigestBytes));
  if (!std::equal(digest_bytes.begin(), digest_bytes.end(), computed.span().begin())) {
    return Error(ErrorCode::IntegrityMismatch, "snapshot digest does not match its content");
  }
  BSM_TRY_ASSIGN(payload,
                 parse_json(std::string_view(reinterpret_cast<const char*>(payload_bytes.data()),
                                             payload_bytes.size())));
  SnapshotImage image;
  image.payload = std::move(payload);
  image.sequence = sequence;
  image.digest = computed;
  return image;
}

struct RecoveredStore {
  Manifest manifest;
  JsonValue snapshot_payload;
  std::vector<JournalRecord> records;
  Digest chain;
  std::uint64_t tail_bytes_discarded = 0;
  bool tail_truncated = false;
  std::vector<std::string> notes;
};

// Finds whether a valid frame header for the expected sequence appears later in the
// remaining bytes. When it does, the damage is in the middle of the journal and the
// store is corrupt; when it does not, the damage is a torn tail from an interrupted
// append and may be discarded conservatively.
[[nodiscard]] bool later_frame_exists(const std::vector<std::uint8_t>& bytes,
                                      std::size_t offset, std::uint64_t expected_sequence) {
  for (std::size_t probe = offset + 1; probe + kFrameHeaderBytes <= bytes.size(); ++probe) {
    if (std::memcmp(bytes.data() + probe, kJournalMagic, 4) != 0) {
      continue;
    }
    if (crc32c(std::span<const std::uint8_t>(bytes.data() + probe, 28)) !=
        get_u32(bytes.data() + probe + 28)) {
      continue;
    }
    if (get_u64(bytes.data() + probe + 8) >= expected_sequence) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] Result<RecoveredStore> read_store_artifacts(const std::string& root,
                                                          bool allow_torn_tail) {
  const std::string manifest_path = detail::join_path(root, detail::kManifestName);
  BSM_TRY_ASSIGN(manifest_bytes, detail::read_file(manifest_path, kMaxManifestBytes));
  if (manifest_bytes.empty()) {
    return Error(ErrorCode::StoreCorrupt, "store manifest is empty");
  }
  const std::string manifest_text(reinterpret_cast<const char*>(manifest_bytes.data()),
                                  manifest_bytes.size());
  BSM_TRY_ASSIGN(manifest_value, parse_json(manifest_text));
  BSM_TRY_ASSIGN(manifest, manifest_from_json(manifest_value));

  RecoveredStore recovered;
  recovered.manifest = manifest;

  const std::string snapshot_path = detail::join_path(root, manifest.snapshot_name);
  BSM_TRY_ASSIGN(snapshot_bytes,
                 detail::read_file(snapshot_path, limits::kMaxSnapshotPayload + 4096));
  BSM_TRY_ASSIGN(image, decode_snapshot(snapshot_bytes));
  if (image.sequence != manifest.snapshot_sequence) {
    return Error(ErrorCode::StoreCorrupt,
                 "snapshot sequence does not match the manifest");
  }
  if (!(image.digest == manifest.snapshot_digest)) {
    return Error(ErrorCode::IntegrityMismatch,
                 "snapshot digest does not match the manifest");
  }
  recovered.snapshot_payload = std::move(image.payload);

  const std::string journal_path = detail::join_path(root, manifest.journal_name);
  BSM_TRY_ASSIGN(journal_bytes,
                 detail::read_file(journal_path, limits::kMaxJournalSegmentBytes * 4u));
  recovered.notes.push_back("journal bytes read: " +
                            std::to_string(journal_bytes.size()));

  std::size_t offset = 0;
  std::uint64_t expected_sequence = manifest.journal_base_sequence;
  Digest chain = manifest.snapshot_chain;
  while (offset < journal_bytes.size()) {
    const std::size_t remaining = journal_bytes.size() - offset;
    if (remaining < kFrameHeaderBytes) {
      if (!allow_torn_tail) {
        return Error(ErrorCode::TruncatedInput,
                     "journal ends inside a frame header");
      }
      recovered.tail_truncated = true;
      recovered.tail_bytes_discarded = remaining;
      break;
    }
    FrameHeader header;
    Error header_error = Error(ErrorCode::StoreCorrupt, "journal frame header is invalid");
    if (!header_is_valid(journal_bytes.data() + offset, header, header_error)) {
      if (header_error.code() == ErrorCode::UnsupportedFormatVersion) {
        // A complete header with a supported checksum but an unsupported version is not a
        // torn write; it is state this build cannot read.
        return header_error;
      }
      if (!allow_torn_tail) {
        return header_error;
      }
      if (later_frame_exists(journal_bytes, offset, expected_sequence + 1)) {
        JsonObjectBuilder detail;
        detail.set_uint("byte_offset", static_cast<std::uint64_t>(offset));
        BSM_TRY_ASSIGN(detail_value, detail.build());
        return Error(ErrorCode::StoreCorrupt,
                     "journal contains a damaged frame before further valid frames")
            .with_detail(detail_value);
      }
      recovered.tail_truncated = true;
      recovered.tail_bytes_discarded = remaining;
      break;
    }
    if (header.sequence != expected_sequence + 1) {
      return Error(ErrorCode::StoreCorrupt,
                   "journal sequence is not contiguous at byte " +
                       std::to_string(offset));
    }
    const std::size_t frame_bytes =
        kFrameHeaderBytes + static_cast<std::size_t>(header.payload_length) + kDigestBytes;
    if (header.payload_length > limits::kMaxJournalFramePayload) {
      if (!allow_torn_tail) {
        return Error(ErrorCode::LimitExceeded, "journal frame payload is too large");
      }
      recovered.tail_truncated = true;
      recovered.tail_bytes_discarded = remaining;
      break;
    }
    if (remaining < frame_bytes) {
      if (!allow_torn_tail) {
        return Error(ErrorCode::TruncatedInput, "journal ends inside a frame");
      }
      if (later_frame_exists(journal_bytes, offset, expected_sequence + 1)) {
        return Error(ErrorCode::StoreCorrupt,
                     "journal contains a truncated frame before further valid frames");
      }
      recovered.tail_truncated = true;
      recovered.tail_bytes_discarded = remaining;
      break;
    }
    const std::span<const std::uint8_t> header_bytes(journal_bytes.data() + offset,
                                                     kFrameHeaderBytes);
    const std::span<const std::uint8_t> payload_bytes(
        journal_bytes.data() + offset + kFrameHeaderBytes, header.payload_length);
    if (crc32c(payload_bytes) != header.payload_crc) {
      return Error(ErrorCode::IntegrityMismatch,
                   "journal payload checksum does not match at sequence " +
                       std::to_string(header.sequence));
    }
    const Digest expected_chain = chain_of(chain, header_bytes, payload_bytes);
    const std::span<const std::uint8_t> stored_chain(
        journal_bytes.data() + offset + kFrameHeaderBytes + header.payload_length,
        kDigestBytes);
    if (!std::equal(stored_chain.begin(), stored_chain.end(), expected_chain.span().begin())) {
      return Error(ErrorCode::IntegrityMismatch,
                   "journal chain does not link at sequence " +
                       std::to_string(header.sequence));
    }
    BSM_TRY_ASSIGN(payload,
                   parse_json(std::string_view(
                       reinterpret_cast<const char*>(payload_bytes.data()),
                       payload_bytes.size())));
    if (!payload.is_object()) {
      return Error(ErrorCode::SchemaViolation,
                   "journal record payload must be a canonical object");
    }
    JournalRecord record;
    record.sequence = header.sequence;
    record.kind = header.kind;
    record.payload = std::move(payload);
    recovered.records.push_back(std::move(record));
    chain = expected_chain;
    offset += frame_bytes;
    expected_sequence = header.sequence;
  }
  recovered.chain = chain;
  return recovered;
}

[[nodiscard]] Result<Unit> initialize_store(const std::string& root) {
  const Manifest manifest = []() {
    Manifest fresh;
    fresh.generation = 1;
    fresh.epoch = 1;
    fresh.snapshot_name = detail::snapshot_name(1);
    fresh.snapshot_sequence = 0;
    fresh.snapshot_chain = Digest();
    fresh.journal_name = detail::journal_name(1);
    fresh.journal_base_sequence = 0;
    return fresh;
  }();

  BSM_TRY_ASSIGN(empty_payload, JsonValue::object(JsonValue::Object{}));
  BSM_TRY_ASSIGN(encoded, encode_snapshot(empty_payload, 0));
  const std::string snapshot_path = detail::join_path(root, manifest.snapshot_name);
  BSM_RETURN_IF_ERROR(detail::write_file_durable(snapshot_path, encoded));
  BSM_TRY_ASSIGN(bytes, detail::read_file(snapshot_path, limits::kMaxSnapshotPayload + 4096));
  BSM_TRY_ASSIGN(image, decode_snapshot(bytes));

  Manifest published = manifest;
  published.snapshot_digest = image.digest;
  const std::string journal_path = detail::join_path(root, manifest.journal_name);
  BSM_TRY_ASSIGN(journal, detail::FileHandle::open_append(journal_path));
  BSM_RETURN_IF_ERROR(journal.flush_durable());
  journal.close();
  return write_manifest(root, published);
}

[[nodiscard]] bool is_temporary_name(const std::string& name) {
  return detail::has_temporary_suffix(name);
}

}  // namespace

Result<JsonValue> RecoveryReport::to_json() const {
  JsonObjectBuilder root;
  root.set_bool("created_new_store", created_new_store);
  root.set_uint("generation", generation);
  root.set_uint("epoch", epoch);
  root.set_text("previous_incarnation", previous_incarnation.hex());
  root.set_text("incarnation", incarnation.hex());
  root.set_uint("records_replayed", static_cast<std::uint64_t>(records_replayed));
  root.set_uint("journal_bytes", journal_bytes);
  root.set_uint("tail_bytes_discarded", tail_bytes_discarded);
  root.set_bool("tail_truncated", tail_truncated);
  root.set_text("snapshot_digest", snapshot_digest.hex());
  root.set_text("chain", chain.hex());
  JsonArrayBuilder notes_array;
  for (const std::string& item : notes) {
    notes_array.push_text(item);
  }
  BSM_TRY_ASSIGN(notes_value, notes_array.build());
  root.set("notes", notes_value);
  JsonArrayBuilder orphans_array;
  for (const std::string& name : removed_orphans) {
    orphans_array.push_text(name);
  }
  BSM_TRY_ASSIGN(orphans_value, orphans_array.build());
  root.set("removed_orphans", orphans_value);
  return root.build();
}

Result<JsonValue> StoreVerification::to_json() const {
  JsonObjectBuilder root;
  root.set_uint("generation", generation);
  root.set_uint("epoch", epoch);
  root.set_uint("journal_records", static_cast<std::uint64_t>(journal_records));
  root.set_uint("journal_bytes", journal_bytes);
  root.set_text("snapshot_digest", snapshot_digest.hex());
  root.set_text("chain", chain.hex());
  JsonArrayBuilder notes_array;
  for (const std::string& item : notes) {
    notes_array.push_text(item);
  }
  BSM_TRY_ASSIGN(notes_value, notes_array.build());
  root.set("notes", notes_value);
  return root.build();
}

struct Store::Impl {
  detail::ProcessLock lock;
  detail::FileHandle journal;
  std::string snapshot_name;
  std::string journal_name;
  Digest chain;
};

Store::Store(Store&& other) noexcept
    : impl_(other.impl_),
      options_(std::move(other.options_)),
      recovery_(std::move(other.recovery_)),
      snapshot_payload_(std::move(other.snapshot_payload_)),
      records_(std::move(other.records_)),
      generation_(other.generation_),
      epoch_(other.epoch_),
      last_sequence_(other.last_sequence_),
      journal_records_(other.journal_records_),
      journal_bytes_(other.journal_bytes_),
      incarnation_(other.incarnation_) {
  other.impl_ = nullptr;
}

Store& Store::operator=(Store&& other) noexcept {
  if (this != &other) {
    delete impl_;
    impl_ = other.impl_;
    other.impl_ = nullptr;
    options_ = std::move(other.options_);
    recovery_ = std::move(other.recovery_);
    snapshot_payload_ = std::move(other.snapshot_payload_);
    records_ = std::move(other.records_);
    generation_ = other.generation_;
    epoch_ = other.epoch_;
    last_sequence_ = other.last_sequence_;
    journal_records_ = other.journal_records_;
    journal_bytes_ = other.journal_bytes_;
    incarnation_ = other.incarnation_;
  }
  return *this;
}

Store::~Store() { delete impl_; }

bool Store::should_compact() const noexcept {
  return journal_bytes_ >= options_.compact_journal_bytes ||
         journal_records_ >= options_.compact_journal_records;
}

Result<Store> Store::open(const StoreOptions& options) {
  BSM_TRY_ASSIGN(validated_root, detail::validate_store_root(options.root));
  BSM_TRY_ASSIGN(root, detail::absolute_path(validated_root));

  BSM_TRY_ASSIGN(root_exists, detail::file_exists(root));
  if (!root_exists) {
    if (!options.create_if_missing) {
      return Error(ErrorCode::StoreNotFound,
                   "store root '" + root + "' does not exist");
    }
    BSM_RETURN_IF_ERROR(detail::create_directories(root));
  }

  auto impl = std::make_unique<Impl>();
  BSM_TRY_ASSIGN(lock, detail::ProcessLock::acquire(detail::join_path(root, detail::kLockName)));
  impl->lock = std::move(lock);

  const std::string manifest_path = detail::join_path(root, detail::kManifestName);
  BSM_TRY_ASSIGN(manifest_exists, detail::file_exists(manifest_path));

  Store store;
  store.options_ = options;
  store.options_.root = root;

  if (!manifest_exists) {
    BSM_TRY_ASSIGN(names, detail::list_directory(root));
    for (const std::string& name : names) {
      if (name == detail::kLockName || is_temporary_name(name)) {
        continue;
      }
      return Error(ErrorCode::StoreAmbiguous,
                   "store root contains '" + name +
                       "' but no manifest, so its authoritative generation is unknown");
    }
    for (const std::string& name : names) {
      if (!is_temporary_name(name)) {
        continue;
      }
      BSM_RETURN_IF_ERROR(detail::validate_store_file(name));
      BSM_RETURN_IF_ERROR(detail::remove_file(detail::join_path(root, name)));
      store.recovery_.removed_orphans.push_back(name);
    }
    BSM_RETURN_IF_ERROR(initialize_store(root));
    store.recovery_.created_new_store = true;
    store.recovery_.notes.push_back("initialized a new store generation");
  }

  BSM_TRY_ASSIGN(recovered, read_store_artifacts(root, true));
  store.recovery_.generation = recovered.manifest.generation;
  store.recovery_.previous_incarnation = recovered.manifest.last_incarnation;
  store.recovery_.records_replayed = recovered.records.size();
  store.recovery_.tail_bytes_discarded = recovered.tail_bytes_discarded;
  store.recovery_.tail_truncated = recovered.tail_truncated;
  store.recovery_.snapshot_digest = recovered.manifest.snapshot_digest;
  store.recovery_.chain = recovered.chain;
  for (const std::string& note : recovered.notes) {
    store.recovery_.notes.push_back(note);
  }

  BSM_TRY_ASSIGN(next_epoch, checked_increment(recovered.manifest.epoch));
  BSM_TRY_ASSIGN(incarnation, IncarnationId::generate());

  Manifest updated = recovered.manifest;
  updated.epoch = next_epoch;
  updated.last_incarnation = incarnation;
  BSM_RETURN_IF_ERROR(write_manifest(root, updated));

  impl->snapshot_name = recovered.manifest.snapshot_name;
  impl->journal_name = recovered.manifest.journal_name;
  impl->chain = recovered.chain;
  BSM_TRY_ASSIGN(journal, detail::FileHandle::open_append(
                              detail::join_path(root, recovered.manifest.journal_name)));
  BSM_TRY_ASSIGN(journal_size, journal.size());
  impl->journal = std::move(journal);

  // Remove artifacts the current manifest does not reference. They can only be the
  // remains of an interrupted publication, and they are never authoritative.
  {
    const std::string keep_snapshot = impl->snapshot_name;
    const std::string keep_journal = impl->journal_name;
    BSM_TRY_ASSIGN(names, detail::list_directory(root));
    for (const std::string& name : names) {
      if (name == detail::kManifestName || name == detail::kLockName ||
          name == keep_snapshot || name == keep_journal) {
        continue;
      }
      if (!is_temporary_name(name) && !detail::is_snapshot_name(name) &&
          !detail::is_journal_name(name)) {
        continue;
      }
      BSM_RETURN_IF_ERROR(detail::validate_store_file(name));
      BSM_RETURN_IF_ERROR(detail::remove_file(detail::join_path(root, name)));
      store.recovery_.removed_orphans.push_back(name);
    }
  }

  store.impl_ = impl.release();
  store.snapshot_payload_ = std::move(recovered.snapshot_payload);
  store.records_ = std::move(recovered.records);
  store.generation_ = recovered.manifest.generation;
  store.epoch_ = next_epoch;
  store.incarnation_ = incarnation;
  store.last_sequence_ = recovered.manifest.snapshot_sequence;
  for (const JournalRecord& record : store.records_) {
    store.last_sequence_ = record.sequence;
  }
  store.journal_records_ = store.records_.size();
  store.journal_bytes_ = journal_size;
  store.recovery_.epoch = next_epoch;
  store.recovery_.incarnation = incarnation;
  store.recovery_.journal_bytes = journal_size;

  return store;
}

Result<Unit> Store::append(JournalRecordKind kind, JsonValue payload) {
  if (impl_ == nullptr) {
    return Error(ErrorCode::StoreNotOpen, "store is not open");
  }
  if (!payload.is_object()) {
    return Error(ErrorCode::SchemaViolation,
                 "journal record payload must be a canonical object");
  }
  detail::crash_point("journal_before_append");
  BSM_TRY_ASSIGN(next_sequence, checked_increment(last_sequence_));
  Digest chain;
  BSM_TRY_ASSIGN(frame, encode_frame(next_sequence, kind, payload, impl_->chain, chain));
  BSM_RETURN_IF_ERROR(impl_->journal.write_all(frame));
  BSM_RETURN_IF_ERROR(impl_->journal.flush_durable());
  detail::crash_point("journal_after_append");

  JournalRecord record;
  record.sequence = next_sequence;
  record.kind = kind;
  record.payload = std::move(payload);
  records_.push_back(std::move(record));
  last_sequence_ = next_sequence;
  impl_->chain = chain;
  ++journal_records_;
  journal_bytes_ += frame.size();
  return Unit{};
}

Result<Unit> Store::publish_snapshot(JsonValue snapshot_payload) {
  if (impl_ == nullptr) {
    return Error(ErrorCode::StoreNotOpen, "store is not open");
  }
  if (!snapshot_payload.is_object()) {
    return Error(ErrorCode::SchemaViolation, "snapshot payload must be a canonical object");
  }
  detail::crash_point("snapshot_before_write");
  BSM_TRY_ASSIGN(next_generation, checked_increment(generation_));
  const std::string new_snapshot = detail::snapshot_name(next_generation);
  const std::string new_journal = detail::journal_name(next_generation);

  BSM_TRY_ASSIGN(encoded, encode_snapshot(snapshot_payload, last_sequence_));
  const std::string snapshot_target = detail::join_path(options_.root, new_snapshot);
  const std::string snapshot_temporary = snapshot_target + detail::kTemporarySuffix;
  BSM_RETURN_IF_ERROR(detail::write_file_durable(snapshot_temporary, encoded));

  // Read the staged snapshot back and decode it before it can be referenced.
  BSM_TRY_ASSIGN(staged, detail::read_file(snapshot_temporary, limits::kMaxSnapshotPayload + 4096));
  if (staged.size() != encoded.size()) {
    return Error(ErrorCode::IoFailure, "staged snapshot did not read back at its size");
  }
  BSM_TRY_ASSIGN(staged_image, decode_snapshot(staged));
  if (staged_image.sequence != last_sequence_) {
    return Error(ErrorCode::IoFailure, "staged snapshot did not decode to itself");
  }
  BSM_RETURN_IF_ERROR(detail::replace_file(snapshot_temporary, snapshot_target));
  detail::crash_point("snapshot_after_write");

  const std::string journal_path = detail::join_path(options_.root, new_journal);
  {
    BSM_TRY_ASSIGN(segment, detail::FileHandle::open_append(journal_path));
    BSM_RETURN_IF_ERROR(segment.flush_durable());
  }

  Manifest manifest;
  manifest.generation = next_generation;
  manifest.epoch = epoch_;
  manifest.last_incarnation = incarnation_;
  manifest.snapshot_name = new_snapshot;
  manifest.snapshot_digest = staged_image.digest;
  manifest.snapshot_sequence = last_sequence_;
  manifest.snapshot_chain = impl_->chain;
  manifest.journal_name = new_journal;
  manifest.journal_base_sequence = last_sequence_;

  const std::string previous_snapshot = impl_->snapshot_name;
  const std::string previous_journal = impl_->journal_name;
  detail::crash_point("snapshot_before_manifest");
  BSM_RETURN_IF_ERROR(write_manifest(options_.root, manifest));
  detail::crash_point("snapshot_after_publish");

  BSM_TRY_ASSIGN(journal, detail::FileHandle::open_append(journal_path));
  impl_->journal.close();
  impl_->journal = std::move(journal);
  impl_->snapshot_name = new_snapshot;
  impl_->journal_name = new_journal;

  generation_ = next_generation;
  records_.clear();
  journal_records_ = 0;
  journal_bytes_ = 0;
  snapshot_payload_ = std::move(snapshot_payload);

  // The superseded artifacts are unreferenced from the moment the manifest was
  // replaced; removing them is housekeeping, not part of the commit.
  if (!previous_snapshot.empty() && previous_snapshot != new_snapshot) {
    BSM_RETURN_IF_ERROR(detail::remove_file(detail::join_path(options_.root, previous_snapshot)));
  }
  if (!previous_journal.empty() && previous_journal != new_journal) {
    BSM_RETURN_IF_ERROR(detail::remove_file(detail::join_path(options_.root, previous_journal)));
  }
  detail::crash_point("snapshot_after_cleanup");
  return Unit{};
}

Result<bool> Store::maybe_compact(const JsonValue& snapshot_payload) {
  if (!should_compact()) {
    return false;
  }
  BSM_RETURN_IF_ERROR(publish_snapshot(snapshot_payload));
  return true;
}

Result<StoreVerification> Store::verify() const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::StoreNotOpen, "store is not open");
  }
  BSM_TRY_ASSIGN(recovered, read_store_artifacts(options_.root, true));
  StoreVerification verification;
  verification.generation = recovered.manifest.generation;
  verification.epoch = recovered.manifest.epoch;
  verification.journal_records = recovered.records.size();
  verification.snapshot_digest = recovered.manifest.snapshot_digest;
  verification.chain = recovered.chain;
  verification.notes = recovered.notes;
  BSM_TRY_ASSIGN(names, detail::list_directory(options_.root));
  for (const std::string& name : names) {
    if (detail::is_journal_name(name) || detail::is_snapshot_name(name) ||
        name == detail::kManifestName) {
      BSM_TRY_ASSIGN(size, detail::file_size(detail::join_path(options_.root, name)));
      if (detail::is_journal_name(name)) {
        verification.journal_bytes += size;
      }
    }
  }
  if (recovered.tail_truncated) {
    verification.notes.push_back("journal tail is torn and would be discarded on open");
  }
  if (!(recovered.chain == impl_->chain)) {
    verification.notes.push_back(
        "on-disk chain differs from the in-memory chain, which means another writer "
        "published state");
  }
  return verification;
}

Result<Unit> Store::close() {
  if (impl_ == nullptr) {
    return ok_status();
  }
  impl_->journal.close();
  impl_->lock.release();
  delete impl_;
  impl_ = nullptr;
  return ok_status();
}

}  // namespace black_start_manager
