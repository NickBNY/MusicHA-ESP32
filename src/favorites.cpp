/**
 * Favorites row data, the favorites loader and the "last played source".
 * See include/favorites.h.
 */

#include "ui_common.h"
#include "config.h"
#include "favorites.h"
#include "sonos_sync.h"

#define FAV_TITLE_MAX   48
#define FAV_XML_MAX     3000     // a favorite's DIDL <item> with its r:resMD

struct FavSlot {
    char  title[FAV_TITLE_MAX];
    char* xml;                   // PSRAM, FAV_XML_MAX bytes
};

static void favoritesRememberAsync(char kind, const String& uri, const String& meta, const String& title);
static void favoritesFlushPending();

static FavSlot           s_fav[FAV_SHOWN];
static int               s_count   = 0;
static volatile uint32_t s_version = 0;
static SemaphoreHandle_t s_lock    = nullptr;

static volatile bool     s_fetching       = false;
static volatile bool     s_want_refresh   = true;     // first fetch is wanted from the start
static uint32_t          s_next_fetch_ms  = 0;
static uint32_t          s_armed_at_ms    = 0;
static String            s_fetched_for;               // RINCON the list was fetched through

int         favoritesCount()   { return s_count; }
uint32_t    favoritesVersion() { return s_version; }
void        favoritesRequestRefresh() { s_want_refresh = true; }

const char* favoritesTitle(int i) {
    if (i < 0 || i >= s_count) return "";
    return s_fav[i].title;
}

bool favoritesTitleCopy(int i, char* out, size_t n) {
    if (!out || n == 0) return false;
    out[0] = '\0';
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) return false;
    const bool ok = i >= 0 && i < s_count;
    if (ok) strlcpy(out, s_fav[i].title, n);
    xSemaphoreGive(s_lock);
    return ok;
}

// ── Loader ──────────────────────────────────────────────────────────────────
static void fetchTask(void*) {
    String didl = sonos.browseContent("FV:2", 0, FAV_SHOWN);

    char*  xml[FAV_SHOWN] = {nullptr};
    char   titles[FAV_SHOWN][FAV_TITLE_MAX] = {{0}};
    int    n = 0;

    int pos = 0;
    while (n < FAV_SHOWN && pos < (int)didl.length()) {
        int a = didl.indexOf("<item", pos);
        if (a < 0) break;
        int b = didl.indexOf("</item>", a);
        if (b < 0) break;
        String item = didl.substring(a, b + 7);
        pos = b + 7;

        String title = sonos.decodeHTML(sonos.extractXML(item, "dc:title"));
        if (title.length() == 0) continue;
        if (item.length() >= FAV_XML_MAX) item = item.substring(0, FAV_XML_MAX - 1);

        char* buf = (char*)heap_caps_malloc(FAV_XML_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (char*)malloc(FAV_XML_MAX);
        if (!buf) break;
        strlcpy(buf, item.c_str(), FAV_XML_MAX);
        strlcpy(titles[n], title.c_str(), FAV_TITLE_MAX);
        xml[n++] = buf;
    }

    if (n > 0 && s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (int i = 0; i < FAV_SHOWN; i++) {
            if (s_fav[i].xml) { free(s_fav[i].xml); s_fav[i].xml = nullptr; }
        }
        for (int i = 0; i < n; i++) {
            strlcpy(s_fav[i].title, titles[i], FAV_TITLE_MAX);
            s_fav[i].xml = xml[i];
        }
        s_count = n;
        s_version = s_version + 1;
        xSemaphoreGive(s_lock);
        Serial.printf("[FAV] Loaded %d favorite(s)\n", n);
    } else {
        for (int i = 0; i < n; i++) free(xml[i]);
        if (n == 0) Serial.println("[FAV] No favorites returned");
    }

    s_fetching = false;
    vTaskDelete(NULL);
}

void favoritesTick() {
    favoritesFlushPending();   // cheap flag test; the NVS write needs this task's stack
    static uint32_t last_look = 0;
    const uint32_t now = millis();
    if ((uint32_t)(now - last_look) < SYNC_POLL_MS) return;
    last_look = now;

    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_fetching || !sonos.tasksRunning()) return;
    SonosDevice* d = sonos.getCurrentDevice();
    if (!d || !d->connected) return;

    if (s_armed_at_ms == 0) { s_armed_at_ms = now ? now : 1; s_next_fetch_ms = now + 30000UL; return; }

    // A different speaker can have different services signed in, so the list is
    // fetched again when the selection changes.
    String rin;
    if (xSemaphoreTake(sonos.getDeviceMutex(), pdMS_TO_TICKS(20)) == pdTRUE) {
        rin = d->rinconID;
        xSemaphoreGive(sonos.getDeviceMutex());
    } else {
        return;
    }
    if (rin != s_fetched_for) s_want_refresh = true;

    const bool periodic = (int32_t)(now - s_next_fetch_ms) >= 0;
    if (!s_want_refresh && !periodic) return;
    if ((int32_t)(now - s_next_fetch_ms) < 0 && s_fetched_for.length() == 0) return;   // 30 s settle after start
    if (!sonosSyncIdle()) return;

    s_want_refresh   = false;
    s_fetched_for    = rin;
    s_next_fetch_ms  = now + FAV_REFRESH_MS;
    s_fetching       = true;
    if (xTaskCreatePinnedToCore(fetchTask, "FavFetch", 8192, NULL, 1, NULL, 1) != pdPASS) {
        s_fetching = false;
        Serial.println("[FAV] Could not start the loader");
    }
}

