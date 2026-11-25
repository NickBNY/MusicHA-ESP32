#pragma once
/**
 * Home Assistant (ESPHome native API) budgets and timings.
 *
 * Kept out of config.h so that file stays small. Framework-free: no Arduino,
 * FreeRTOS, lwIP or LVGL include, so the host tests can read these numbers.
 * The reasoning behind each value is in the design note for this feature
 * (Decision 2: SDIO/DMA safety).
 */

// TCP port of the ESPHome native API.
#define HA_PORT                    6053

// Internal (DMA-capable) memory the feature may use. The task starts only
// when free DMA >= ART_MIN_DMA_PRE_BURST + HA_DMA_BUDGET_BYTES.
#define HA_DMA_BUDGET_BYTES        8192

// A client is accepted only when free DMA >= ART_MIN_DMA_PRE_BURST + this.
#define HA_ACCEPT_DMA_MARGIN       4096

// Session buffers (PSRAM). RX also caps the frame payload size.
#define HA_RX_BUF_BYTES            1024
#define HA_TX_BUF_BYTES            1536

// Largest single send(): one TCP segment (MSS is 1436).
#define HA_TX_CHUNK_BYTES          1400

// HA task stack (PSRAM, the task never touches NVS or flash).
#define HA_TASK_STACK_BYTES        6144

// Close a client that sends no Hello within this time.
#define HA_HELLO_TIMEOUT_MS        5000

// Close a client that sends no bytes for this long (HA pings every 20 s).
#define HA_IDLE_TIMEOUT_MS         120000

// Close a client whose owed replies stay deferred by the radio gate this long.
#define HA_DEFER_LIMIT_MS          30000

// Consecutive quiet-radio samples (1 s apart) below the art DMA floor that
// shed the feature.
#define HA_SHED_SAMPLES            3

// Back-off after a shed or a start failure.
#define HA_BACKOFF_MS              60000

// SO_RCVBUF for the mDNS UDP socket.
#define HA_MDNS_RCVBUF_BYTES       2048
