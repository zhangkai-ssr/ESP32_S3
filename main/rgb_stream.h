#pragma once

/**
 * Initialise the RGB test stream module (call once before rgb_stream_start).
 */
void rgb_stream_init(void);

/**
 * Start the RGB test TCP server task on port 3335.
 * Streams ~500 KB/s of fake RGB pixel data with MCU timestamps for
 * time-sync validation. Each packet embeds time_sync_now_us() so the
 * host-side client can compute the host↔MCU clock regression.
 */
void rgb_stream_start(void);
