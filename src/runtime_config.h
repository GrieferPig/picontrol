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

    void init();

    // Called from core0 (USB CDC command handler) or core1.
    bool enqueueAutoupdate(int row, int col, bool enable, uint16_t intervalMs);
    bool enqueueAutoupdateAll(bool enable, uint16_t intervalMs);

    // Called from core1.
    bool tryDequeueAutoupdate(AutoupdateRequest &out);
}
