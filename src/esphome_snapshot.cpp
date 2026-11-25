/**
 * Home Assistant: the mainAppTask side. Samples the panel into an HaSnapshot
 * once per second, only while a client is subscribed, and hands it to the HA
 * task (haPublishSnapshot, zero-wait). The HA task never touches `sonos`.
 *
 * Also keeps the edge counters (Wi-Fi reconnects, discovery runs, DMA
 * recoveries, art DMA aborts). They count while the feature is enabled, with
 * or without a client. No Wi-Fi event hook: the edges are read from state.
 */

#include "ui_common.h"
#include "config.h"
#include "clock_screen.h"
#include "reboot_log.h"
#include "sonos_sync.h"
#include "esphome_api.h"
#include "esphome_entities.h"
#include <esp_core_dump.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

using namespace esphome;

// Written by the art task, read here.
static volatile bool     s_counting       = false;
static volatile uint32_t s_art_dma_aborts = 0;

static uint32_t s_wifi_reconnects = 0;
static uint32_t s_discovery_runs  = 0;
static uint32_t s_dma_recoveries  = 0;
static bool     s_prev_wifi_up    = false;
static bool     s_prev_discover   = false;
static bool     s_prev_recovery   = false;

static bool s_coredump_checked = false;
static bool s_coredump_present = false;

// Not on the mainAppTask stack (8192 bytes): about 700 bytes of snapshot and
// 300 of history text.
static HaSnapshot s_snap;
static char       s_hist[320];

static uint32_t s_next_sample_ms = 0;

void haNoteArtDmaAbort() {
    if (s_counting) s_art_dma_aborts = s_art_dma_aborts + 1;
}

void haSnapshotEnable(bool on) {
    if (on && !s_counting) {
        s_wifi_reconnects = s_discovery_runs = s_dma_recoveries = 0;
        s_art_dma_aborts  = 0;
        s_prev_wifi_up    = WiFi.status() == WL_CONNECTED;
        s_prev_discover   = sonos.isDiscovering();
        s_prev_recovery   = art_dma_recovery_requested;
        s_next_sample_ms  = 0;
    }
    s_counting = on;
}

static void countEdges() {
    // A reconnect is a rise to "connected" after having been down.
    const bool wifi_up = WiFi.status() == WL_CONNECTED;
    if (wifi_up && !s_prev_wifi_up) s_wifi_reconnects++;
    s_prev_wifi_up = wifi_up;

    const bool disc = sonos.isDiscovering();
    if (disc && !s_prev_discover) s_discovery_runs++;
    s_prev_discover = disc;

    const bool rec = art_dma_recovery_requested;
    if (rec && !s_prev_recovery) s_dma_recoveries++;
    s_prev_recovery = rec;
}

static void fillBasic(HaSnapshot& s) {
    SonosDevice* d = sonos.getCurrentDevice();
    if (d) {
        const char* track  = d->currentTrack.c_str();
        const char* artist = d->currentArtist.c_str();
        char line[kTextMax + 8];
        if (track && track[0]) {
            if (artist && artist[0]) snprintf(line, sizeof(line), "%s - %s", artist, track);
            else                     snprintf(line, sizeof(line), "%s", track);
        } else {
            snprintf(line, sizeof(line), "Idle");
        }
        copyText(s.now_playing, sizeof(s.now_playing), line);
        s.playing = d->isPlaying;
        copyText(s.speaker, sizeof(s.speaker), d->roomName.c_str());
        s.volume = (float)d->volume;
        s.setValid(E_SPEAKER, true);
        s.setValid(E_VOLUME, true);
    } else {
        copyText(s.now_playing, sizeof(s.now_playing), "Idle");
        s.playing = false;
    }
    s.setValid(E_NOW_PLAYING, true);
    s.setValid(E_PLAYING, true);
    s.wifi_rssi = (float)WiFi.RSSI();
    s.setValid(E_WIFI_RSSI, true);
    s.uptime_s = millis() / 1000;
    s.setValid(E_UPTIME, true);
}

static uint32_t stackMark(TaskHandle_t h) {
    return h ? (uint32_t)uxTaskGetStackHighWaterMark(h) : 0;
}

