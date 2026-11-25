// Host harness server for the ESPHome native API code under include/.
//
// Compiles esphome_session.h, esphome_entities.h and esphome_mdns.h unchanged
// and serves them on 127.0.0.1 so the real aioesphomeapi client and
// python-zeroconf (tools/esphome_host/check_client.py) can test them.
//
//   host_server <tcp-port> [--debug] [--quiet] [--udp-port N]
//
// --quiet sends the initial states only and no periodic pushes, so the
// connection is idle and the client's own 20 s keepalive Ping must fire.
//
// Prints "READY" once listening. One client at a time. A fake snapshot
// changes every second. The mDNS part is a plain UDP socket on --udp-port
// (no multicast): a query is parsed with parseQuery and the reply built with
// buildResponse is sent back to the sender. This is a host test tool, not
// firmware: blocking-free poll() loop, no radio gates.
//
// Build: g++ -std=gnu++17 -Wall -Wextra -Iinclude -o /tmp/esphome_host_server \
//        tools/esphome_host/host_server.cpp
// FIRMWARE_VERSION is passed with -D by run.sh (read from include/ui_common.h).

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "esphome_entities.h"
#include "esphome_mdns.h"
#include "esphome_session.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0-harness"
#endif

using namespace esphome;

static uint32_t nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static int listenTcp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0 || listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

static int bindUdp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

