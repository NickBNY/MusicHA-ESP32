/**
 * Background upkeep. See include/sonos_sync.h for the schedule and
 * include/sonos_sync_logic.h for the timing rules (host-tested in test/test_sync).
 *
 * Design notes
 * ------------
 * - The scheduler (sonosSyncTick) runs on mainAppTask and only reads clocks,
 *   flags and heap counters (the daily memory check runs inline, see
 *   memoryCheckTick). Network work happens in a one-shot task, so a slow speaker can never stall
 *   LVGL or touch. That is the rule issue #69 established for deferred discovery.
 *   The task never calls LVGL: it changes devices[] under deviceMutex and the UI
 *   learns of it through the UPDATE_GROUPS notification, like the polling task.
 * - Every network call goes through network_mutex (sendSOAP, getRoomName, the
 *   SSDP sends), so the SDIO crash defences apply unchanged.
 * - A refresh never empties the speaker list while it has speakers in it, playing
 *   or not. See planRefresh() in sonos_sync_logic.h.
 * - "Idle" is deliberately strict. A refresh is worth nothing next to a dropped
 *   album cover, so it waits for a quiet panel and is simply postponed otherwise.
 */

#include "ui_common.h"
#include "config.h"
#include "clock_screen.h"
#include "reboot_log.h"
#include "display_driver.h"
#include "sonos_sync.h"
#include "sonos_sync_logic.h"
#include <esp_heap_caps.h>

#define JOB_REFRESH 0x1
#define JOB_DAILY   0x2

// Written only by mainAppTask (the scheduler) and by the sync task while
// s_running is set; the scheduler does not touch them until it is cleared.
// Exception: s_sched.nextMem belongs to the scheduler alone (memoryCheckTick),
// a separate aligned word the sync task never writes.
static volatile bool s_running           = false;   // a sync task is in flight
static volatile bool s_refresh_requested = false;
static volatile bool s_reboot_requested  = false;
static sonos_sync::Schedule s_sched      = {};

static const sonos_sync::Timing kTiming = {
    SPEAKER_SYNC_FIRST_MS, SPEAKER_SYNC_INTERVAL_MS, SPEAKER_ZERO_RETRY_MS,
    SPEAKER_IDLE_REFRESH_MS, SPEAKER_IDLE_QUIET_MS, DAILY_CHECK_INTERVAL_MS,
};

// Nobody at the screen and no transfer in flight. Enough for jobs that do not
// use the radio (the memory check).
static bool panelQuiet() {
    const uint32_t now = millis();
    if (sonos_sync::untouchedMs(now, last_touch_time) < SYNC_IDLE_MS) return false;
    if (ota_in_progress || art_download_in_progress || clock_photo_in_progress) return false;
    return true;
}

bool sonosSyncIdle() {
    if (!panelQuiet()) return false;
    if (WiFi.status() != WL_CONNECTED) return false;
    if (sonos.isDiscovering()) return false;
    // Same floor the artwork path uses: a refresh is never worth an allocation the
    // next album cover needs.
    if (heap_caps_get_free_size(MALLOC_CAP_DMA) < ART_MIN_DMA_PRE_BURST) return false;
    return true;
}

void sonosSyncRequestRefresh() { s_refresh_requested = true; }

bool sonosMemoryExhausted() {
    const sonos_sync::MemSample s = {
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_free_size(MALLOC_CAP_DMA),
    };
    static const sonos_sync::MemSample kFloor = {
        MEM_MIN_INTERNAL_BYTES, MEM_MIN_PSRAM_BYTES, ART_TCP_RCVBUF_DL_SAFETY,
    };
    Serial.printf("[SYNC] Memory check: internal %uKB, PSRAM %uKB, DMA %uKB\n",
                  (unsigned)(s.internalFree / 1024), (unsigned)(s.psramFree / 1024),
                  (unsigned)(s.dmaFree / 1024));
    return sonos_sync::exhausted(s, kFloor);
}

