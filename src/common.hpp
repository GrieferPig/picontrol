#pragma once
#include <cstdint>

class InterruptSerialPIO;

static constexpr uint16_t MODULE_MAX_PAYLOAD = 2048; // 2KB payloads

enum ModuleOrientation : uint8_t
{
    UP,
    RIGHT,
    DOWN,
    LEFT
};

enum ModuleType : uint8_t
{
    FADER = 0,
    KNOB = 1,
    BUTTON = 2,
    BUTTON_MATRIX = 3,
    ENCODER = 4,
    JOYSTICK = 5,
    PROXIMITY = 6
};

enum ModuleProtocol : uint8_t
{
    PROTOCOL_UART = 0,
};

// Status codes for responses
enum ModuleStatus : uint8_t
{
    MODULE_STATUS_OK = 0,
    MODULE_STATUS_ERROR = 1,
    MODULE_STATUS_UNSUPPORTED = 2,
};

enum ModuleParameterDataType : uint8_t
{
    PARAM_TYPE_INT = 0,
    PARAM_TYPE_FLOAT = 1,
    PARAM_TYPE_BOOL = 2,
};

// Module capability flags (bitmask)
enum ModuleCapabilities : uint8_t
{
    MODULE_CAP_AUTOUPDATE = 1u << 0,
    MODULE_CAP_ROTATION_AWARE = 1u << 1, // When rotated 180°, flip output values using min/max (except bool)
};

// On-wire payloads must be tightly packed
#pragma pack(push, 1)
union ModuleParameterValue
{
    int32_t intValue;
    float floatValue;
    uint8_t boolValue;
};

union ModuleParameterMinMax
{
    struct
    {
        int32_t intMin;
        int32_t intMax;
    };
    struct
    {
        float floatMin;
        float floatMax;
    };
};

typedef struct
{
    uint8_t id;
    char name[32];
    ModuleParameterDataType dataType;
    ModuleParameterValue value;
    ModuleParameterMinMax minMax;
} ModuleParameter;

typedef struct
{
    ModuleProtocol protocol;
    ModuleType type;

    char name[32];
    char manufacturer[32];
    char fwVersion[16];
    uint8_t compatibleHostVersion;
    uint8_t capabilities; // ModuleCapabilities bitmask

    uint8_t physicalSizeRow;
    uint8_t physicalSizeCol;
    uint8_t portLocationRow;
    uint8_t portLocationCol;

    uint8_t parameterCount;
    ModuleParameter parameters[32];
} Module;
#pragma pack(pop)

typedef struct
{
    int row;
    int col;

    bool hasModule;
    Module module;

    ModuleOrientation orientation;

    // Runtime state
    InterruptSerialPIO *serial;
    bool configured;
    uint8_t txPin;
    uint8_t rxPin;
} Port;

// Module command structure
typedef enum : uint8_t
{
    CMD_PING = 0x00,
    CMD_GET_PROPERTIES = 0x01,
    CMD_SET_PARAMETER = 0x02,
    CMD_GET_PARAMETER = 0x03,
    CMD_RESET_MODULE = 0x04,
    // Enables module-driven updates: when enabled, host should stop polling.
    // intervalMs==0 means "push only on change"; otherwise module may also push periodically.
    CMD_SET_AUTOUPDATE = 0x05,
    CMD_RESPONSE = 0x80,
} ModuleMessageId;

typedef struct
{
    uint8_t moduleRow;
    uint8_t moduleCol;
    ModuleMessageId commandId;
    uint16_t payloadLength;
    uint8_t payload[MODULE_MAX_PAYLOAD];
} ModuleMessage;

// Host command structure.
// Packed payloads for each command
#pragma pack(push, 1)
typedef struct
{
    uint8_t magic; // 0x55
} ModuleMessagePingPayload;

typedef struct
{
    uint8_t requestId;
    Module module; // Full descriptor to hydrate host-side Module
} ModuleMessageGetPropertiesPayload;

typedef struct
{
    uint8_t parameterId;
    ModuleParameterDataType dataType;
    ModuleParameterValue value;
} ModuleMessageSetParameterPayload;

typedef struct
{
    uint8_t parameterId;
} ModuleMessageGetParameterPayload;

typedef struct
{
    uint8_t magic; // 0xA5
} ModuleMessageResetPayload;

typedef struct
{
    uint8_t enable;      // 0=disable (host polls), 1=enable (module pushes)
    uint16_t intervalMs; // 0=on-change only
} ModuleMessageSetAutoupdatePayload;

typedef struct
{
    ModuleStatus status;
    ModuleMessageId inResponseTo;
    uint16_t payloadLength;
    uint8_t payload[MODULE_MAX_PAYLOAD - 4]; // total payload (including header fields) capped at 4KB
} ModuleMessageResponsePayload;
#pragma pack(pop)

static_assert(sizeof(ModuleMessageGetPropertiesPayload) <= MODULE_MAX_PAYLOAD, "GetProperties payload exceeds protocol maximum");
static_assert(sizeof(ModuleMessageResponsePayload) <= MODULE_MAX_PAYLOAD, "Response payload exceeds protocol maximum");