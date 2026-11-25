/**
 * Host tests for include/esphome_entities.h and include/esphome_session.h
 *
 *     pio test -e native
 *
 * Expected message bytes were produced by aioesphomeapi's own protobuf
 * classes (api_pb2, SerializeToString) and framed by hand, so a passing test
 * means the panel writes what Home Assistant's client library reads.
 */

#include <unity.h>
#include <string.h>
#include "esphome_session.h"

using namespace esphome;

// ---------------------------------------------------------------------------
// Fake sink: records bytes, can defer or take a few bytes at a time.
// ---------------------------------------------------------------------------

struct FakeSink {
    uint8_t buf[16384];
    size_t len;
    int mode;       // 0 take all, 1 defer, 2 take at most `step`, 3 broken
    size_t step;
    int calls;
};

static int sinkFn(void* ctx, const uint8_t* d, size_t n) {
    FakeSink* s = (FakeSink*)ctx;
    s->calls++;
    if (s->mode == 1) return 0;
    if (s->mode == 3) return -1;
    TEST_ASSERT_TRUE_MESSAGE(n <= HA_TX_CHUNK_BYTES, "chunk larger than one segment");
    size_t take = n;
    if (s->mode == 2 && take > s->step) take = s->step;
    TEST_ASSERT_TRUE(s->len + take <= sizeof(s->buf));
    memcpy(s->buf + s->len, d, take);
    s->len += take;
    return (int)take;
}

static FakeSink g_sink;
static Sink sink() { return Sink{sinkFn, &g_sink}; }

struct Frame {
    uint32_t type;
    size_t len;
    uint8_t payload[512];
};
static Frame g_frames[80];
static int g_nframes;

// Splits everything the sink received into frames, then clears the sink.
static int collect() {
    FrameParser p;
    g_nframes = 0;
    for (size_t i = 0; i < g_sink.len; i++) {
        FrameParser::Status s = p.push(g_sink.buf[i]);
        TEST_ASSERT_TRUE(s == FrameParser::NEED_MORE || s == FrameParser::FRAME);
        if (s == FrameParser::FRAME) {
            if (g_nframes >= 80) {  // counted, not stored
                g_nframes++;
                continue;
            }
            Frame& f = g_frames[g_nframes++];
            f.type = p.type();
            f.len = p.length();
            TEST_ASSERT_TRUE(f.len <= sizeof(f.payload));
            memcpy(f.payload, p.payload(), f.len);
        }
    }
    g_sink.len = 0;
    return g_nframes;
}

static const DeviceIdentity kId = {
    "sonosesp-ddeeff", "SonosESP Panel", "AA:BB:CC:DD:EE:FF", "2.4.0", "Jan  1 2026 00:00:00",
    "JC4880P443C 4in (ESP32-P4 v1.0)", "SonosESP", "OpenSurface.SonosESP", "2.4.0",
};

static HaSnapshot g_snap;
static Session g_s;

static void fillSnapshot(HaSnapshot& s) {
    s.clear();
    copyText(s.now_playing, sizeof(s.now_playing), "Artist - Title");
    s.playing = true;
    copyText(s.speaker, sizeof(s.speaker), "Kitchen");
    s.volume = 42;
    s.wifi_rssi = -60;
    s.uptime_s = 1000;
    s.heap_free = 100 * 1024;
    s.dma_free = 60 * 1024;
    for (int i = 0; i < kStackCount; i++) s.stack[i] = 1000 + i;
    copyText(s.reset_reason, sizeof(s.reset_reason), "power-on");
    copyText(s.reboot_history, sizeof(s.reboot_history), "power-on 0");
    s.speaker_count = 3;
    for (int i = 0; i < E_COUNT; i++) s.setValid(i, true);
    s.setValid(E_SYNC_DAILY_AGE, false);
}

static void send(const uint8_t* d, size_t n, uint32_t now = 0) { g_s.feed(d, n, sink(), now); }

static void sendMsg(uint32_t type, const uint8_t* payload = nullptr, size_t n = 0,
                    uint32_t now = 0) {
    uint8_t buf[64];
    size_t sz = writeFrame(buf, sizeof(buf), type, payload, n);
    TEST_ASSERT_TRUE(sz > 0);
    send(buf, sz, now);
}

static const uint8_t kHelloReq[] = {0x00, 0x13, 0x01, 0x0A, 0x0D, 0x61, 0x69, 0x6F,
                                    0x65, 0x73, 0x70, 0x68, 0x6F, 0x6D, 0x65, 0x61,
                                    0x70, 0x69, 0x10, 0x01, 0x18, 0x0A};

void setUp(void) {
    memset(&g_sink, 0, sizeof(g_sink));
    fillSnapshot(g_snap);
    g_s.begin(false, kId);
    g_s.setSnapshot(&g_snap);
}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Entity table
// ---------------------------------------------------------------------------