// ── Last source ─────────────────────────────────────────────────────────────
bool lastSourceExists() {
    return wifiPrefs.getString(NVS_KEY_LAST_SRC_URI, "").length() > 0 &&
           wifiPrefs.getString(NVS_KEY_LAST_SRC_KIND, "").length() > 0;
}

String lastSourceTitle() { return wifiPrefs.getString(NVS_KEY_LAST_SRC_TITLE, ""); }

void lastSourceSave(char kind, const String& uri, const String& meta, const String& title) {
    // NVS strings top out near 4000 bytes. A source whose metadata does not fit is
    // not remembered at all: replaying it with the metadata cut off would fail in
    // the same silent way a favourite without r:resMD does.
    if (uri.length() == 0 || uri.length() > 900 || meta.length() > 3000) return;
    const String k(kind);
    if (wifiPrefs.getString(NVS_KEY_LAST_SRC_URI, "") == uri &&
        wifiPrefs.getString(NVS_KEY_LAST_SRC_KIND, "") == k) return;   // unchanged: spare the flash
    wifiPrefs.putString(NVS_KEY_LAST_SRC_KIND, k);
    wifiPrefs.putString(NVS_KEY_LAST_SRC_URI, uri);
    wifiPrefs.putString(NVS_KEY_LAST_SRC_META, meta);
    wifiPrefs.putString(NVS_KEY_LAST_SRC_TITLE, title.substring(0, 60));
    Serial.printf("[SRC] Remembered last source: %s\n", title.c_str());
}

// Same decisions as the Sources screen's row handler (ui_settings_screens.cpp);
// see the notes there for why r:resMD, and why container and URI metadata are
// escaped differently. Kept in one place here so the Favorites row and "play the
// last source" cannot drift from it.
bool favoritePlayItemXML(const String& itemXML) {
    String uri = decodeHTMLEntities(sonos.extractXML(itemXML, "res"));
    const String title = sonos.decodeHTML(sonos.extractXML(itemXML, "dc:title"));

    if (uri.length() == 0) {
        String resMD = sonos.extractXML(itemXML, "r:resMD");
        if (resMD.length() > 0) {
            resMD = decodeHTMLEntities(resMD);
            uri = sonos.extractXML(resMD, "res");
        }
    }
    if (uri.length() == 0) { Serial.println("[FAV] No URI in favorite"); return false; }

    if (uri.startsWith("x-rincon-cpcontainer:")) {
        String meta = sonos.extractXML(itemXML, "r:resMD");
        if (meta.length() == 0) meta = itemXML;
        const bool ok = sonos.playContainer(uri.c_str(), meta.c_str());
        if (ok) favoritesRememberAsync('C', uri, meta, title);
        return ok;
    }
    const String resMD = sonos.extractXML(itemXML, "r:resMD");
    const String meta  = resMD.length() ? decodeHTMLEntities(resMD) : itemXML;
    const bool ok = sonos.playURI(uri.c_str(), meta.c_str());
    if (ok) favoritesRememberAsync('U', uri, meta, title);
    return ok;
}

