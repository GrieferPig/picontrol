/**
 * Thread-safe, non-blocking debug printf for multi-core RP2040.
 *
 * Both core 0 and core 1 call printf() which writes to Serial (CDC #0 via
 * TinyUSB).  Adafruit_USBD_CDC::write() has NO multi-core locking, so
 * concurrent calls from the two cores corrupt TinyUSB's internal FIFO and
 * eventually lock up the entire USB stack.
 *
 * This header provides dbg_printf() which:
 *   1. Acquires a Pico SDK mutex (safe across cores).
 *   2. Checks Serial TX space – if the buffer is too full, the message is
 *      silently dropped so that neither core ever spin-waits inside
 *      Serial.write().
 *   3. Releases the mutex.
 *
 * Usage: replace every bare printf(...) with dbg_printf(...) in ALL source
 * files that run on either core.
 */

#pragma once

#include <Arduino.h>
#include <cstdio>
#include <cstdarg>
#include <pico/mutex.h>

// ── Globals (defined in debug_printf.cpp) ───────────────────────────────────

extern mutex_t g_debugPrintMutex;
extern volatile bool g_debugPrintInited;

// ── API ─────────────────────────────────────────────────────────────────────

/// Call once from core 0's setup() before any debug output.
void dbg_printf_init();

/**
 * printf-like debug output that is safe to call from either core.
 *
 * - Acquires a cross-core mutex.
 * - Drops the message if Serial TX has fewer than 48 free bytes (avoids the
 *   blocking spin-loop inside Adafruit_USBD_CDC::write()).
 * - Formats at most 192 characters per call.
 */
void dbg_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
