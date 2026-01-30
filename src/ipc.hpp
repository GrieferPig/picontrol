#pragma once

#include <cstdint>

namespace IPC
{
    struct SetParameterRequest
    {
        int8_t row;
        int8_t col;
        uint8_t paramId;
        uint8_t dataType; // 0=int, 1=float, 2=bool
        char valueStr[32];
    };

    struct SetCalibRequest
    {
        int8_t row;
        int8_t col;
        uint8_t paramId;
        int32_t minValue;
        int32_t maxValue;
    };

    struct ListModulesRequest
    {
        uint8_t *moduleListBuffer;
    };

    void init();

    // Called from core1.
    // Set parameter request (sent from CDC to core1)
    bool enqueueSetParameter(int row, int col, uint8_t paramId, uint8_t dataType, const char *valueStr);
    bool tryDequeueSetParameter(SetParameterRequest &out);

    // Set calibration request (sent from CDC to core1)
    bool enqueueSetCalib(int row, int col, uint8_t paramId, int32_t minValue, int32_t maxValue);
    bool tryDequeueSetCalib(SetCalibRequest &out);

    // Sync mapping request (sent from CDC to core1)
    struct SyncMappingRequest
    {
        int8_t row;
        int8_t col;
        uint8_t applyToAll; // 0/1
    };
    bool enqueueSyncMapping(int row, int col);
    bool enqueueSyncMappingAll();
    bool tryDequeueSyncMapping(SyncMappingRequest &out);
}