// ── Jobs (sync task) ────────────────────────────────────────────────────────
static int doRefresh() {
    int n;
    if (sonos_sync::planRefresh(sonos.getDeviceCount()) == sonos_sync::PLAN_FULL_SCAN) {
        Serial.println("[SYNC] No speakers known - scanning");
        n = sonos.discoverDevices(true);
    } else {
        // Non-destructive whatever happens: known speakers first, then one found
        // by a short SSDP probe. When neither answers the list is left as it is
        // and the next attempt comes after SPEAKER_ZERO_RETRY_MS.
        n = sonos.refreshSpeakersFromTopology();
        if (n < 0) {
            Serial.println("[SYNC] No known speaker answered - probing for one");
            n = sonos.refreshSpeakersViaProbe();
        }
        if (n < 0) {
            Serial.println("[SYNC] No speaker answered - keeping the remembered list");
            n = 0;   // retry soon; the list itself is untouched
        }
    }
    if (n > 0) sonos.selectLastSelection();
    return n;
}

// A speaker (and so a group) must be selected: restore the last selection rather
// than leaving the player empty. Only once the Sonos tasks run; before that the
// deferred bring-up in main.cpp owns discovery. The memory half of the daily
// check is not here: it runs inline in sonosSyncTick (see memoryCheckTick).
static void doDaily() {
    if (sonos.tasksRunning() && !sonos.getCurrentDevice()) {
        Serial.println("[SYNC] Daily check: nothing selected");
        if (sonos.getDeviceCount() == 0 && sonosSyncIdle()) sonos.discoverDevices(true);
        if (!sonos.selectLastSelection())
            Serial.println("[SYNC] Daily check: no speaker to select");
    }
}

// When the jobs last finished (never 0 once set), for the Home Assistant debug entities.
static volatile uint32_t s_last_refresh_ms = 0;
static volatile uint32_t s_last_daily_ms   = 0;
uint32_t sonosSyncLastRefreshMs() { return s_last_refresh_ms; }
uint32_t sonosSyncLastDailyMs()   { return s_last_daily_ms; }

static void syncTask(void* arg) {
    const uint32_t jobs = (uint32_t)(uintptr_t)arg;
    if (jobs & JOB_REFRESH) {
        const int n = doRefresh();
        sonos_sync::refreshDone(s_sched, millis(), n, kTiming);
        s_last_refresh_ms = millis() | 1u;   // for the Home Assistant debug entities
        if (n <= 0)
            Serial.printf("[SYNC] No speakers found - retrying in %lu min\n",
                          (unsigned long)(SPEAKER_ZERO_RETRY_MS / 60000UL));
    }
    if (jobs & JOB_DAILY) {
        doDaily();
        sonos_sync::dailyDone(s_sched, millis(), kTiming);
        s_last_daily_ms = millis() | 1u;     // for the Home Assistant debug entities
    }
    Serial.printf("[SYNC] Done (stack headroom %u bytes)\n",
                  (unsigned)uxTaskGetStackHighWaterMark(NULL));
    __sync_synchronize();   // s_sched writes land before the scheduler may read them
    s_running = false;
    vTaskDelete(NULL);
}

// ── Scheduler (mainAppTask) ────────────────────────────────────────────────
// Daily memory check, inline on mainAppTask. It needs no network and no task of
// its own on purpose: when internal memory is what ran out, there may be no
// contiguous block left for a task stack, and the restart must still happen.
// Two samples MEM_RECHECK_MS apart, taken on separate scheduler passes, so
// nothing blocks.
static void memoryCheckTick(uint32_t now) {
    static sonos_sync::MemCheck check = {};
    if (!sonos_sync::memDue(s_sched, now) || !panelQuiet()) return;
    if (!sonos_sync::memSampleNow(check, now, MEM_RECHECK_MS)) return;
    switch (sonos_sync::memCheckStep(check, now, sonosMemoryExhausted())) {
        case sonos_sync::MEM_WAIT:
            Serial.printf("[SYNC] Memory low - checking again in %lu s\n",
                          (unsigned long)(MEM_RECHECK_MS / 1000UL));
            break;
        case sonos_sync::MEM_EXHAUSTED:
            Serial.println("[SYNC] Daily check: memory exhausted - restart requested");
            s_reboot_requested = true;
            sonos_sync::memDone(s_sched, now, kTiming);
            break;
        case sonos_sync::MEM_OK:
            sonos_sync::memDone(s_sched, now, kTiming);
            break;
    }
}