void test_entity_counts(void) {
    TEST_ASSERT_EQUAL_INT(6, entityCount(false));
    TEST_ASSERT_EQUAL_INT(30, entityCount(true));
    TEST_ASSERT_EQUAL_INT(30, (int)E_COUNT);
}

void test_entity_keys_unique_and_fnv1(void) {
    for (int i = 0; i < E_COUNT; i++) {
        TEST_ASSERT_EQUAL_HEX32(fnv1(kEntities[i].object_id), entityKey(i));
        for (int j = i + 1; j < E_COUNT; j++) {
            TEST_ASSERT_TRUE(entityKey(i) != entityKey(j));
            TEST_ASSERT_TRUE(strcmp(kEntities[i].object_id, kEntities[j].object_id) != 0);
        }
    }
}

// The keys published in the design note, so a renamed id is caught here.
void test_entity_keys_match_design(void) {
    TEST_ASSERT_EQUAL_HEX32(0xF1142A44, entityKey(E_NOW_PLAYING));
    TEST_ASSERT_EQUAL_HEX32(0x6E6F9C21, entityKey(E_PLAYING));
    TEST_ASSERT_EQUAL_HEX32(0xA3DA01BC, entityKey(E_SPEAKER));
    TEST_ASSERT_EQUAL_HEX32(0x8FFEBE43, entityKey(E_VOLUME));
    TEST_ASSERT_EQUAL_HEX32(0x3800D7CC, entityKey(E_WIFI_RSSI));
    TEST_ASSERT_EQUAL_HEX32(0x4EEE9F69, entityKey(E_UPTIME));
    TEST_ASSERT_EQUAL_HEX32(0x86134E68, entityKey(E_HEAP_FREE));
    TEST_ASSERT_EQUAL_HEX32(0x018BC57A, entityKey(E_STACK_LYRICS));
    TEST_ASSERT_EQUAL_HEX32(0x4A8ECB93, entityKey(E_REBOOT_HISTORY));
    TEST_ASSERT_EQUAL_HEX32(0x2A82613A, entityKey(E_HA_TX_DEFERRALS));
    TEST_ASSERT_EQUAL_HEX32(0xDF43F5C6, entityKey(E_SYNC_DAILY_AGE));
    TEST_ASSERT_EQUAL_HEX32(0x006F984A, entityKey(E_COREDUMP_PRESENT));
}

void test_entity_categories(void) {
    for (int i = 0; i < E_COUNT; i++) {
        bool primary = i == E_NOW_PLAYING || i == E_PLAYING || i == E_SPEAKER || i == E_VOLUME;
        TEST_ASSERT_EQUAL_UINT8(primary ? CAT_NONE : CAT_DIAGNOSTIC, kEntities[i].category);
    }
}

void test_copy_text_bounded_utf8(void) {
    char small[4];
    copyText(small, sizeof(small), "ab\xC3\xA9");
    TEST_ASSERT_EQUAL_STRING("ab", small);
    copyText(small, sizeof(small), nullptr);
    TEST_ASSERT_EQUAL_STRING("", small);
}

void test_value_of_rounds_memory_to_kb(void) {
    g_snap.heap_free = 10 * 1024 + 511;
    TEST_ASSERT_EQUAL_FLOAT(10.0f, valueOf(g_snap, E_HEAP_FREE).num);
    g_snap.heap_free = 10 * 1024 + 512;
    TEST_ASSERT_EQUAL_FLOAT(11.0f, valueOf(g_snap, E_HEAP_FREE).num);
    TEST_ASSERT_EQUAL_FLOAT(1002.0f, valueOf(g_snap, E_STACK_NET).num);
    TEST_ASSERT_TRUE(valueOf(g_snap, E_SYNC_DAILY_AGE).missing);
    Value t = valueOf(g_snap, E_SPEAKER);
    TEST_ASSERT_EQUAL_UINT32(7, t.text_len);
}

// ---------------------------------------------------------------------------
// Push rules, fake clock
// ---------------------------------------------------------------------------

static Value num(float f) {
    Value v = {};
    v.num = f;
    return v;
}
static Value txt(const char* s) {
    Value v = {};
    v.text = s;
    v.text_len = strlen(s);
    return v;
}

void test_push_first_always(void) {
    PushState p;
    TEST_ASSERT_TRUE(p.shouldPush(E_UPTIME, num(1), 0));
    TEST_ASSERT_FALSE(p.shouldPush(-1, num(1), 0));
    TEST_ASSERT_FALSE(p.shouldPush(E_COUNT, num(1), 0));
}

void test_push_text_change_with_spacing(void) {
    PushState p;
    p.record(E_NOW_PLAYING, txt("A - 1"), 1000);
    TEST_ASSERT_FALSE(p.shouldPush(E_NOW_PLAYING, txt("A - 1"), 9000));
    TEST_ASSERT_FALSE(p.shouldPush(E_NOW_PLAYING, txt("A - 2"), 2999));
    TEST_ASSERT_TRUE(p.shouldPush(E_NOW_PLAYING, txt("A - 2"), 3000));
}

