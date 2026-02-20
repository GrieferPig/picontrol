#include "debug_printf.h"

mutex_t g_debugPrintMutex;
volatile bool g_debugPrintInited = false;

void dbg_printf_init()
{
    if (!g_debugPrintInited)
    {
        mutex_init(&g_debugPrintMutex);
        g_debugPrintInited = true;
    }
}

void dbg_printf(const char *fmt, ...)
{
    if (!g_debugPrintInited)
        return;

    // Try to acquire the mutex without blocking for too long.
    // If the other core is currently printing, just drop this message.
    if (!mutex_try_enter(&g_debugPrintMutex, nullptr))
        return;

    // Non-blocking check: only write if Serial TX FIFO has enough room.
    // This prevents Adafruit_USBD_CDC::write() from entering its internal
    // spin-loop (yield → tud_task → retry), which is the path that, when
    // hit concurrently from two cores, corrupts the TinyUSB FIFO.
    if (Serial && Serial.availableForWrite() >= 48)
    {
        char buf[192];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n > 0)
        {
            size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
            Serial.write((const uint8_t *)buf, len);
        }
    }

    mutex_exit(&g_debugPrintMutex);
}
