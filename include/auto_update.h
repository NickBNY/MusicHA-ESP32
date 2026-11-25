#pragma once
/**
 * Auto-update - the decision logic.
 *
 * The daily background check (otaBackgroundCheckTick in src/ui_handlers.cpp)
 * finds a release; whether the panel may then install it WITHOUT being asked is
 * decided here, in one ordered list of reasons, so each reason can be tested on
 * the host and logged by name on the device.
 *
 * Two rules matter most:
 *   - Automatic installs only come from a repository the builder chose. A
 *     firmware image replaces the whole device, so a build that merely points at
 *     the upstream project does not install upstream releases on its own; the
 *     owner of a fork opts in by publishing from their own repo (GITHUB_REPO) or
 *     by setting AUTOUPDATE_ALLOW_UPSTREAM.
 *   - Each version is tried automatically once. The caller records it before the
 *     install starts, so a release that fails to apply cannot put the panel in
 *     a restart loop.
 *
 * Free of Arduino, FreeRTOS and LVGL on purpose, so `pio test -e native`
 * covers it (test/test_auto_update).
 */
#include <stdint.h>
#include <string.h>

namespace auto_update {

// True only when `candidate` is strictly newer than `current` by major.minor.patch.
// Parsing stops at the first non-digit/non-dot, so a "-nightly.abc1234" suffix is
// ignored, and numerically equal is not newer. A raw `!=` would offer a silent
// downgrade after a re-tagged release or a rollback.
inline bool semverNewer(const char* candidate, const char* current) {
    auto parse = [](const char* v, int out[3]) {
        out[0] = out[1] = out[2] = 0;
        if (!v) return;
        int part = 0, val = 0;
        for (const char* p = v; *p && part < 3; p++) {
            char c = *p;
            if (c >= '0' && c <= '9')      val = val * 10 + (c - '0');
            else if (c == '.')           { out[part++] = val; val = 0; }
            else break;                    // prerelease suffix ends the numeric part
        }
        if (part < 3) out[part] = val;
    };
    int a[3], b[3];
    parse(candidate, a);
    parse(current, b);
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

// Compile-time string equality, so the build can decide the install source
// without any runtime cost.
constexpr bool sameRepo(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

// The project this firmware was forked from. Not a secret; it is the default
// GITHUB_REPO in include/ui_common.h.
constexpr const char* UPSTREAM_REPO = "OpenSurface/SonosESP";

constexpr bool sourceIsUpstream(const char* repo) {
    return sameRepo(repo, UPSTREAM_REPO);
}

// Why an armed install may not start yet (or ever). Evaluated in this order.
enum class Gate {
    Allowed,
    Disabled,          // switch is off
    WrongChannel,      // Nightly: "newer" means "any different build"
    NoAsset,           // nothing newer, or no download URL
    SourceNotAllowed,  // build points at upstream and did not opt in
    Offline,           // no WiFi, or an install is already running
    NotIdle,           // someone touched the panel recently
    AlreadyTried,      // this version was auto-installed before
};

inline Gate installGate(bool enabled, int channel, bool sourceAllowed,
                        bool updateAvailable, bool hasUrl, bool wifiUp,
                        uint32_t idleMs, uint32_t needIdleMs,
                        const char* latest, const char* triedVersion) {
    if (!enabled)                         return Gate::Disabled;
    if (channel != 0)                     return Gate::WrongChannel;
    if (!sourceAllowed)                   return Gate::SourceNotAllowed;
    if (!updateAvailable || !hasUrl)      return Gate::NoAsset;
    if (!wifiUp)                          return Gate::Offline;
    if (idleMs < needIdleMs)              return Gate::NotIdle;
    if (latest && triedVersion && latest[0] != '\0' &&
        strcmp(latest, triedVersion) == 0) return Gate::AlreadyTried;
    return Gate::Allowed;
}

inline const char* gateName(Gate g) {
    switch (g) {
        case Gate::Allowed:          return "allowed";
        case Gate::Disabled:         return "disabled";
        case Gate::WrongChannel:     return "wrong channel";
        case Gate::NoAsset:          return "no update or URL";
        case Gate::SourceNotAllowed: return "source not allowed";
        case Gate::Offline:          return "offline or busy";
        case Gate::NotIdle:          return "panel in use";
        case Gate::AlreadyTried:     return "already tried";
    }
    return "?";
}

}  // namespace auto_update
