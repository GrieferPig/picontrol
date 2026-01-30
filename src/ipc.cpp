#include "ipc.hpp"

#include <cstring>
#include <pico/util/queue.h>
#include <pico/sync.h>

namespace IPC
{

    static queue_t g_setParameterQ;
    static queue_t g_setCalibQ;
    static queue_t g_syncMappingQ;
    static bool g_inited = false;
    static critical_section_t g_initLock;
    static bool g_initLockInited = false;

    static void initOnce()
    {

        if (!g_inited)
        {
            critical_section_init(&g_initLock);
            critical_section_enter_blocking(&g_initLock);
            queue_init(&g_setParameterQ, sizeof(SetParameterRequest), 32);
            queue_init(&g_setCalibQ, sizeof(SetCalibRequest), 32);
            queue_init(&g_syncMappingQ, sizeof(SyncMappingRequest), 32);
            g_inited = true;
            critical_section_exit(&g_initLock);
        }
    }

    void init()
    {
        initOnce();
    }

    bool enqueueSetParameter(int row, int col, uint8_t paramId, uint8_t dataType, const char *valueStr)
    {
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
        return queue_try_remove(&g_setParameterQ, &out);
    }

    bool enqueueSyncMapping(int row, int col)
    {
        SyncMappingRequest req{};
        req.row = static_cast<int8_t>(row);
        req.col = static_cast<int8_t>(col);
        req.applyToAll = 0;
        return queue_try_add(&g_syncMappingQ, &req);
    }

    bool enqueueSyncMappingAll()
    {
        SyncMappingRequest req{};
        req.row = -1;
        req.col = -1;
        req.applyToAll = 1;
        return queue_try_add(&g_syncMappingQ, &req);
    }

    bool tryDequeueSyncMapping(SyncMappingRequest &out)
    {
        return queue_try_remove(&g_syncMappingQ, &out);
    }

    bool enqueueSetCalib(int row, int col, uint8_t paramId, int32_t minValue, int32_t maxValue)
    {
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
