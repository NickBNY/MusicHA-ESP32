/**
 * Host tests for include/esphome_proto.h
 *
 *     pio test -e native
 *
 * Every byte this parser sees comes from the network, so the tests lean on
 * malformed input: truncated and overlong varints, oversize frames, the Noise
 * preamble, and frames split at every possible byte boundary.
 */

#include <unity.h>
#include <string.h>
#include "esphome_proto.h"

using namespace esphome;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Varint
// ---------------------------------------------------------------------------

void test_varint_round_trips(void) {
    const uint32_t vals[] = {0, 1, 127, 128, 300, 16383, 16384, 0x0FFFFFFF, 0xFFFFFFFF};
    for (uint32_t v : vals) {
        uint8_t buf[10];
        size_t n = writeVarint(buf, sizeof(buf), v);
        TEST_ASSERT_EQUAL_UINT32(varintSize(v), n);
        uint32_t back = 0x12345678;
        TEST_ASSERT_EQUAL_UINT32(n, readVarint(buf, n, &back));
        TEST_ASSERT_EQUAL_UINT32(v, back);
    }
}

void test_varint_known_bytes(void) {
    uint8_t buf[5];
    TEST_ASSERT_EQUAL_UINT32(2, writeVarint(buf, sizeof(buf), 300));
    TEST_ASSERT_EQUAL_HEX8(0xAC, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, buf[1]);
    TEST_ASSERT_EQUAL_UINT32(5, writeVarint(buf, sizeof(buf), 0xFFFFFFFF));
    TEST_ASSERT_EQUAL_HEX8(0x0F, buf[4]);
}

void test_varint_write_overflow(void) {
    uint8_t buf[1];
    TEST_ASSERT_EQUAL_UINT32(0, writeVarint(buf, 1, 128));
    TEST_ASSERT_EQUAL_UINT32(0, writeVarint(buf, 0, 0));
}

void test_varint_rejects_truncated(void) {
    const uint8_t t1[] = {0x80};
    const uint8_t t2[] = {0xFF, 0xFF, 0xFF};
    uint32_t v;
    TEST_ASSERT_EQUAL_UINT32(0, readVarint(t1, sizeof(t1), &v));
    TEST_ASSERT_EQUAL_UINT32(0, readVarint(t2, sizeof(t2), &v));
    TEST_ASSERT_EQUAL_UINT32(0, readVarint(t1, 0, &v));
}

