#pragma once
#include <cstdint>

class InterruptSerialPIO;

enum ModuleOrientation
{
    UP,
    RIGHT,
    DOWN,
    LEFT
};
enum ModuleType
{
    FADER = 0,
    KNOB = 1,
    BUTTON = 2,
    BUTTON_MATRIX = 3,
    ENCODER = 4,
    JOYSTICK = 5,
    PROXIMITY = 6
};

enum ModuleProtocol
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

enum ModuleParameterDataType
{
    PARAM_TYPE_INT = 0,
    PARAM_TYPE_FLOAT = 1,
    PARAM_TYPE_BOOL = 2,
};

union ModuleParameterValue
{
    int intValue;
    float floatValue;
    bool boolValue;
};

union ModuleParameterMinMax
{
    struct
    {
        int intMin;
        int intMax;
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

    uint8_t physicalSizeRow;
    uint8_t physicalSizeCol;
    uint8_t portLocationRow;
    uint8_t portLocationCol;

    uint8_t parameterCount;
    ModuleParameter parameters[32];
} Module;

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
} Port;

// Module command structure
typedef enum
{
    CMD_PING = 0x00,
    CMD_GET_PROPERTIES = 0x01,
    CMD_SET_PARAMETER = 0x02,
    CMD_GET_PARAMETER = 0x03,
    CMD_RESET_MODULE = 0x04,
    CMD_RESPONSE = 0x80,
} ModuleMessageId;

typedef struct
{
    uint8_t moduleRow;
    uint8_t moduleCol;
    ModuleMessageId commandId;
    uint8_t payloadLength;
    uint8_t payload[0xff];
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
    ModuleStatus status;
    ModuleMessageId inResponseTo;
    uint8_t payloadLength;
    uint8_t payload[32];
} ModuleMessageResponsePayload;
#pragma pack(pop)