static int sinkSend(void* ctx, const uint8_t* data, size_t len) {
    int fd = *(int*)ctx;
    ssize_t n = send(fd, data, len, 0);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

// Values change every second so state pushes can be observed. The text
// changes every 3 s and contains multi-byte UTF-8. sync_daily_age is always
// unavailable to exercise missing_state.
static void fillSnapshot(HaSnapshot& s, uint32_t now_ms, uint32_t start_ms) {
    uint32_t sec = (now_ms - start_ms) / 1000u;
    s.clear();
    char buf[96];
    snprintf(buf, sizeof(buf), "Beyonc\xC3\xA9 - Track %u \xE2\x99\xAA", (unsigned)(sec / 3u));
    copyText(s.now_playing, sizeof(s.now_playing), buf);
    s.playing = (sec / 5u) % 2u == 0;
    copyText(s.speaker, sizeof(s.speaker), "Living Room");
    s.volume = (float)(20u + sec % 30u);
    s.wifi_rssi = -50.0f - (float)((sec * 4u) % 40u);
    s.uptime_s = sec;
    s.heap_free = 200000u + 1024u * (sec % 7u);
    s.heap_min = 150000u;
    s.heap_largest = 120000u;
    s.dma_free = 60000u + 1024u * (sec % 5u);
    s.psram_free = 20u * 1024u * 1024u;
    for (int i = 0; i < kStackCount; i++) s.stack[i] = 1000u + 100u * (uint32_t)i;
    copyText(s.reset_reason, sizeof(s.reset_reason), "power on");
    copyText(s.reboot_history, sizeof(s.reboot_history), "power on 12s; software 3600s");
    s.unexpected_reboots = 1;
    s.wifi_reconnects = 2;
    s.discovery_runs = 3;
    s.art_dma_aborts = 4;
    s.dma_recoveries = 5;
    s.ha_shed_events = 6;
    s.ha_tx_deferrals = sec;
    s.speaker_count = 7;
    s.sync_refresh_age_s = sec;
    s.sync_daily_age_s = 0;
    s.coredump_present = false;
    for (int i = 0; i < E_COUNT; i++) s.setValid(i, i != E_SYNC_DAILY_AGE);
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <tcp-port> [--debug] [--quiet] [--udp-port N]\n", argv[0]);
        return 2;
    }
    uint16_t port = (uint16_t)atoi(argv[1]);
    uint16_t udp_port = 0;
    bool debug = false;
    bool quiet = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--debug")) debug = true;
        else if (!strcmp(argv[i], "--quiet")) quiet = true;
        else if (!strcmp(argv[i], "--udp-port") && i + 1 < argc) udp_port = (uint16_t)atoi(argv[++i]);
        else {
            fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }

    static const uint8_t mac[6] = {0x24, 0xEC, 0x4A, 0x12, 0x34, 0x56};
    static char mac_colon[18], mac_hex[13], instance[16];
    mdns::formatMacColon(mac, mac_colon);
    mdns::formatMacHex(mac, mac_hex);
    mdns::formatInstance(mac, instance);

    DeviceIdentity id = {};
    id.name = instance;
    id.friendly_name = "SonosESP Panel";
    id.mac = mac_colon;
    id.esphome_version = FIRMWARE_VERSION;
    id.compilation_time = __DATE__ " " __TIME__;
    id.model = "JC4880P443C 4in (ESP32-P4 v1.3)";
    id.manufacturer = "SonosESP";
    id.project_name = "OpenSurface.SonosESP";
    id.project_version = FIRMWARE_VERSION;

    int lfd = listenTcp(port);
    if (lfd < 0) {
        fprintf(stderr, "cannot listen on 127.0.0.1:%u: %s\n", port, strerror(errno));
        return 1;
    }
    int ufd = -1;
    if (udp_port) {
        ufd = bindUdp(udp_port);
        if (ufd < 0) {
            fprintf(stderr, "cannot bind udp 127.0.0.1:%u: %s\n", udp_port, strerror(errno));
            return 1;
        }
    }

    static Session session;
    static HaSnapshot snap;
    int cfd = -1;
    uint32_t start = nowMs();
    uint32_t last_tick = 0;
    uint32_t connected_at = 0;
    unsigned pings_seen = 0;
    mdns::Host host = {instance, {127, 0, 0, 1}, HA_PORT, mac_hex};
    host.port = port;

    printf("READY\n");
    fflush(stdout);

    for (;;) {
        pollfd fds[3];
        int n = 0;
        fds[n++] = {lfd, POLLIN, 0};
        int ci = -1, ui = -1;
        if (cfd >= 0) {
            ci = n;
            fds[n++] = {cfd, (short)(POLLIN | (session.hasPendingOutput() ? POLLOUT : 0)), 0};
        }
        if (ufd >= 0) {
            ui = n;
            fds[n++] = {ufd, POLLIN, 0};
        }
        poll(fds, (nfds_t)n, 100);
        uint32_t now = nowMs();
        Sink sink = {sinkSend, &cfd};

        if (fds[0].revents & POLLIN) {
            int fd = accept(lfd, nullptr, nullptr);
            if (fd >= 0) {
                if (cfd >= 0) {
                    close(fd);  // one client at a time
                } else {
                    cfd = fd;
                    fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL, 0) | O_NONBLOCK);
                    int one = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    session.begin(debug, id);
                    fillSnapshot(snap, now, start);
                    session.setSnapshot(&snap);
                    connected_at = now;
                    last_tick = now;
                    pings_seen = 0;
                    fprintf(stderr, "[harness] client connected debug=%d entities=%d\n", (int)debug,
                            session.entityTotal());
                }
            }
        }

        if (ci >= 0 && cfd >= 0) {
            if (fds[ci].revents & (POLLIN | POLLHUP | POLLERR)) {
                uint8_t buf[512];
                ssize_t r = recv(cfd, buf, sizeof(buf), 0);
                if (r > 0) {
                    // Count Ping requests (type 7, empty payload: 00 00 07).
                    for (ssize_t i = 0; i + 2 < r; i++)
                        if (buf[i] == 0x00 && buf[i + 1] == 0x00 && buf[i + 2] == 0x07) {
                            pings_seen++;
                            fprintf(stderr, "[harness] ping %u at +%u ms\n", pings_seen,
                                    (unsigned)(now - connected_at));
                        }
                    session.feed(buf, (size_t)r, sink, now);
                } else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    fprintf(stderr, "[harness] client gone (recv=%zd)\n", r);
                    close(cfd);
                    cfd = -1;
                }
            }
        }

        if (cfd >= 0) {
            if (now - last_tick >= 1000u) {
                last_tick = now;
                fillSnapshot(snap, now, start);
                if (quiet) session.flush(sink, now);
                else session.pushStates(sink, now);
            } else {
                session.flush(sink, now);
            }
            if (!session.helloDone() && now - connected_at >= HA_HELLO_TIMEOUT_MS) {
                fprintf(stderr, "[harness] no Hello within %d ms, closing\n", HA_HELLO_TIMEOUT_MS);
                close(cfd);
                cfd = -1;
            } else if (session.closed()) {
                fprintf(stderr, "[harness] session closed reason=%d pings=%u\n",
                        (int)session.closeReason(), pings_seen);
                shutdown(cfd, SHUT_WR);
                close(cfd);
                cfd = -1;
            }
        }

        if (ui >= 0 && (fds[ui].revents & POLLIN)) {
            uint8_t pkt[1500];
            sockaddr_in from = {};
            socklen_t fl = sizeof(from);
            ssize_t r = recvfrom(ufd, pkt, sizeof(pkt), 0, (sockaddr*)&from, &fl);
            if (r > 0) {
                uint8_t want = mdns::parseQuery(pkt, (size_t)r, instance);
                uint8_t out[512];
                size_t sz = want ? mdns::buildResponse(want, host, 120, out, sizeof(out)) : 0;
                fprintf(stderr, "[harness] mdns query len=%zd want=0x%02x reply=%zu\n", r, want, sz);
                if (sz) sendto(ufd, out, sz, 0, (sockaddr*)&from, fl);
            }
        }
    }
}
