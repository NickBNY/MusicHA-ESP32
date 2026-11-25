#pragma once
/**
 * Home Assistant (ESPHome native API) support: firmware-side interface.
 *
 * Off by default. With the feature off, haTick() reads one flag and returns:
 * no task, no socket, no allocation. See the design note for the radio and
 * DMA safety rules; the protocol itself lives in the framework-free headers
 * esphome_session.h, esphome_entities.h and esphome_mdns.h.
 *
 * Declarations only. Do not add heavy includes here.
 */
#include <stddef.h>
#include <stdint.h>

namespace esphome { struct HaSnapshot; }

// From mainAppTask, every loop. Cheap when the feature is off.
void haTick();

// Settings > General > Home Assistant. Both persist to NVS, so call them from
// mainAppTask (the LVGL callbacks already run there).
void haSetEnabled(bool on);
void haSetDebug(bool on);
bool haIsEnabled();
bool haIsDebug();

// "Off", "Waiting for Wi-Fi", "Waiting for memory", "Out of memory",
// "Port error", "Listening", "Connected" or "Paused".
void haStatus(char* buf, size_t n);

// "192.168.x.y:6053 (sonosesp-<mac6>)", or "Not connected" without Wi-Fi.
void haAddress(char* buf, size_t n);

// Called by the art task next to its DMA abort counter.
void haNoteArtDmaAbort();

// ---- Internal: shared by src/esphome_api.cpp and src/esphome_snapshot.cpp ----

// esphome_snapshot.cpp (mainAppTask side)
void haSnapshotEnable(bool on);                         // starts or stops edge counting
void haSnapshotTick(bool subscribed, bool debug);       // once per haTick()

// esphome_api.cpp
bool haClientSubscribed();
bool haPublishSnapshot(const esphome::HaSnapshot& s);   // zero-wait hand-over
uint32_t haShedEvents();
uint32_t haTxDeferrals();
