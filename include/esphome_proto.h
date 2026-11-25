#pragma once
/**
 * ESPHome native API, plaintext framing and protobuf encoding.
 * Framework-free and allocation-free so it compiles on the host
 * (test/test_esphome_proto) and in the host harness.
 *
 * Plaintext frame: 0x00, varint payload length, varint message type, payload.
 * A frame that starts with 0x01 is the Noise protocol, which is not supported.
 *
 * Every inbound byte comes from the network, so the parser is strict:
 * length varint at most 3 bytes and at most kMaxPayload, type varint at most
 * 2 bytes, anything else is an error the caller answers by closing.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ha_config.h"

namespace esphome {

// Protobuf wire types.
enum WireType : uint8_t {
    WT_VARINT = 0,
    WT_FIXED64 = 1,
    WT_LEN = 2,
    WT_FIXED32 = 5,
};

static const size_t kMaxPayload = HA_RX_BUF_BYTES;
// Preamble (1) + length varint (3) + type varint (2).
static const size_t kFrameHeaderMax = 6;

// ---------------------------------------------------------------------------
// Varint
// ---------------------------------------------------------------------------

// Writes v as a varint. Returns the byte count, or 0 when cap is too small.
inline size_t writeVarint(uint8_t* out, size_t cap, uint64_t v) {
    size_t n = 0;
    do {
        if (n >= cap) return 0;
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        out[n++] = b;
    } while (v);
    return n;
}

inline size_t varintSize(uint64_t v) {
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

// Reads a 32-bit varint. Returns bytes consumed, or 0 when the input is
// truncated, longer than 5 bytes, or the value does not fit in 32 bits.
inline size_t readVarint(const uint8_t* in, size_t len, uint32_t* out) {
    uint32_t v = 0;
    for (size_t i = 0; i < 5; i++) {
        if (i >= len) return 0;
        uint8_t b = in[i];
        if (i == 4 && (b & 0xF0)) return 0;
        v |= (uint32_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) {
            if (out) *out = v;
            return i + 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Writer: encodes protobuf fields into a caller buffer. Any write that does
// not fit sets overflow and leaves the buffer unusable for this message.
//
// The field helpers omit proto3 default values (0, false, empty string),
// as protobuf itself does. fixed32 keys are always written.
// ---------------------------------------------------------------------------

class Writer {
public:
    Writer(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap), len_(0), overflow_(false) {}

    size_t size() const { return len_; }
    bool overflow() const { return overflow_; }
    const uint8_t* data() const { return buf_; }

    void raw(const void* p, size_t n) {
        if (overflow_ || n > cap_ - len_) {
            overflow_ = true;
            return;
        }
        if (n) memcpy(buf_ + len_, p, n);
        len_ += n;
    }

    void varint(uint64_t v) {
        if (overflow_) return;
        size_t n = writeVarint(buf_ + len_, cap_ - len_, v);
        if (!n) {
            overflow_ = true;
            return;
        }
        len_ += n;
    }

    void tag(uint32_t field, WireType wt) { varint(((uint64_t)field << 3) | wt); }

    void fixed32Raw(uint32_t v) {
        uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        raw(b, 4);
    }

    // Field helpers.
    void fieldUint(uint32_t field, uint32_t v) {
        if (!v) return;
        tag(field, WT_VARINT);
        varint(v);
    }
    // int32: negative values are sign-extended to 10 bytes, as protobuf does.
    void fieldInt(uint32_t field, int32_t v) {
        if (!v) return;
        tag(field, WT_VARINT);
        varint((uint64_t)(int64_t)v);
    }
    void fieldBool(uint32_t field, bool v) {
        if (!v) return;
        tag(field, WT_VARINT);
        varint(1);
    }
    void fieldFixed32(uint32_t field, uint32_t v) {
        tag(field, WT_FIXED32);
        fixed32Raw(v);
    }
    void fieldFloat(uint32_t field, float f) {
        uint32_t v;
        memcpy(&v, &f, 4);
        if (!v) return;  // +0.0 is the default
        tag(field, WT_FIXED32);
        fixed32Raw(v);
    }
    void fieldString(uint32_t field, const char* s, size_t n) {
        if (!s || !n) return;
        tag(field, WT_LEN);
        varint(n);
        raw(s, n);
    }
    void fieldString(uint32_t field, const char* s) { fieldString(field, s, s ? strlen(s) : 0); }

private:
    uint8_t* buf_;
    size_t cap_;
    size_t len_;
    bool overflow_;
};

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

// Size of the header for a given payload length and type.
inline size_t frameHeaderSize(uint32_t type, size_t payload_len) {
    return 1 + varintSize(payload_len) + varintSize(type);
}

// Writes a complete frame into out. Returns its size, or 0 when it does not
// fit or the payload is larger than the 3-byte length varint can carry.
inline size_t writeFrame(uint8_t* out, size_t cap, uint32_t type, const uint8_t* payload,
                         size_t payload_len) {
    if (payload_len > 0x1FFFFF || type > 0x3FFF) return 0;
    size_t hdr = frameHeaderSize(type, payload_len);
    if (cap < hdr || payload_len > cap - hdr) return 0;
    size_t n = 0;
    out[n++] = 0x00;
    n += writeVarint(out + n, cap - n, payload_len);
    n += writeVarint(out + n, cap - n, type);
    if (payload_len) memmove(out + n, payload, payload_len);
    return n + payload_len;
}

// For building in place: the payload was written at buf + kFrameHeaderMax.
// Moves it down behind the real header. Returns the frame size or 0.
inline size_t frameInPlace(uint8_t* buf, size_t cap, uint32_t type, size_t payload_len) {
    if (cap < kFrameHeaderMax || payload_len > cap - kFrameHeaderMax) return 0;
    return writeFrame(buf, cap, type, buf + kFrameHeaderMax, payload_len);
}

// Incremental parser. Feed bytes in any chunking, one at a time is fine.
// After FRAME, type()/payload()/length() are valid until the next push().
// After an error the parser stays in that error until reset().
class FrameParser {
public:
    enum Status : uint8_t {
        NEED_MORE = 0,
        FRAME,
        ERR_NOISE,         // first byte 0x01: client wants encryption
        ERR_BAD_PREAMBLE,  // first byte neither 0x00 nor 0x01
        ERR_BAD_VARINT,    // length over 3 bytes or type over 2 bytes
        ERR_OVERSIZE,      // payload over kMaxPayload
    };

    FrameParser() { reset(); }

    void reset() {
        state_ = S_PREAMBLE;
        err_ = NEED_MORE;
        len_ = 0;
        type_ = 0;
        got_ = 0;
        vbytes_ = 0;
    }

    Status error() const { return err_; }
    uint32_t type() const { return type_; }
    const uint8_t* payload() const { return buf_; }
    size_t length() const { return len_; }

    Status push(uint8_t b) {
        switch (state_) {
            case S_ERROR:
                return err_;
            case S_DONE:
                // Previous frame consumed: this byte starts the next one.
                state_ = S_PREAMBLE;
                [[fallthrough]];
            case S_PREAMBLE:
                if (b == 0x01) return fail(ERR_NOISE);
                if (b != 0x00) return fail(ERR_BAD_PREAMBLE);
                state_ = S_LEN;
                len_ = 0;
                type_ = 0;
                got_ = 0;
                vbytes_ = 0;
                return NEED_MORE;
            case S_LEN:
                len_ |= (size_t)(b & 0x7F) << (7 * vbytes_);
                vbytes_++;
                if (b & 0x80) {
                    if (vbytes_ >= 3) return fail(ERR_BAD_VARINT);
                    return NEED_MORE;
                }
                if (len_ > kMaxPayload) return fail(ERR_OVERSIZE);
                state_ = S_TYPE;
                vbytes_ = 0;
                return NEED_MORE;
            case S_TYPE:
                type_ |= (uint32_t)(b & 0x7F) << (7 * vbytes_);
                vbytes_++;
                if (b & 0x80) {
                    if (vbytes_ >= 2) return fail(ERR_BAD_VARINT);
                    return NEED_MORE;
                }
                if (len_ == 0) {
                    state_ = S_DONE;
                    return FRAME;
                }
                state_ = S_PAYLOAD;
                return NEED_MORE;
            case S_PAYLOAD:
                buf_[got_++] = b;
                if (got_ == len_) {
                    state_ = S_DONE;
                    return FRAME;
                }
                return NEED_MORE;
        }
        return NEED_MORE;
    }

private:
    enum State : uint8_t { S_PREAMBLE, S_LEN, S_TYPE, S_PAYLOAD, S_DONE, S_ERROR };

    Status fail(Status e) {
        state_ = S_ERROR;
        err_ = e;
        return e;
    }

    State state_;
    Status err_;
    size_t len_;
    uint32_t type_;
    size_t got_;
    uint8_t vbytes_;
    uint8_t buf_[kMaxPayload];
};

// ---------------------------------------------------------------------------
// Reading one field of a received message (only Hello is ever read).
// Returns true and the value when a varint field with that number exists.
// Returns false when absent or the body is malformed; *malformed tells which.
// ---------------------------------------------------------------------------

inline bool findVarintField(const uint8_t* p, size_t len, uint32_t field, uint32_t* out,
                            bool* malformed) {
    size_t i = 0;
    bool found = false;
    bool bad = false;
    if (malformed) *malformed = false;
    while (i < len) {
        uint32_t key;
        size_t n = readVarint(p + i, len - i, &key);
        if (!n) { bad = true; break; }
        i += n;
        uint32_t wt = key & 7, fn = key >> 3;
        if (wt == WT_VARINT) {
            uint32_t v;
            n = readVarint(p + i, len - i, &v);
            if (!n) {
                // Tolerate 64-bit varints of other fields by skipping up to 10 bytes.
                size_t k = 0;
                while (k < 10 && i + k < len && (p[i + k] & 0x80)) k++;
                if (k >= 10 || i + k >= len) { bad = true; break; }
                i += k + 1;
                continue;
            }
            i += n;
            if (fn == field) {
                if (out) *out = v;
                found = true;
            }
        } else if (wt == WT_LEN) {
            uint32_t l;
            n = readVarint(p + i, len - i, &l);
            if (!n || l > len - i - n) { bad = true; break; }
            i += n + l;
        } else if (wt == WT_FIXED32) {
            if (len - i < 4) { bad = true; break; }
            i += 4;
        } else if (wt == WT_FIXED64) {
            if (len - i < 8) { bad = true; break; }
            i += 8;
        } else {
            bad = true;  // groups (3, 4) and reserved wire types
            break;
        }
    }
    if (bad || i != len) {
        if (malformed) *malformed = true;
        return false;
    }
    return found;
}

// ---------------------------------------------------------------------------
// FNV-1 (multiply then xor), the 32-bit hash ESPHome uses for entity keys.
// ---------------------------------------------------------------------------

constexpr uint32_t fnv1(const char* s) {
    uint32_t h = 2166136261u;
    while (s && *s) {
        h *= 16777619u;
        h ^= (uint8_t)*s++;
    }
    return h;
}

// ---------------------------------------------------------------------------
// UTF-8 truncation: the number of bytes of str to keep so the result is at
// most maxBytes long and never ends inside a multi-byte code point.
// ---------------------------------------------------------------------------

inline size_t utf8TruncateBytes(const char* str, size_t maxBytes = 250) {
    if (!str) return 0;
    size_t n = 0;
    while (str[n] && n <= maxBytes) n++;
    if (n <= maxBytes) return n;
    n = maxBytes;
    // Byte n is the first byte cut off. Back up while it is a continuation
    // byte, so the cut lands on the start of the straddling code point.
    while (n > 0 && ((uint8_t)str[n] & 0xC0) == 0x80) n--;
    return n;
}

}  // namespace esphome