void test_varint_rejects_overlong(void) {
    const uint8_t six[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
    const uint8_t big5[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x10};  // over 32 bits
    uint32_t v;
    TEST_ASSERT_EQUAL_UINT32(0, readVarint(six, sizeof(six), &v));
    TEST_ASSERT_EQUAL_UINT32(0, readVarint(big5, sizeof(big5), &v));
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

void test_writer_fields(void) {
    uint8_t buf[64];
    Writer w(buf, sizeof(buf));
    w.fieldUint(1, 1);
    w.fieldUint(2, 0);  // default: omitted
    w.fieldBool(3, true);
    w.fieldBool(4, false);
    w.fieldFixed32(5, 0x8FFEBE43);
    w.fieldFloat(6, 42.0f);
    w.fieldFloat(7, 0.0f);
    w.fieldString(8, "ab");
    w.fieldString(9, "");
    w.fieldInt(10, -1);
    const uint8_t want[] = {0x08, 0x01, 0x18, 0x01, 0x2D, 0x43, 0xBE, 0xFE, 0x8F,
                            0x35, 0x00, 0x00, 0x28, 0x42, 0x42, 0x02, 0x61, 0x62,
                            0x50, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
    TEST_ASSERT_FALSE(w.overflow());
    TEST_ASSERT_EQUAL_UINT32(sizeof(want), w.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, buf, sizeof(want));
}

void test_writer_overflow_sticks(void) {
    uint8_t buf[4];
    Writer w(buf, sizeof(buf));
    w.fieldString(1, "hello");
    TEST_ASSERT_TRUE(w.overflow());
    w.fieldUint(2, 1);
    TEST_ASSERT_TRUE(w.overflow());
}

// ---------------------------------------------------------------------------
// Frame writer
// ---------------------------------------------------------------------------

void test_frame_writer_bytes(void) {
    uint8_t out[16];
    const uint8_t pl[] = {0x08, 0x01};
    size_t n = writeFrame(out, sizeof(out), 2, pl, sizeof(pl));
    const uint8_t want[] = {0x00, 0x02, 0x02, 0x08, 0x01};
    TEST_ASSERT_EQUAL_UINT32(sizeof(want), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, n);
    n = writeFrame(out, sizeof(out), 8, nullptr, 0);
    TEST_ASSERT_EQUAL_UINT32(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x08, out[2]);
}

void test_frame_writer_overflow(void) {
    uint8_t out[4];
    const uint8_t pl[] = {1, 2};
    TEST_ASSERT_EQUAL_UINT32(0, writeFrame(out, sizeof(out), 2, pl, sizeof(pl)));
    TEST_ASSERT_EQUAL_UINT32(0, writeFrame(out, 2, 8, nullptr, 0));
    TEST_ASSERT_EQUAL_UINT32(4, writeFrame(out, 4, 2, pl, 1));
}

void test_frame_in_place(void) {
    uint8_t buf[32];
    memset(buf, 0xEE, sizeof(buf));
    buf[kFrameHeaderMax] = 0x08;
    buf[kFrameHeaderMax + 1] = 0x01;
    size_t n = frameInPlace(buf, sizeof(buf), 300, 2);
    const uint8_t want[] = {0x00, 0x02, 0xAC, 0x02, 0x08, 0x01};
    TEST_ASSERT_EQUAL_UINT32(sizeof(want), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, buf, n);
    TEST_ASSERT_EQUAL_UINT32(0, frameInPlace(buf, 7, 1, 2));
}

// ---------------------------------------------------------------------------
// Frame parser
// ---------------------------------------------------------------------------

// A HelloRequest as aioesphomeapi serializes it (client_info, api 1.10).
static const uint8_t kHelloReq[] = {0x00, 0x13, 0x01, 0x0A, 0x0D, 0x61, 0x69, 0x6F,
                                    0x65, 0x73, 0x70, 0x68, 0x6F, 0x6D, 0x65, 0x61,
                                    0x70, 0x69, 0x10, 0x01, 0x18, 0x0A};

void test_parser_whole_frame(void) {
    FrameParser p;
    int frames = 0;
    for (size_t i = 0; i < sizeof(kHelloReq); i++) {
        FrameParser::Status s = p.push(kHelloReq[i]);
        if (s == FrameParser::FRAME) {
            frames++;
            TEST_ASSERT_EQUAL_UINT32(sizeof(kHelloReq) - 1, i);
        } else {
            TEST_ASSERT_EQUAL(FrameParser::NEED_MORE, s);
        }
    }
    TEST_ASSERT_EQUAL_INT(1, frames);
    TEST_ASSERT_EQUAL_UINT32(1, p.type());
    TEST_ASSERT_EQUAL_UINT32(0x13, p.length());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kHelloReq + 3, p.payload(), 0x13);
}

// Split the stream at every point, as recv() may.
void test_parser_fragmented_everywhere(void) {
    for (size_t cut = 0; cut <= sizeof(kHelloReq); cut++) {
        FrameParser p;
        int frames = 0;
        for (size_t i = 0; i < cut; i++)
            if (p.push(kHelloReq[i]) == FrameParser::FRAME) frames++;
        for (size_t i = cut; i < sizeof(kHelloReq); i++)
            if (p.push(kHelloReq[i]) == FrameParser::FRAME) frames++;
        TEST_ASSERT_EQUAL_INT(1, frames);
        TEST_ASSERT_EQUAL_UINT32(1, p.type());
    }
}

void test_parser_back_to_back(void) {
    // Ping, Hello, empty ListEntities, all in one buffer.
    uint8_t buf[64];
    size_t n = 0;
    const uint8_t ping[] = {0x00, 0x00, 0x07};
    const uint8_t list[] = {0x00, 0x00, 0x0B};
    memcpy(buf + n, ping, 3);
    n += 3;
    memcpy(buf + n, kHelloReq, sizeof(kHelloReq));
    n += sizeof(kHelloReq);
    memcpy(buf + n, list, 3);
    n += 3;
    FrameParser p;
    uint32_t types[4] = {};
    int frames = 0;
    for (size_t i = 0; i < n; i++)
        if (p.push(buf[i]) == FrameParser::FRAME && frames < 4) types[frames++] = p.type();
    TEST_ASSERT_EQUAL_INT(3, frames);
    TEST_ASSERT_EQUAL_UINT32(7, types[0]);
    TEST_ASSERT_EQUAL_UINT32(1, types[1]);
    TEST_ASSERT_EQUAL_UINT32(11, types[2]);
}

void test_parser_two_byte_type_and_length(void) {
    // Length 200 (0xC8 0x01), type 300 (0xAC 0x02).
    FrameParser p;
    p.push(0x00);
    p.push(0xC8);
    p.push(0x01);
    p.push(0xAC);
    TEST_ASSERT_EQUAL(FrameParser::NEED_MORE, p.push(0x02));
    FrameParser::Status s = FrameParser::NEED_MORE;
    for (int i = 0; i < 200; i++) s = p.push((uint8_t)i);
    TEST_ASSERT_EQUAL(FrameParser::FRAME, s);
    TEST_ASSERT_EQUAL_UINT32(300, p.type());
    TEST_ASSERT_EQUAL_UINT32(200, p.length());
    TEST_ASSERT_EQUAL_UINT8(199, p.payload()[199]);
}

void test_parser_max_payload_accepted(void) {
    FrameParser p;
    uint8_t len[3];
    size_t ln = writeVarint(len, sizeof(len), kMaxPayload);
    p.push(0x00);
    for (size_t i = 0; i < ln; i++) p.push(len[i]);
    p.push(0x63);
    FrameParser::Status s = FrameParser::NEED_MORE;
    for (size_t i = 0; i < kMaxPayload; i++) s = p.push(0xAA);
    TEST_ASSERT_EQUAL(FrameParser::FRAME, s);
}

void test_parser_oversize(void) {
    FrameParser p;
    uint8_t len[3];
    size_t ln = writeVarint(len, sizeof(len), kMaxPayload + 1);
    p.push(0x00);
    FrameParser::Status s = FrameParser::NEED_MORE;
    for (size_t i = 0; i < ln; i++) s = p.push(len[i]);
    TEST_ASSERT_EQUAL(FrameParser::ERR_OVERSIZE, s);
}

void test_parser_length_varint_too_long(void) {
    FrameParser p;
    p.push(0x00);
    p.push(0x80);
    TEST_ASSERT_EQUAL(FrameParser::NEED_MORE, p.push(0x80));
    // A third byte with the continuation bit set would make a 4-byte varint.
    FrameParser q;
    q.push(0x00);
    q.push(0x80);
    q.push(0x80);
    TEST_ASSERT_EQUAL(FrameParser::ERR_BAD_VARINT, q.push(0x80));
}

void test_parser_type_varint_too_long(void) {
    FrameParser p;
    p.push(0x00);
    p.push(0x00);
    p.push(0x80);
    TEST_ASSERT_EQUAL(FrameParser::ERR_BAD_VARINT, p.push(0x80));
}

void test_parser_noise_and_bad_preamble(void) {
    FrameParser p;
    TEST_ASSERT_EQUAL(FrameParser::ERR_NOISE, p.push(0x01));
    FrameParser q;
    TEST_ASSERT_EQUAL(FrameParser::ERR_BAD_PREAMBLE, q.push('G'));  // "GET /" from a browser
}

void test_parser_error_is_sticky_until_reset(void) {
    FrameParser p;
    p.push(0x05);
    TEST_ASSERT_EQUAL(FrameParser::ERR_BAD_PREAMBLE, p.push(0x00));
    TEST_ASSERT_EQUAL(FrameParser::ERR_BAD_PREAMBLE, p.error());
    p.reset();
    p.push(0x00);
    p.push(0x00);
    TEST_ASSERT_EQUAL(FrameParser::FRAME, p.push(0x07));
}

void test_parser_bad_preamble_on_second_frame(void) {
    FrameParser p;
    p.push(0x00);
    p.push(0x00);
    TEST_ASSERT_EQUAL(FrameParser::FRAME, p.push(0x07));
    TEST_ASSERT_EQUAL(FrameParser::ERR_BAD_PREAMBLE, p.push(0x02));
}

// ---------------------------------------------------------------------------
// Field reader
// ---------------------------------------------------------------------------

void test_find_varint_field(void) {
    const uint8_t* body = kHelloReq + 3;
    size_t len = sizeof(kHelloReq) - 3;
    uint32_t v = 0;
    bool bad = true;
    TEST_ASSERT_TRUE(findVarintField(body, len, 2, &v, &bad));
    TEST_ASSERT_FALSE(bad);
    TEST_ASSERT_EQUAL_UINT32(1, v);
    TEST_ASSERT_TRUE(findVarintField(body, len, 3, &v, &bad));
    TEST_ASSERT_EQUAL_UINT32(10, v);
    TEST_ASSERT_FALSE(findVarintField(body, len, 7, &v, &bad));
    TEST_ASSERT_FALSE(bad);
    TEST_ASSERT_FALSE(findVarintField(nullptr, 0, 2, &v, &bad));
    TEST_ASSERT_FALSE(bad);
}

void test_find_varint_field_malformed(void) {
    const uint8_t len_past_end[] = {0x0A, 0x7F, 0x61};
    const uint8_t trunc_fixed[] = {0x0D, 0x01, 0x02};
    const uint8_t bad_wire[] = {0x0B};
    const uint8_t key_only[] = {0x0A};  // length field with no length
    bool bad = false;
    uint32_t v;
    TEST_ASSERT_FALSE(findVarintField(len_past_end, sizeof(len_past_end), 2, &v, &bad));
    TEST_ASSERT_TRUE(bad);
    TEST_ASSERT_FALSE(findVarintField(trunc_fixed, sizeof(trunc_fixed), 2, &v, &bad));
    TEST_ASSERT_TRUE(bad);
    TEST_ASSERT_FALSE(findVarintField(bad_wire, sizeof(bad_wire), 2, &v, &bad));
    TEST_ASSERT_TRUE(bad);
    TEST_ASSERT_FALSE(findVarintField(key_only, sizeof(key_only), 2, &v, &bad));
    TEST_ASSERT_TRUE(bad);
}

// ---------------------------------------------------------------------------
// FNV-1 and UTF-8
// ---------------------------------------------------------------------------

void test_fnv1_vectors(void) {
    TEST_ASSERT_EQUAL_HEX32(0x811C9DC5, fnv1(""));
    TEST_ASSERT_EQUAL_HEX32(0x050C5D7E, fnv1("a"));
    TEST_ASSERT_EQUAL_HEX32(0xF1142A44, fnv1("now_playing"));
    static_assert(fnv1("a") == 0x050C5D7Eu, "fnv1 must be constexpr");
}

void test_utf8_short_string_kept(void) {
    TEST_ASSERT_EQUAL_UINT32(4, utf8TruncateBytes("Idle"));
    TEST_ASSERT_EQUAL_UINT32(0, utf8TruncateBytes(""));
    TEST_ASSERT_EQUAL_UINT32(0, utf8TruncateBytes(nullptr));
}

void test_utf8_cut_at_ascii(void) {
    char s[300];
    memset(s, 'x', sizeof(s) - 1);
    s[sizeof(s) - 1] = 0;
    TEST_ASSERT_EQUAL_UINT32(250, utf8TruncateBytes(s));
    TEST_ASSERT_EQUAL_UINT32(10, utf8TruncateBytes(s, 10));
}

void test_utf8_never_splits_code_point(void) {
    // "e acute" is 2 bytes, the euro sign 3, an emoji 4.
    const char* two = "ab\xC3\xA9";        // a b e-acute
    const char* three = "a\xE2\x82\xAC";   // a euro
    const char* four = "a\xF0\x9F\x8E\xB5"; // a musical note
    TEST_ASSERT_EQUAL_UINT32(2, utf8TruncateBytes(two, 3));
    TEST_ASSERT_EQUAL_UINT32(4, utf8TruncateBytes(two, 4));
    TEST_ASSERT_EQUAL_UINT32(1, utf8TruncateBytes(three, 2));
    TEST_ASSERT_EQUAL_UINT32(1, utf8TruncateBytes(three, 3));
    TEST_ASSERT_EQUAL_UINT32(4, utf8TruncateBytes(three, 4));
    for (size_t m = 1; m <= 4; m++) TEST_ASSERT_EQUAL_UINT32(1, utf8TruncateBytes(four, m));
    TEST_ASSERT_EQUAL_UINT32(5, utf8TruncateBytes(four, 5));
}

void test_utf8_long_multibyte_at_250(void) {
    // 249 ASCII then a 3-byte euro: the euro straddles 250 and is dropped.
    char s[260];
    memset(s, 'x', 249);
    memcpy(s + 249, "\xE2\x82\xAC", 3);
    s[252] = 0;
    TEST_ASSERT_EQUAL_UINT32(249, utf8TruncateBytes(s));
    // 247 ASCII then the euro: ends exactly at 250 and is kept.
    memset(s, 'x', 247);
    memcpy(s + 247, "\xE2\x82\xAC", 3);
    s[250] = 'y';
    s[251] = 0;
    TEST_ASSERT_EQUAL_UINT32(250, utf8TruncateBytes(s));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_varint_round_trips);
    RUN_TEST(test_varint_known_bytes);
    RUN_TEST(test_varint_write_overflow);
    RUN_TEST(test_varint_rejects_truncated);
    RUN_TEST(test_varint_rejects_overlong);
    RUN_TEST(test_writer_fields);
    RUN_TEST(test_writer_overflow_sticks);
    RUN_TEST(test_frame_writer_bytes);
    RUN_TEST(test_frame_writer_overflow);
    RUN_TEST(test_frame_in_place);
    RUN_TEST(test_parser_whole_frame);
    RUN_TEST(test_parser_fragmented_everywhere);
    RUN_TEST(test_parser_back_to_back);
    RUN_TEST(test_parser_two_byte_type_and_length);
    RUN_TEST(test_parser_max_payload_accepted);
    RUN_TEST(test_parser_oversize);
    RUN_TEST(test_parser_length_varint_too_long);
    RUN_TEST(test_parser_type_varint_too_long);
    RUN_TEST(test_parser_noise_and_bad_preamble);
    RUN_TEST(test_parser_error_is_sticky_until_reset);
    RUN_TEST(test_parser_bad_preamble_on_second_frame);
    RUN_TEST(test_find_varint_field);
    RUN_TEST(test_find_varint_field_malformed);
    RUN_TEST(test_fnv1_vectors);
    RUN_TEST(test_utf8_short_string_kept);
    RUN_TEST(test_utf8_cut_at_ascii);
    RUN_TEST(test_utf8_never_splits_code_point);
    RUN_TEST(test_utf8_long_multibyte_at_250);
    return UNITY_END();
}
