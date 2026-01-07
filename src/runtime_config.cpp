#include "runtime_config.h"

#include <cstring>
#include <pico/util/queue.h>
#include <pico/sync.h>

namespace runtime_config
{
    namespace
    {
        static queue_t g_autoupdateQ;
        static queue_t g_rotationOverrideQ;
        static queue_t g_setParameterQ;
        static queue_t g_setCalibQ;
        static queue_t g_syncMappingQ;
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
                queue_init(&g_rotationOverrideQ, sizeof(RotationOverrideRequest), 32);
                queue_init(&g_setParameterQ, sizeof(SetParameterRequest), 32);
                queue_init(&g_setCalibQ, sizeof(SetCalibRequest), 32);
                queue_init(&g_syncMappingQ, sizeof(SyncMappingRequest), 32);
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

    bool enqueueRotationOverride(int row, int col, bool rotated180)
    {
        initOnce();
        RotationOverrideRequest req{};
        req.row = static_cast<int8_t>(row);
        req.col = static_cast<int8_t>(col);
        req.rotated180 = rotated180 ? 1 : 0;
        req.applyToAll = 0;
        return queue_try_add(&g_rotationOverrideQ, &req);
    }

    bool enqueueRotationOverrideAll(bool rotated180)
    {
        initOnce();
        RotationOverrideRequest req{};
        req.row = -1;
        req.col = -1;
        req.rotated180 = rotated180 ? 1 : 0;
        req.applyToAll = 1;
        return queue_try_add(&g_rotationOverrideQ, &req);
    }

    bool tryDequeueRotationOverride(RotationOverrideRequest &out)
    {
        initOnce();
        return queue_try_remove(&g_rotationOverrideQ, &out);
    }

    bool enqueueSetParameter(int row, int col, uint8_t paramId, uint8_t dataType, const char *valueStr)
    {
        initOnce();
        SetParameterRequest req{};
        req.row = static_cast<int8_t>(row);
        req.col = static_cast<int8_t>(col);
        req.paramId = paramId;
        req.dataType = dataType;
        if (valueStr)
        {
            strncpy(req.valueStr, valueStr, sizeof(req.valueStr) - 1);
            req.valueStr[sizeof(req.valueStr) - 1] = '\0';
        }
        return queue_try_add(&g_setParameterQ, &req);
    }

    bool tryDequeueSetParameter(SetParameterRequest &out)
    {
        initOnce();
        return queue_try_remove(&g_setParameterQ, &out);
    }

    bool enqueueSyncMapping(int row, int col)
    {
        initOnce();
        SyncMappingRequest req{};
        req.row = static_cast<int8_t>(row);
        req.col = static_cast<int8_t>(col);
        req.applyToAll = 0;
        return queue_try_add(&g_syncMappingQ, &req);
    }

    bool enqueueSyncMappingAll()
    {
        initOnce();
        SyncMappingRequest req{};
        req.row = -1;
        req.col = -1;
        req.applyToAll = 1;
        return queue_try_add(&g_syncMappingQ, &req);
    }

    bool tryDequeueSyncMapping(SyncMappingRequest &out)
    {
        initOnce();
        return queue_try_remove(&g_syncMappingQ, &out);
    }

    bool enqueueSetCalib(int row, int col, uint8_t paramId, int32_t minValue, int32_t maxValue)
    {
        initOnce();
        SetCalibRequest req{};
        req.row = static_cast<int8_t>(row);
        req.col = static_cast<int8_t>(col);
        req.paramId = paramId;
        req.minValue = minValue;
        req.maxValue = maxValue;
        return queue_try_add(&g_setCalibQ, &req);
    }

    bool tryDequeueSetCalib(SetCalibRequest &out)
    {
        initOnce();
        return queue_try_remove(&g_setCalibQ, &out);
    }
}
