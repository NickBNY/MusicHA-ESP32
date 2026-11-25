/**
 * Home Assistant (ESPHome native API) server, firmware side.
 *
 * The protocol is in the framework-free headers (esphome_session.h,
 * esphome_mdns.h), tested on the host. This file owns what cannot be tested
 * there: sockets, the radio gate, the DMA budget, the task and its memory.
 *
 * Rules, from the design note (Decision 2):
 *  - Off by default. While off, haTick() reads one flag. No task, no socket,
 *    no allocation, no Wi-Fi event hook.
 *  - The task stack and every buffer are in PSRAM. Internal SRAM use is the
 *    TCB, the lwIP sockets and in-flight pbufs. The task never touches NVS or
 *    flash, which is what makes a PSRAM stack legal.
 *  - Every transmitted byte (TCP replies, state pushes, mDNS) goes through
 *    haSend(): radio gate, sdioPreWait outside the mutex, network_mutex with a
 *    100 ms timeout, then one non-blocking send.
 *  - The feature loses before artwork does: start gate, accept gate, shed rule.
 *  - Read-only. No command message is handled anywhere.
 *  - One Serial.printf per start, stop, accept and close. Nothing periodic.
 */

#include "ui_common.h"
#include "config.h"
#include "esphome_api.h"
#include "ha_config.h"
#include "esphome_session.h"
#include "esphome_mdns.h"
#include "ui_network_guard.h"
#include <errno.h>
#include <new>
#include <lwip/sockets.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>

using namespace esphome;

// ---------------------------------------------------------------------------
// State shared between mainAppTask (haTick, the Settings card) and the task
// ---------------------------------------------------------------------------

enum HaState : uint8_t {
    HS_OFF,
    HS_WAIT_WIFI,
    HS_WAIT_MEM,
    HS_OUT_OF_MEM,
    HS_PORT_ERROR,
    HS_LISTENING,
    HS_CONNECTED,
    HS_PAUSED,
};

static const char* kStateText[] = {
    "Off", "Waiting for Wi-Fi", "Waiting for memory", "Out of memory",
    "Port error", "Listening", "Connected", "Paused",
};

static volatile bool     s_work       = true;   // first haTick() does the lazy NVS load
static bool              s_loaded     = false;
static volatile bool     s_enabled    = false;
static volatile bool     s_debug      = false;
static volatile HaState  s_state      = HS_OFF;
static volatile bool     s_stop       = false;  // asks the task to exit
static volatile bool     s_task_alive = false;
static volatile uint32_t s_task_dead_ms = 0;
static bool              s_dead_set   = false;
static volatile bool     s_subscribed = false;
static volatile uint32_t s_shed_events   = 0;
static volatile uint32_t s_tx_deferrals  = 0;
static volatile uint32_t s_eagain_since  = 0;   // first EAGAIN of the current stall, 0 = none

// A deadline that is "due" until it is set, and wrap-safe afterwards.
struct Deadline {
    uint32_t at = 0;
    bool     on = false;
    bool due(uint32_t now) const { return !on || (int32_t)(now - at) >= 0; }
    void set(uint32_t now, uint32_t delay) { at = now + delay; on = true; }
    void clear() { on = false; }
};

static Deadline s_retry;   // haTick: next start attempt

// Device identity, built once on mainAppTask before the task starts.
static char s_name[16];
static char s_mac_colon[18];
static char s_mac_hex[13];
static char s_model[56];
static bool s_identity_ready = false;

// Task resources. The block and the stack are PSRAM.
struct HaBlock {
    Session    sess;
    uint8_t    rx[HA_RX_BUF_BYTES];
    uint8_t    mdns_rx[512];
    uint8_t    mdns_tx[512];
    HaSnapshot shared;      // written by mainAppTask under s_mtx
    HaSnapshot local;       // the task's own copy, read by the session
    uint32_t   shared_ver;
    uint32_t   local_ver;
};

static HaBlock* volatile s_block = nullptr;
static StackType_t*      s_stack = nullptr;
static StaticTask_t      s_tcb;
static StaticSemaphore_t s_mtx_buf;
static SemaphoreHandle_t s_mtx = nullptr;

// Task-side sockets (read by haSend callers).
static int s_lsock = -1;
static int s_csock = -1;
static int s_usock = -1;

