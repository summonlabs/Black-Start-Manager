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

// Proof obligations: content identity, canonical encoding, identity validation, and
// checked arithmetic. These are the primitives every other guarantee rests on.

#include <cstdint>
#include <string>
#include <vector>

#include "black_start_manager/canonical.hpp"
#include "black_start_manager/checked.hpp"
#include "black_start_manager/digest.hpp"
#include "black_start_manager/ids.hpp"
#include "test_harness.hpp"

using namespace black_start_manager;

BSM_TEST(sha256_matches_published_vectors) {
  BSM_CHECK_EQ(sha256(std::string()).hex(),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852"
                           "b855"));
  BSM_CHECK_EQ(sha256(std::string("abc")).hex(),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f200"
                           "15ad"));
  BSM_CHECK_EQ(
      sha256(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")).hex(),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  // Incremental hashing over a one-byte-at-a-time update must agree with the one-shot form.
  Sha256 incremental;
  const std::string payload = "the quick brown fox jumps over the lazy dog";
  for (const char character : payload) {
    incremental.update(std::string_view(&character, 1));
  }
  BSM_CHECK_EQ(incremental.finish().hex(), sha256(payload).hex());
}

BSM_TEST(crc32c_matches_the_published_check_value) {
  const std::string check = "123456789";
  const std::span<const std::uint8_t> bytes(
      reinterpret_cast<const std::uint8_t*>(check.data()), check.size());
  BSM_CHECK_EQ(crc32c(bytes), 0xE3069283u);
  BSM_CHECK_EQ(crc32c(std::span<const std::uint8_t>()), 0u);
}

BSM_TEST(digest_hex_round_trip_and_refusals) {
  const Digest digest = Digest::of("value");
  BSM_CHECK_OK(parsed, Digest::from_hex(digest.hex()));
  BSM_CHECK(parsed == digest);
  BSM_CHECK(!digest.is_zero());
  BSM_CHECK(Digest().is_zero());
  BSM_CHECK(!Digest::from_hex("0011").ok());
  BSM_CHECK(!Digest::from_hex(std::string(64, 'z')).ok());
  const std::string odd(63, 'a');
  BSM_CHECK(!Digest::from_hex(odd).ok());
}

BSM_TEST(canonical_json_sorts_keys_and_escapes_minimally) {
  BSM_CHECK_OK(builder_value, JsonValue::object(JsonValue::Object{}));
  JsonObjectBuilder builder;
  builder.set_text("z", "last");
  builder.set_int("a", -12);
  builder.set_bool("m", true);
  builder.set_text("escape", "quote\" backslash\\ newline\n tab\t");
  builder.set_null("nothing");
  BSM_CHECK_OK(value, builder.build());
  BSM_CHECK_EQ(canonical_json(value),
               std::string("{\"a\":-12,\"escape\":\"quote\\\" backslash\\\\ newline"
                           "\\n tab\\t\",\"m\":true,\"nothing\":null,\"z\":\"last\"}"));
  BSM_CHECK_EQ(canonical_json(builder_value), std::string("{}"));
}

BSM_TEST(canonical_json_refuses_numbers_outside_the_subset) {
  BSM_CHECK(!parse_json("1.5").ok());
  BSM_CHECK(!parse_json("1e3").ok());
  BSM_CHECK(!parse_json("01").ok());
  BSM_CHECK(!parse_json("-0").ok());
  BSM_CHECK(!parse_json("+1").ok());
  BSM_CHECK(!parse_json("9223372036854775808").ok());
  BSM_CHECK_OK(lowest, parse_json("-9223372036854775808"));
  BSM_CHECK_EQ(lowest.as_integer(), std::numeric_limits<std::int64_t>::min());
  BSM_CHECK(!JsonValue::unsigned_integer(
                 static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u)
                 .ok());
}

BSM_TEST(canonical_json_refuses_ambiguous_input) {
  BSM_CHECK(!parse_json("{\"a\":1,\"a\":2}").ok());
  BSM_CHECK(!parse_json("{\"a\":1,}").ok());
  BSM_CHECK(!parse_json("[1,]").ok());
  BSM_CHECK(!parse_json("{").ok());
  BSM_CHECK(!parse_json("\"unterminated").ok());
  BSM_CHECK(!parse_json("").ok());
  BSM_CHECK(!parse_json("null null").ok());
  BSM_CHECK(!parse_json("{\"a\":1} trailing").ok());
  BSM_CHECK(!parse_json("\"raw control \x01\"").ok());
  BSM_CHECK(!parse_json(std::string(200, '[') + std::string(200, ']')).ok());
}

BSM_TEST(canonical_json_decodes_escapes_and_refuses_invalid_utf8) {
  BSM_CHECK_OK(simple, parse_json("\"\\u00e9\""));
  BSM_CHECK_EQ(simple.as_string(), std::string("\xc3\xa9"));
  BSM_CHECK_OK(pair, parse_json("\"\\ud83d\\ude00\""));
  BSM_CHECK_EQ(pair.as_string(), std::string("\xf0\x9f\x98\x80"));
  BSM_CHECK(!parse_json("\"\\ud83d\"").ok());
  BSM_CHECK(!parse_json("\"\\udc00\"").ok());
  BSM_CHECK(!parse_json("\"\\ud83dx\"").ok());
  // An overlong encoding of '/' must be refused.
  BSM_CHECK(!parse_json(std::string("\"\xc0\xaf\"")).ok());
  BSM_CHECK(!is_valid_utf8(std::string("\xff")));
  BSM_CHECK(is_valid_utf8("plain ascii"));
}

BSM_TEST(identifier_validation_is_strict) {
  BSM_CHECK(IdentifierText::is_valid("obligation-1"));
  BSM_CHECK(IdentifierText::is_valid("a:b@c.d_e-f"));
  BSM_CHECK(!IdentifierText::is_valid(""));
  BSM_CHECK(!IdentifierText::is_valid("."));
  BSM_CHECK(!IdentifierText::is_valid(".."));
  BSM_CHECK(!IdentifierText::is_valid(".hidden"));
  BSM_CHECK(!IdentifierText::is_valid("trailing."));
  BSM_CHECK(!IdentifierText::is_valid("with space"));
  BSM_CHECK(!IdentifierText::is_valid("with/slash"));
  BSM_CHECK(!IdentifierText::is_valid("with\\backslash"));
  BSM_CHECK(!IdentifierText::is_valid(std::string(200, 'a')));
  BSM_CHECK(!IdentifierText::is_valid(std::string("high\xc3\xa9")));
  BSM_CHECK(!IdentifierText::is_valid(std::string("nul\0byte", 8)));
  BSM_CHECK_OK(parsed, ObligationId::parse("isolation-a"));
  BSM_CHECK_EQ(parsed.str(), std::string("isolation-a"));
  BSM_CHECK(!ObligationId::parse("bad name").ok());
}

BSM_TEST(raw_identities_round_trip_and_refuse_bad_hex) {
  BSM_CHECK_OK(session, SessionId::generate());
  BSM_CHECK(!session.is_zero());
  BSM_CHECK_OK(again, SessionId::from_hex(session.hex()));
  BSM_CHECK(again == session);
  BSM_CHECK(SessionId().is_zero());
  BSM_CHECK(!SessionId::from_hex("abcd").ok());
  BSM_CHECK(!SessionId::from_hex(std::string(32, 'x')).ok());
  BSM_CHECK_OK(attempt, AttemptId::generate());
  BSM_CHECK_EQ(attempt.hex().size(), std::size_t{64});
  BSM_CHECK_OK(incarnation, IncarnationId::generate());
  // Distinct tag types cannot be compared at all, which is the point of the strong
  // identities: the check is that their texts differ.
  BSM_CHECK(!(incarnation.hex() == session.hex()));
}

BSM_TEST(checked_arithmetic_refuses_overflow_and_underflow) {
  const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
  BSM_CHECK(!checked_add<std::uint64_t>(maximum, 1u).ok());
  BSM_CHECK(!checked_increment<std::uint64_t>(maximum).ok());
  BSM_CHECK(!checked_sub<std::uint64_t>(0u, 1u).ok());
  BSM_CHECK(!checked_mul<std::uint64_t>(maximum, 2u).ok());
  BSM_CHECK_OK(sum, checked_add<std::uint64_t>(maximum - 1u, 1u));
  BSM_CHECK_EQ(sum, maximum);
  BSM_CHECK(!checked_add<std::int64_t>(std::numeric_limits<std::int64_t>::max(), 1).ok());
  BSM_CHECK(!checked_sub<std::int64_t>(std::numeric_limits<std::int64_t>::min(), 1).ok());
  BSM_CHECK(!checked_mul<std::int64_t>(std::numeric_limits<std::int64_t>::max(), 2).ok());
  BSM_CHECK(!checked_mul<std::int64_t>(std::numeric_limits<std::int64_t>::min(), -1).ok());
  BSM_CHECK_OK(product, checked_mul<std::int64_t>(-3, -4));
  BSM_CHECK_EQ(product, 12);
  BSM_CHECK((!checked_within<std::uint64_t, 10ull>(11u, "value").ok()));
  BSM_CHECK((checked_within<std::uint64_t, 10ull>(10u, "value").ok()));
}

BSM_TEST(error_rendering_is_canonical_and_stable) {
  Error error(ErrorCode::GateNotReady, "the gate is not ready");
  const std::string text = error.to_json_text();
  BSM_CHECK_OK(parsed, parse_json(text));
  BSM_CHECK_EQ(canonical_json(parsed), text);
  BSM_CHECK_EQ(std::string(to_string(ErrorCode::GateNotReady)), std::string("gate_not_ready"));
  BSM_CHECK_EQ(text.find("gate_not_ready") != std::string::npos, true);
}

BSM_TEST_MAIN("bsm_test_primitives")
