#pragma once

#include <cstdint>

namespace runtime_config
{
    struct AutoupdateRequest
    {
        int8_t row;     // -1 when "all"
        int8_t col;     // -1 when "all"
        uint8_t enable; // 0/1
        uint16_t intervalMs;
        uint8_t applyToAll; // 0/1
    };

    struct RotationOverrideRequest
    {
        int8_t row;         // -1 when "all"
        int8_t col;         // -1 when "all"
        uint8_t rotated180; // 0/1
        uint8_t applyToAll; // 0/1
    };

    struct SetParameterRequest
    {
        int8_t row;
        int8_t col;
        uint8_t paramId;
        uint8_t dataType; // 0=int, 1=float, 2=bool
        char valueStr[32];
    };

    void init();

    // Called from core0 (USB CDC command handler) or core1.
    bool enqueueAutoupdate(int row, int col, bool enable, uint16_t intervalMs);
    bool enqueueAutoupdateAll(bool enable, uint16_t intervalMs);

    // Rotation override persistence (180° only; hardware cannot detect it)
    bool enqueueRotationOverride(int row, int col, bool rotated180);
    bool enqueueRotationOverrideAll(bool rotated180);

    // Called from core1.
    bool tryDequeueAutoupdate(AutoupdateRequest &out);

    bool tryDequeueRotationOverride(RotationOverrideRequest &out);

    // Set parameter request (sent from CDC to core1)
    bool enqueueSetParameter(int row, int col, uint8_t paramId, uint8_t dataType, const char *valueStr);
    bool tryDequeueSetParameter(SetParameterRequest &out);

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
