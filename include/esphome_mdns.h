#pragma once
/**
 * Minimal mDNS responder logic for ESPHome discovery: query parser and
 * response / announce / goodbye builder. Framework-free, no sockets, no
 * allocation (test/test_esphome_mdns).
 *
 * Answers only what Home Assistant needs to find the panel:
 *   PTR  _esphomelib._tcp.local           -> <instance>._esphomelib._tcp.local
 *   PTR  _services._dns-sd._udp.local     -> _esphomelib._tcp.local
 *   SRV  <instance>._esphomelib._tcp.local -> <instance>.local:6053
 *   TXT  <instance>._esphomelib._tcp.local  "mac=<12 lower-case hex>"
 *   A    <instance>.local                  -> IPv4
 * No probing (the instance name carries the MAC suffix), no known-answer
 * suppression. Every packet byte is untrusted: names are decoded with bounds
 * checks and a limit on compression pointers so a pointer loop cannot spin.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace esphome {
namespace mdns {

static const uint16_t kPort = 5353;
static const uint16_t kTypeA = 1;
static const uint16_t kTypePTR = 12;
static const uint16_t kTypeTXT = 16;
static const uint16_t kTypeSRV = 33;
static const uint16_t kTypeANY = 255;
static const uint16_t kClassIN = 1;
static const uint16_t kCacheFlush = 0x8000;

static const int kMaxPointerJumps = 16;
static const int kMaxQuestions = 16;
static const size_t kMaxName = 255;
static const uint32_t kAnswerIntervalMs = 1000;

static const char kService[] = "_esphomelib._tcp.local";
static const char kEnum[] = "_services._dns-sd._udp.local";

// Records wanted by a query (bitmask).
enum Want : uint8_t {
    WANT_PTR_SERVICE = 0x01,
    WANT_PTR_ENUM = 0x02,
    WANT_SRV = 0x04,
    WANT_TXT = 0x08,
    WANT_A = 0x10,
    WANT_UNICAST = 0x80,  // a question had the QU bit set
};
static const uint8_t kWantAll = WANT_PTR_SERVICE | WANT_SRV | WANT_TXT | WANT_A;

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

// Decodes the name at off as lower-case dotted text into out (cap >= 1).
// Returns the offset just after the name in the packet, or 0 when the name
// is malformed, runs past the packet, is too long, or loops.
inline size_t readName(const uint8_t* p, size_t len, size_t off, char* out, size_t cap) {
    size_t pos = off, end = 0, n = 0;
    int jumps = 0;
    if (!cap) return 0;
    for (;;) {
        if (pos >= len) return 0;
        uint8_t b = p[pos];
        if (b == 0) {
            if (!end) end = pos + 1;
            break;
        }
        if ((b & 0xC0) == 0xC0) {
            if (pos + 1 >= len) return 0;
            size_t ptr = ((size_t)(b & 0x3F) << 8) | p[pos + 1];
            if (!end) end = pos + 2;
            if (++jumps > kMaxPointerJumps) return 0;
            if (ptr >= pos) return 0;  // only backward pointers
            pos = ptr;
            continue;
        }
        if (b & 0xC0) return 0;  // reserved label types
        if (pos + 1 + b > len) return 0;
        size_t need = (n ? 1 : 0) + b;
        if (n + need > kMaxName || n + need >= cap) return 0;
        if (n) out[n++] = '.';
        for (size_t i = 0; i < b; i++) {
            char c = (char)p[pos + 1 + i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            out[n++] = c;
        }
        pos += 1 + b;
    }
    out[n] = 0;
    return end;
}

// Case-insensitive: does name equal prefix + suffix?
inline bool nameIs(const char* name, const char* prefix, const char* suffix) {
    const char* parts[2] = {prefix ? prefix : "", suffix ? suffix : ""};
    size_t i = 0;
    for (int k = 0; k < 2; k++) {
        for (const char* s = parts[k]; *s; s++, i++) {
            char a = name[i], b = *s;
            if (!a) return false;
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) return false;
        }
    }
    return name[i] == 0;
}

// Returns the records a query asks for (0: not for us, a response, or
// malformed). instance is "sonosesp-<mac6>".
inline uint8_t parseQuery(const uint8_t* p, size_t len, const char* instance) {
    if (!p || len < 12 || !instance) return 0;
    uint16_t flags = rd16(p + 2);
    if (flags & 0x8000) return 0;           // a response
    if ((flags >> 11) & 0x0F) return 0;     // opcode must be QUERY
    int qd = rd16(p + 4);
    if (qd > kMaxQuestions) qd = kMaxQuestions;
    size_t off = 12;
    uint8_t mask = 0;
    char name[kMaxName + 1];
    for (int q = 0; q < qd; q++) {
        size_t next = readName(p, len, off, name, sizeof(name));
        if (!next || next + 4 > len) return 0;
        uint16_t qtype = rd16(p + next);
        uint16_t qclass = rd16(p + next + 2);
        off = next + 4;
        uint16_t cls = qclass & 0x7FFF;
        if (cls != kClassIN && cls != kTypeANY) continue;
        uint8_t got = 0;
        bool any = qtype == kTypeANY;
        if (nameIs(name, kService, nullptr)) {
            if (any || qtype == kTypePTR) got |= WANT_PTR_SERVICE;
        } else if (nameIs(name, kEnum, nullptr)) {
            if (any || qtype == kTypePTR) got |= WANT_PTR_ENUM;
        } else if (nameIs(name, instance, "._esphomelib._tcp.local")) {
            if (any || qtype == kTypeSRV) got |= WANT_SRV;
            if (any || qtype == kTypeTXT) got |= WANT_TXT;
        } else if (nameIs(name, instance, ".local")) {
            if (any || qtype == kTypeA) got |= WANT_A;
        }
        if (got && (qclass & 0x8000)) got |= WANT_UNICAST;
        mask |= got;
    }
    return mask;
}

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

class PacketWriter {
public:
    PacketWriter(uint8_t* b, size_t cap) : b_(b), cap_(cap), n_(0), bad_(false) {}
    size_t size() const { return bad_ ? 0 : n_; }
    bool bad() const { return bad_; }
    size_t pos() const { return n_; }
    void u8(uint8_t v) {
        if (n_ >= cap_) {
            bad_ = true;
            return;
        }
        b_[n_++] = v;
    }
    void u16(uint16_t v) {
        u8((uint8_t)(v >> 8));
        u8((uint8_t)v);
    }
    void u32(uint32_t v) {
        u16((uint16_t)(v >> 16));
        u16((uint16_t)v);
    }
    void bytes(const void* p, size_t len) {
        for (size_t i = 0; i < len; i++) u8(((const uint8_t*)p)[i]);
    }
    // Label-encodes the dotted name a + b (b may be null).
    void name(const char* a, const char* b = nullptr) {
        char tmp[kMaxName + 1];
        int len = snprintf(tmp, sizeof(tmp), "%s%s", a ? a : "", b ? b : "");
        if (len <= 0 || (size_t)len >= sizeof(tmp)) {
            bad_ = true;
            return;
        }
        const char* s = tmp;
        while (*s) {
            const char* dot = strchr(s, '.');
            size_t l = dot ? (size_t)(dot - s) : strlen(s);
            if (l == 0 || l > 63) {
                bad_ = true;
                return;
            }
            u8((uint8_t)l);
            bytes(s, l);
            s += l;
            if (*s == '.') s++;
        }
        u8(0);
    }
    void patch16(size_t at, uint16_t v) {
        if (bad_ || at + 2 > n_) return;
        b_[at] = (uint8_t)(v >> 8);
        b_[at + 1] = (uint8_t)v;
    }

private:
    uint8_t* b_;
    size_t cap_;
    size_t n_;
    bool bad_;
};

struct Host {
    const char* instance;  // sonosesp-<mac6>
    uint8_t ipv4[4];
    uint16_t port;         // 6053
    const char* mac_hex;   // 12 lower-case hex digits
};

namespace detail {
// Writes type, class, ttl and a placeholder rdlength; returns where rdata starts.
inline size_t recordHead(PacketWriter& w, uint16_t type, uint16_t cls, uint32_t ttl) {
    w.u16(type);
    w.u16(cls);
    w.u32(ttl);
    w.u16(0);
    return w.pos();
}
inline void recordEnd(PacketWriter& w, size_t rdata_start) {
    if (w.bad()) return;
    w.patch16(rdata_start - 2, (uint16_t)(w.pos() - rdata_start));
}
}  // namespace detail

// Builds a response with the records in mask (a PTR for the service pulls in
// SRV, TXT and A; SRV pulls in A). Returns the packet size, or 0 when out is
// too small or nothing was asked for.
inline size_t buildResponse(uint8_t mask, const Host& h, uint32_t ttl, uint8_t* out, size_t cap) {
    if (!h.instance || !h.mac_hex) return 0;
    if (mask & WANT_PTR_SERVICE) mask |= WANT_SRV | WANT_TXT | WANT_A;
    if (mask & WANT_SRV) mask |= WANT_A;
    PacketWriter w(out, cap);
    w.u16(0);       // id
    w.u16(0x8400);  // response, authoritative
    w.u16(0);       // questions
    size_t an_at = w.pos();
    w.u16(0);       // answers, patched below
    w.u16(0);
    w.u16(0);
    uint16_t an = 0;
    size_t rd;
    if (mask & WANT_PTR_ENUM) {
        w.name(kEnum);
        rd = detail::recordHead(w, kTypePTR, kClassIN, ttl);
        w.name(kService);
        detail::recordEnd(w, rd);
        an++;
    }
    if (mask & WANT_PTR_SERVICE) {
        w.name(kService);
        rd = detail::recordHead(w, kTypePTR, kClassIN, ttl);
        w.name(h.instance, "._esphomelib._tcp.local");
        detail::recordEnd(w, rd);
        an++;
    }
    if (mask & WANT_SRV) {
        w.name(h.instance, "._esphomelib._tcp.local");
        rd = detail::recordHead(w, kTypeSRV, kCacheFlush | kClassIN, ttl);
        w.u16(0);  // priority
        w.u16(0);  // weight
        w.u16(h.port);
        w.name(h.instance, ".local");
        detail::recordEnd(w, rd);
        an++;
    }
    if (mask & WANT_TXT) {
        w.name(h.instance, "._esphomelib._tcp.local");
        rd = detail::recordHead(w, kTypeTXT, kCacheFlush | kClassIN, ttl);
        size_t ml = strlen(h.mac_hex);
        if (ml > 250) return 0;
        w.u8((uint8_t)(4 + ml));
        w.bytes("mac=", 4);
        w.bytes(h.mac_hex, ml);
        detail::recordEnd(w, rd);
        an++;
    }
    if (mask & WANT_A) {
        w.name(h.instance, ".local");
        rd = detail::recordHead(w, kTypeA, kCacheFlush | kClassIN, ttl);
        w.bytes(h.ipv4, 4);
        detail::recordEnd(w, rd);
        an++;
    }
    if (!an || w.bad()) return 0;
    w.patch16(an_at, an);
    return w.size();
}

// Unsolicited announcement of every record.
inline size_t buildAnnounce(const Host& h, uint32_t ttl, uint8_t* out, size_t cap) {
    return buildResponse(kWantAll, h, ttl, out, cap);
}

// Goodbye: the same records with TTL 0, so caches drop them at once.
inline size_t buildGoodbye(const Host& h, uint8_t* out, size_t cap) {
    return buildResponse(kWantAll, h, 0, out, cap);
}

// ---------------------------------------------------------------------------
// One multicast answer per second at most. Wrap-safe.
// ---------------------------------------------------------------------------

class RateLimiter {
public:
    RateLimiter() : used_(false), last_(0) {}
    bool allow(uint32_t now_ms) {
        if (used_ && (uint32_t)(now_ms - last_) < kAnswerIntervalMs) return false;
        used_ = true;
        last_ = now_ms;
        return true;
    }
    void reset() { used_ = false; }

private:
    bool used_;
    uint32_t last_;
};

// ---------------------------------------------------------------------------
// Names derived from the Wi-Fi MAC.
// ---------------------------------------------------------------------------

// "aabbccddeeff"
inline void formatMacHex(const uint8_t mac[6], char out[13]) {
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// "AA:BB:CC:DD:EE:FF"
inline void formatMacColon(const uint8_t mac[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
}

// "sonosesp-ddeeff"
inline void formatInstance(const uint8_t mac[6], char out[16]) {
    snprintf(out, 16, "sonosesp-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

}  // namespace mdns
}  // namespace esphome