static inline bool dueMs(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

static void setState(HaState s) { s_state = s; }

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static void ensureLoaded() {
    if (s_loaded) return;
    s_loaded  = true;
    s_enabled = wifiPrefs.getBool(NVS_KEY_HA_ON, false);
    s_debug   = wifiPrefs.getBool(NVS_KEY_HA_DEBUG, false);
    if (s_enabled) {
        haSnapshotEnable(true);
        setState(HS_WAIT_WIFI);
    }
}

bool haIsEnabled() { ensureLoaded(); return s_enabled; }
bool haIsDebug()   { ensureLoaded(); return s_debug; }

void haSetEnabled(bool on) {
    ensureLoaded();
    if (on == s_enabled) return;
    wifiPrefs.putBool(NVS_KEY_HA_ON, on);
    s_enabled = on;
    s_work    = true;
    if (on) {
        // s_stop is left alone: a task still shutting down must finish first,
        // and haTick() starts the new one once it is gone.
        s_retry.clear();
        haSnapshotEnable(true);
        setState(HS_WAIT_WIFI);
    } else {
        s_stop = true;
        haSnapshotEnable(false);
        setState(HS_OFF);
    }
}

void haSetDebug(bool on) {
    ensureLoaded();
    if (on == s_debug) return;
    wifiPrefs.putBool(NVS_KEY_HA_DEBUG, on);
    s_debug = on;   // the task closes the client so Home Assistant re-lists entities
}

void haStatus(char* buf, size_t n) {
    if (!buf || !n) return;
    HaState s = s_state;
    snprintf(buf, n, "%s", kStateText[s <= HS_PAUSED ? s : HS_OFF]);
}

static void formatIdentity() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    mdns::formatInstance(mac, s_name);
    mdns::formatMacColon(mac, s_mac_colon);
    mdns::formatMacHex(mac, s_mac_hex);
    const unsigned rev = (unsigned)ESP.getChipRevision();
    snprintf(s_model, sizeof(s_model), "%s (ESP32-P4 v%u.%u)",
#if SCREEN_SIZE == 7
             "JC1060P470C 7in",
#else
             "JC4880P443C 4in",
#endif
             rev / 100, rev % 100);
    s_identity_ready = true;
}

void haAddress(char* buf, size_t n) {
    if (!buf || !n) return;
    if (WiFi.status() != WL_CONNECTED) {
        snprintf(buf, n, "Not connected");
        return;
    }
    if (!s_identity_ready) formatIdentity();
    IPAddress ip = WiFi.localIP();
    snprintf(buf, n, "%u.%u.%u.%u:%u (%s)", ip[0], ip[1], ip[2], ip[3], (unsigned)HA_PORT, s_name);
}

bool haClientSubscribed() { return s_subscribed; }
uint32_t haShedEvents()   { return s_shed_events; }
uint32_t haTxDeferrals()  { return s_tx_deferrals; }

bool haPublishSnapshot(const HaSnapshot& s) {
    if (!s_mtx || !s_task_alive) return false;
    if (xSemaphoreTake(s_mtx, 0) != pdTRUE) return false;
    bool ok = false;
    HaBlock* b = s_block;
    if (b) {
        memcpy(&b->shared, &s, sizeof(HaSnapshot));
        b->shared_ver++;
        ok = true;
    }
    xSemaphoreGive(s_mtx);
    return ok;
}

// ---------------------------------------------------------------------------
// Radio gate and the single transmit path
// ---------------------------------------------------------------------------

static size_t dmaFree() { return heap_caps_get_free_size(MALLOC_CAP_DMA); }

// Nothing else is using the radio, and DMA is above the artwork floor.
static bool haRadioQuiet() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (art_download_in_progress || clock_photo_in_progress || ota_in_progress) return false;
    if (art_dma_recovery_requested) return false;
    if (sonos.isDiscovering()) return false;
    return dmaFree() >= (size_t)ART_MIN_DMA_PRE_BURST;
}

static inline void noteDeferral() { s_tx_deferrals = s_tx_deferrals + 1; }