void test_push_rssi_deadband(void) {
    PushState p;
    p.record(E_WIFI_RSSI, num(-60), 0);
    TEST_ASSERT_FALSE(p.shouldPush(E_WIFI_RSSI, num(-62), 100000));
    TEST_ASSERT_FALSE(p.shouldPush(E_WIFI_RSSI, num(-58), 100000));
    TEST_ASSERT_TRUE(p.shouldPush(E_WIFI_RSSI, num(-63), 1));
    TEST_ASSERT_TRUE(p.shouldPush(E_WIFI_RSSI, num(-57), 1));
}

void test_push_uptime_every_minute(void) {
    PushState p;
    p.record(E_UPTIME, num(100), 0);
    TEST_ASSERT_FALSE(p.shouldPush(E_UPTIME, num(159), 59999));
    TEST_ASSERT_TRUE(p.shouldPush(E_UPTIME, num(160), 60000));
}

void test_push_memory_rounded_and_spaced(void) {
    PushState p;
    HaSnapshot s;
    s.clear();
    s.setValid(E_HEAP_FREE, true);
    s.heap_free = 10240;
    p.record(E_HEAP_FREE, valueOf(s, E_HEAP_FREE), 0);
    s.heap_free = 10300;  // still 10 kB
    TEST_ASSERT_FALSE(p.shouldPush(E_HEAP_FREE, valueOf(s, E_HEAP_FREE), 100000));
    s.heap_free = 20480;
    TEST_ASSERT_FALSE(p.shouldPush(E_HEAP_FREE, valueOf(s, E_HEAP_FREE), 14999));
    TEST_ASSERT_TRUE(p.shouldPush(E_HEAP_FREE, valueOf(s, E_HEAP_FREE), 15000));
}

void test_push_stack_and_counters_on_change(void) {
    PushState p;
    p.record(E_STACK_MAIN, num(1200), 0);
    TEST_ASSERT_FALSE(p.shouldPush(E_STACK_MAIN, num(1200), 500000));
    TEST_ASSERT_TRUE(p.shouldPush(E_STACK_MAIN, num(1100), 1));
    p.record(E_ART_DMA_ABORTS, num(0), 0);
    TEST_ASSERT_TRUE(p.shouldPush(E_ART_DMA_ABORTS, num(1), 0));
    Value b = {};
    b.flag = true;
    p.record(E_PLAYING, b, 0);
    TEST_ASSERT_FALSE(p.shouldPush(E_PLAYING, b, 0));
    b.flag = false;
    TEST_ASSERT_TRUE(p.shouldPush(E_PLAYING, b, 0));
}

void test_push_missing_transitions(void) {
    PushState p;
    Value m = {};
    m.missing = true;
    p.record(E_SYNC_REFRESH_AGE, m, 0);
    TEST_ASSERT_FALSE(p.shouldPush(E_SYNC_REFRESH_AGE, m, 999999));
    TEST_ASSERT_TRUE(p.shouldPush(E_SYNC_REFRESH_AGE, num(5), 1));
    p.record(E_SYNC_REFRESH_AGE, num(5), 1);
    TEST_ASSERT_TRUE(p.shouldPush(E_SYNC_REFRESH_AGE, m, 2));
}

void test_push_millis_wraparound(void) {
    PushState p;
    const uint32_t t0 = 0xFFFFF000u;  // 4096 ms before the wrap
    p.record(E_NOW_PLAYING, txt("a"), t0);
    p.record(E_HEAP_FREE, num(10), t0);
    p.record(E_UPTIME, num(1), t0);
    uint32_t after = 3000;  // 7096 ms after t0
    TEST_ASSERT_TRUE(p.shouldPush(E_NOW_PLAYING, txt("b"), after));
    TEST_ASSERT_FALSE(p.shouldPush(E_HEAP_FREE, num(11), after));
    TEST_ASSERT_TRUE(p.shouldPush(E_HEAP_FREE, num(11), t0 + 15000));
    TEST_ASSERT_FALSE(p.shouldPush(E_UPTIME, num(2), t0 + 59999));
    TEST_ASSERT_TRUE(p.shouldPush(E_UPTIME, num(2), t0 + 60000));
}

// ---------------------------------------------------------------------------
// Builders against bytes from aioesphomeapi's protobuf classes
// ---------------------------------------------------------------------------

void test_build_hello_response(void) {
    const uint8_t want[] = {0x00, 0x25, 0x02, 0x08, 0x01, 0x10, 0x0A, 0x1A, 0x0E, 0x53,
                            0x6F, 0x6E, 0x6F, 0x73, 0x45, 0x53, 0x50, 0x20, 0x32, 0x2E,
                            0x34, 0x2E, 0x30, 0x22, 0x0F, 0x73, 0x6F, 0x6E, 0x6F, 0x73,
                            0x65, 0x73, 0x70, 0x2D, 0x64, 0x64, 0x65, 0x65, 0x66, 0x66};
    uint8_t out[128];
    size_t n = buildHelloResponse(out, sizeof(out), "SonosESP 2.4.0", "sonosesp-ddeeff");
    TEST_ASSERT_EQUAL_UINT32(sizeof(want), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, n);
    TEST_ASSERT_EQUAL_UINT32(0, buildHelloResponse(out, 20, "SonosESP 2.4.0", "sonosesp-ddeeff"));
}

