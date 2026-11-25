/**
 * Sonos Device Discovery - SSDP/UPnP Implementation
 * Handles multicast/broadcast discovery and device metadata fetching
 */

#include "sonos_controller.h"
#include "ui_common.h"
#include "sonos_topology.h"
#include "sonos_sync_logic.h"
#include <esp_task_wdt.h>

// ============================================================================
// Discovery
// ============================================================================
int SonosController::discoverDevices(bool background) {
    // Off the UI task this must not touch LVGL (lv_refr_now from another task
    // races the renderer) or feed a task watchdog it is not subscribed to.
    auto wdt = [background]() { if (!background) esp_task_wdt_reset(); };
    auto ui  = [background](uint32_t ms) { if (!background) { lv_tick_inc(ms); lv_refr_now(NULL); } };

    // One scan at a time. The background refresh, the deferred bring-up and the
    // Scan button can all ask. A Scan press waits for a background refresh to
    // finish (a few seconds; the spinner keeps turning) and then scans, because a
    // button labelled Scan must scan. A background caller never waits: it reports
    // what is there and lets the schedule try again.
    if (!tryBeginScan()) {
        if (background) {
            Serial.println("[SONOS] A scan is already running - not starting another");
            return deviceCount;
        }
        const uint32_t t0 = millis();
        while (!tryBeginScan()) {
            if ((uint32_t)(millis() - t0) > 25000UL) {
                Serial.println("[SONOS] Previous scan still running after 25 s - not scanning");
                return deviceCount;
            }
            wdt();
            ui(20);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    // Held from here until every return: set before deviceCount goes to 0 and
    // cleared by the destructor. batteryPollStep() checks it and leaves devices[]
    // alone while this rewrites it (issue #165).
    struct ScanSlot {
        SonosController* c;
        ~ScanSlot() { c->endScan(); }
    } scan_slot{this};

    if (!devices) { Serial.println("[SONOS] devices array not allocated"); return 0; }
    Serial.printf("[SONOS] Starting discovery...\n");

    // Remember WHICH speaker is selected, not just its slot number.
    //
    // This function empties devices[] and refills it in SSDP-reply order, which it
    // documents further down as non-deterministic — but currentDeviceIndex was
    // never touched, and no caller re-selects afterwards (selection only happens
    // on a row tap). So if the rooms came back in a different order, index 0 was
    // still "selected" while now pointing at a different speaker: transport
    // commands, the volume slider and the sleep timer all silently moved to
    // another room, and the devices screen highlighted that room as the chosen
    // one. A reboot restored the old choice from NVS, which is what made it look
    // intermittent rather than reproducible.
    //
    // Captured by identity here and matched back after dedup below.
    String prev_rincon;
    IPAddress prev_ip;
    if (SonosDevice* cur = getCurrentDevice()) {
        prev_rincon = cur->rinconID;
        prev_ip     = cur->ip;
    }
    // Under the lock: the polling task holds pointers into devices[] and reads
    // them against deviceCount. Dropping it to 0 unlocked meant getCurrentDevice()
    // could hand back a slot this function was about to overwrite.
    if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(200))) {
        deviceCount = 0;
        xSemaphoreGive(deviceMutex);
    } else {
        deviceCount = 0;   // proceed anyway; a scan the user asked for must not stall
    }

    udp.stop();
    vTaskDelay(pdMS_TO_TICKS(50));

    IPAddress multicast(239, 255, 255, 250);
    // Bind a UDP socket to receive unicast SSDP responses (replies go to the sender's source port)
    if (!udp.begin(1900)) {
        Serial.printf("[SONOS] UDP begin failed on port 1900\n");
        return 0;
    }

    const char* msg =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";

    // Send discovery to both multicast (239.255.255.250) AND broadcast (255.255.255.255)
    // This ensures discovery works even if routers don't properly handle multicast
    // Send 5 bursts with 500ms spacing for balance between coverage and speed
    IPAddress broadcast(255, 255, 255, 255);

    for (int burst = 0; burst < 5; burst++) {
        // Held only across the two sends, never across the listen window below:
        // the receive loop runs for 15s and holding the link that long would
        // starve polling and the art task into the SDIO idle/clock-gate state
        // the cooldowns exist to avoid.
        const bool locked = network_mutex &&
            xSemaphoreTake(network_mutex, pdMS_TO_TICKS(NETWORK_MUTEX_TIMEOUT_MS)) == pdTRUE;

        // Send to multicast address (standard UPnP)
        udp.beginPacket(multicast, 1900);
        udp.write((const uint8_t*)msg, strlen(msg));
        udp.endPacket();

        // Also send to broadcast address (for networks with multicast issues)
        udp.beginPacket(broadcast, 1900);
        udp.write((const uint8_t*)msg, strlen(msg));
        udp.endPacket();

        if (locked) {
            last_network_end_ms = millis();
            xSemaphoreGive(network_mutex);
        }

        Serial.printf("[SONOS] Sent discovery burst %d/5 (multicast + broadcast)\n", burst + 1);

        if (burst < 4) {
            vTaskDelay(pdMS_TO_TICKS(500));  // 500ms between bursts - balance between coverage and speed
        }
    }

    int rawDeviceCount = 0;  // Count of IPs found before dedup
    unsigned long start = millis();
    unsigned long lastUIUpdate = 0;
    while (millis() - start < 15000) {  // 15 seconds total (5 bursts * 0.5s = 2.5s send + 12.5s listen)
        int size = udp.parsePacket();
        if (size > 0) {
            char buf[1025];  // 1024 + 1 for null terminator
            int len = udp.read(buf, sizeof(buf) - 1);
            if (len > 0 && len < (int)sizeof(buf)) {  // Safety check
                buf[len] = 0;
                String resp = buf;
                resp.toLowerCase();
                if (resp.indexOf("sonos") >= 0 || resp.indexOf("zoneplayer") >= 0) {
                    IPAddress ip = udp.remoteIP();

                    bool exists = false;
                    for (int i = 0; i < deviceCount; i++) {
                        if (devices[i].ip == ip) { exists = true; break; }
                    }

                    // Initialising a slot writes several Strings (roomName,
                    // rinconID, repeatMode, groupCoordinatorUUID). A String
                    // assignment frees the previous buffer, so this has to be
                    // closed to the polling task and the UI, both of which read
                    // those fields.
                    const bool slot_locked =
                        !exists && deviceCount < MAX_SONOS_DEVICES &&
                        xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(200)) == pdTRUE;

                    if (!exists && deviceCount < MAX_SONOS_DEVICES) {
                        devices[deviceCount].ip = ip;
                        devices[deviceCount].pairIP = IPAddress();
                        devices[deviceCount].roomName = ip.toString();
                        // Identity MUST be cleared with the rest of the slot. deviceCount
                        // was reset to 0 at the top of this scan, so slot 0 is very often
                        // the fast-boot cached speaker — and getRoomName() below leaves
                        // rinconID untouched when the description fetch fails. Without this
                        // line an unidentified responder inherits the previous occupant's
                        // RINCON, two entries then claim the same id, and the Groups screen
                        // renders a phantom speaker named by its bare IP.
                        devices[deviceCount].rinconID = "";
                        // Same for capabilities: unknown until getRoomName() answers.
                        devices[deviceCount].hasLineIn = false;
                        devices[deviceCount].lineInKnown = false;
                        devices[deviceCount].isPlaying = false;
                        devices[deviceCount].volume = 50;
                        devices[deviceCount].isMuted = false;
                        devices[deviceCount].shuffleMode = false;
                        devices[deviceCount].repeatMode = "NONE";
                        devices[deviceCount].connected = false;
                        devices[deviceCount].errorCount = 0;
                        devices[deviceCount].currentTrackNumber = 0;
                        devices[deviceCount].totalTracks = 0;
                        devices[deviceCount].queueSize = 0;
                        devices[deviceCount].groupCoordinatorUUID = "";
                        devices[deviceCount].isGroupCoordinator = true;  // Standalone by default
                        devices[deviceCount].groupMemberCount = 1;

                        Serial.printf("[SONOS] SSDP Response #%d: %s\n", deviceCount + 1, ip.toString().c_str());
                        deviceCount++;
                        rawDeviceCount++;
                        if (slot_locked) xSemaphoreGive(deviceMutex);
                    } else if (exists) {
                        Serial.printf("[SONOS] Ignoring duplicate SSDP response from: %s\n", ip.toString().c_str());
                    }

                    if (deviceCount >= MAX_SONOS_DEVICES) {
                        Serial.printf("[SONOS] Reached MAX_SONOS_DEVICES limit (%d)\n", MAX_SONOS_DEVICES);
                    }
                }
            }
        }

        // Update UI periodically to keep spinner animating
        if (millis() - lastUIUpdate > 20) {
            ui(20);   // Force display refresh (UI task only)
            lastUIUpdate = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    udp.stop();

    Serial.printf("[SONOS] Discovery window closed. Found %d raw IP(s) before deduplication.\n", rawDeviceCount);

    if (deviceCount == 0) {
        Serial.printf("[SONOS] No Sonos devices responded to discovery. Check network connectivity and ensure devices are powered on.\n");
        return 0;
    }

    if (deviceCount == 1 && rawDeviceCount == 1) {
        Serial.printf("[SONOS] Only 1 device found. If you have more speakers, try scanning again or check:\n");
        Serial.printf("[SONOS]   - All speakers are powered on and connected to WiFi\n");
        Serial.printf("[SONOS]   - ESP32 and Sonos devices are on the same network/VLAN\n");
        Serial.printf("[SONOS]   - Router allows multicast/UPnP traffic\n");
    }

    // Fetch room names for all discovered devices.
    //
    // The result is CHECKED, and a failure is retried (issue #182).
    //
    // v2.1.1 gave getRoomName() a network_mutex gate, and that gate gives up and
    // returns false when the mutex is busy — without ever sending the request.
    // The compaction below then drops any device still carrying its placeholder
    // name and no RINCON. Before v2.1.1 there was no gate, so the GET was always
    // attempted and a device was only dropped when the speaker itself failed to
    // answer. Afterwards, a speaker could be dropped merely because the radio was
    // busy at the moment we reached it — and the art task holds network_mutex for
    // the length of a cover download, so "busy" is the normal state during a scan
    // with music playing. A real speaker vanished from the list.
    //
    // A skipped probe is a local scheduling accident, not a property of the
    // speaker, so it must not be terminal. Retry each failure a couple of times
    // with a short gap; by then an art download will usually have finished.
    // Genuine failures (speaker off, wrong VLAN, no HTTP) still fall through to
    // the drop, exactly as they did before.
    Serial.printf("[SONOS] Fetching room names for %d device(s)...\n", deviceCount);
    for (int i = 0; i < deviceCount; i++) {
        Serial.printf("[SONOS] Fetching room name %d/%d from %s\n", i + 1, deviceCount, devices[i].ip.toString().c_str());
        wdt();  // Each getRoomName() takes ~1-2s; 17 devices × 2s = 34s > 30s WDT

        bool ok = getRoomName(&devices[i]);
        for (int attempt = 1; !ok && attempt <= DISCOVERY_ROOMNAME_RETRIES; attempt++) {
            Serial.printf("[SONOS]   Retry %d/%d for %s\n",
                          attempt, DISCOVERY_ROOMNAME_RETRIES, devices[i].ip.toString().c_str());
            // Let whoever holds network_mutex finish. Short enough that the worst
            // case stays inside the watchdog window, which is fed each iteration.
            vTaskDelay(pdMS_TO_TICKS(DISCOVERY_ROOMNAME_RETRY_MS));
            wdt();
            ok = getRoomName(&devices[i]);
        }
        if (!ok) {
            Serial.printf("[SONOS]   -> %s did not answer its description after %d attempts\n",
                          devices[i].ip.toString().c_str(), DISCOVERY_ROOMNAME_RETRIES + 1);
        }
        Serial.printf("[SONOS]   -> Room name: '%s'\n", devices[i].roomName.c_str());

        // Update UI while fetching room names
        ui(10);
    }

    // Drop anything that answered SSDP but never resolved into a real speaker.
    //
    // udp.begin(1900) binds the SSDP multicast port, so this socket also receives
    // unsolicited NOTIFY/ssdp:alive traffic from the whole LAN, not just replies to our
    // M-SEARCH. Anything whose payload merely contains "sonos" or "zoneplayer" is accepted
    // above. A real ZonePlayer answers /xml/device_description.xml with a room name and a
    // UDN; something that answers neither is not a speaker we can drive — every group,
    // queue and favourite URI is built from the RINCON id.
    //
    // Conservative on purpose: a device is only dropped when it has NO RINCON *and*
    // getRoomName() never replaced the placeholder name (roomName is still the raw IP).
    // A speaker that names itself but loses its UDN to a transient failure is kept.
    {
        // `devices[kept] = devices[i]` copies a struct holding ten-odd Strings:
        // the destination's buffers are freed and reallocated. Compaction runs
        // while the polling task may be reading the very slots being shuffled,
        // so the whole pass is closed to it. Released BEFORE the early return
        // below - returning while holding it would strand the mutex for good.
        const bool compact_locked =
            xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(500)) == pdTRUE;
        int kept = 0;
        for (int i = 0; i < deviceCount; i++) {
            bool identified = devices[i].rinconID.length() > 0 ||
                              devices[i].roomName != devices[i].ip.toString();
            if (identified) {
                if (i != kept) devices[kept] = devices[i];
                kept++;
            } else {
                Serial.printf("[SONOS]   [DROPPED] %s answered SSDP but has no room name or "
                              "RINCON — not a controllable ZonePlayer\n",
                              devices[i].ip.toString().c_str());
            }
        }
        if (kept != deviceCount) {
            Serial.printf("[SONOS] Dropped %d unidentified responder(s)\n", deviceCount - kept);
            deviceCount = kept;
        }
        if (compact_locked) xSemaphoreGive(deviceMutex);
        if (deviceCount == 0) {
            Serial.println("[SONOS] No identifiable Sonos devices after filtering");
            return 0;
        }
    }

    // Fetch zone group topology to identify coordinator IPs for stereo pairs / bonded zones.
    // Both speakers in a stereo pair respond to SSDP with the same room name, but only the
    // coordinator IP accepts commands. SSDP response order is non-deterministic, so we must
    // explicitly identify the coordinator rather than just keeping the first-seen IP.
    const int MAX_COORDINATORS = 16;
    String coordinatorRINCONs[MAX_COORDINATORS];
    int coordCount = 0;
    // Bonded subs, surrounds and the second half of a stereo pair, by RINCON
    // (issue #211). Filled by the same GetZoneGroupState call; stays empty when
    // the topology request fails, which leaves the discovery result as it was.
    const int MAX_HIDDEN = 16;
    String hiddenRincons[MAX_HIDDEN];
    int hiddenCount = 0;
    if (deviceCount > 0) {
        wdt();  // getRoomName loop may have consumed most of the WDT window
        Serial.printf("[SONOS] Fetching zone topology from %s...\n", devices[0].ip.toString().c_str());
        coordCount = fetchTopologyCoordinators(devices[0].ip, coordinatorRINCONs, MAX_COORDINATORS,
                                               hiddenRincons, MAX_HIDDEN, &hiddenCount);
        if (coordCount == 0) {
            Serial.printf("[SONOS] Topology unavailable - falling back to first-seen deduplication\n");
        }
    }

    // Helper: returns true if dev's RINCON ID appears in the coordinator list
    auto isCoordinator = [&](const SonosDevice& dev) -> bool {
        if (dev.rinconID.length() == 0 || coordCount == 0) return false;
        String rinconLower = dev.rinconID;
        rinconLower.toLowerCase();
        for (int k = 0; k < coordCount; k++) {
            if (coordinatorRINCONs[k] == rinconLower) return true;
        }
        return false;
    };

    // Coordinator-aware deduplication by room name.
    // When two IPs share the same room name (stereo pair / bonded zone), keep the coordinator.
    Serial.printf("[SONOS] Starting coordinator-aware deduplication...\n");
    // Same reasoning as compaction above: this pass both reads every slot's
    // Strings and overwrites slots wholesale. No network or LVGL call happens
    // inside it, so holding the lock for the pass is safe - and it has no early
    // return, so there is nothing to strand.
    const bool dedup_locked = xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(500)) == pdTRUE;
    int uniqueCount = 0;
    for (int i = 0; i < deviceCount; i++) {
        String normalizedCurrent = devices[i].roomName;
        normalizedCurrent.trim();
        normalizedCurrent.toLowerCase();
        bool iIsCoord = isCoordinator(devices[i]);

        bool isDuplicate = false;
        int replaceIdx = -1;  // index in unique list to swap (if i is coordinator and j is not)
        int keptIdx = -1;     // the entry i duplicates
        bool twin = false;    // a stereo pair: same room, different speaker

        for (int j = 0; j < uniqueCount; j++) {
            String normalizedExisting = devices[j].roomName;
            normalizedExisting.trim();
            normalizedExisting.toLowerCase();

            // Same room name OR same RINCON. Name alone was not enough: an entry that
            // failed its description fetch keeps the placeholder IP as its name, so it
            // never collided with the real speaker it was duplicating.
            bool sameName = (normalizedExisting == normalizedCurrent);
            bool sameUuid = devices[i].rinconID.length() > 0 &&
                            SonosController::uuidEquals(devices[j].rinconID, devices[i].rinconID);

            if (sameName || sameUuid) {
                if (sameUuid && !sameName) {
                    Serial.printf("[SONOS]   [SAME RINCON] '%s' (%s) and '%s' (%s) share %s\n",
                        devices[i].roomName.c_str(), devices[i].ip.toString().c_str(),
                        devices[j].roomName.c_str(), devices[j].ip.toString().c_str(),
                        devices[i].rinconID.c_str());
                }
                isDuplicate = true;
                keptIdx = j;
                twin = sameName && !sameUuid;
                if (iIsCoord && !isCoordinator(devices[j])) {
                    // Current (i) is the coordinator; existing (j) is the slave — swap
                    replaceIdx = j;
                    Serial.printf("[SONOS]   [COORD SWAP] '%s': slave %s → coordinator %s\n",
                        devices[i].roomName.c_str(),
                        devices[j].ip.toString().c_str(),
                        devices[i].ip.toString().c_str());
                } else {
                    Serial.printf("[SONOS]   [DUPLICATE] '%s' (%s) filtered (keeping %s%s)\n",
                        devices[i].roomName.c_str(), devices[i].ip.toString().c_str(),
                        devices[j].ip.toString().c_str(),
                        isCoordinator(devices[j]) ? " [coord]" : "");
                }
                break;
            }
        }

        if (!isDuplicate) {
            Serial.printf("[SONOS]   [UNIQUE] '%s' (%s)%s\n",
                devices[i].roomName.c_str(), devices[i].ip.toString().c_str(),
                iIsCoord ? " [coord]" : "");
            if (i != uniqueCount) {
                devices[uniqueCount] = devices[i];
            }
            uniqueCount++;
        } else if (replaceIdx >= 0) {
            const IPAddress slaveIP = devices[replaceIdx].ip;
            devices[replaceIdx] = devices[i];  // Replace slave with coordinator
            if (twin) devices[replaceIdx].pairIP = slaveIP;
        } else if (twin && keptIdx >= 0 && devices[keptIdx].pairIP == IPAddress()) {
            // The dropped half of a stereo pair still has a battery if it is a
            // portable (issue #165). Keep its address so it can be read. First
            // twin wins: a home-theatre room drops several, none of them portable.
            devices[keptIdx].pairIP = devices[i].ip;
        }
    }

    int filteredCount = deviceCount - uniqueCount;
    if (filteredCount > 0) {
        Serial.printf("[SONOS] Filtered %d duplicate(s) from stereo pairs / bonded zones\n", filteredCount);
    } else {
        Serial.printf("[SONOS] No duplicates found - all %d devices are unique\n", uniqueCount);
    }
    deviceCount = uniqueCount;

    // Drop bonded Subs / Sub Minis, surrounds and invisible pair halves (#211).
    // After the stereo-pair dedup on purpose: that pass reads the twin's address
    // for the battery badge, and only needs the twin to still be present.
    // Never drops the last speaker, so a mis-parse cannot empty the list.
    if (hiddenCount > 0) {
        int kept = 0;
        for (int i = 0; i < deviceCount; i++) {
            bool hide = false;
            for (int h = 0; h < hiddenCount && !hide; h++)
                hide = uuidEquals(devices[i].rinconID, hiddenRincons[h]);
            if (hide && deviceCount - (i - kept) > 1) {
                Serial.printf("[SONOS]   [BONDED/HIDDEN] '%s' (%s) excluded by topology\n",
                              devices[i].roomName.c_str(), devices[i].ip.toString().c_str());
                continue;
            }
            if (i != kept) devices[kept] = devices[i];
            kept++;
        }
        deviceCount = kept;
    }
    if (dedup_locked) xSemaphoreGive(deviceMutex);

    // Re-point the selection at the same speaker, wherever it landed.
    //
    // RINCON first — it is the stable identity and survives a DHCP lease change;
    // IP as a fallback for a speaker whose description fetch failed and left the
    // id empty. Only runs when something was selected before: prev_rincon and
    // prev_ip are both empty on a first scan, so a fresh boot is unaffected and
    // nothing here auto-selects anything that was not already chosen.
    //
    // -1 when the old speaker is genuinely gone is the correct answer, not a
    // regression: getCurrentDevice() returns nullptr for it and the UI already
    // handles that with the "Device Not Connected" state. Silently driving
    // whichever room happens to occupy the old slot is the worse outcome.
    if (prev_rincon.length() > 0 || (uint32_t)prev_ip != 0) {
        int restored = -1;
        for (int i = 0; i < deviceCount; i++) {
            if ((prev_rincon.length() > 0 && uuidEquals(devices[i].rinconID, prev_rincon)) ||
                devices[i].ip == prev_ip) {
                restored = i;
                break;
            }
        }
        if (restored != currentDeviceIndex) {
            if (restored >= 0) {
                Serial.printf("[SONOS] Selected speaker '%s' moved to slot %d - selection followed it\n",
                              devices[restored].roomName.c_str(), restored);
            } else {
                Serial.println("[SONOS] Previously selected speaker is no longer present - selection cleared");
            }
            currentDeviceIndex = restored;
        }
    }

    // Fetch playing state for each unique zone so the devices screen shows which rooms are active
    Serial.printf("[SONOS] Fetching playing state for %d zone(s)...\n", deviceCount);
    for (int i = 0; i < deviceCount; i++) {
        // Same reason as the room-name loop above: fetchDevicePlayingState() blocks
        // for up to its 3s HTTP timeout, and MAX_SONOS_DEVICES is 32 — 11 slow zones
        // already exceeds the 30s WDT, and this runs on the WDT-subscribed mainAppTask.
        wdt();
        devices[i].isPlaying = fetchDevicePlayingState(&devices[i]);
        Serial.printf("[SONOS]   '%s' (%s): %s\n",
            devices[i].roomName.c_str(), devices[i].ip.toString().c_str(),
            devices[i].isPlaying ? "PLAYING" : "stopped");
        // Keep UI alive during the SOAP calls
        ui(10);
    }

    if (deviceCount > 0) {
        prefs.putString("device_ip", devices[0].ip.toString());
        // Remember the whole list, not just the selected room, so the next boot
        // starts with every speaker and group instead of one.
        cacheDeviceList();
    }
    // Off the UI task nobody repaints the speaker and group screens for us.
    if (background) notifyUI(UPDATE_GROUPS);

    Serial.printf("[SONOS] Discovery complete: %d visible zone(s)\n", deviceCount);
    return deviceCount;
}

