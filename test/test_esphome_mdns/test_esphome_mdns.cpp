/**
 * Host tests for include/esphome_mdns.h
 *
 *     pio test -e native
 *
 * Every mDNS packet on the LAN reaches the parser, so the malformed cases
 * matter as much as the happy one: compression pointer loops, names that run
 * off the end, responses from other hosts. The query used for the happy path
 * was produced by python-zeroconf (DNSOutgoing), the library Home Assistant
 * uses, and uses name compression.
 */

#include <unity.h>
#include <string.h>
#include "esphome_mdns.h"

using namespace esphome::mdns;

static const char* kInst = "sonosesp-ddeeff";

static Host host() {
    Host h = {kInst, {192, 168, 1, 50}, 6053, "aabbccddeeff"};
    return h;
}

void setUp(void) {}
void tearDown(void) {}

// PTR _esphomelib._tcp.local, SRV <instance> (QU bit), A <instance>.local.
static const uint8_t kQuery[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B, 0x5F,
    0x65, 0x73, 0x70, 0x68, 0x6F, 0x6D, 0x65, 0x6C, 0x69, 0x62, 0x04, 0x5F, 0x74, 0x63,
    0x70, 0x05, 0x6C, 0x6F, 0x63, 0x61, 0x6C, 0x00, 0x00, 0x0C, 0x00, 0x01, 0x0F, 0x73,
    0x6F, 0x6E, 0x6F, 0x73, 0x65, 0x73, 0x70, 0x2D, 0x64, 0x64, 0x65, 0x65, 0x66, 0x66,
    0xC0, 0x0C, 0x00, 0x21, 0x80, 0x01, 0x0F, 0x73, 0x6F, 0x6E, 0x6F, 0x73, 0x65, 0x73,
    0x70, 0x2D, 0x64, 0x64, 0x65, 0x65, 0x66, 0x66, 0xC0, 0x1D, 0x00, 0x01, 0x00, 0x01};

// ---------------------------------------------------------------------------
// Query parsing
// ---------------------------------------------------------------------------

void test_parse_zeroconf_query(void) {
    uint8_t m = parseQuery(kQuery, sizeof(kQuery), kInst);
    TEST_ASSERT_EQUAL_HEX8(WANT_PTR_SERVICE | WANT_SRV | WANT_A | WANT_UNICAST, m);
}

void test_parse_other_instance_ignored(void) {
    TEST_ASSERT_EQUAL_HEX8(WANT_PTR_SERVICE, parseQuery(kQuery, sizeof(kQuery), "sonosesp-000000"));
}