void test_build_device_info(void) {
    const uint8_t want[] = {0x00, 0x38, 0x0A, 0x12, 0x01, 0x6E, 0x1A, 0x11, 0x41, 0x41, 0x3A, 0x42,
                            0x42, 0x3A, 0x43, 0x43, 0x3A, 0x44, 0x44, 0x3A, 0x45, 0x45, 0x3A, 0x46,
                            0x46, 0x22, 0x05, 0x32, 0x2E, 0x34, 0x2E, 0x30, 0x2A, 0x01, 0x74, 0x32,
                            0x01, 0x6D, 0x42, 0x02, 0x70, 0x6E, 0x4A, 0x02, 0x70, 0x76, 0x62, 0x08,
                            0x53, 0x6F, 0x6E, 0x6F, 0x73, 0x45, 0x53, 0x50, 0x6A, 0x01, 0x46};
    DeviceIdentity id = {"n", "F", "AA:BB:CC:DD:EE:FF", "2.4.0", "t", "m", "SonosESP", "pn", "pv"};
    uint8_t out[128];
    size_t n = buildDeviceInfo(out, sizeof(out), id);
    TEST_ASSERT_EQUAL_UINT32(sizeof(want), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, n);
}

void test_build_list_sensor(void) {
    const uint8_t want[] = {0x00, 0x37, 0x10, 0x0A, 0x09, 0x77, 0x69, 0x66, 0x69, 0x5F, 0x72, 0x73,
                            0x73, 0x69, 0x15, 0xCC, 0xD7, 0x00, 0x38, 0x1A, 0x0B, 0x57, 0x69, 0x46,
                            0x69, 0x20, 0x73, 0x69, 0x67, 0x6E, 0x61, 0x6C, 0x32, 0x03, 0x64, 0x42,
                            0x6D, 0x4A, 0x0F, 0x73, 0x69, 0x67, 0x6E, 0x61, 0x6C, 0x5F, 0x73, 0x74,
                            0x72, 0x65, 0x6E, 0x67, 0x74, 0x68, 0x50, 0x01, 0x68, 0x02};
    uint8_t out[128];
    size_t n = buildListEntity(out, sizeof(out), E_WIFI_RSSI);
    TEST_ASSERT_EQUAL_UINT32(sizeof(want), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, n);
}

void test_build_list_text(void) {
    const uint8_t want[] = {0x00, 0x1F, 0x12, 0x0A, 0x0B, 0x6E, 0x6F, 0x77, 0x5F, 0x70, 0x6C, 0x61,
                            0x79, 0x69, 0x6E, 0x67, 0x15, 0x44, 0x2A, 0x14, 0xF1, 0x1A, 0x0B, 0x4E,
                            0x6F, 0x77, 0x20, 0x70, 0x6C, 0x61, 0x79, 0x69, 0x6E, 0x67};
    uint8_t out[128];
    size_t n = buildListEntity(out, sizeof(out), E_NOW_PLAYING);
    TEST_ASSERT_EQUAL_UINT32(sizeof(want), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, n);
}

void test_build_list_binary(void) {
    const uint8_t want[] = {0x00, 0x20, 0x0C, 0x0A, 0x07, 0x70, 0x6C, 0x61, 0x79, 0x69, 0x6E, 0x67,
                            0x15, 0x21, 0x9C, 0x6F, 0x6E, 0x1A, 0x07, 0x50, 0x6C, 0x61, 0x79, 0x69,
                            0x6E, 0x67, 0x2A, 0x07, 0x72, 0x75, 0x6E, 0x6E, 0x69, 0x6E, 0x67};
    const uint8_t want_cd[] = {
        0x00, 0x35, 0x0C, 0x0A, 0x10, 0x63, 0x6F, 0x72, 0x65, 0x64, 0x75, 0x6D, 0x70, 0x5F,
        0x70, 0x72, 0x65, 0x73, 0x65, 0x6E, 0x74, 0x15, 0x4A, 0x98, 0x6F, 0x00, 0x1A, 0x11,
        0x43, 0x6F, 0x72, 0x65, 0x20, 0x64, 0x75, 0x6D, 0x70, 0x20, 0x70, 0x72, 0x65, 0x73,
        0x65, 0x6E, 0x74, 0x2A, 0x07, 0x70, 0x72, 0x6F, 0x62, 0x6C, 0x65, 0x6D, 0x48, 0x02};
    uint8_t out[128];
    size_t n = buildListEntity(out, sizeof(out), E_PLAYING);
    TEST_ASSERT_EQUAL_UINT32(sizeof(want), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, n);
    n = buildListEntity(out, sizeof(out), E_COREDUMP_PRESENT);
    TEST_ASSERT_EQUAL_UINT32(sizeof(want_cd), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_cd, out, n);
    TEST_ASSERT_EQUAL_UINT32(0, buildListEntity(out, sizeof(out), E_COUNT));
}

