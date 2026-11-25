/**
 * Anonymous install counter — see include/analytics.h for exactly what is sent
 * and what deliberately is not.
 */

#include "analytics.h"
#include "ui_common.h"
#include "config.h"
#include "ui_network_guard.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// One shot per boot, but spent on a real attempt rather than on reaching this
// function. The first version set it at the top, which meant a panel that was
// merely busy at t=90s — artwork holding network_mutex, a cooldown still
// running, DMA momentarily under the gate — was never counted for that entire
// boot. Those are the conditions a device playing music is in most of the time,
// so it biased the count against exactly the panels that are being used.
//
// Transient skips now back off and retry instead. The budget is what stops a
// dead endpoint or a permanently busy radio becoming a ping loop: after
// ANALYTICS_MAX_TRIES the boot is written off, uncounted and quietly.
static bool     s_analytics_done = false;
static uint8_t  s_tries          = 0;
static uint32_t s_next_try_ms    = 0;
// Set while analyticsSendTask runs; analyticsTick() stays out until it ends.
// The task writes s_analytics_done / s_tries / s_next_try_ms and clears this
// after a barrier, so mainAppTask reads them only once the task is done.
static volatile bool s_busy = false;

// Back off and come back, unless the budget is gone.
static void analyticsRetryLater(const char* why) {
    if (++s_tries >= ANALYTICS_MAX_TRIES) {
        s_analytics_done = true;
        Serial.printf("[STATS] Giving up after %d tries (%s)\n", s_tries, why);
        return;
    }
    s_next_try_ms = millis() + ANALYTICS_RETRY_MS;
    Serial.printf("[STATS] %s - retry %d/%d in %lus\n", why, s_tries,
                  ANALYTICS_MAX_TRIES - 1, ANALYTICS_RETRY_MS / 1000UL);
}

static void analyticsSendTask(void*);

void analyticsTick() {
    if (s_busy) return;
    if (s_analytics_done) return;
    if (millis() < ANALYTICS_BOOT_DELAY_MS) return;
    if (s_next_try_ms && (int32_t)(millis() - s_next_try_ms) < 0) return;
    if (WiFi.status() != WL_CONNECTED) return;   // try again once the link is up

    if (!analytics_enabled) {
        s_analytics_done = true;                 // a setting, not a transient
        Serial.println("[STATS] Disabled - nothing sent");
        return;
    }

    // DMA floor, checked before anything is allocated. A TLS session costs
    // ~32KB of DMA, and a usage counter is never worth an allocation the next
    // artwork download needs. Skipping is the correct outcome: the count is
    // approximate by design and one missing panel-boot changes nothing.
    if (heap_caps_get_free_size(MALLOC_CAP_DMA) < ART_MIN_DMA_PRE_BURST) {
        analyticsRetryLater("DMA too low");
        return;
    }

    // Same envelope as every other network path in this firmware: the general
    // cooldown plus the HTTPS/TLS-teardown gate, then network_mutex. Discovery
    // skipping this is what caused #182, so nothing new gets to skip it either.
    // Would sdioPreWait() actually wait? Check without blocking, and defer if
    // so.
    //
    // This matters more than it looks. sdioPreWait() enforces its cooldowns
    // with vTaskDelay - up to 200ms general + 3000ms HTTPS + 3000ms
    // inter-download = 6.2s. The send now runs on its own task, so those waits
    // no longer stall lv_timer_handler(), but a retry budget that pays 6.2s per
    // attempt would still turn one stall into twelve. Mirroring the guard's own
    // conditions here, on mainAppTask, makes a deferral cost microseconds, which
    // is what makes the budget affordable.
    const unsigned long now = millis();
    const bool radio_busy =
        art_download_in_progress ||
        (last_network_end_ms      && now - last_network_end_ms      < SDIO_GENERAL_COOLDOWN_MS) ||
        (last_https_end_ms        && now - last_https_end_ms        < SDIO_HTTPS_COOLDOWN_MS)   ||
        (last_art_download_end_ms && now - last_art_download_end_ms < SDIO_HTTPS_COOLDOWN_MS);
    if (radio_busy) {
        analyticsRetryLater("radio busy");
        return;
    }

    // The TLS GET runs on a one-shot internal-stack task. On mainAppTask it held
    // the LVGL task for up to ~27 s (sdioPreWait 6.2 s, mutex 5 s, connect 8 s,
    // read 8 s) with the watchdog unsubscribed, and ran TLS on its 8 KB stack.
    s_busy = true;
    if (xTaskCreatePinnedToCore(analyticsSendTask, "Stats", 8192, NULL, 1, NULL, 1) != pdPASS) {
        s_busy = false;
        analyticsRetryLater("no task");
    }
}

static void analyticsSendTask(void*) {
    // Now a near-no-op, but still called: it is the documented envelope every
    // network path goes through, and skipping it is what caused #182. The
    // false branch is unreachable (sdioPreWait only returns false from its
    // abort checks and this caller passes no abort flags) - kept so the
    // contract still holds if one is ever added.
    if (!sdioPreWait("STATS", SDIO_WAIT_HTTPS_COOLDOWN)) {
        analyticsRetryLater("SDIO busy");
        __sync_synchronize();
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    if (!network_mutex ||
        xSemaphoreTake(network_mutex, pdMS_TO_TICKS(ANALYTICS_MUTEX_WAIT_MS)) != pdTRUE) {
        analyticsRetryLater("network busy");
        __sync_synchronize();
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    // Past every transient gate: this is the one real attempt. Spend the
    // one-shot here so a dead endpoint cannot turn into a ping loop.
    s_analytics_done = true;

    // Version and panel size, as a path. GoatCounter shows each as its own row,
    // so the dashboard gives the version spread and the 4"/7" split for free.
    char url[160];
    snprintf(url, sizeof(url),
             "https://%s/count?p=/boot/%s/%din&t=SonosESP+boot",
             ANALYTICS_HOST, FIRMWARE_VERSION, SCREEN_SIZE);

    WiFiClientSecure client;
    client.setInsecure();          // same posture as lyrics, weather, art and OTA
    HTTPClient http;
    http.setTimeout(8000);
    http.setConnectTimeout(8000);

    if (http.begin(client, url)) {
        // Identifies the fleet in the dashboard's browser column, and keeps the
        // request from being filtered as an unknown bot. No device identifier.
        http.setUserAgent("SonosESP/" FIRMWARE_VERSION " (esp32-p4)");
        int code = http.GET();
        Serial.printf("[STATS] Ping %s (%d)\n", code == 200 ? "ok" : "failed", code);
        http.end();
    } else {
        Serial.println("[STATS] Ping failed - begin()");
    }
    client.stop();

    // Stamp both, exactly as the weather and lyrics paths do, so the next SOAP
    // and the next artwork download still see their drain window.
    last_network_end_ms = millis();
    last_https_end_ms   = millis();
    xSemaphoreGive(network_mutex);

    __sync_synchronize();
    s_busy = false;
    vTaskDelete(NULL);
}
