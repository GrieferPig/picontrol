#pragma once

#include <cstdint>

namespace runtime_query
{
    enum class RequestType : uint8_t
    {
        LIST_MODULES = 0,
    };

    struct Request
    {
        RequestType type;
    };

    void init();

    // Called from core0 (CDC command handler) or core1.
    bool enqueueListModules();

    // Called from core1.
    bool tryDequeue(Request &out);
}
