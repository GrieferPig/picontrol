#pragma once
#include <cstdint>

struct InterruptSerialPIO;

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
    PARAM_TYPE_LED = 3,
};

// Module capability flags (bitmask)
enum ModuleCapabilities : uint8_t
{
    MODULE_CAP_AUTOUPDATE = 1u << 0,
    MODULE_CAP_ROTATION_AWARE = 1u << 1, // When rotated 180°, flip output values using min/max (except bool)
};

enum ModuleParameterAccess : uint8_t
{
    ACCESS_READ = 1 << 0,
    ACCESS_WRITE = 1 << 1,
    ACCESS_READ_WRITE = ACCESS_READ | ACCESS_WRITE
};

// On-wire payloads must be tightly packed
#pragma pack(push, 1)

// LED value structure
typedef struct
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t status; // 0=off, 1=on
} LEDValue;

// LED range structure
typedef struct
{
    uint8_t rMin;
    uint8_t rMax;
    uint8_t gMin;
    uint8_t gMax;
    uint8_t bMin;
    uint8_t bMax;
} LEDRange;

union ModuleParameterValue
{
    int32_t intValue;
    float floatValue;
    uint8_t boolValue;
    LEDValue ledValue;
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
    LEDRange ledRange;
};

typedef struct
{
    uint8_t id;
    char name[32];
    ModuleParameterDataType dataType;
    uint8_t access;
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
    CMD_GET_MAPPINGS = 0x06,
    CMD_SET_MAPPINGS = 0x07,
    CMD_SET_CALIB = 0x08,
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
    uint8_t parameterId;
    int32_t minValue;
    int32_t maxValue;
} ModuleMessageSetCalibPayload;

// Mapping structures for wire protocol
// Must match ModuleMapping in module_mapping_config.h but packed
#pragma pack(push, 1)
struct WireCurvePoint
{
    uint8_t x;
    uint8_t y;
};

struct WireCurve
{
    uint8_t count;
    WireCurvePoint points[4];
    WireCurvePoint controls[3];
};

struct WireActionTargetMidiNote
{
    uint8_t channel;
    uint8_t noteNumber;
    uint8_t velocity;
};

struct WireActionTargetMidiCC
{
    uint8_t channel;
    uint8_t ccNumber;
    uint8_t value;
};

struct WireActionTargetKeyboard
{
    uint8_t keycode;
    uint8_t modifier;
};

union WireActionTarget
{
    WireActionTargetMidiNote midiNote;
    WireActionTargetMidiCC midiCC;
    WireActionTargetKeyboard keyboard;
};

struct WireModuleMapping
{
    uint8_t paramId;
    uint8_t type; // ActionType
    WireCurve curve;
    WireActionTarget target;
};

typedef struct
{
    uint8_t count;
    WireModuleMapping mappings[8]; // Max 8 mappings per module
} ModuleMessageGetMappingsPayload;

typedef struct
{
    uint8_t count;
    WireModuleMapping mappings[8];
} ModuleMessageSetMappingsPayload;
#pragma pack(pop)

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