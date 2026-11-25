#pragma once
/**
 * One ESPHome native API conversation (plaintext, read-only), as a state
 * machine over bytes in and a Sink for bytes out. Framework-free: the same
 * code runs in the firmware HA task (Sink = haSend) and in the host harness
 * (Sink = a socket), and is tested in test/test_esphome_session.
 *
 * Handled: Hello, Authentication (no password), Ping, DeviceInfo,
 * ListEntities, SubscribeStates, Disconnect. Everything else, including
 * SubscribeLogs (28), SubscribeHomeassistantServices (34) and
 * SubscribeHomeAssistantStates (38), is skipped by length with no reply.
 * No command message is handled: nothing on the panel can be controlled.
 *
 * Output is never generated faster than the Sink takes it. Replies owed to
 * the peer are kept as flags and counters, the entity list and the initial
 * states as cursors, and bytes are produced into one fixed buffer only when
 * there is room. A Sink that defers (returns 0) leaves the bytes queued for
 * the next flush(). Nothing here allocates.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esphome_entities.h"
#include "esphome_proto.h"
#include "ha_config.h"

namespace esphome {

enum MsgType : uint32_t {
    MSG_HELLO_REQ = 1,
    MSG_HELLO_RESP = 2,
    MSG_AUTH_REQ = 3,
    MSG_AUTH_RESP = 4,
    MSG_DISCONNECT_REQ = 5,
    MSG_DISCONNECT_RESP = 6,
    MSG_PING_REQ = 7,
    MSG_PING_RESP = 8,
    MSG_DEVICE_INFO_REQ = 9,
    MSG_DEVICE_INFO_RESP = 10,
    MSG_LIST_ENTITIES_REQ = 11,
    MSG_LIST_BINARY = 12,
    MSG_LIST_SENSOR = 16,
    MSG_LIST_TEXT = 18,
    MSG_LIST_DONE = 19,
    MSG_SUBSCRIBE_STATES = 20,
    MSG_STATE_BINARY = 21,
    MSG_STATE_SENSOR = 25,
    MSG_STATE_TEXT = 27,
    MSG_SUBSCRIBE_LOGS = 28,
    MSG_SUBSCRIBE_HA_SERVICES = 34,
    MSG_SUBSCRIBE_HA_STATES = 38,
};

static const uint32_t kApiMajor = 1;
static const uint32_t kApiMinor = 10;
static const int kMaxProtocolErrors = 8;
static const uint8_t kMaxOwedPings = 4;

// Strings must outlive the Session (the firmware keeps them in statics).
struct DeviceIdentity {
    const char* name;              // sonosesp-<mac6>
    const char* friendly_name;     // SonosESP Panel
    const char* mac;               // AA:BB:CC:DD:EE:FF
    const char* esphome_version;   // FIRMWARE_VERSION
    const char* compilation_time;  // __DATE__ " " __TIME__
    const char* model;
    const char* manufacturer;
    const char* project_name;
    const char* project_version;
};

// Output path. fn returns the number of bytes taken (0..len); 0 means
// "deferred, try again later"; a negative value means the link is broken.
struct Sink {
    int (*fn)(void* ctx, const uint8_t* data, size_t len);
    void* ctx;
};

// ---------------------------------------------------------------------------
// Message builders. Each writes one complete frame into out and returns its
// size, or 0 when it does not fit.
// ---------------------------------------------------------------------------

namespace detail {
inline size_t finish(uint8_t* out, size_t cap, uint32_t type, const Writer& w) {
    if (w.overflow()) return 0;
    return frameInPlace(out, cap, type, w.size());
}
inline Writer payloadWriter(uint8_t* out, size_t cap) {
    if (cap < kFrameHeaderMax) return Writer(out, 0);
    return Writer(out + kFrameHeaderMax, cap - kFrameHeaderMax);
}
}  // namespace detail

inline size_t buildEmpty(uint8_t* out, size_t cap, uint32_t type) {
    return writeFrame(out, cap, type, nullptr, 0);
}

inline size_t buildHelloResponse(uint8_t* out, size_t cap, const char* server_info,
                                 const char* name) {
    Writer w = detail::payloadWriter(out, cap);
    w.fieldUint(1, kApiMajor);
    w.fieldUint(2, kApiMinor);
    w.fieldString(3, server_info);
    w.fieldString(4, name);
    return detail::finish(out, cap, MSG_HELLO_RESP, w);
}

// invalid_password = false is the proto3 default, so the payload is empty.
inline size_t buildAuthResponse(uint8_t* out, size_t cap) {
    return buildEmpty(out, cap, MSG_AUTH_RESP);
}

inline size_t buildDeviceInfo(uint8_t* out, size_t cap, const DeviceIdentity& id) {
    Writer w = detail::payloadWriter(out, cap);
    // 1 uses_password = false and 19 api_encryption_supported = false are
    // defaults and therefore omitted.
    w.fieldString(2, id.name);
    w.fieldString(3, id.mac);
    w.fieldString(4, id.esphome_version);
    w.fieldString(5, id.compilation_time);
    w.fieldString(6, id.model);
    w.fieldString(8, id.project_name);
    w.fieldString(9, id.project_version);
    w.fieldString(12, id.manufacturer);
    w.fieldString(13, id.friendly_name);
    return detail::finish(out, cap, MSG_DEVICE_INFO_RESP, w);
}

inline size_t buildListEntity(uint8_t* out, size_t cap, int id) {
    if (id < 0 || id >= E_COUNT) return 0;
    const EntityDef& e = kEntities[id];
    Writer w = detail::payloadWriter(out, cap);
    w.fieldString(1, e.object_id);
    w.fieldFixed32(2, entityKey(id));
    w.fieldString(3, e.name);
    uint32_t type = 0;
    switch (e.kind) {
        case KIND_BINARY:
            type = MSG_LIST_BINARY;
            w.fieldString(5, e.device_class);
            w.fieldUint(9, e.category);
            break;
        case KIND_SENSOR:
            type = MSG_LIST_SENSOR;
            w.fieldString(6, e.unit);
            w.fieldInt(7, e.accuracy_decimals);
            w.fieldString(9, e.device_class);
            w.fieldUint(10, e.state_class);
            w.fieldUint(13, e.category);
            break;
        case KIND_TEXT:
            type = MSG_LIST_TEXT;
            w.fieldUint(7, e.category);
            w.fieldString(8, e.device_class);
            break;
    }
    return detail::finish(out, cap, type, w);
}

inline size_t buildState(uint8_t* out, size_t cap, int id, const Value& v) {
    if (id < 0 || id >= E_COUNT) return 0;
    const EntityDef& e = kEntities[id];
    Writer w = detail::payloadWriter(out, cap);
    w.fieldFixed32(1, entityKey(id));
    uint32_t type = 0;
    switch (e.kind) {
        case KIND_BINARY:
            type = MSG_STATE_BINARY;
            if (!v.missing) w.fieldBool(2, v.flag);
            break;
        case KIND_SENSOR:
            type = MSG_STATE_SENSOR;
            if (!v.missing) w.fieldFloat(2, v.num);
            break;
        case KIND_TEXT:
            type = MSG_STATE_TEXT;
            if (!v.missing) w.fieldString(2, v.text, v.text_len);
            break;
    }
    w.fieldBool(3, v.missing);
    return detail::finish(out, cap, type, w);
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

class Session {
public:
    enum CloseReason : uint8_t {
        CLOSE_NONE = 0,
        CLOSE_PEER_DISCONNECT,  // DisconnectRequest answered
        CLOSE_NOISE,            // client asked for encryption
        CLOSE_BAD_PREAMBLE,
        CLOSE_BAD_VARINT,
        CLOSE_OVERSIZE,
        CLOSE_PROTOCOL_ERRORS,  // kMaxProtocolErrors in a row
        CLOSE_LINK,             // Sink reported a broken link
    };

    Session() { begin(false, DeviceIdentity{}); }

    void begin(bool debug, const DeviceIdentity& id) {
        debug_ = debug;
        id_ = id;
        snprintf(server_info_, sizeof(server_info_), "SonosESP %s",
                 id.esphome_version ? id.esphome_version : "");
        snap_ = nullptr;
        parser_.reset();
        push_.reset();
        out_len_ = 0;
        now_ = 0;
        hello_done_ = false;
        subscribed_ = false;
        closed_ = false;
        reason_ = CLOSE_NONE;
        errors_ = 0;
        client_major_ = 0;
        owe_hello_ = owe_auth_ = owe_devinfo_ = owe_disconnect_ = false;
        disconnect_queued_ = false;
        owe_pings_ = 0;
        list_next_ = -1;
        state_next_ = -1;
    }

    // The HA task's own copy of the latest snapshot. May be null (then every
    // state is sent as missing_state).
    void setSnapshot(const HaSnapshot* s) { snap_ = s; }

    // Bytes received from the peer. Returns false when the caller must close.
    bool feed(const uint8_t* data, size_t len, const Sink& sink, uint32_t now_ms = 0) {
        now_ = now_ms;
        for (size_t i = 0; i < len && !closed_ && !owe_disconnect_; i++) {
            FrameParser::Status st = parser_.push(data[i]);
            if (st == FrameParser::NEED_MORE) continue;
            if (st == FrameParser::FRAME) {
                handle(parser_.type(), parser_.payload(), parser_.length());
                continue;
            }
            close(st == FrameParser::ERR_NOISE          ? CLOSE_NOISE
                  : st == FrameParser::ERR_BAD_PREAMBLE ? CLOSE_BAD_PREAMBLE
                  : st == FrameParser::ERR_OVERSIZE     ? CLOSE_OVERSIZE
                                                        : CLOSE_BAD_VARINT);
        }
        if (!closed_) flush(sink, now_ms);
        return !closed_;
    }

    // Retries queued output and produces more while the Sink takes it.
    bool flush(const Sink& sink, uint32_t now_ms = 0) {
        now_ = now_ms;
        while (!closed_) {
            fill();
            if (!out_len_) break;
            size_t before = out_len_;
            if (!drain(sink)) break;
            if (out_len_ == before) break;  // deferred
            if (out_len_) break;            // partly taken: wait for the next flush
        }
        if (!closed_ && disconnect_queued_ && !out_len_) close(CLOSE_PEER_DISCONNECT);
        return !closed_;
    }

    // Queues state messages for entities whose push rule fires, then flushes.
    // Does nothing until the peer subscribed and the initial states are out.
    bool pushStates(const Sink& sink, uint32_t now_ms) {
        now_ = now_ms;
        if (closed_ || !subscribed_ || owe_disconnect_ || state_next_ >= 0 || !snap_)
            return flush(sink, now_ms);
        int n = entityCount(debug_);
        for (int id = 0; id < n; id++) {
            Value v = valueOf(*snap_, id);
            if (!push_.shouldPush(id, v, now_ms)) continue;
            size_t sz = buildState(out_ + out_len_, sizeof(out_) - out_len_, id, v);
            if (!sz) break;  // buffer full: the rest go next tick
            out_len_ += sz;
            push_.record(id, v, now_ms);
        }
        return flush(sink, now_ms);
    }

    bool closed() const { return closed_; }
    CloseReason closeReason() const { return reason_; }
    bool helloDone() const { return hello_done_; }
    bool subscribed() const { return subscribed_; }
    bool debug() const { return debug_; }
    uint32_t clientMajor() const { return client_major_; }
    int protocolErrors() const { return errors_; }
    int entityTotal() const { return entityCount(debug_); }

    // True while bytes are queued or replies are owed: the firmware closes
    // the client when this stays true for HA_DEFER_LIMIT_MS.
    bool hasPendingOutput() const { return out_len_ > 0 || owesReplies(); }
    size_t queuedBytes() const { return out_len_; }

private:
    bool owesReplies() const {
        return owe_hello_ || owe_auth_ || owe_devinfo_ || owe_pings_ || list_next_ >= 0 ||
               state_next_ >= 0 || (owe_disconnect_ && !disconnect_queued_);
    }

    void close(CloseReason r) {
        if (closed_) return;
        closed_ = true;
        reason_ = r;
    }

    void protocolError() {
        if (++errors_ >= kMaxProtocolErrors) close(CLOSE_PROTOCOL_ERRORS);
    }

    void handle(uint32_t type, const uint8_t* p, size_t len) {
        switch (type) {
            case MSG_HELLO_REQ: {
                bool malformed = false;
                uint32_t major = 0;
                findVarintField(p, len, 2, &major, &malformed);
                if (malformed) {
                    protocolError();
                    return;
                }
                client_major_ = major > 0xFFFF ? 0xFFFF : major;
                hello_done_ = true;
                owe_hello_ = true;
                errors_ = 0;
                return;
            }
            case MSG_PING_REQ:
                if (owe_pings_ < kMaxOwedPings) owe_pings_++;
                errors_ = 0;
                return;
            case MSG_DISCONNECT_REQ:
                // Answer, drop anything not yet produced, then close. Bytes
                // already queued still go out first.
                owe_disconnect_ = true;
                owe_hello_ = owe_auth_ = owe_devinfo_ = false;
                owe_pings_ = 0;
                list_next_ = -1;
                state_next_ = -1;
                return;
            case MSG_AUTH_REQ:
            case MSG_DEVICE_INFO_REQ:
            case MSG_LIST_ENTITIES_REQ:
            case MSG_SUBSCRIBE_STATES:
                if (!hello_done_) {
                    protocolError();
                    return;
                }
                errors_ = 0;
                if (type == MSG_AUTH_REQ) owe_auth_ = true;
                else if (type == MSG_DEVICE_INFO_REQ) owe_devinfo_ = true;
                else if (type == MSG_LIST_ENTITIES_REQ) list_next_ = 0;
                else {
                    subscribed_ = true;
                    state_next_ = 0;
                }
                return;
            default:
                // Unknown or unsupported: skipped by length, no reply.
                return;
        }
    }

    // Appends one frame built by fn; false when it does not fit.
    template <typename F>
    bool append(F fn) {
        size_t sz = fn(out_ + out_len_, sizeof(out_) - out_len_);
        if (sz) {
            out_len_ += sz;
            return true;
        }
        return false;
    }

    // Produces owed output into the buffer while it fits. An item too large
    // even for an empty buffer is dropped so it cannot stall the session.
    void fill() {
        for (;;) {
            bool ok = false;
            if (owe_hello_) {
                ok = append([&](uint8_t* o, size_t c) {
                    return buildHelloResponse(o, c, server_info_, id_.name);
                });
                if (ok || !out_len_) owe_hello_ = false;
            } else if (owe_auth_) {
                ok = append([](uint8_t* o, size_t c) { return buildAuthResponse(o, c); });
                if (ok || !out_len_) owe_auth_ = false;
            } else if (owe_pings_) {
                ok = append([](uint8_t* o, size_t c) { return buildEmpty(o, c, MSG_PING_RESP); });
                if (ok || !out_len_) owe_pings_--;
            } else if (owe_devinfo_) {
                ok = append([&](uint8_t* o, size_t c) { return buildDeviceInfo(o, c, id_); });
                if (ok || !out_len_) owe_devinfo_ = false;
            } else if (list_next_ >= 0) {
                int n = entityCount(debug_);
                int id = list_next_;
                if (id < n) {
                    ok = append([&](uint8_t* o, size_t c) { return buildListEntity(o, c, id); });
                    if (ok || !out_len_) list_next_++;
                } else {
                    ok = append([](uint8_t* o, size_t c) { return buildEmpty(o, c, MSG_LIST_DONE); });
                    if (ok || !out_len_) list_next_ = -1;
                }
            } else if (state_next_ >= 0) {
                int n = entityCount(debug_);
                int id = state_next_;
                if (id >= n) {
                    state_next_ = -1;
                    continue;
                }
                Value v = {};
                v.missing = true;
                if (snap_) v = valueOf(*snap_, id);
                ok = append([&](uint8_t* o, size_t c) { return buildState(o, c, id, v); });
                if (ok) push_.record(id, v, now_);
                if (ok || !out_len_) state_next_++;
            } else if (owe_disconnect_ && !disconnect_queued_) {
                ok = append([](uint8_t* o, size_t c) {
                    return buildEmpty(o, c, MSG_DISCONNECT_RESP);
                });
                // Always the last frame; flush() closes once it is out.
                if (ok) disconnect_queued_ = true;
                return;
            } else {
                return;
            }
            if (!ok) return;
        }
    }

    // Sends queued bytes in chunks. False when the link broke.
    bool drain(const Sink& sink) {
        while (out_len_) {
            size_t chunk = out_len_ < HA_TX_CHUNK_BYTES ? out_len_ : HA_TX_CHUNK_BYTES;
            int r = sink.fn ? sink.fn(sink.ctx, out_, chunk) : -1;
            if (r < 0) {
                close(CLOSE_LINK);
                return false;
            }
            if (r == 0) return true;
            size_t taken = (size_t)r > chunk ? chunk : (size_t)r;
            memmove(out_, out_ + taken, out_len_ - taken);
            out_len_ -= taken;
            if (taken < chunk) return true;
        }
        return true;
    }

    bool debug_;
    DeviceIdentity id_;
    char server_info_[48];
    const HaSnapshot* snap_;
    FrameParser parser_;
    PushState push_;
    uint8_t out_[HA_TX_BUF_BYTES];
    size_t out_len_;
    uint32_t now_;
    bool hello_done_;
    bool subscribed_;
    bool closed_;
    CloseReason reason_;
    int errors_;
    uint32_t client_major_;
    bool owe_hello_, owe_auth_, owe_devinfo_, owe_disconnect_;
    bool disconnect_queued_;
    uint8_t owe_pings_;
    int list_next_;
    int state_next_;
};

}  // namespace esphome
