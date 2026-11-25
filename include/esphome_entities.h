#pragma once
/**
 * Home Assistant entity table, the state snapshot, and the push-rate rules.
 * Framework-free and allocation-free (test/test_esphome_session covers it).
 *
 * The first kBasicCount entities are listed whenever the feature is on; the
 * rest only when Debug is on. Keys are fnv1(object_id), the ESPHome rule.
 * Do not rename an object_id: Home Assistant keys its registry on it.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esphome_proto.h"

namespace esphome {

enum Kind : uint8_t { KIND_TEXT, KIND_BINARY, KIND_SENSOR };

// Protobuf enum values (api.proto EntityCategory, SensorStateClass).
enum Category : uint8_t { CAT_NONE = 0, CAT_CONFIG = 1, CAT_DIAGNOSTIC = 2 };
enum StateClass : uint8_t { SC_NONE = 0, SC_MEASUREMENT = 1, SC_TOTAL_INCREASING = 2 };

// When a changed value is pushed.
enum PushRule : uint8_t {
    RULE_CHANGE,    // on change, no spacing (binary, counters, stack marks, volume)
    RULE_TEXT,      // on change, at least kTextSpacingMs apart
    RULE_RSSI,      // change of at least kRssiDeadband dB
    RULE_PERIODIC,  // at most every kPeriodicMs (uptime, ages)
    RULE_MEMORY,    // value already rounded to 1 kB, at most every kMemorySpacingMs
};

static const uint32_t kTextSpacingMs = 2000;
static const float kRssiDeadband = 3.0f;
static const uint32_t kPeriodicMs = 60000;
static const uint32_t kMemorySpacingMs = 15000;
static const size_t kTextMax = 250;  // Home Assistant caps state at 255 characters

enum EntityId : uint8_t {
    // Basic, always listed.
    E_NOW_PLAYING,
    E_PLAYING,
    E_SPEAKER,
    E_VOLUME,
    E_WIFI_RSSI,
    E_UPTIME,
    // Debug only.
    E_HEAP_FREE,
    E_HEAP_MIN,
    E_HEAP_LARGEST,
    E_DMA_FREE,
    E_PSRAM_FREE,
    E_STACK_MAIN,
    E_STACK_ART,
    E_STACK_NET,
    E_STACK_POLL,
    E_STACK_CLKBG,
    E_STACK_LYRICS,
    E_RESET_REASON,
    E_REBOOT_HISTORY,
    E_UNEXPECTED_REBOOTS,
    E_WIFI_RECONNECTS,
    E_DISCOVERY_RUNS,
    E_ART_DMA_ABORTS,
    E_DMA_RECOVERIES,
    E_HA_SHED_EVENTS,
    E_HA_TX_DEFERRALS,
    E_SPEAKER_COUNT,
    E_SYNC_REFRESH_AGE,
    E_SYNC_DAILY_AGE,
    E_COREDUMP_PRESENT,
    E_COUNT
};

static const int kBasicCount = 6;
static const int kStackCount = 6;

struct EntityDef {
    const char* object_id;
    const char* name;
    Kind kind;
    const char* unit;
    const char* device_class;
    StateClass state_class;
    int8_t accuracy_decimals;
    Category category;
    PushRule rule;
};

// clang-format off
static const EntityDef kEntities[E_COUNT] = {
    {"now_playing",        "Now playing",        KIND_TEXT,   "",    "",                SC_NONE,             0, CAT_NONE,       RULE_TEXT},
    {"playing",            "Playing",            KIND_BINARY, "",    "running",         SC_NONE,             0, CAT_NONE,       RULE_CHANGE},
    {"speaker",            "Speaker",            KIND_TEXT,   "",    "",                SC_NONE,             0, CAT_NONE,       RULE_TEXT},
    {"volume",             "Volume",             KIND_SENSOR, "%",   "",                SC_MEASUREMENT,      0, CAT_NONE,       RULE_CHANGE},
    {"wifi_rssi",          "WiFi signal",        KIND_SENSOR, "dBm", "signal_strength", SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_RSSI},
    {"uptime",             "Uptime",             KIND_SENSOR, "s",   "duration",        SC_TOTAL_INCREASING, 0, CAT_DIAGNOSTIC, RULE_PERIODIC},
    {"heap_free",          "Heap free",          KIND_SENSOR, "kB",  "",                SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_MEMORY},
    {"heap_min",           "Heap minimum",       KIND_SENSOR, "kB",  "",                SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_MEMORY},
    {"heap_largest",       "Heap largest block", KIND_SENSOR, "kB",  "",                SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_MEMORY},
    {"dma_free",           "DMA free",           KIND_SENSOR, "kB",  "",                SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_MEMORY},
    {"psram_free",         "PSRAM free",         KIND_SENSOR, "kB",  "",                SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_MEMORY},
    {"stack_main",         "Stack main",         KIND_SENSOR, "B",   "",                SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_CHANGE},
    {"stack_art",          "Stack art",          KIND_SENSOR, "B",   "",                SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_CHANGE},
    {"stack_net",          "Stack network",      KIND_SENSOR, "B",   "",                SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_CHANGE},
    {"stack_poll",         "Stack poll",         KIND_SENSOR, "B",   "",                SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_CHANGE},
    {"stack_clkbg",        "Stack clock photo",  KIND_SENSOR, "B",   "",                SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_CHANGE},
    {"stack_lyrics",       "Stack lyrics",       KIND_SENSOR, "B",   "",                SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_CHANGE},
    {"reset_reason",       "Reset reason",       KIND_TEXT,   "",    "",                SC_NONE,             0, CAT_DIAGNOSTIC, RULE_TEXT},
    {"reboot_history",     "Reboot history",     KIND_TEXT,   "",    "",                SC_NONE,             0, CAT_DIAGNOSTIC, RULE_TEXT},
    {"unexpected_reboots", "Unexpected reboots", KIND_SENSOR, "",    "",                SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_CHANGE},
    {"wifi_reconnects",    "WiFi reconnects",    KIND_SENSOR, "",    "",                SC_TOTAL_INCREASING, 0, CAT_DIAGNOSTIC, RULE_CHANGE},
    {"discovery_runs",     "Discovery runs",     KIND_SENSOR, "",    "",                SC_TOTAL_INCREASING, 0, CAT_DIAGNOSTIC, RULE_CHANGE},
    {"art_dma_aborts",     "Art DMA aborts",     KIND_SENSOR, "",    "",                SC_TOTAL_INCREASING, 0, CAT_DIAGNOSTIC, RULE_CHANGE},
    {"dma_recoveries",     "DMA recoveries",     KIND_SENSOR, "",    "",                SC_TOTAL_INCREASING, 0, CAT_DIAGNOSTIC, RULE_CHANGE},
    {"ha_shed_events",     "HA shed events",     KIND_SENSOR, "",    "",                SC_TOTAL_INCREASING, 0, CAT_DIAGNOSTIC, RULE_CHANGE},
    {"ha_tx_deferrals",    "HA send deferrals",  KIND_SENSOR, "",    "",                SC_TOTAL_INCREASING, 0, CAT_DIAGNOSTIC, RULE_CHANGE},
    {"speaker_count",      "Speaker count",      KIND_SENSOR, "",    "",                SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_CHANGE},
    {"sync_refresh_age",   "Speaker refresh age", KIND_SENSOR, "s",  "duration",        SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_PERIODIC},
    {"sync_daily_age",     "Daily check age",    KIND_SENSOR, "s",   "duration",        SC_MEASUREMENT,      0, CAT_DIAGNOSTIC, RULE_PERIODIC},
    {"coredump_present",   "Core dump present",  KIND_BINARY, "",    "problem",         SC_NONE,             0, CAT_DIAGNOSTIC, RULE_CHANGE},
};
// clang-format on

inline uint32_t entityKey(int id) { return fnv1(kEntities[id].object_id); }

// Number of entities listed for a session.
inline int entityCount(bool debug) { return debug ? (int)E_COUNT : kBasicCount; }

// ---------------------------------------------------------------------------
// Snapshot: every source value, copied on mainAppTask and handed to the HA
// task. Fixed-size, no String. A clear bit in `valid` means "unavailable"
// and is sent as missing_state.
// ---------------------------------------------------------------------------

struct HaSnapshot {
    char now_playing[kTextMax + 1];
    bool playing;
    char speaker[64];
    float volume;               // percent
    float wifi_rssi;            // dBm
    uint32_t uptime_s;
    uint32_t heap_free;         // bytes; pushed as kB
    uint32_t heap_min;
    uint32_t heap_largest;
    uint32_t dma_free;
    uint32_t psram_free;
    uint32_t stack[kStackCount];  // bytes, order main art net poll clkbg lyrics
    char reset_reason[32];
    char reboot_history[kTextMax + 1];
    uint32_t unexpected_reboots;
    uint32_t wifi_reconnects;
    uint32_t discovery_runs;
    uint32_t art_dma_aborts;
    uint32_t dma_recoveries;
    uint32_t ha_shed_events;
    uint32_t ha_tx_deferrals;
    uint32_t speaker_count;
    uint32_t sync_refresh_age_s;
    uint32_t sync_daily_age_s;
    bool coredump_present;
    uint32_t valid;  // bit (1u << EntityId)

    void clear() {
        memset(this, 0, sizeof(*this));
    }
    void setValid(int id, bool v) {
        if (v) valid |= (1u << id);
        else valid &= ~(1u << id);
    }
    bool isValid(int id) const { return (valid >> id) & 1u; }
};

// Bounded copy into a snapshot text field, cut at a UTF-8 boundary.
inline void copyText(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t n = src ? utf8TruncateBytes(src, cap - 1) : 0;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

// One entity's current value, as it will be sent.
struct Value {
    bool missing;
    bool flag;          // binary
    float num;          // sensor
    const char* text;   // text, not NUL-terminated at text_len
    size_t text_len;
};

inline float roundKb(uint32_t bytes) { return (float)((bytes + 512u) / 1024u); }

inline Value valueOf(const HaSnapshot& s, int id) {
    Value v = {};
    v.missing = !s.isValid(id);
    switch (id) {
        case E_NOW_PLAYING: v.text = s.now_playing; break;
        case E_PLAYING: v.flag = s.playing; break;
        case E_SPEAKER: v.text = s.speaker; break;
        case E_VOLUME: v.num = s.volume; break;
        case E_WIFI_RSSI: v.num = s.wifi_rssi; break;
        case E_UPTIME: v.num = (float)s.uptime_s; break;
        case E_HEAP_FREE: v.num = roundKb(s.heap_free); break;
        case E_HEAP_MIN: v.num = roundKb(s.heap_min); break;
        case E_HEAP_LARGEST: v.num = roundKb(s.heap_largest); break;
        case E_DMA_FREE: v.num = roundKb(s.dma_free); break;
        case E_PSRAM_FREE: v.num = roundKb(s.psram_free); break;
        case E_STACK_MAIN:
        case E_STACK_ART:
        case E_STACK_NET:
        case E_STACK_POLL:
        case E_STACK_CLKBG:
        case E_STACK_LYRICS: v.num = (float)s.stack[id - E_STACK_MAIN]; break;
        case E_RESET_REASON: v.text = s.reset_reason; break;
        case E_REBOOT_HISTORY: v.text = s.reboot_history; break;
        case E_UNEXPECTED_REBOOTS: v.num = (float)s.unexpected_reboots; break;
        case E_WIFI_RECONNECTS: v.num = (float)s.wifi_reconnects; break;
        case E_DISCOVERY_RUNS: v.num = (float)s.discovery_runs; break;
        case E_ART_DMA_ABORTS: v.num = (float)s.art_dma_aborts; break;
        case E_DMA_RECOVERIES: v.num = (float)s.dma_recoveries; break;
        case E_HA_SHED_EVENTS: v.num = (float)s.ha_shed_events; break;
        case E_HA_TX_DEFERRALS: v.num = (float)s.ha_tx_deferrals; break;
        case E_SPEAKER_COUNT: v.num = (float)s.speaker_count; break;
        case E_SYNC_REFRESH_AGE: v.num = (float)s.sync_refresh_age_s; break;
        case E_SYNC_DAILY_AGE: v.num = (float)s.sync_daily_age_s; break;
        case E_COREDUMP_PRESENT: v.flag = s.coredump_present; break;
        default: v.missing = true; break;
    }
    if (v.text) v.text_len = utf8TruncateBytes(v.text, kTextMax);
    return v;
}

// ---------------------------------------------------------------------------
// Push state: what was last sent per entity and when. shouldPush() decides,
// record() commits once the state message is queued. All time arithmetic is
// unsigned subtraction, so it survives millis() wrapping at 2^32.
// ---------------------------------------------------------------------------

inline uint32_t fnv1n(const char* s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h *= 16777619u;
        h ^= (uint8_t)s[i];
    }
    return h;
}

class PushState {
public:
    PushState() { reset(); }

    void reset() {
        memset(sent_, 0, sizeof(sent_));
        memset(missing_, 0, sizeof(missing_));
        memset(fp_, 0, sizeof(fp_));
        memset(num_, 0, sizeof(num_));
        memset(at_, 0, sizeof(at_));
    }

    bool sent(int id) const { return sent_[id]; }

    bool shouldPush(int id, const Value& v, uint32_t now_ms) const {
        if (id < 0 || id >= E_COUNT) return false;
        if (!sent_[id]) return true;
        if (v.missing != missing_[id]) return true;
        if (v.missing) return false;
        const EntityDef& e = kEntities[id];
        uint32_t fp = fingerprint(e.kind, v);
        uint32_t elapsed = now_ms - at_[id];
        switch (e.rule) {
            case RULE_CHANGE: return fp != fp_[id];
            case RULE_TEXT: return fp != fp_[id] && elapsed >= kTextSpacingMs;
            case RULE_RSSI: {
                float d = v.num - num_[id];
                if (d < 0) d = -d;
                return d >= kRssiDeadband;
            }
            case RULE_PERIODIC: return fp != fp_[id] && elapsed >= kPeriodicMs;
            case RULE_MEMORY: return fp != fp_[id] && elapsed >= kMemorySpacingMs;
        }
        return false;
    }

    void record(int id, const Value& v, uint32_t now_ms) {
        if (id < 0 || id >= E_COUNT) return;
        sent_[id] = true;
        missing_[id] = v.missing;
        fp_[id] = v.missing ? 0 : fingerprint(kEntities[id].kind, v);
        num_[id] = v.num;
        at_[id] = now_ms;
    }

    static uint32_t fingerprint(Kind k, const Value& v) {
        if (k == KIND_TEXT) return fnv1n(v.text ? v.text : "", v.text_len);
        if (k == KIND_BINARY) return v.flag ? 1u : 0u;
        uint32_t bits;
        memcpy(&bits, &v.num, 4);
        return bits;
    }

private:
    bool sent_[E_COUNT];
    bool missing_[E_COUNT];
    uint32_t fp_[E_COUNT];
    float num_[E_COUNT];
    uint32_t at_[E_COUNT];
};

}  // namespace esphome
