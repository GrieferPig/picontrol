#include "runtime_config.h"

#include <pico/util/queue.h>
#include <pico/sync.h>

namespace runtime_config
{
    namespace
    {
        static queue_t g_autoupdateQ;
        static bool g_inited = false;
        static critical_section_t g_initLock;
        static bool g_initLockInited = false;

        static void initLockOnce()
        {
            if (g_initLockInited)
                return;
            critical_section_init(&g_initLock);
            g_initLockInited = true;
        }

        static void initOnce()
        {
            initLockOnce();
            critical_section_enter_blocking(&g_initLock);
            if (!g_inited)
            {
                queue_init(&g_autoupdateQ, sizeof(AutoupdateRequest), 32);
                g_inited = true;
            }
            critical_section_exit(&g_initLock);
        }
    }

    void init()
    {
        initOnce();
    }

    bool enqueueAutoupdate(int row, int col, bool enable, uint16_t intervalMs)
    {
        initOnce();
        AutoupdateRequest req{};
        req.row = static_cast<int8_t>(row);
        req.col = static_cast<int8_t>(col);
        req.enable = enable ? 1 : 0;
        req.intervalMs = intervalMs;
        req.applyToAll = 0;
        return queue_try_add(&g_autoupdateQ, &req);
    }

    bool enqueueAutoupdateAll(bool enable, uint16_t intervalMs)
    {
        initOnce();
        AutoupdateRequest req{};
        req.row = -1;
        req.col = -1;
        req.enable = enable ? 1 : 0;
        req.intervalMs = intervalMs;
        req.applyToAll = 1;
        return queue_try_add(&g_autoupdateQ, &req);
    }

    bool tryDequeueAutoupdate(AutoupdateRequest &out)
    {
        initOnce();
        return queue_try_remove(&g_autoupdateQ, &out);
    }
}
