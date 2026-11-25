#pragma once
/**
 * Background upkeep: keeps the panel's picture of the Sonos system current
 * without the user pressing Scan.
 *
 *   - on start, 45 s after boot: refresh the remembered speakers and groups
 *   - every 4 h: refresh them again; with none at all, run a full scan
 *   - every 24 h: make sure a speaker is selected (the last selection), and
 *     restart the panel if memory has run out (the memory check runs inline on
 *     the calling task, so it works when no task stack can be allocated)
 *
 * Every job runs only while the panel is idle (see sonosSyncIdle). Call
 * sonosSyncTick() from the main application task; it is cheap and does its work
 * in short-lived tasks of its own.
 */

void sonosSyncTick();

// True when upkeep may use the radio: nobody touching the screen, no artwork /
// photo / firmware transfer in flight, DMA above the artwork floor.
bool sonosSyncIdle();

// Ask for a speaker refresh at the next idle moment (e.g. after a manual scan or
// a speaker change). Does not reset the 4-hourly schedule.
void sonosSyncRequestRefresh();

// Daily maintenance result, shown in the log only.
bool sonosMemoryExhausted();

// millis() when the last speaker refresh / daily check finished; 0 = never.
uint32_t sonosSyncLastRefreshMs();
uint32_t sonosSyncLastDailyMs();