void test_build_states(void) {
    const uint8_t sensor[] = {0x00, 0x0A, 0x19, 0x0D, 0x43, 0xBE, 0xFE,
                              0x8F, 0x15, 0x00, 0x00, 0x28, 0x42};
    const uint8_t text[] = {0x00, 0x0B, 0x1B, 0x0D, 0x44, 0x2A, 0x14,
                            0xF1, 0x12, 0x04, 0x49, 0x64, 0x6C, 0x65};
    const uint8_t missing[] = {0x00, 0x07, 0x1B, 0x0D, 0xBC, 0x01, 0xDA, 0xA3, 0x18, 0x01};
    const uint8_t binary[] = {0x00, 0x07, 0x15, 0x0D, 0x21, 0x9C, 0x6F, 0x6E, 0x10, 0x01};
    uint8_t out[64];
    size_t n = buildState(out, sizeof(out), E_VOLUME, num(42));
    TEST_ASSERT_EQUAL_UINT32(sizeof(sensor), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sensor, out, n);
    n = buildState(out, sizeof(out), E_NOW_PLAYING, txt("Idle"));
    TEST_ASSERT_EQUAL_UINT32(sizeof(text), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(text, out, n);
    Value m = txt("ignored");
    m.missing = true;
    n = buildState(out, sizeof(out), E_SPEAKER, m);
    TEST_ASSERT_EQUAL_UINT32(sizeof(missing), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(missing, out, n);
    Value b = {};
    b.flag = true;
    n = buildState(out, sizeof(out), E_PLAYING, b);
    TEST_ASSERT_EQUAL_UINT32(sizeof(binary), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(binary, out, n);
}

void test_build_text_state_250_bytes_fits(void) {
    char s[kTextMax + 1];
    memset(s, 'x', kTextMax);
    s[kTextMax] = 0;
    uint8_t out[300];
    size_t n = buildState(out, sizeof(out), E_REBOOT_HISTORY, txt(s));
    // header 0x00, len (2 bytes), type; key 5; state tag, len (2 bytes), 250.
    TEST_ASSERT_EQUAL_UINT32(4 + 5 + 3 + kTextMax, n);
}

// ---------------------------------------------------------------------------
// Session over a fake sink
// ---------------------------------------------------------------------------

static uint32_t keyOf(const Frame& f) {
    // Entity list: key is field 2 (tag 0x15) after object_id; states: field 1 (tag 0x0D) first.
    for (size_t i = 0; i + 5 <= f.len; i++) {
        uint8_t tag = f.payload[i];
        if ((f.type == MSG_STATE_BINARY || f.type == MSG_STATE_SENSOR || f.type == MSG_STATE_TEXT)
                ? tag == 0x0D && i == 0
                : tag == 0x15 && i == 2 + f.payload[1]) {
            return (uint32_t)f.payload[i + 1] | ((uint32_t)f.payload[i + 2] << 8) |
                   ((uint32_t)f.payload[i + 3] << 16) | ((uint32_t)f.payload[i + 4] << 24);
        }
    }
    return 0;
}

static void handshake() {
    send(kHelloReq, sizeof(kHelloReq));
    sendMsg(MSG_AUTH_REQ);
    sendMsg(MSG_DEVICE_INFO_REQ);
    sendMsg(MSG_LIST_ENTITIES_REQ);
    sendMsg(MSG_SUBSCRIBE_STATES);
}

void test_session_full_flow_basic(void) {
    send(kHelloReq, sizeof(kHelloReq));
    TEST_ASSERT_EQUAL_INT(1, collect());
    TEST_ASSERT_EQUAL_UINT32(MSG_HELLO_RESP, g_frames[0].type);
    TEST_ASSERT_TRUE(g_s.helloDone());
    TEST_ASSERT_EQUAL_UINT32(1, g_s.clientMajor());
    uint8_t hello[128];
    size_t hn = buildHelloResponse(hello, sizeof(hello), "SonosESP 2.4.0", "sonosesp-ddeeff");
    TEST_ASSERT_EQUAL_UINT32(hn - 3, g_frames[0].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(hello + 3, g_frames[0].payload, hn - 3);

    sendMsg(MSG_AUTH_REQ);
    TEST_ASSERT_EQUAL_INT(1, collect());
    TEST_ASSERT_EQUAL_UINT32(MSG_AUTH_RESP, g_frames[0].type);
    TEST_ASSERT_EQUAL_UINT32(0, g_frames[0].len);  // invalid_password = false

    sendMsg(MSG_DEVICE_INFO_REQ);
    TEST_ASSERT_EQUAL_INT(1, collect());
    TEST_ASSERT_EQUAL_UINT32(MSG_DEVICE_INFO_RESP, g_frames[0].type);

    sendMsg(MSG_LIST_ENTITIES_REQ);
    TEST_ASSERT_EQUAL_INT(7, collect());
    for (int i = 0; i < 6; i++) {
        uint32_t want = kEntities[i].kind == KIND_TEXT     ? MSG_LIST_TEXT
                        : kEntities[i].kind == KIND_BINARY ? MSG_LIST_BINARY
                                                           : MSG_LIST_SENSOR;
        TEST_ASSERT_EQUAL_UINT32(want, g_frames[i].type);
        TEST_ASSERT_EQUAL_HEX32(entityKey(i), keyOf(g_frames[i]));
    }
    TEST_ASSERT_EQUAL_UINT32(MSG_LIST_DONE, g_frames[6].type);

    TEST_ASSERT_FALSE(g_s.subscribed());
    sendMsg(MSG_SUBSCRIBE_STATES);
    TEST_ASSERT_TRUE(g_s.subscribed());
    TEST_ASSERT_EQUAL_INT(6, collect());
    for (int i = 0; i < 6; i++) TEST_ASSERT_EQUAL_HEX32(entityKey(i), keyOf(g_frames[i]));

    sendMsg(MSG_PING_REQ);
    TEST_ASSERT_EQUAL_INT(1, collect());
    TEST_ASSERT_EQUAL_UINT32(MSG_PING_RESP, g_frames[0].type);

    TEST_ASSERT_FALSE(g_s.closed());
    sendMsg(MSG_DISCONNECT_REQ);
    TEST_ASSERT_EQUAL_INT(1, collect());
    TEST_ASSERT_EQUAL_UINT32(MSG_DISCONNECT_RESP, g_frames[0].type);
    TEST_ASSERT_TRUE(g_s.closed());
    TEST_ASSERT_EQUAL(Session::CLOSE_PEER_DISCONNECT, g_s.closeReason());
    TEST_ASSERT_FALSE(g_s.hasPendingOutput());
}

void test_session_debug_lists_30(void) {
    g_s.begin(true, kId);
    g_s.setSnapshot(&g_snap);
    send(kHelloReq, sizeof(kHelloReq));
    collect();
    sendMsg(MSG_LIST_ENTITIES_REQ);
    // More than one TX buffer of entity messages: produced in several rounds.
    TEST_ASSERT_EQUAL_INT(31, collect());
    for (int i = 0; i < 30; i++) TEST_ASSERT_EQUAL_HEX32(entityKey(i), keyOf(g_frames[i]));
    TEST_ASSERT_EQUAL_UINT32(MSG_LIST_DONE, g_frames[30].type);
    sendMsg(MSG_SUBSCRIBE_STATES);
    TEST_ASSERT_EQUAL_INT(30, collect());
    // sync_daily_age is unavailable: missing_state, no state field.
    const Frame& f = g_frames[E_SYNC_DAILY_AGE];
    TEST_ASSERT_EQUAL_UINT32(MSG_STATE_SENSOR, f.type);
    TEST_ASSERT_EQUAL_UINT32(7, f.len);
    TEST_ASSERT_EQUAL_HEX8(0x18, f.payload[5]);
    TEST_ASSERT_EQUAL_HEX8(0x01, f.payload[6]);
}

void test_session_states_without_snapshot_are_missing(void) {
    g_s.setSnapshot(nullptr);
    send(kHelloReq, sizeof(kHelloReq));
    sendMsg(MSG_SUBSCRIBE_STATES);
    TEST_ASSERT_EQUAL_INT(7, collect());
    for (int i = 1; i < 7; i++) {
        const Frame& f = g_frames[i];
        TEST_ASSERT_EQUAL_UINT32(7, f.len);
        TEST_ASSERT_EQUAL_HEX8(0x18, f.payload[5]);
    }
}

void test_session_whole_handshake_in_one_buffer_byte_at_a_time(void) {
    uint8_t buf[128];
    size_t n = 0;
    memcpy(buf, kHelloReq, sizeof(kHelloReq));
    n += sizeof(kHelloReq);
    const uint32_t types[] = {MSG_AUTH_REQ, MSG_DEVICE_INFO_REQ, MSG_LIST_ENTITIES_REQ,
                              MSG_SUBSCRIBE_STATES, MSG_PING_REQ};
    for (uint32_t t : types) n += writeFrame(buf + n, sizeof(buf) - n, t, nullptr, 0);
    for (size_t i = 0; i < n; i++) send(buf + i, 1);
    // hello, auth, devinfo, 6 list + done, 6 states, ping
    TEST_ASSERT_EQUAL_INT(1 + 1 + 1 + 7 + 6 + 1, collect());
}

void test_session_unknown_and_unsupported_skipped(void) {
    send(kHelloReq, sizeof(kHelloReq));
    collect();
    const uint8_t junk[] = {1, 2, 3, 4, 5};
    sendMsg(99, junk, sizeof(junk));
    sendMsg(MSG_SUBSCRIBE_LOGS, junk, 2);  // level, dump_config
    sendMsg(MSG_SUBSCRIBE_HA_SERVICES);
    sendMsg(MSG_SUBSCRIBE_HA_STATES);
    TEST_ASSERT_EQUAL_INT(0, collect());
    TEST_ASSERT_FALSE(g_s.closed());
    sendMsg(MSG_PING_REQ);
    TEST_ASSERT_EQUAL_INT(1, collect());
    TEST_ASSERT_EQUAL_UINT32(MSG_PING_RESP, g_frames[0].type);
    TEST_ASSERT_EQUAL_INT(0, g_s.protocolErrors());
}

void test_session_eight_errors_close(void) {
    for (int i = 0; i < 7; i++) sendMsg(MSG_LIST_ENTITIES_REQ);  // before Hello
    TEST_ASSERT_FALSE(g_s.closed());
    TEST_ASSERT_EQUAL_INT(7, g_s.protocolErrors());
    sendMsg(MSG_DEVICE_INFO_REQ);
    TEST_ASSERT_TRUE(g_s.closed());
    TEST_ASSERT_EQUAL(Session::CLOSE_PROTOCOL_ERRORS, g_s.closeReason());
    TEST_ASSERT_EQUAL_INT(0, collect());
}

void test_session_errors_reset_by_valid_message(void) {
    for (int i = 0; i < 7; i++) sendMsg(MSG_SUBSCRIBE_STATES);
    send(kHelloReq, sizeof(kHelloReq));
    TEST_ASSERT_EQUAL_INT(0, g_s.protocolErrors());
    const uint8_t bad_hello[] = {0x0A, 0x7F};  // string length past the end
    for (int i = 0; i < 7; i++) sendMsg(MSG_HELLO_REQ, bad_hello, sizeof(bad_hello));
    TEST_ASSERT_FALSE(g_s.closed());
    sendMsg(MSG_HELLO_REQ, bad_hello, sizeof(bad_hello));
    TEST_ASSERT_TRUE(g_s.closed());
}

void test_session_noise_closes(void) {
    const uint8_t noise[] = {0x01, 0x00, 0x00};
    TEST_ASSERT_FALSE(g_s.feed(noise, sizeof(noise), sink(), 0));
    TEST_ASSERT_EQUAL(Session::CLOSE_NOISE, g_s.closeReason());
    TEST_ASSERT_EQUAL_INT(0, collect());
}

void test_session_oversize_closes(void) {
    const uint8_t big[] = {0x00, 0x81, 0x08, 0x01};  // length 1025
    TEST_ASSERT_FALSE(g_s.feed(big, sizeof(big), sink(), 0));
    TEST_ASSERT_EQUAL(Session::CLOSE_OVERSIZE, g_s.closeReason());
}

void test_session_bad_preamble_closes(void) {
    const char* http = "GET / HTTP/1.1\r\n";
    TEST_ASSERT_FALSE(g_s.feed((const uint8_t*)http, strlen(http), sink(), 0));
    TEST_ASSERT_EQUAL(Session::CLOSE_BAD_PREAMBLE, g_s.closeReason());
}

void test_session_deferred_sink_keeps_output(void) {
    g_sink.mode = 1;
    send(kHelloReq, sizeof(kHelloReq));
    sendMsg(MSG_PING_REQ);
    TEST_ASSERT_TRUE(g_s.hasPendingOutput());
    TEST_ASSERT_EQUAL_INT(0, collect());
    g_sink.mode = 0;
    TEST_ASSERT_TRUE(g_s.flush(sink(), 0));
    TEST_ASSERT_EQUAL_INT(2, collect());
    TEST_ASSERT_EQUAL_UINT32(MSG_HELLO_RESP, g_frames[0].type);
    TEST_ASSERT_EQUAL_UINT32(MSG_PING_RESP, g_frames[1].type);
    TEST_ASSERT_FALSE(g_s.hasPendingOutput());
}

void test_session_partial_sink_stream_intact(void) {
    g_s.begin(true, kId);
    g_s.setSnapshot(&g_snap);
    g_sink.mode = 2;
    g_sink.step = 7;
    handshake();
    for (int i = 0; i < 2000 && g_s.hasPendingOutput(); i++) g_s.flush(sink(), 0);
    TEST_ASSERT_FALSE(g_s.hasPendingOutput());
    // hello, auth, devinfo, 30 list + done, 30 states
    TEST_ASSERT_EQUAL_INT(1 + 1 + 1 + 31 + 30, collect());
}

// A ping flood against a stalled link: the queue stays inside the fixed
// buffer plus a small owed counter, whatever the peer sends.
void test_session_ping_flood_bounded(void) {
    g_sink.mode = 1;
    for (int i = 0; i < 1000; i++) sendMsg(MSG_PING_REQ);
    TEST_ASSERT_TRUE(g_s.queuedBytes() <= HA_TX_BUF_BYTES);
    const int in_buffer = (int)(HA_TX_BUF_BYTES / 3);  // a ping reply is 3 bytes
    g_sink.mode = 0;
    for (int i = 0; i < 10 && g_s.hasPendingOutput(); i++) g_s.flush(sink(), 0);
    TEST_ASSERT_EQUAL_INT(in_buffer + kMaxOwedPings, collect());
}

void test_session_broken_link_closes(void) {
    g_sink.mode = 3;
    send(kHelloReq, sizeof(kHelloReq));
    TEST_ASSERT_TRUE(g_s.closed());
    TEST_ASSERT_EQUAL(Session::CLOSE_LINK, g_s.closeReason());
}

void test_session_disconnect_after_queued_output(void) {
    g_sink.mode = 1;
    send(kHelloReq, sizeof(kHelloReq));
    sendMsg(MSG_DISCONNECT_REQ);
    sendMsg(MSG_PING_REQ);  // ignored after a Disconnect
    TEST_ASSERT_FALSE(g_s.closed());
    g_sink.mode = 0;
    g_s.flush(sink(), 0);
    TEST_ASSERT_TRUE(g_s.closed());
    TEST_ASSERT_EQUAL_INT(2, collect());
    TEST_ASSERT_EQUAL_UINT32(MSG_HELLO_RESP, g_frames[0].type);
    TEST_ASSERT_EQUAL_UINT32(MSG_DISCONNECT_RESP, g_frames[1].type);
}

void test_session_push_states_rules(void) {
    handshake();
    collect();
    // Nothing changed: nothing sent.
    TEST_ASSERT_TRUE(g_s.pushStates(sink(), 100));
    TEST_ASSERT_EQUAL_INT(0, collect());
    g_snap.volume = 50;
    copyText(g_snap.now_playing, sizeof(g_snap.now_playing), "Other - Song");
    g_s.pushStates(sink(), 1000);
    TEST_ASSERT_EQUAL_INT(1, collect());  // volume now, text waits for 2 s spacing
    TEST_ASSERT_EQUAL_HEX32(entityKey(E_VOLUME), keyOf(g_frames[0]));
    g_s.pushStates(sink(), 2000);
    TEST_ASSERT_EQUAL_INT(1, collect());
    TEST_ASSERT_EQUAL_HEX32(entityKey(E_NOW_PLAYING), keyOf(g_frames[0]));
    g_s.pushStates(sink(), 2500);
    TEST_ASSERT_EQUAL_INT(0, collect());
}

void test_session_push_before_subscribe_does_nothing(void) {
    send(kHelloReq, sizeof(kHelloReq));
    collect();
    g_snap.volume = 99;
    g_s.pushStates(sink(), 5000);
    TEST_ASSERT_EQUAL_INT(0, collect());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_entity_counts);
    RUN_TEST(test_entity_keys_unique_and_fnv1);
    RUN_TEST(test_entity_keys_match_design);
    RUN_TEST(test_entity_categories);
    RUN_TEST(test_copy_text_bounded_utf8);
    RUN_TEST(test_value_of_rounds_memory_to_kb);
    RUN_TEST(test_push_first_always);
    RUN_TEST(test_push_text_change_with_spacing);
    RUN_TEST(test_push_rssi_deadband);
    RUN_TEST(test_push_uptime_every_minute);
    RUN_TEST(test_push_memory_rounded_and_spaced);
    RUN_TEST(test_push_stack_and_counters_on_change);
    RUN_TEST(test_push_missing_transitions);
    RUN_TEST(test_push_millis_wraparound);
    RUN_TEST(test_build_hello_response);
    RUN_TEST(test_build_device_info);
    RUN_TEST(test_build_list_sensor);
    RUN_TEST(test_build_list_text);
    RUN_TEST(test_build_list_binary);
    RUN_TEST(test_build_states);
    RUN_TEST(test_build_text_state_250_bytes_fits);
    RUN_TEST(test_session_full_flow_basic);
    RUN_TEST(test_session_debug_lists_30);
    RUN_TEST(test_session_states_without_snapshot_are_missing);
    RUN_TEST(test_session_whole_handshake_in_one_buffer_byte_at_a_time);
    RUN_TEST(test_session_unknown_and_unsupported_skipped);
    RUN_TEST(test_session_eight_errors_close);
    RUN_TEST(test_session_errors_reset_by_valid_message);
    RUN_TEST(test_session_noise_closes);
    RUN_TEST(test_session_oversize_closes);
    RUN_TEST(test_session_bad_preamble_closes);
    RUN_TEST(test_session_deferred_sink_keeps_output);
    RUN_TEST(test_session_partial_sink_stream_intact);
    RUN_TEST(test_session_ping_flood_bounded);
    RUN_TEST(test_session_broken_link_closes);
    RUN_TEST(test_session_disconnect_after_queued_output);
    RUN_TEST(test_session_push_states_rules);
    RUN_TEST(test_session_push_before_subscribe_does_nothing);
    return UNITY_END();
}