static void fillDebug(HaSnapshot& s) {
    s.heap_free    = (uint32_t)esp_get_free_heap_size();
    s.heap_min     = (uint32_t)esp_get_minimum_free_heap_size();
    s.heap_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s.dma_free     = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
    s.psram_free   = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    for (int id = E_HEAP_FREE; id <= E_PSRAM_FREE; id++) s.setValid(id, true);

    // Same handles as logHeapStatus() in main.cpp. NULL is mainAppTask.
    s.stack[0] = stackMark(NULL);
    s.stack[1] = stackMark(albumArtTaskHandle);
    s.stack[2] = stackMark(sonos.getNetworkTaskHandle());
    s.stack[3] = stackMark(sonos.getPollingTaskHandle());
    s.stack[4] = stackMark(clockBgTaskHandle);
    s.stack[5] = stackMark(lyricsTaskHandle);
    for (int id = E_STACK_MAIN; id <= E_STACK_LYRICS; id++) s.setValid(id, true);

    // Reboot history: newest first, "label uptime", cut at a UTF-8 boundary.
    const reboot_log::Log& log = rebootLogHistory();
    uint32_t unexpected = 0;
    size_t used = 0;
    s_hist[0] = 0;
    for (int i = 0; i < log.count; i++) {
        const reboot_log::Entry* e = reboot_log::at(log, i);
        if (!e) break;
        if (reboot_log::unexpected(*e)) unexpected++;
        if (i == 0) {
            copyText(s.reset_reason, sizeof(s.reset_reason), reboot_log::label(*e));
            s.setValid(E_RESET_REASON, true);
        }
        char up[16];
        reboot_log::formatUptime(up, sizeof(up), e->uptime_s);
        if (used < sizeof(s_hist) - 1) {
            int w = snprintf(s_hist + used, sizeof(s_hist) - used, "%s%s %s",
                             i ? ", " : "", reboot_log::label(*e), up);
            if (w > 0) used += (size_t)w;
            if (used > sizeof(s_hist) - 1) used = sizeof(s_hist) - 1;
        }
    }
    if (log.count) {
        copyText(s.reboot_history, sizeof(s.reboot_history), s_hist);
        s.setValid(E_REBOOT_HISTORY, true);
    }
    s.unexpected_reboots = unexpected;
    s.setValid(E_UNEXPECTED_REBOOTS, true);

    s.wifi_reconnects = s_wifi_reconnects;
    s.discovery_runs  = s_discovery_runs;
    s.art_dma_aborts  = s_art_dma_aborts;
    s.dma_recoveries  = s_dma_recoveries;
    s.ha_shed_events  = haShedEvents();
    s.ha_tx_deferrals = haTxDeferrals();
    s.speaker_count   = (uint32_t)sonos.getDeviceCount();
    for (int id = E_WIFI_RECONNECTS; id <= E_SPEAKER_COUNT; id++) s.setValid(id, true);

    const uint32_t now = millis();
    const uint32_t refresh = sonosSyncLastRefreshMs();
    const uint32_t daily   = sonosSyncLastDailyMs();
    if (refresh) {
        s.sync_refresh_age_s = (uint32_t)(now - refresh) / 1000;
        s.setValid(E_SYNC_REFRESH_AGE, true);
    }
    if (daily) {
        s.sync_daily_age_s = (uint32_t)(now - daily) / 1000;
        s.setValid(E_SYNC_DAILY_AGE, true);
    }

    // Evaluated once, the first time debug is on: it reads flash.
    if (!s_coredump_checked) {
        s_coredump_checked = true;
        s_coredump_present = esp_core_dump_image_check() == ESP_OK;
    }
    s.coredump_present = s_coredump_present;
    s.setValid(E_COREDUMP_PRESENT, true);
}

void haSnapshotTick(bool subscribed, bool debug) {
    countEdges();
    if (!subscribed) return;
    const uint32_t now = millis();
    if ((int32_t)(now - s_next_sample_ms) < 0) return;
    s_next_sample_ms = now + 1000;

    s_snap.clear();
    fillBasic(s_snap);
    if (debug) fillDebug(s_snap);
    haPublishSnapshot(s_snap);   // busy or task gone: the next second tries again
}
