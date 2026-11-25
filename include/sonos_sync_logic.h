#pragma once
/**
 * Background upkeep: the scheduling arithmetic, framework-free.
 *
 * Split out of sonos_sync.cpp so the timing rules can be tested on the host
 * (test/test_sync). Nothing here touches Arduino, FreeRTOS, LVGL or the network:
 * the caller passes the clock, the speaker count and the heap figures in.
 *
 * Every comparison against millis() goes through due()/elapsed(), which subtract
 * first and so keep working across the 49.7-day wrap of a 32-bit millisecond clock.
 */

#include <stddef.h>
#include <stdint.h>

namespace sonos_sync {

// `at` has been reached. Wrap-safe for deadlines up to 24.8 days ahead.
inline bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

// At least `span` ms have passed since `since`. Wrap-safe.
inline bool elapsed(uint32_t now, uint32_t since, uint32_t span) {
    return (uint32_t)(now - since) >= span;
}

// How long since `lastTouch`, for a timestamp another task writes. When the
// touch lands after `now` was read, now - lastTouch is "negative" and would read
// as about 49 days of idleness; that is clamped to 0 (just touched).
inline uint32_t untouchedMs(uint32_t now, uint32_t lastTouch) {
    const int32_t d = (int32_t)(now - lastTouch);
    return d < 0 ? 0 : (uint32_t)d;
}

// ---------------------------------------------------------------------------
// Speaker refresh schedule
// ---------------------------------------------------------------------------
struct Timing {
    uint32_t firstMs;        // first refresh after the Sonos tasks start
    uint32_t intervalMs;     // regular refresh (the 4-hour floor)
    uint32_t zeroRetryMs;    // no speakers known: look again this soon
    uint32_t idleRefreshMs;  // opportunistic refresh this long after the last one...
    uint32_t idleQuietMs;    // ...once the panel has been untouched this long
    uint32_t dailyMs;        // selection + memory check
};

struct Schedule {
    bool     started;
    uint32_t nextRefresh;    // regular deadline
    uint32_t lastRefresh;    // when the last refresh finished (or the schedule started)
    uint32_t nextDaily;      // selection check (sync task)
    uint32_t nextMem;        // memory check (inline on the scheduler, no task)
};

inline void start(Schedule& s, uint32_t now, const Timing& t) {
    s.started     = true;
    s.nextRefresh = now + t.firstMs;
    s.lastRefresh = now;
    s.nextDaily   = now + t.dailyMs;
    s.nextMem     = now + t.dailyMs;
}

// Whether a refresh should run now. `known` is the number of speakers in the
// list, `untouchedMs` how long since the last touch. A refresh that finds the
// panel busy is postponed by the caller, never dropped: this keeps returning true.
inline bool refreshDue(const Schedule& s, uint32_t now, int known, uint32_t untouchedMs,
                       const Timing& t) {
    if (!s.started) return false;
    if (due(now, s.nextRefresh)) return true;
    // Opportunistic: a quiet panel refreshes more often than the 4-hour floor.
    if (known > 0 && untouchedMs >= t.idleQuietMs && elapsed(now, s.lastRefresh, t.idleRefreshMs))
        return true;
    return false;
}

// Record a finished refresh. `found` is the number of speakers afterwards; with
// none, the next attempt comes after zeroRetryMs instead of the full interval.
inline void refreshDone(Schedule& s, uint32_t now, int found, const Timing& t) {
    s.lastRefresh = now;
    s.nextRefresh = now + (found > 0 ? t.intervalMs : t.zeroRetryMs);
}

inline bool dailyDue(const Schedule& s, uint32_t now) {
    return s.started && due(now, s.nextDaily);
}

inline void dailyDone(Schedule& s, uint32_t now, const Timing& t) {
    s.nextDaily = now + t.dailyMs;
}

inline bool memDue(const Schedule& s, uint32_t now) {
    return s.started && due(now, s.nextMem);
}

inline void memDone(Schedule& s, uint32_t now, const Timing& t) {
    s.nextMem = now + t.dailyMs;
}

// A task create failed (no contiguous block for its stack): try again this much
// later instead of on the next 1 s scheduler pass.
inline void postpone(uint32_t& retryAt, uint32_t now, uint32_t ms) { retryAt = now + ms; }

// What a scheduled refresh does.
//
//   TOPOLOGY   speakers are known: ask one of them for GetZoneGroupState and
//              update the list in place. If none of them answers, a short SSDP
//              probe finds any speaker and the same topology call is made to it.
//              The list is never emptied, playing or not.
//   FULL_SCAN  the list is empty, so there is nothing to lose: the regular
//              discovery (SSDP + description per speaker) rebuilds it.
enum Plan : uint8_t { PLAN_TOPOLOGY, PLAN_FULL_SCAN };
inline Plan planRefresh(int known) { return known > 0 ? PLAN_TOPOLOGY : PLAN_FULL_SCAN; }

// ---------------------------------------------------------------------------
// Deferred bring-up back-off (no speaker reachable since boot)
// ---------------------------------------------------------------------------
// base, 2*base, 4*base ... capped. failures == 0 means no wait.
inline uint32_t backoffMs(uint8_t failures, uint32_t base, uint32_t cap) {
    if (failures == 0) return 0;
    uint32_t w = base;
    for (uint8_t i = 1; i < failures && w < cap; i++) w = (w > cap / 2) ? cap : w * 2;
    return w > cap ? cap : w;
}

// ---------------------------------------------------------------------------
// Daily memory check
// ---------------------------------------------------------------------------
struct MemSample {
    size_t internalFree;
    size_t psramFree;
    size_t dmaFree;
};

// Below any floor counts as exhausted. The caller samples twice, some seconds
// apart, and restarts only when both samples are exhausted, so one transient
// allocation (an album cover being decoded) cannot trigger a restart.
inline bool exhausted(const MemSample& s, const MemSample& floor) {
    return s.internalFree < floor.internalFree ||
           s.psramFree    < floor.psramFree    ||
           s.dmaFree      < floor.dmaFree;
}

// The two samples, taken without blocking: the scheduler calls memSampleNow()
// on each pass and, when it says so, samples and feeds the result to
// memCheckStep(). The first exhausted sample opens a window; the second one,
// taken once recheckMs has passed, decides. A healthy sample closes the check.
struct MemCheck {
    bool     pending;        // first sample was exhausted, waiting for the second
    uint32_t firstAt;
};

enum MemVerdict : uint8_t { MEM_WAIT, MEM_OK, MEM_EXHAUSTED };

inline bool memSampleNow(const MemCheck& c, uint32_t now, uint32_t recheckMs) {
    return !c.pending || elapsed(now, c.firstAt, recheckMs);
}

inline MemVerdict memCheckStep(MemCheck& c, uint32_t now, bool exhaustedNow) {
    if (!c.pending) {
        if (!exhaustedNow) return MEM_OK;
        c.pending = true;
        c.firstAt = now;
        return MEM_WAIT;
    }
    c.pending = false;
    return exhaustedNow ? MEM_EXHAUSTED : MEM_OK;
}

// ---------------------------------------------------------------------------
// Speaker memory: the line-in field of the dev_list blob
// ---------------------------------------------------------------------------
// '1' has line-in, '0' has none, '?' the description was never fetched, so the
// next refresh asks again. Anything else (a blob from a newer build) reads as '?'.
inline char lineInField(bool known, bool has) { return known ? (has ? '1' : '0') : '?'; }

inline void parseLineInField(char c, bool& known, bool& has) {
    known = (c == '1' || c == '0');
    has   = (c == '1');
}

}  // namespace sonos_sync