// Fetch zone group topology via GetZoneGroupState SOAP and extract coordinator RINCON IDs.
// Returns the number of coordinators found (0 on failure).
// Coordinators are stored lowercase for case-insensitive comparison.
//
// NOTE: /status/topology was deprecated and no longer returns coordinator info on modern
// Sonos firmware. GetZoneGroupState (ZoneGroupTopology service) is the correct API.
// The ZoneGroupState inner XML may be HTML-entity-encoded inside the SOAP response.
int SonosController::fetchTopologyCoordinators(IPAddress ip, String* coordinatorRINCONs, int maxCount,
                                               String* hidden, int maxHidden, int* hiddenCount) {
    if (hiddenCount) *hiddenCount = 0;
    HTTPClient http;
    char url[128];
    snprintf(url, sizeof(url), "http://%s:1400/ZoneGroupTopology/Control", ip.toString().c_str());

    static const char soapBody[] =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:GetZoneGroupState xmlns:u=\"urn:schemas-upnp-org:service:ZoneGroupTopology:1\">"
        "</u:GetZoneGroupState>"
        "</s:Body></s:Envelope>";

    // Serialised on network_mutex like every other network call in the firmware.
    //
    // Discovery used to be the ONE path that skipped it. Every other caller -
    // sendSOAP(), the album art download, the lyrics fetch, the clock's weather,
    // the battery probe - takes this mutex, because the C6 SDIO link asserts when
    // two TCP conversations fill pkt_rxbuff at the same time. That is the whole
    // reason the mutex exists (see the SDIO defence notes). A scan fired one of
    // these per speaker straight past all eight defence layers, while the polling
    // task was mid-SOAP and the art task could be mid-download.
    if (!network_mutex ||
        xSemaphoreTake(network_mutex, pdMS_TO_TICKS(NETWORK_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        Serial.println("[SONOS] Topology fetch skipped - network busy");
        return 0;
    }

    http.begin(url);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    http.addHeader("SOAPAction", "\"urn:schemas-upnp-org:service:ZoneGroupTopology:1#GetZoneGroupState\"");

    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();  // only when subscribed (not the background sync task)
    //                     http.setTimeout(3000) bounds this call
                           // but WDT reset ensures no 30s window overrun even if timeout
                           // fires at the edge of the window (e.g. 17 × 2s = 34s loop)
    int count = 0;
    int code = http.POST((uint8_t*)soapBody, strlen(soapBody));
    if (code == 200) {
        String xml = http.getString();
        // The ZoneGroupState inner XML is typically HTML-entity-encoded inside the SOAP response
        // (e.g. &lt;ZoneGroups&gt; ... &lt;/ZoneGroups&gt;).  Decode then lowercase to handle
        // both "Coordinator=" and any case variation uniformly.
        xml.replace("&lt;", "<");
        xml.replace("&gt;", ">");
        xml.replace("&quot;", "\"");
        xml.replace("&amp;", "&");  // must be last to avoid double-decoding
        xml.toLowerCase();

        // Parse all Coordinator="RINCON_..." attributes from ZoneGroup elements.
        // After decoding + lowercase, they appear as: coordinator="rincon_..."
        int searchPos = 0;
        while (count < maxCount) {
            int pos = xml.indexOf("coordinator=\"", searchPos);
            if (pos < 0) break;
            pos += 13;  // skip past coordinator="
            int end = xml.indexOf("\"", pos);
            if (end <= pos) break;
            String rincon = xml.substring(pos, end);
            if (rincon.length() > 0) {
                coordinatorRINCONs[count++] = rincon;
                Serial.printf("[SONOS]   Topology coordinator: %s\n", rincon.c_str());
            }
            searchPos = end + 1;
        }
        Serial.printf("[SONOS] Topology: %d coordinator(s) found\n", count);

        // Bonded / invisible members, from the same payload (issue #211).
        if (hidden && hiddenCount && maxHidden > 0) {
            static sonos_topology::Member s_members[48];   // one caller at a time: discovery is serialised
            const int n = sonos_topology::parseMembers(xml.c_str(), xml.length(), s_members, 48);
            for (int i = 0; i < n && *hiddenCount < maxHidden; i++) {
                if (!s_members[i].hidden) continue;
                hidden[*hiddenCount] = String(s_members[i].uuid.ptr, s_members[i].uuid.len);
                (*hiddenCount)++;
            }
        }
    } else {
        Serial.printf("[SONOS] GetZoneGroupState failed (HTTP %d)\n", code);
    }
    http.end();
    // Same bookkeeping sendSOAP() does, so the art and lyrics cooldowns can see
    // that the link was busy just now.
    last_network_end_ms = millis();
    xSemaphoreGive(network_mutex);
    return count;
}

// Send a GetTransportInfo SOAP request directly to a device's IP and return true if playing.
// Used during discovery to populate isPlaying for all zones (not just the selected one).
bool SonosController::fetchDevicePlayingState(SonosDevice* dev) {
    HTTPClient http;
    char url[128];
    snprintf(url, sizeof(url), "http://%s:1400/MediaRenderer/AVTransport/Control",
             dev->ip.toString().c_str());

    static const char soapBody[] =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:GetTransportInfo xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID></u:GetTransportInfo>"
        "</s:Body></s:Envelope>";

    // Serialised - see the note in fetchTopologyCoordinators(). This one runs once
    // per discovered zone, so on a large system it was the biggest offender.
    if (!network_mutex ||
        xSemaphoreTake(network_mutex, pdMS_TO_TICKS(NETWORK_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        Serial.printf("[SONOS]   Playing-state fetch skipped for %s - network busy\n",
                      dev->ip.toString().c_str());
        return false;
    }

    http.begin(url);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    http.addHeader("SOAPAction", "\"urn:schemas-upnp-org:service:AVTransport:1#GetTransportInfo\"");

    bool playing = false;
    int code = http.POST((uint8_t*)soapBody, strlen(soapBody));
    if (code == 200) {
        String resp = http.getString();
        playing = (resp.indexOf("PLAYING") >= 0);
    }
    http.end();
    last_network_end_ms = millis();
    xSemaphoreGive(network_mutex);
    return playing;
}

bool SonosController::getRoomName(SonosDevice* dev) {
    // Whether the description was actually fetched AND parsed. Returned so a
    // caller that persists hasLineIn can tell a real "no line-in" from a failed
    // request; see the cache write in tryLoadCachedDevice().
    bool parsed = false;
    HTTPClient http;
    char url[128];
    snprintf(url, sizeof(url), "http://%s:1400/xml/device_description.xml", dev->ip.toString().c_str());

    // Serialised - see the note in fetchTopologyCoordinators(). Like the
    // playing-state probe, this runs once per responder, so a scan of a large
    // system used to put a dozen unserialised TCP conversations onto the SDIO
    // link while the polling task was using it.
    if (!network_mutex ||
        xSemaphoreTake(network_mutex, pdMS_TO_TICKS(NETWORK_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        Serial.printf("[SONOS]   Room name fetch skipped for %s - network busy\n",
                      dev->ip.toString().c_str());
        return false;
    }

    http.begin(url);
    http.setTimeout(3000);  // Increased from 2s to 3s for slower networks

    int code = http.GET();
    String xml;
    if (code == 200) xml = http.getString();  // Keep String for XML parsing (indexOf/substring)
    http.end();
    last_network_end_ms = millis();
    xSemaphoreGive(network_mutex);

    if (code != 200) {
        Serial.printf("[SONOS]   HTTP GET failed with code %d for %s (keeping IP as name)\n",
                      code, dev->ip.toString().c_str());
        if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(100))) {
            dev->hasLineIn = false;   // default off: a failed fetch must not offer a dead row
            dev->lineInKnown = false; // ...but remembered as unknown, so it is asked again
            xSemaphoreGive(deviceMutex);
        }
        return false;
    }

    // Parse into locals first, then publish under deviceMutex.
    //
    // roomName and rinconID are Strings that the polling task and the UI both
    // read. Assigning a String frees the old heap buffer and allocates a new
    // one, so writing them straight into the shared slot - which is what this
    // used to do - could free a buffer another task was still reading from.
    String roomName, rincon;
    const bool hasLineIn = (xml.indexOf("AudioIn") >= 0);

    int start = xml.indexOf("<roomName>");
    int end   = xml.indexOf("</roomName>");
    if (start > 0 && end > start) {
        // Decode XML entities (&apos;) and UTF-8 smart punctuation (e.g. U+2019
        // curly apostrophe in "Raquel's Office") so they don't render as tofu in
        // the ASCII-only Montserrat label font.
        roomName = decodeHTML(xml.substring(start + 10, end));
        parsed = true;
    } else {
        Serial.printf("[SONOS]   Failed to parse room name from XML for %s\n", dev->ip.toString().c_str());
    }

    start = xml.indexOf("<UDN>uuid:");
    end   = xml.indexOf("</UDN>", start);
    if (start > 0 && end > start) {
        rincon = xml.substring(start + 10, end);
    } else {
        Serial.printf("[SONOS]   Failed to parse RINCON ID from XML for %s\n", dev->ip.toString().c_str());
    }

    if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(100))) {
        // Line-in is not browsable content — ObjectID "AI:" returns nothing on
        // S2 — so the only way to know a player has a physical input is whether
        // it advertises the AudioIn service here. Five/Amp/Port/Beam do; One
        // and SL do not, and offering them a Line-In source would be a dead row.
        dev->hasLineIn = hasLineIn;
        dev->lineInKnown = true;
        if (roomName.length() > 0) dev->roomName = roomName;
        if (rincon.length()   > 0) dev->rinconID = rincon;
        xSemaphoreGive(deviceMutex);
    } else {
        Serial.printf("[SONOS]   Parsed %s but deviceMutex busy - not published\n",
                      dev->ip.toString().c_str());
        return false;
    }

    if (parsed) Serial.printf("[SONOS]   Room name fetched successfully: '%s'\n", roomName.c_str());
    Serial.printf("[SONOS]   Line-in: %s\n", hasLineIn ? "yes" : "no");
    if (rincon.length() > 0) Serial.printf("[SONOS]   RINCON ID: %s\n", rincon.c_str());
    return parsed;
}

String SonosController::getCachedDeviceIP() {
    return prefs.getString("device_ip", "");
}

void SonosController::cacheDeviceIP(String ip) {
    prefs.putString("device_ip", ip);
}

void SonosController::cacheSelectedDevice() {
    SonosDevice* dev = getCurrentDevice();
    if (!dev) {
        Serial.println("[SONOS] Cannot cache - no device selected");
        return;
    }

    prefs.putString("cached_ip", dev->ip.toString());
    prefs.putString("cached_room", dev->roomName);
    prefs.putString("cached_rincon", dev->rinconID);
    // Line-in capability is discovered from device_description.xml and is NOT
    // re-derivable on the fast-boot path, so it has to be cached with the rest
    // or the Sources screen loses its Line-In row on every reboot.
    // Only a fetched value: an unknown one leaves the key unset (-1 on read), so
    // the next fast boot resolves it instead of trusting a default false.
    if (dev->lineInKnown) prefs.putInt("cached_linein", dev->hasLineIn ? 1 : 0);
    else if (prefs.isKey("cached_linein")) prefs.remove("cached_linein");

    Serial.printf("[SONOS] Cached device: %s (%s) [%s]\n",
                  dev->roomName.c_str(),
                  dev->ip.toString().c_str(),
                  dev->rinconID.c_str());
}

bool SonosController::tryLoadCachedDevice() {
    if (!devices) { Serial.println("[SONOS] devices array not allocated"); return false; }
    String cachedIP = prefs.getString("cached_ip", "");
    String cachedRoom = prefs.getString("cached_room", "");
    String cachedRincon = prefs.getString("cached_rincon", "");

    if (cachedIP.length() == 0 || cachedRoom.length() == 0) {
        Serial.println("========================================");
        Serial.println("[SONOS] No cached device in NVS");
        Serial.println("[SONOS] Running full SSDP discovery (~15 seconds)...");
        Serial.println("========================================");
        return false;
    }

    Serial.printf("[SONOS] Found cached device: %s (%s)\n", cachedRoom.c_str(), cachedIP.c_str());

    // Parse IP address
    IPAddress ip;
    if (!ip.fromString(cachedIP)) {
        Serial.printf("[SONOS] Invalid cached IP: %s - will run discovery\n", cachedIP.c_str());
        return false;
    }

    // Quick HTTP check to verify device is reachable (with short timeout)
    HTTPClient http;
    char url[128];
    snprintf(url, sizeof(url), "http://%s:1400/xml/device_description.xml", cachedIP.c_str());

    http.begin(url);
    http.setTimeout(5000);  // 5s timeout - speaker may take a few seconds to respond after WiFi join

    Serial.printf("[SONOS] Verifying cached device is reachable at %s...\n", cachedIP.c_str());
    int code = http.GET();
    http.end();

    if (code != 200) {
        Serial.println("========================================");
        Serial.printf("[SONOS] Cached device '%s' unreachable (HTTP %d)\n", cachedRoom.c_str(), code);
        Serial.println("[SONOS] Running full SSDP discovery (~15 seconds)...");
        Serial.println("========================================");
        return false;
    }

    // Device is reachable - add it to the device list.
    // decodeHTML the cached name so legacy NVS values written before the discovery-side
    // decode (with raw U+2019 / &apos; etc.) render cleanly on first boot after upgrade.
    deviceCount = 1;
    devices[0].ip = ip;
    devices[0].roomName = decodeHTML(cachedRoom);
    devices[0].rinconID = cachedRincon;
    devices[0].isPlaying = false;
    devices[0].volume = 50;
    devices[0].isMuted = false;
    devices[0].shuffleMode = false;
    devices[0].repeatMode = "NONE";
    devices[0].connected = false;
    devices[0].errorCount = 0;
    devices[0].currentTrackNumber = 0;
    devices[0].totalTracks = 0;
    devices[0].queueSize = 0;
    devices[0].groupCoordinatorUUID = "";
    devices[0].isGroupCoordinator = true;
    devices[0].groupMemberCount = 1;
    devices[0].pairIP = IPAddress();

    // -1 = cached by a build that did not store this yet. Resolve it once from
    // the device description (the device was just confirmed reachable above), and
    // cacheSelectedDevice() will persist it from then on.
    int cachedLineIn = prefs.getInt("cached_linein", -1);
    if (cachedLineIn < 0) {
        Serial.println("[SONOS]   Line-in unknown in cache - resolving once");
        // Persist ONLY on a successful fetch. getRoomName() defaults the flag to
        // false and only raises it on HTTP 200, so writing unconditionally would
        // cache one timed-out request as "no line-in" permanently — and the
        // sentinel below means it would never be retried. Leaving the key unset
        // costs one GET on the next boot instead.
        if (getRoomName(&devices[0]))
            prefs.putInt("cached_linein", devices[0].hasLineIn ? 1 : 0);
        else
            Serial.println("[SONOS]   Line-in fetch failed - not caching, will retry");
    } else {
        devices[0].hasLineIn = (cachedLineIn == 1);
        devices[0].lineInKnown = true;
    }

    // The rest of the remembered system: every other speaker, with its group.
    // The selected speaker stays in slot 0 (callers rely on it).
    const int remembered = loadCachedDeviceList();

    Serial.println("========================================");
    Serial.println("[SONOS] ✓ FAST BOOT: Device loaded from NVS cache");
    Serial.printf("[SONOS]   Speaker: %s\n", cachedRoom.c_str());
    Serial.printf("[SONOS]   IP: %s\n", cachedIP.c_str());
    Serial.printf("[SONOS]   RINCON: %s\n", cachedRincon.c_str());
    Serial.printf("[SONOS]   Line-in: %s\n", devices[0].hasLineIn ? "yes" : "no");
    Serial.printf("[SONOS]   Remembered speakers: %d (refreshed in the background)\n", remembered + 1);
    Serial.println("[SONOS]   Boot time saved: ~13 seconds");
    Serial.println("========================================");

    return true;
}

// ============================================================================
// Speaker memory
// ----------------------------------------------------------------------------
// One NVS blob holds the whole system as the panel last saw it: a line per room,
//     ip|RINCON|room name|line-in 1/0/?|pair ip|coordinator RINCON|members
// where '?' means the description was never fetched (asked again on refresh).
// so a reboot starts with every speaker and every group already on screen. The
// background refresh (sonos_sync.cpp) then corrects whatever changed while the
// panel was off. Text, not a packed struct, so a firmware that adds a field is
// still able to read what an older one wrote.
// ============================================================================
#define DEVLIST_KEY      "dev_list"
#define DEVLIST_MAX_BYTES 3900

static void resetDeviceSlot(SonosDevice& d, const IPAddress& ip,
                            const String& room, const String& rincon) {
    d.ip = ip;
    d.pairIP = IPAddress();
    d.roomName = room;
    d.rinconID = rincon;
    d.name = "";
    d.isPlaying = false;
    d.volume = 50;
    d.isMuted = false;
    d.shuffleMode = false;
    d.repeatMode = "NONE";
    d.currentTrack = "";
    d.currentArtist = "";
    d.currentAlbum = "";
    d.albumArtURL = "";
    d.currentURI = "";
    d.isRadioStation = d.isLineIn = d.isTvAudio = false;
    d.hasLineIn = false;
    d.lineInKnown = false;
    d.connected = false;
    d.errorCount = 0;
    d.currentTrackNumber = 0;
    d.totalTracks = 0;
    d.queueSize = 0;
    d.groupCoordinatorUUID = rincon;     // standalone until the topology says otherwise
    d.isGroupCoordinator = true;
    d.groupMemberCount = 1;
}

static String sanitizeField(String v) {
    v.replace("|", " ");
    v.replace("\n", " ");
    v.replace("\r", " ");
    return v;
}

void SonosController::cacheDeviceList() {
    if (!devices || deviceCount <= 0) return;
    String out;
    out.reserve(1024);
    if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;   // next refresh will retry
    for (int i = 0; i < deviceCount; i++) {
        const SonosDevice& d = devices[i];
        if (d.rinconID.length() == 0) continue;            // unidentified: nothing worth remembering
        String line;
        line += d.ip.toString();                           line += '|';
        line += d.rinconID;                                line += '|';
        line += sanitizeField(d.roomName);                 line += '|';
        line += sonos_sync::lineInField(d.lineInKnown, d.hasLineIn); line += '|';
        line += ((uint32_t)d.pairIP != 0) ? d.pairIP.toString() : String("");
        line += '|';
        line += d.groupCoordinatorUUID;                    line += '|';
        line += String(d.groupMemberCount);                line += '\n';
        if (out.length() + line.length() > DEVLIST_MAX_BYTES) break;   // keep what fits
        out += line;
    }
    xSemaphoreGive(deviceMutex);
    if (out.length() == 0) return;

    // Skip the flash write when nothing changed: NVS wear is the one cost of a
    // 4-hourly refresh that is worth avoiding.
    String prev = prefs.getString(DEVLIST_KEY, "");
    if (prev == out) return;
    prefs.putString(DEVLIST_KEY, out);
    Serial.printf("[SONOS] Remembered %d speaker(s) for the next boot\n", deviceCount);
}

// Appends the remembered speakers other than slot 0 (the selected one) and
// restores their groups. Returns how many were added.
int SonosController::loadCachedDeviceList() {
    if (!devices || deviceCount < 1) return 0;
    String blob = prefs.getString(DEVLIST_KEY, "");
    if (blob.length() == 0) return 0;

    int added = 0;
    int pos = 0;
    // Group data for slot 0 and every added slot, applied after all rooms exist so
    // member counts can be rebuilt from what is actually present.
    while (pos < (int)blob.length() && deviceCount < MAX_SONOS_DEVICES) {
        int eol = blob.indexOf('\n', pos);
        if (eol < 0) eol = blob.length();
        String line = blob.substring(pos, eol);
        pos = eol + 1;

        String f[7];
        int n = 0, start = 0;
        for (int i = 0; i <= (int)line.length() && n < 7; i++) {
            if (i == (int)line.length() || line[i] == '|') { f[n++] = line.substring(start, i); start = i + 1; }
        }
        if (n < 4 || f[1].length() == 0) continue;

        IPAddress ip;
        if (!ip.fromString(f[0])) continue;

        // Slot 0 is the verified, selected speaker: keep its fresh data, just take
        // the remembered group.
        bool isSelected = uuidEquals(devices[0].rinconID, f[1]) || devices[0].ip == ip;
        SonosDevice* d = nullptr;
        if (isSelected) {
            d = &devices[0];
        } else {
            bool dup = false;
            for (int i = 0; i < deviceCount; i++) if (uuidEquals(devices[i].rinconID, f[1])) { dup = true; break; }
            if (dup) continue;
            d = &devices[deviceCount];
            resetDeviceSlot(*d, ip, decodeHTML(f[2]), f[1]);
            sonos_sync::parseLineInField(f[3].length() == 1 ? f[3][0] : '?',
                                         d->lineInKnown, d->hasLineIn);
            if (n >= 5 && f[4].length()) { IPAddress p; if (p.fromString(f[4])) d->pairIP = p; }
            deviceCount++;
            added++;
        }
        if (n >= 6 && f[5].length()) {
            d->groupCoordinatorUUID = f[5];
            d->isGroupCoordinator = uuidEquals(f[5], d->rinconID);
        }
        if (n >= 7 && f[6].length()) d->groupMemberCount = max(1, (int)f[6].toInt());
    }
    Serial.printf("[SONOS] Restored %d remembered speaker(s) and their groups from NVS\n", added);
    return added;
}

bool SonosController::selectLastSelection() {
    if (!devices || deviceCount <= 0) return false;
    if (getCurrentDevice()) return true;
    // Not while a scan is rewriting the list: the index found below would not
    // survive it. The next daily check or refresh tries again.
    if (!tryBeginScan()) return false;
    struct ScanSlot { SonosController* c; ~ScanSlot() { c->endScan(); } } slot{this};
    if (deviceCount <= 0) return false;

    const String rin = prefs.getString("cached_rincon", "");
    const String ip  = prefs.getString("cached_ip", "");
    int idx = -1;
    for (int i = 0; i < deviceCount && idx < 0; i++) {
        if (rin.length() > 0 && uuidEquals(devices[i].rinconID, rin)) idx = i;
        else if (ip.length() > 0 && devices[i].ip.toString() == ip) idx = i;
    }
    if (idx < 0) {
        idx = 0;   // the last choice is gone from the system: a room beats no room
        Serial.println("[SONOS] Last selection not found - defaulting to the first speaker");
    } else {
        Serial.printf("[SONOS] Restored last selection: '%s'\n", devices[idx].roomName.c_str());
    }
    selectDevice(idx);
    return true;
}

// ----------------------------------------------------------------------------
// One scan at a time
// ----------------------------------------------------------------------------
// The Scan button (mainAppTask), the deferred bring-up and the sync task can all
// rewrite devices[]. A plain `if (discovering) ...; discovering = true;` leaves a
// window in which two of them both get in, so the test and the set are one
// critical section.
static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;

bool SonosController::tryBeginScan() {
    bool ok = false;
    portENTER_CRITICAL(&s_scan_mux);
    if (!discovering) { discovering = true; ok = true; }
    portEXIT_CRITICAL(&s_scan_mux);
    return ok;
}

void SonosController::endScan() {
    portENTER_CRITICAL(&s_scan_mux);
    discovering = false;
    portEXIT_CRITICAL(&s_scan_mux);
}

// ----------------------------------------------------------------------------
// refreshSpeakersFromTopology - keep the list in step with the system, quietly.
// One GetZoneGroupState call names every room, its address and whether the Sonos
// app would show it, which is everything a scan finds and without 15 seconds of
// SSDP. That is what makes it fit to run in the background during playback.
// ----------------------------------------------------------------------------
int SonosController::refreshSpeakersFromTopology() {
    if (!devices || deviceCount <= 0) return -1;
    if (!tryBeginScan()) return -1;

    // The selected speaker first (it is the one known to be awake), then up to two
    // others: one switched-off speaker must not send us to SSDP.
    int order[3];
    int tries = 0;
    const int cur = currentDeviceIndex;
    if (cur >= 0 && cur < deviceCount) order[tries++] = cur;
    for (int i = 0; i < deviceCount && tries < 3; i++) if (i != cur) order[tries++] = i;

    int result = -1;
    for (int t = 0; t < tries && result < 0; t++) {
        String resp = sendSOAP(&devices[order[t]], "ZoneGroupTopology", "GetZoneGroupState", "");
        if (resp.length() == 0) {
            Serial.printf("[SYNC] %s did not answer GetZoneGroupState\n",
                          devices[order[t]].ip.toString().c_str());
            continue;
        }
        result = applySpeakerTopology(resp);
    }
    endScan();
    return result;
}

int SonosController::refreshSpeakersViaProbe() {
    if (!devices) return -1;
    if (!tryBeginScan()) return -1;

    IPAddress found[3];
    const int nf = ssdpProbe(found, 3, SSDP_PROBE_LISTEN_MS);
    int result = -1;
    for (int f = 0; f < nf && result < 0; f++) {
        // sendSOAP() addresses a SonosDevice. A known speaker at that address is
        // used as is; otherwise the first unused slot (beyond deviceCount, so no
        // other task looks at it) carries the address for this one request.
        SonosDevice* target = nullptr;
        for (int i = 0; i < deviceCount; i++) if (devices[i].ip == found[f]) { target = &devices[i]; break; }
        if (!target && deviceCount < MAX_SONOS_DEVICES) {
            target = &devices[deviceCount];
            target->ip = found[f];
            target->errorCount = 0;
            target->connected = false;
        }
        if (!target) continue;
        String resp = sendSOAP(target, "ZoneGroupTopology", "GetZoneGroupState", "");
        if (resp.length() > 0) result = applySpeakerTopology(resp);
    }
    endScan();
    if (nf == 0) Serial.println("[SYNC] SSDP probe: no speaker answered");
    return result;
}

// Same M-SEARCH as discoverDevices(), but two bursts instead of five, a listen
// window of a few seconds instead of 15, it stops at maxOut responders, and it
// writes only to `out`. Caller holds the scan slot (the udp socket is shared).
int SonosController::ssdpProbe(IPAddress* out, int maxOut, uint32_t listenMs) {
    if (!out || maxOut <= 0) return 0;
    udp.stop();
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!udp.begin(1900)) {
        Serial.println("[SYNC] SSDP probe: UDP begin failed");
        return 0;
    }
    static const char msg[] =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
    const IPAddress multicast(239, 255, 255, 250);
    const IPAddress broadcast(255, 255, 255, 255);
    for (int burst = 0; burst < 2; burst++) {
        const bool locked = network_mutex &&
            xSemaphoreTake(network_mutex, pdMS_TO_TICKS(NETWORK_MUTEX_TIMEOUT_MS)) == pdTRUE;
        udp.beginPacket(multicast, 1900);
        udp.write((const uint8_t*)msg, sizeof(msg) - 1);
        udp.endPacket();
        udp.beginPacket(broadcast, 1900);
        udp.write((const uint8_t*)msg, sizeof(msg) - 1);
        udp.endPacket();
        if (locked) {
            last_network_end_ms = millis();
            xSemaphoreGive(network_mutex);
        }
        if (burst == 0) vTaskDelay(pdMS_TO_TICKS(500));
    }

    int n = 0;
    const uint32_t start = millis();
    char buf[513];
    while (n < maxOut && (uint32_t)(millis() - start) < listenMs) {
        const int size = udp.parsePacket();
        if (size > 0) {
            const int len = udp.read(buf, sizeof(buf) - 1);
            if (len > 0) {
                buf[len] = 0;
                for (int i = 0; i < len; i++) if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] += 32;
                if (strstr(buf, "sonos") || strstr(buf, "zoneplayer")) {
                    const IPAddress ip = udp.remoteIP();
                    bool dup = false;
                    for (int k = 0; k < n; k++) if (out[k] == ip) { dup = true; break; }
                    if (!dup) out[n++] = ip;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    udp.stop();
    Serial.printf("[SYNC] SSDP probe: %d responder(s)\n", n);
    return n;
}

int SonosController::applySpeakerTopology(String& resp) {
    // Case kept: these are room names. Entities are decoded once, which turns the
    // SOAP-escaped inner XML back into markup; names still carry their own &apos;.
    const size_t len = sonos_topology::decodeEntitiesInPlace(&resp[0], resp.length(), false);

    // Static, not stack (~1.8 KB). Only ever used by the holder of the scan slot.
    static sonos_topology::Member s_mem[MAX_SONOS_DEVICES * 2];
    const int n = sonos_topology::parseMembers(resp.c_str(), len, s_mem, MAX_SONOS_DEVICES * 2);
    int visible = 0;
    for (int i = 0; i < n; i++) if (!s_mem[i].hidden) visible++;
    if (visible == 0) {
        Serial.println("[SYNC] Topology listed no rooms - keeping the current list");
        return -1;
    }

    int added = 0, removed = 0, renamed = 0, moved = 0;
    if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(500)) != pdTRUE) return -1;

    // Rooms that are in the system: update in place, or add. New slots are filled
    // before deviceCount is raised, so a reader never sees a half-written one.
    for (int m = 0; m < n; m++) {
        if (s_mem[m].hidden) continue;
        char host[20];
        IPAddress ip;
        const bool haveIp = sonos_topology::locationHost(s_mem[m].location, host, sizeof(host)) &&
                            ip.fromString(host);
        char ubuf[64];
        const size_t ul = s_mem[m].uuid.len < sizeof(ubuf) - 1 ? s_mem[m].uuid.len : sizeof(ubuf) - 1;
        memcpy(ubuf, s_mem[m].uuid.ptr, ul);
        ubuf[ul] = '\0';
        const String uuid(ubuf);
        const String name = s_mem[m].name.ptr
            ? decodeHTML(String(s_mem[m].name.ptr, s_mem[m].name.len)) : String();

        int found = -1;
        for (int i = 0; i < deviceCount; i++) if (uuidEquals(devices[i].rinconID, uuid)) { found = i; break; }
        if (found >= 0) {
            if (haveIp && !(devices[found].ip == ip)) { devices[found].ip = ip; devices[found].errorCount = 0; moved++; }
            if (name.length() && devices[found].roomName != name) { devices[found].roomName = name; renamed++; }
        } else if (deviceCount < MAX_SONOS_DEVICES && haveIp) {
            resetDeviceSlot(devices[deviceCount], ip, name.length() ? name : ip.toString(), uuid);
            deviceCount++;
            added++;
        }
    }

    // Rooms that left the system, or are now a bonded Sub / surround / pair half
    // (issue #211 on a list remembered from before the fix). The selected room is
    // never removed here: a speaker that is merely switched off drops out of the
    // topology, and the panel keeps showing it as "not connected" rather than
    // swapping the room under the user. Entries without a RINCON are left alone.
    //
    // Removal shifts later slots down. The UI screens address rooms by index and
    // repaint on the UPDATE_GROUPS sent below; this only runs after SYNC_IDLE_MS
    // without a touch, so a tap on a stale row is not a practical race.
    for (int i = deviceCount - 1; i >= 0; i--) {
        if (i == currentDeviceIndex) continue;
        if (devices[i].rinconID.length() == 0) continue;
        bool visibleNow = false;
        for (int m = 0; m < n && !visibleNow; m++)
            visibleNow = !s_mem[m].hidden &&
                sonos_topology::uuidEquals(devices[i].rinconID.c_str(), devices[i].rinconID.length(),
                                           s_mem[m].uuid.ptr, s_mem[m].uuid.len);
        if (visibleNow) continue;
        Serial.printf("[SYNC]   '%s' is no longer a room of its own - removed\n",
                      devices[i].roomName.c_str());
        for (int k = i; k < deviceCount - 1; k++) devices[k] = devices[k + 1];
        deviceCount--;
        if (currentDeviceIndex > i) currentDeviceIndex--;
        removed++;
    }
    xSemaphoreGive(deviceMutex);

    // Groups from the same payload: lowercase it (the group parser matches tag
    // names in lowercase and RINCONs case-blind) instead of a second SOAP. Room
    // names were copied out above, so their case no longer matters here.
    char* p = &resp[0];
    for (size_t i = 0; i < len; i++) if (p[i] >= 'A' && p[i] <= 'Z') p[i] += 32;
    applyGroupTopology(p, len);

    // Capabilities (line-in) come from the device description. Asked for every
    // room whose flag is not known yet: the rooms just added, and any whose
    // earlier fetch failed (remembered as '?' in the blob). Retried like
    // discoverDevices() does, because a busy network_mutex is the normal state
    // during playback. A room still unanswered stays unknown and is asked again on
    // the next refresh; the default false is never persisted as a result. Only
    // rooms present in this topology are asked, so a switched-off selected room
    // costs nothing. The scan slot (held by the caller) keeps the slots in place.
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].lineInKnown || devices[i].rinconID.length() == 0) continue;
        bool present = false;
        for (int m = 0; m < n && !present; m++)
            present = !s_mem[m].hidden &&
                sonos_topology::uuidEquals(devices[i].rinconID.c_str(), devices[i].rinconID.length(),
                                           s_mem[m].uuid.ptr, s_mem[m].uuid.len);
        if (!present) continue;
        bool ok = getRoomName(&devices[i]);
        for (int attempt = 1; !ok && attempt <= DISCOVERY_ROOMNAME_RETRIES; attempt++) {
            vTaskDelay(pdMS_TO_TICKS(DISCOVERY_ROOMNAME_RETRY_MS));
            ok = getRoomName(&devices[i]);
        }
        if (!ok)
            Serial.printf("[SYNC]   Line-in of '%s' still unknown - asking again next refresh\n",
                          devices[i].roomName.c_str());
    }

    cacheDeviceList();
    Serial.printf("[SYNC] Speakers refreshed: %d room(s) (+%d new, -%d gone, %d renamed, %d moved)\n",
                  deviceCount, added, removed, renamed, moved);
    return deviceCount;
}