// Returns bytes taken (0..len), 0 = deferred, -1 = link broken. fd is the TCP
// client when `to` is null, else the UDP socket and `to` the destination.
static int haSend(int fd, const uint8_t* data, size_t len, const struct sockaddr_in* to,
                  volatile bool* abort_flag) {
    if (!len || fd < 0) return 0;
    if (!haRadioQuiet()) { noteDeferral(); return 0; }
    if (!sdioPreWait("HA", 0, abort_flag)) return 0;
    if (!network_mutex || xSemaphoreTake(network_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        noteDeferral();
        return 0;
    }
    if (!haRadioQuiet()) {   // re-check under the mutex
        xSemaphoreGive(network_mutex);
        noteDeferral();
        return 0;
    }
    int r;
    if (to) {
        r = lwip_sendto(fd, data, len, MSG_DONTWAIT, (const struct sockaddr*)to, sizeof(*to));
    } else {
        size_t n = len > HA_TX_CHUNK_BYTES ? (size_t)HA_TX_CHUNK_BYTES : len;
        r = lwip_send(fd, data, n, MSG_DONTWAIT);
    }
    const int err = errno;
    last_network_end_ms = millis();
    xSemaphoreGive(network_mutex);

    if (r >= 0) {
        if (!to) s_eagain_since = 0;
        return r;
    }
    if (err == EAGAIN || err == EWOULDBLOCK || err == ENOMEM || err == ENOBUFS) {
        noteDeferral();
        if (!to && !s_eagain_since) s_eagain_since = millis() | 1u;
        return 0;
    }
    return -1;
}

static int sinkFn(void*, const uint8_t* d, size_t n) {
    return haSend(s_csock, d, n, nullptr, &s_stop);
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------

static void setNonBlocking(int fd) {
    int fl = lwip_fcntl(fd, F_GETFL, 0);
    lwip_fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void closeFd(int& fd) {
    if (fd >= 0) lwip_close(fd);
    fd = -1;
}

static int openListener(int* err_out) {
    int s = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { *err_out = errno; return -1; }
    int one = 1;
    lwip_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons(HA_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (lwip_bind(s, (struct sockaddr*)&a, sizeof(a)) < 0 || lwip_listen(s, 1) < 0) {
        *err_out = errno;
        lwip_close(s);
        return -1;
    }
    setNonBlocking(s);
    return s;
}

static int openMdns() {
    int s = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int one = 1;
    lwip_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons(mdns::kPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (lwip_bind(s, (struct sockaddr*)&a, sizeof(a)) < 0) { lwip_close(s); return -1; }
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = htonl(0xE00000FBu);   // 224.0.0.251
    mreq.imr_interface.s_addr = (uint32_t)WiFi.localIP();
    if (lwip_setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        lwip_close(s);
        return -1;
    }
    int rcv = HA_MDNS_RCVBUF_BYTES;
    lwip_setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    uint8_t ttl = 255;
    lwip_setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));   // best effort
    setNonBlocking(s);
    return s;
}

static void mdnsDest(struct sockaddr_in* d) {
    memset(d, 0, sizeof(*d));
    d->sin_family      = AF_INET;
    d->sin_port        = htons(mdns::kPort);
    d->sin_addr.s_addr = htonl(0xE00000FBu);
}

static bool mdnsHost(mdns::Host* h) {
    IPAddress ip = WiFi.localIP();
    if ((uint32_t)ip == 0) return false;
    h->instance = s_name;
    for (int i = 0; i < 4; i++) h->ipv4[i] = ip[i];
    h->port    = HA_PORT;
    h->mac_hex = s_mac_hex;
    return true;
}

// ---------------------------------------------------------------------------
// The task
// ---------------------------------------------------------------------------

static const DeviceIdentity* identity() {
    static DeviceIdentity id;
    id.name             = s_name;
    id.friendly_name    = "SonosESP Panel";
    id.mac              = s_mac_colon;
    id.esphome_version  = FIRMWARE_VERSION;
    id.compilation_time = __DATE__ " " __TIME__;
    id.model            = s_model;
    id.manufacturer     = "SonosESP";
    id.project_name     = "OpenSurface.SonosESP";
    id.project_version  = FIRMWARE_VERSION;
    return &id;
}

static const char* closeText(Session::CloseReason r) {
    switch (r) {
        case Session::CLOSE_NONE: return "none";
        case Session::CLOSE_PEER_DISCONNECT: return "peer disconnect";
        case Session::CLOSE_NOISE: return "encryption not supported";
        case Session::CLOSE_BAD_PREAMBLE: return "bad preamble";
        case Session::CLOSE_BAD_VARINT: return "bad length";
        case Session::CLOSE_OVERSIZE: return "oversize frame";
        case Session::CLOSE_PROTOCOL_ERRORS: return "protocol errors";
        case Session::CLOSE_LINK: return "link";
    }
    return "?";
}

static void haTask(void*) {
    HaBlock* b = s_block;
    const Sink sink = {sinkFn, nullptr};
    mdns::RateLimiter limiter;

    bool     open        = false;
    bool     mdns_on     = false;
    Deadline reopen;                   // earliest next open
    Deadline shed_sample;
    Deadline push_at;
    Deadline announce_at;
    int      announce_left = 0;
    int      low_samples   = 0;
    uint32_t last_ip       = 0;
    uint32_t client_since  = 0;
    uint32_t last_rx       = 0;
    uint32_t pending_since = 0;

    auto closeClient = [&](const char* why) {
        if (s_csock < 0) return;
        Serial.printf("[HA] client closed: %s\n", why);
        closeFd(s_csock);
        s_subscribed  = false;
        s_eagain_since = 0;
        pending_since = 0;
        if (open) setState(HS_LISTENING);
    };

    auto closeAll = [&](bool goodbye) {
        closeClient("stop");
        if (goodbye && s_usock >= 0) {
            mdns::Host h;
            struct sockaddr_in dst;
            mdnsDest(&dst);
            if (mdnsHost(&h)) {
                size_t n = mdns::buildGoodbye(h, b->mdns_tx, sizeof(b->mdns_tx));
                if (n) haSend(s_usock, b->mdns_tx, n, &dst, nullptr);   // only when quiet
            }
        }
        closeFd(s_usock);
        closeFd(s_lsock);
        open    = false;
        mdns_on = false;
        announce_left = 0;
    };

    while (!s_stop) {
        const uint32_t now = millis();

        // Snapshot from mainAppTask: zero-wait hand-over.
        if (s_mtx && xSemaphoreTake(s_mtx, 0) == pdTRUE) {
            if (b->shared_ver != b->local_ver) {
                memcpy(&b->local, &b->shared, sizeof(HaSnapshot));
                b->local_ver = b->shared_ver;
            }
            xSemaphoreGive(s_mtx);
        }

        // Pause: Wi-Fi down, OTA, or the art task's Wi-Fi recovery. Sockets are
        // closed and reopened 10 s after the condition clears.
        const bool wifi_up = WiFi.status() == WL_CONNECTED;
        if (!wifi_up || ota_in_progress || art_dma_recovery_requested) {
            if (open || s_csock >= 0) {
                Serial.printf("[HA] paused (%s)\n", !wifi_up ? "wifi" : "ota/recovery");
                closeAll(false);
            }
            reopen.set(now, 10000);
            setState(wifi_up ? HS_PAUSED : HS_WAIT_WIFI);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!open) {
            if (!reopen.due(now)) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            const size_t dma_before = dmaFree();
            if (dma_before < (size_t)(ART_MIN_DMA_PRE_BURST + HA_DMA_BUDGET_BYTES)) {
                setState(HS_WAIT_MEM);
                reopen.set(now, 60000);
                continue;
            }
            int err = 0;
            s_lsock = openListener(&err);
            if (s_lsock < 0) {
                Serial.printf("[HA] listen failed errno=%d\n", err);
                setState(HS_PORT_ERROR);
                reopen.set(now, 30000);
                continue;
            }
            s_usock = openMdns();
            mdns_on = s_usock >= 0;
            open    = true;
            low_samples = 0;
            shed_sample.set(now, 1000);
            push_at.set(now, 1000);
            limiter.reset();
            announce_left = mdns_on ? 2 : 0;
            announce_at.clear();
            last_ip = (uint32_t)WiFi.localIP();
            setState(HS_LISTENING);
            Serial.printf("[HA] start port=%u mdns=%d dma=%u after=%u\n", (unsigned)HA_PORT,
                          mdns_on ? 1 : 0, (unsigned)dma_before, (unsigned)dmaFree());
        }

        // Shed rule: with the radio quiet, DMA under the artwork floor for
        // HA_SHED_SAMPLES one-second samples in a row closes everything.
        if (shed_sample.due(now)) {
            shed_sample.set(now, 1000);
            const bool quiet_radio = !art_download_in_progress && !clock_photo_in_progress &&
                                     !ota_in_progress && !art_dma_recovery_requested &&
                                     !sonos.isDiscovering() &&
                                     (!network_mutex || xSemaphoreGetMutexHolder(network_mutex) == NULL);
            if (quiet_radio && dmaFree() < (size_t)ART_MIN_DMA_PRE_BURST) low_samples++;
            else low_samples = 0;
            if (low_samples >= HA_SHED_SAMPLES) {
                Serial.printf("[HA] shed dma=%u\n", (unsigned)dmaFree());
                s_shed_events = s_shed_events + 1;
                closeAll(false);
                reopen.set(now, HA_BACKOFF_MS);
                setState(HS_PAUSED);
                continue;
            }
        }

        // Wait for traffic, at most 200 ms.
        fd_set rd;
        FD_ZERO(&rd);
        int maxfd = -1;
        if (s_lsock >= 0) { FD_SET(s_lsock, &rd); if (s_lsock > maxfd) maxfd = s_lsock; }
        if (s_csock >= 0) { FD_SET(s_csock, &rd); if (s_csock > maxfd) maxfd = s_csock; }
        if (s_usock >= 0) { FD_SET(s_usock, &rd); if (s_usock > maxfd) maxfd = s_usock; }
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 200000;
        int ready = lwip_select(maxfd + 1, &rd, nullptr, nullptr, &tv);
        const uint32_t t = millis();

        // New connection. Only one client at a time.
        if (ready > 0 && s_lsock >= 0 && FD_ISSET(s_lsock, &rd)) {
            int c = lwip_accept(s_lsock, nullptr, nullptr);
            if (c >= 0) {
                const size_t dma = dmaFree();
                if (s_csock >= 0 || dma < (size_t)(ART_MIN_DMA_PRE_BURST + HA_ACCEPT_DMA_MARGIN)) {
                    Serial.printf("[HA] connection refused (%s) dma=%u\n",
                                  s_csock >= 0 ? "busy" : "memory", (unsigned)dma);
                    lwip_close(c);
                } else {
                    setNonBlocking(c);
                    int rcv = 2048, one = 1;
                    lwip_setsockopt(c, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
                    lwip_setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    s_csock = c;
                    b->sess.begin(s_debug, *identity());
                    b->sess.setSnapshot(&b->local);
                    client_since = last_rx = t;
                    pending_since = 0;
                    s_eagain_since = 0;
                    push_at.set(t, 1000);
                    setState(HS_CONNECTED);
                    Serial.printf("[HA] client accepted dma=%u after=%u\n", (unsigned)dma,
                                  (unsigned)dmaFree());
                }
            }
        }

        // Client bytes. recv is not gated: it only drains lwIP.
        if (s_csock >= 0 && ready > 0 && FD_ISSET(s_csock, &rd)) {
            int n = lwip_recv(s_csock, b->rx, sizeof(b->rx), MSG_DONTWAIT);
            if (n == 0) {
                closeClient("peer closed");
            } else if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) closeClient("recv error");
            } else {
                last_rx = t;
                if (!b->sess.feed(b->rx, (size_t)n, sink, t)) {
                    closeClient(closeText(b->sess.closeReason()));
                }
            }
        }

        // mDNS queries.
        if (s_usock >= 0 && ready > 0 && FD_ISSET(s_usock, &rd)) {
            int n = lwip_recvfrom(s_usock, b->mdns_rx, sizeof(b->mdns_rx), MSG_DONTWAIT, nullptr, nullptr);
            if (n > 0) {
                uint8_t want = mdns::parseQuery(b->mdns_rx, (size_t)n, s_name);
                mdns::Host h;
                if (want && limiter.allow(t) && mdnsHost(&h)) {
                    size_t len = mdns::buildResponse(want & mdns::kWantAll, h, 120, b->mdns_tx,
                                                     sizeof(b->mdns_tx));
                    struct sockaddr_in dst;
                    mdnsDest(&dst);
                    if (len) haSend(s_usock, b->mdns_tx, len, &dst, &s_stop);
                }
            }
        }

        // mDNS announcements: two, one second apart, at start and on IP change.
        if (mdns_on) {
            const uint32_t ip = (uint32_t)WiFi.localIP();
            if (ip != last_ip) {
                last_ip = ip;
                announce_left = 2;
                announce_at.clear();
            }
            if (announce_left > 0 && announce_at.due(t)) {
                mdns::Host h;
                struct sockaddr_in dst;
                mdnsDest(&dst);
                if (mdnsHost(&h)) {
                    size_t len = mdns::buildAnnounce(h, 120, b->mdns_tx, sizeof(b->mdns_tx));
                    if (len && haSend(s_usock, b->mdns_tx, len, &dst, &s_stop) > 0) {
                        announce_left--;
                        announce_at.set(t, 1000);
                    }
                }
            }
        }

        // Client upkeep.
        if (s_csock >= 0) {
            if (b->sess.debug() != s_debug) {
                closeClient("debug changed");
            } else if (!b->sess.helloDone() && (uint32_t)(t - client_since) > HA_HELLO_TIMEOUT_MS) {
                closeClient("no hello");
            } else if ((uint32_t)(t - last_rx) > HA_IDLE_TIMEOUT_MS) {
                closeClient("idle");
            } else if (s_eagain_since && (uint32_t)(t - s_eagain_since) > 5000) {
                closeClient("send stalled");
            } else {
                bool ok = true;
                if (b->sess.subscribed() && push_at.due(t)) {
                    push_at.set(t, 1000);
                    ok = b->sess.pushStates(sink, t);
                } else if (b->sess.hasPendingOutput()) {
                    ok = b->sess.flush(sink, t);
                }
                if (!ok || b->sess.closed()) {
                    closeClient(closeText(b->sess.closeReason()));
                } else if (b->sess.hasPendingOutput()) {
                    if (!pending_since) pending_since = t | 1u;
                    else if ((uint32_t)(t - pending_since) > HA_DEFER_LIMIT_MS) closeClient("replies deferred");
                } else {
                    pending_since = 0;
                }
            }
            s_subscribed = s_csock >= 0 && b->sess.subscribed();
        }
    }

    // Clean stop (feature disabled).
    const size_t dma_before = dmaFree();
    closeAll(true);
    s_subscribed = false;
    Serial.printf("[HA] stop dma=%u after=%u\n", (unsigned)dma_before, (unsigned)dmaFree());

    // Detach the block from mainAppTask's publisher, then free it.
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_block = nullptr;
    if (s_mtx) xSemaphoreGive(s_mtx);
    heap_caps_free(b);

    s_task_dead_ms = millis();
    s_dead_set     = true;
    s_task_alive   = false;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// mainAppTask side
// ---------------------------------------------------------------------------

static bool graceOver(uint32_t now) {
    return !s_dead_set || (uint32_t)(now - s_task_dead_ms) >= 200;   // TCB reap window
}

static bool startTask(uint32_t now) {
    if (!s_identity_ready) formatIdentity();
    if (!s_mtx) s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    if (!s_stack) {
        s_stack = (StackType_t*)heap_caps_aligned_alloc(16, HA_TASK_STACK_BYTES, MALLOC_CAP_SPIRAM);
    }
    void* mem = s_stack ? heap_caps_malloc(sizeof(HaBlock), MALLOC_CAP_SPIRAM) : nullptr;
    if (!s_stack || !mem) {
        Serial.println("[HA] PSRAM allocation failed");
        if (mem) heap_caps_free(mem);
        setState(HS_OUT_OF_MEM);
        s_retry.set(now, 60000);
        return false;
    }
    HaBlock* b = new (mem) HaBlock();
    b->shared.clear();
    b->local.clear();
    b->shared_ver = b->local_ver = 0;
    s_block        = b;
    s_stop         = false;
    s_subscribed   = false;
    s_eagain_since = 0;
    s_csock = s_lsock = s_usock = -1;
    s_task_alive   = true;
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(haTask, "HA", HA_TASK_STACK_BYTES / sizeof(StackType_t),
                                                   NULL, 1, s_stack, &s_tcb, 1);
    if (!h) {
        s_task_alive = false;
        s_block      = nullptr;
        heap_caps_free(b);
        Serial.println("[HA] task create failed");
        setState(HS_OUT_OF_MEM);
        s_retry.set(now, 60000);
        return false;
    }
    setState(HS_WAIT_WIFI);   // the task takes over from here
    return true;
}

void haTick() {
    if (!s_work) return;
    ensureLoaded();
    const uint32_t now = millis();

    if (!s_enabled) {
        if (s_task_alive) return;           // closing its sockets
        if (!graceOver(now)) return;
        if (s_stack) {
            heap_caps_free(s_stack);
            s_stack = nullptr;
        }
        s_work = false;
        setState(HS_OFF);
        return;
    }

    haSnapshotTick(s_subscribed, s_debug);   // edge counters run with or without a client

    if (!s_task_alive) {
        if (!graceOver(now) || !s_retry.due(now)) return;
        if (WiFi.status() != WL_CONNECTED) { setState(HS_WAIT_WIFI); return; }
        if (dmaFree() < (size_t)(ART_MIN_DMA_PRE_BURST + HA_DMA_BUDGET_BYTES)) {
            setState(HS_WAIT_MEM);
            s_retry.set(now, 60000);        // re-checked every 60 s
            return;
        }
        startTask(now);
    }
}