// NVS must be written from a task with an internal-SRAM stack, and the play path
// runs on short-lived tasks. The write is queued for mainAppTask (favoritesTick's
// caller) instead of being done in place.
static String s_pend_uri, s_pend_meta, s_pend_title;
static char   s_pend_kind = 0;
static volatile bool s_pend_ready = false;

static void favoritesRememberAsync(char kind, const String& uri, const String& meta, const String& title) {
    if (s_pend_ready) return;
    s_pend_uri = uri; s_pend_meta = meta; s_pend_title = title; s_pend_kind = kind;
    // Publish the Strings before the flag, like the other task/poll handoffs:
    // mainAppTask on the other core must not see ready before the payload.
    __sync_synchronize();
    s_pend_ready = true;
}

// Called from favoritesTick(), which runs on mainAppTask.
static void favoritesFlushPending() {
    if (!s_pend_ready) return;
    lastSourceSave(s_pend_kind, s_pend_uri, s_pend_meta, s_pend_title);
    s_pend_uri = ""; s_pend_meta = ""; s_pend_title = "";
    s_pend_ready = false;
}

// ── Play (always off the UI thread) ─────────────────────────────────────────
static volatile bool s_playing_task = false;

static void playFavTask(void* arg) {
    const int idx = (int)(intptr_t)arg;
    String xml;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (idx >= 0 && idx < s_count && s_fav[idx].xml) xml = String(s_fav[idx].xml);
        xSemaphoreGive(s_lock);
    }
    if (xml.length() > 0) favoritePlayItemXML(xml);
    s_playing_task = false;
    vTaskDelete(NULL);
}

void favoritesPlay(int index) {
    if (s_playing_task || index < 0 || index >= s_count) return;
    s_playing_task = true;
    if (xTaskCreatePinnedToCore(playFavTask, "FavPlay", 8192, (void*)(intptr_t)index, 1, NULL, 1) != pdPASS) {
        s_playing_task = false;
        Serial.println("[FAV] Could not start playback task");
    }
}

static void playLastTask(void*) {
    const String kind = wifiPrefs.getString(NVS_KEY_LAST_SRC_KIND, "");
    const String uri  = wifiPrefs.getString(NVS_KEY_LAST_SRC_URI, "");
    const String meta = wifiPrefs.getString(NVS_KEY_LAST_SRC_META, "");
    const String ttl  = wifiPrefs.getString(NVS_KEY_LAST_SRC_TITLE, "");
    if (uri.length() > 0) {
        Serial.printf("[SRC] No source - playing the last one: %s\n", ttl.c_str());
        if (kind == "C")      sonos.playContainer(uri.c_str(), meta.c_str());
        else if (kind == "P") sonos.playPlaylist(uri.c_str(), ttl.c_str());
        else                  sonos.playURI(uri.c_str(), meta.c_str());
    }
    s_playing_task = false;
    vTaskDelete(NULL);
}

void lastSourcePlay() {
    if (s_playing_task || !lastSourceExists()) return;
    s_playing_task = true;
    if (xTaskCreatePinnedToCore(playLastTask, "SrcPlay", 8192, NULL, 1, NULL, 1) != pdPASS) {
        s_playing_task = false;
        Serial.println("[SRC] Could not start playback task");
    }
}
