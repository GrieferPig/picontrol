#include "runtime_query.h"

#include <pico/util/queue.h>
#include <pico/sync.h>

namespace runtime_query
{
    namespace
    {
        static queue_t g_q;
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
                queue_init(&g_q, sizeof(Request), 16);
                g_inited = true;
            }
            critical_section_exit(&g_initLock);
        }
    }

    void init()
    {
        initOnce();
    }

    bool enqueueListModules()
    {
        initOnce();
        Request r{};
        r.type = RequestType::LIST_MODULES;
        return queue_try_add(&g_q, &r);
    }

    bool tryDequeue(Request &out)
    {
        initOnce();
        return queue_try_remove(&g_q, &out);
    }
}