void sonosSyncTick() {
    static uint32_t last_look = 0;
    static uint32_t create_retry_at = 0;     // set after a failed task create
    static bool     create_backoff  = false;
    const uint32_t now = millis();
    if (!sonos_sync::elapsed(now, last_look, SYNC_POLL_MS)) return;
    last_look = now;

    // A restart asked for by the daily memory check. Performed here, on the task
    // with an internal-SRAM stack (NVS writes assert from a PSRAM stack), and only
    // once the panel has been left alone for a while and no sync task is writing
    // NVS. Music keeps playing: it plays on the Sonos system, not on the panel.
    if (s_reboot_requested) {
        if (sonos_sync::untouchedMs(now, last_touch_time) >= MEM_REBOOT_IDLE_MS &&
            !ota_in_progress && !s_running) {
            Serial.println("[SYNC] Restarting to recover memory");
            rebootNoteCause(reboot_log::CAUSE_LOW_MEMORY);
            sonos.suspendTasks();                 // same quiesce as Settings > Restart
            display_set_brightness(0);
            vTaskDelay(pdMS_TO_TICKS(150));
            ESP.restart();
        }
        return;
    }

    // The daily clock starts at boot: the memory check must run even on a panel
    // that has never found a speaker or has lost Wi-Fi.
    if (!s_sched.started) sonos_sync::start(s_sched, now, kTiming);

    memoryCheckTick(now);
    if (s_reboot_requested) return;

    if (s_running) return;
    if (create_backoff) {
        if (!sonos_sync::due(now, create_retry_at)) return;
        create_backoff = false;
    }

    uint32_t jobs = 0;
    if (sonos_sync::dailyDue(s_sched, now) && panelQuiet()) jobs |= JOB_DAILY;

    // Speaker refresh: only once the Sonos tasks exist (first bring-up belongs to
    // deferredDiscoveryTask in main.cpp). The first refresh is due
    // SPEAKER_SYNC_FIRST_MS after that, not after boot.
    static bool refresh_armed = false;
    if (sonos.tasksRunning()) {
        if (!refresh_armed) {
            refresh_armed = true;
            s_sched.nextRefresh = now + SPEAKER_SYNC_FIRST_MS;
            s_sched.lastRefresh = now;
        }
        const uint32_t untouched = sonos_sync::untouchedMs(now, last_touch_time);
        if ((s_refresh_requested ||
             sonos_sync::refreshDue(s_sched, now, sonos.getDeviceCount(), untouched, kTiming)) &&
            sonosSyncIdle())
            jobs |= JOB_REFRESH;
    }
    if (!jobs) return;                     // not due, or postponed (not dropped)

    if (jobs & JOB_REFRESH) s_refresh_requested = false;
    s_running = true;
    // Internal-SRAM stack (dynamic xTaskCreate), like deferredDiscoveryTask: it
    // writes NVS (speaker list, selection), which asserts from a PSRAM stack.
    if (xTaskCreatePinnedToCore(syncTask, "SpkSync", SYNC_TASK_STACK, (void*)(uintptr_t)jobs,
                                1, NULL, 1) != pdPASS) {
        s_running = false;
        if (jobs & JOB_REFRESH) s_refresh_requested = true;   // still owed
        sonos_sync::postpone(create_retry_at, now, SYNC_TASK_RETRY_MS);
        create_backoff = true;
        Serial.printf("[SYNC] Could not start the sync task - retrying in %lu s\n",
                      (unsigned long)(SYNC_TASK_RETRY_MS / 1000UL));
    }
}