void test_read_name_decompresses(void) {
    char name[256];
    size_t next = readName(kQuery, sizeof(kQuery), 40, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("sonosesp-ddeeff._esphomelib._tcp.local", name);
    TEST_ASSERT_EQUAL_UINT32(58, next);  // after the 2-byte pointer
}

static size_t header(uint8_t* p, uint16_t flags, uint16_t qd) {
    memset(p, 0, 12);
    p[2] = (uint8_t)(flags >> 8);
    p[3] = (uint8_t)flags;
    p[4] = (uint8_t)(qd >> 8);
    p[5] = (uint8_t)qd;
    return 12;
}

static size_t question(uint8_t* p, const char* dotted, uint16_t type, uint16_t cls) {
    size_t n = 0;
    const char* s = dotted;
    while (*s) {
        const char* dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        p[n++] = (uint8_t)l;
        memcpy(p + n, s, l);
        n += l;
        s += l;
        if (*s) s++;
    }
    p[n++] = 0;
    p[n++] = (uint8_t)(type >> 8);
    p[n++] = (uint8_t)type;
    p[n++] = (uint8_t)(cls >> 8);
    p[n++] = (uint8_t)cls;
    return n;
}

void test_parse_each_record_type(void) {
    uint8_t p[512];
    size_t n = header(p, 0, 4);
    n += question(p + n, "_services._dns-sd._udp.local", kTypePTR, kClassIN);
    n += question(p + n, "SonosESP-DDEEFF._ESPHOMELIB._tcp.local", kTypeTXT, kClassIN);
    n += question(p + n, "sonosesp-ddeeff._esphomelib._tcp.local", kTypeSRV, kClassIN);
    n += question(p + n, "sonosesp-ddeeff.local", kTypeA, kClassIN);
    TEST_ASSERT_EQUAL_HEX8(WANT_PTR_ENUM | WANT_TXT | WANT_SRV | WANT_A, parseQuery(p, n, kInst));
}

void test_parse_any_type(void) {
    uint8_t p[256];
    size_t n = header(p, 0, 1);
    n += question(p + n, "sonosesp-ddeeff._esphomelib._tcp.local", kTypeANY, kClassIN);
    TEST_ASSERT_EQUAL_HEX8(WANT_SRV | WANT_TXT, parseQuery(p, n, kInst));
}

void test_parse_wrong_type_or_name(void) {
    uint8_t p[256];
    size_t n = header(p, 0, 3);
    n += question(p + n, "_esphomelib._tcp.local", kTypeA, kClassIN);
    n += question(p + n, "_googlecast._tcp.local", kTypePTR, kClassIN);
    n += question(p + n, "sonosesp-ddeeff.local", kTypeA, 3);  // class CH
    TEST_ASSERT_EQUAL_HEX8(0, parseQuery(p, n, kInst));
}

void test_parse_ignores_responses(void) {
    uint8_t p[256];
    size_t n = header(p, 0x8400, 1);
    n += question(p + n, "_esphomelib._tcp.local", kTypePTR, kClassIN);
    TEST_ASSERT_EQUAL_HEX8(0, parseQuery(p, n, kInst));
}

void test_parse_short_and_null(void) {
    TEST_ASSERT_EQUAL_HEX8(0, parseQuery(kQuery, 11, kInst));
    TEST_ASSERT_EQUAL_HEX8(0, parseQuery(nullptr, 100, kInst));
    TEST_ASSERT_EQUAL_HEX8(0, parseQuery(kQuery, sizeof(kQuery), nullptr));
}

void test_parse_truncated_rejected(void) {
    for (size_t cut = 12; cut < sizeof(kQuery); cut++) {
        uint8_t m = parseQuery(kQuery, cut, kInst);
        TEST_ASSERT_EQUAL_HEX8(0, m);
    }
}

void test_pointer_self_loop_rejected(void) {
    uint8_t p[32];
    size_t n = header(p, 0, 1);
    p[n++] = 0xC0;
    p[n++] = 0x0C;  // points at itself
    p[n++] = 0;
    p[n++] = 12;
    p[n++] = 0;
    p[n++] = 1;
    char name[256];
    TEST_ASSERT_EQUAL_UINT32(0, readName(p, n, 12, name, sizeof(name)));
    TEST_ASSERT_EQUAL_HEX8(0, parseQuery(p, n, kInst));
}

void test_pointer_cycle_rejected(void) {
    // offset 12: label "a", then a pointer back to 12. Each jump goes
    // backwards, so only the jump limit stops it.
    uint8_t p[32];
    size_t n = header(p, 0, 1);
    p[n++] = 1;
    p[n++] = 'a';
    p[n++] = 0xC0;
    p[n++] = 0x0C;
    p[n++] = 0;
    p[n++] = 12;
    p[n++] = 0;
    p[n++] = 1;
    char name[256];
    TEST_ASSERT_EQUAL_UINT32(0, readName(p, n, 12, name, sizeof(name)));
    TEST_ASSERT_EQUAL_HEX8(0, parseQuery(p, n, kInst));
}

void test_pointer_forward_rejected(void) {
    uint8_t p[32];
    size_t n = header(p, 0, 1);
    p[n++] = 0xC0;
    p[n++] = 20;  // forward
    char name[256];
    TEST_ASSERT_EQUAL_UINT32(0, readName(p, n, 12, name, sizeof(name)));
}

void test_reserved_label_type_rejected(void) {
    uint8_t p[32];
    size_t n = header(p, 0, 1);
    p[n++] = 0x41;  // 01xxxxxx
    p[n++] = 0;
    char name[256];
    TEST_ASSERT_EQUAL_UINT32(0, readName(p, n, 12, name, sizeof(name)));
}

void test_name_too_long_for_buffer_rejected(void) {
    uint8_t p[64];
    size_t n = header(p, 0, 1);
    n += question(p + n, "abcdefgh.local", kTypeA, kClassIN);
    char small[8];
    TEST_ASSERT_EQUAL_UINT32(0, readName(p, n, 12, small, sizeof(small)));
}

// ---------------------------------------------------------------------------
// Response builder, checked by walking the packet as a resolver would
// ---------------------------------------------------------------------------

struct Rec {
    char name[256];
    uint16_t type, cls;
    uint32_t ttl;
    size_t rdata;  // offset
    uint16_t rdlen;
};

static int walk(const uint8_t* p, size_t len, Rec* out, int max) {
    TEST_ASSERT_TRUE(len >= 12);
    TEST_ASSERT_EQUAL_HEX16(0x8400, rd16(p + 2));
    TEST_ASSERT_EQUAL_UINT16(0, rd16(p + 4));
    int an = rd16(p + 6);
    TEST_ASSERT_TRUE(an <= max);
    size_t off = 12;
    for (int i = 0; i < an; i++) {
        off = readName(p, len, off, out[i].name, sizeof(out[i].name));
        TEST_ASSERT_TRUE(off > 0 && off + 10 <= len);
        out[i].type = rd16(p + off);
        out[i].cls = rd16(p + off + 2);
        out[i].ttl = ((uint32_t)rd16(p + off + 4) << 16) | rd16(p + off + 6);
        out[i].rdlen = rd16(p + off + 8);
        out[i].rdata = off + 10;
        off += 10 + out[i].rdlen;
        TEST_ASSERT_TRUE(off <= len);
    }
    TEST_ASSERT_EQUAL_UINT32(len, off);
    return an;
}

static const Rec* find(const Rec* r, int n, uint16_t type) {
    for (int i = 0; i < n; i++)
        if (r[i].type == type) return &r[i];
    return nullptr;
}

void test_response_for_service_ptr(void) {
    uint8_t out[512];
    size_t n = buildResponse(WANT_PTR_SERVICE, host(), 120, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    Rec r[8];
    int an = walk(out, n, r, 8);
    TEST_ASSERT_EQUAL_INT(4, an);
    char name[256];

    const Rec* ptr = find(r, an, kTypePTR);
    TEST_ASSERT_NOT_NULL(ptr);
    TEST_ASSERT_EQUAL_STRING("_esphomelib._tcp.local", ptr->name);
    TEST_ASSERT_EQUAL_HEX16(kClassIN, ptr->cls);  // shared record: no cache flush
    readName(out, n, ptr->rdata, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("sonosesp-ddeeff._esphomelib._tcp.local", name);

    const Rec* srv = find(r, an, kTypeSRV);
    TEST_ASSERT_NOT_NULL(srv);
    TEST_ASSERT_EQUAL_STRING("sonosesp-ddeeff._esphomelib._tcp.local", srv->name);
    TEST_ASSERT_EQUAL_HEX16(0x8001, srv->cls);
    TEST_ASSERT_EQUAL_UINT16(6053, rd16(out + srv->rdata + 4));
    readName(out, n, srv->rdata + 6, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("sonosesp-ddeeff.local", name);

    const Rec* txt = find(r, an, kTypeTXT);
    TEST_ASSERT_NOT_NULL(txt);
    TEST_ASSERT_EQUAL_UINT16(17, txt->rdlen);
    TEST_ASSERT_EQUAL_UINT8(16, out[txt->rdata]);
    TEST_ASSERT_EQUAL_MEMORY("mac=aabbccddeeff", out + txt->rdata + 1, 16);

    const Rec* a = find(r, an, kTypeA);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("sonosesp-ddeeff.local", a->name);
    TEST_ASSERT_EQUAL_UINT16(4, a->rdlen);
    const uint8_t ip[] = {192, 168, 1, 50};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ip, out + a->rdata, 4);

    for (int i = 0; i < an; i++) TEST_ASSERT_EQUAL_UINT32(120, r[i].ttl);
}

void test_response_enum_only(void) {
    uint8_t out[256];
    size_t n = buildResponse(WANT_PTR_ENUM, host(), 4500, out, sizeof(out));
    Rec r[4];
    TEST_ASSERT_EQUAL_INT(1, walk(out, n, r, 4));
    TEST_ASSERT_EQUAL_STRING("_services._dns-sd._udp.local", r[0].name);
    char name[256];
    readName(out, n, r[0].rdata, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("_esphomelib._tcp.local", name);
}

void test_response_srv_pulls_in_a(void) {
    uint8_t out[256];
    size_t n = buildResponse(WANT_SRV, host(), 120, out, sizeof(out));
    Rec r[4];
    int an = walk(out, n, r, 4);
    TEST_ASSERT_EQUAL_INT(2, an);
    TEST_ASSERT_NOT_NULL(find(r, an, kTypeSRV));
    TEST_ASSERT_NOT_NULL(find(r, an, kTypeA));
}

void test_response_nothing_or_too_small(void) {
    uint8_t out[512];
    TEST_ASSERT_EQUAL_UINT32(0, buildResponse(0, host(), 120, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT32(0, buildResponse(WANT_UNICAST, host(), 120, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT32(0, buildResponse(WANT_PTR_SERVICE, host(), 120, out, 60));
    Host bad = host();
    bad.instance = nullptr;
    TEST_ASSERT_EQUAL_UINT32(0, buildResponse(WANT_A, bad, 120, out, sizeof(out)));
}

void test_announce_and_goodbye(void) {
    uint8_t out[512];
    size_t n = buildAnnounce(host(), 120, out, sizeof(out));
    Rec r[8];
    int an = walk(out, n, r, 8);
    TEST_ASSERT_EQUAL_INT(4, an);
    for (int i = 0; i < an; i++) TEST_ASSERT_EQUAL_UINT32(120, r[i].ttl);

    n = buildGoodbye(host(), out, sizeof(out));
    an = walk(out, n, r, 8);
    TEST_ASSERT_EQUAL_INT(4, an);
    for (int i = 0; i < an; i++) TEST_ASSERT_EQUAL_UINT32(0, r[i].ttl);
}

// The parser must not answer our own announcements echoed back by the LAN.
void test_own_response_not_treated_as_query(void) {
    uint8_t out[512];
    size_t n = buildAnnounce(host(), 120, out, sizeof(out));
    TEST_ASSERT_EQUAL_HEX8(0, parseQuery(out, n, kInst));
}

// ---------------------------------------------------------------------------
// Rate limiter and names
// ---------------------------------------------------------------------------

void test_rate_limiter(void) {
    RateLimiter r;
    TEST_ASSERT_TRUE(r.allow(5000));
    TEST_ASSERT_FALSE(r.allow(5000));
    TEST_ASSERT_FALSE(r.allow(5999));
    TEST_ASSERT_TRUE(r.allow(6000));
    r.reset();
    TEST_ASSERT_TRUE(r.allow(6001));
}

void test_rate_limiter_wraparound(void) {
    RateLimiter r;
    TEST_ASSERT_TRUE(r.allow(0xFFFFFE00u));  // 512 ms before the wrap
    TEST_ASSERT_FALSE(r.allow(100));         // 612 ms later
    TEST_ASSERT_TRUE(r.allow(488));          // 1000 ms later
}

void test_mac_formats(void) {
    const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    char hex[13], colon[18], inst[16];
    formatMacHex(mac, hex);
    formatMacColon(mac, colon);
    formatInstance(mac, inst);
    TEST_ASSERT_EQUAL_STRING("aabbccddeeff", hex);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", colon);
    TEST_ASSERT_EQUAL_STRING("sonosesp-ddeeff", inst);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_zeroconf_query);
    RUN_TEST(test_parse_other_instance_ignored);
    RUN_TEST(test_read_name_decompresses);
    RUN_TEST(test_parse_each_record_type);
    RUN_TEST(test_parse_any_type);
    RUN_TEST(test_parse_wrong_type_or_name);
    RUN_TEST(test_parse_ignores_responses);
    RUN_TEST(test_parse_short_and_null);
    RUN_TEST(test_parse_truncated_rejected);
    RUN_TEST(test_pointer_self_loop_rejected);
    RUN_TEST(test_pointer_cycle_rejected);
    RUN_TEST(test_pointer_forward_rejected);
    RUN_TEST(test_reserved_label_type_rejected);
    RUN_TEST(test_name_too_long_for_buffer_rejected);
    RUN_TEST(test_response_for_service_ptr);
    RUN_TEST(test_response_enum_only);
    RUN_TEST(test_response_srv_pulls_in_a);
    RUN_TEST(test_response_nothing_or_too_small);
    RUN_TEST(test_announce_and_goodbye);
    RUN_TEST(test_own_response_not_treated_as_query);
    RUN_TEST(test_rate_limiter);
    RUN_TEST(test_rate_limiter_wraparound);
    RUN_TEST(test_mac_formats);
    return UNITY_END();
}
