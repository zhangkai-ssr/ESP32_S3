#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * Start the UDP time synchronisation task.
 * Call this once after Wi-Fi is connected and before starting data streams.
 * The task will perform an initial burst of sync rounds and then re-sync
 * periodically in the background.
 */
void time_sync_start(void);

/**
 * Return the current time on the AP-host time axis (µs).
 * Equivalent to esp_timer_get_time() + time_sync_offset_us().
 * When not yet locked, returns raw esp_timer_get_time().
 */
int64_t time_sync_now_us(void);

/**
 * Return the most recent estimated offset (µs) between the MCU clock and
 * the AP-host clock:  host_time ≈ mcu_time + offset.
 */
int64_t time_sync_offset_us(void);

/**
 * Return true once the initial sync burst has produced at least one
 * successful round.
 */
bool time_sync_is_locked(void);
