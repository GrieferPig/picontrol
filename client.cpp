#include <Arduino.h>
#include <cstring>

// --- Protocol types (mirrors host) ---
static constexpr uint16_t MODULE_MAX_PAYLOAD = 2048;

enum ModuleMessageId : uint8_t
{
    CMD_PING = 0x00,
    CMD_GET_PROPERTIES = 0x01,
    CMD_SET_PARAMETER = 0x02,
    CMD_GET_PARAMETER = 0x03,
    CMD_RESET_MODULE = 0x04,
    CMD_SET_AUTOUPDATE = 0x05,
    CMD_RESPONSE = 0x80,
};

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

enum ModuleProtocol : uint8_t
{
    PROTOCOL_UART = 0,
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

// Packed payload types
union __attribute__((packed)) ModuleParameterValue
{
    int32_t intValue;
    float floatValue;
    uint8_t boolValue;
};

union __attribute__((packed)) ModuleParameterMinMax
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

struct __attribute__((packed)) ModuleParameter
{
    uint8_t id;
    char name[32];
    ModuleParameterDataType dataType;
    ModuleParameterValue value;
    ModuleParameterMinMax minMax;
};

struct __attribute__((packed)) Module
{
    ModuleProtocol protocol;
    ModuleType type;
    char name[32];
    char manufacturer[32];
    char fwVersion[16];
    uint8_t compatibleHostVersion;
    uint8_t capabilities;
    uint8_t physicalSizeRow;
    uint8_t physicalSizeCol;
    uint8_t portLocationRow;
    uint8_t portLocationCol;
    uint8_t parameterCount;
    ModuleParameter parameters[32];
};

struct __attribute__((packed)) ModuleMessageResponsePayload
{
    ModuleStatus status;
    ModuleMessageId inResponseTo;
    uint16_t payloadLength;
    uint8_t payload[MODULE_MAX_PAYLOAD - 4];
};

struct __attribute__((packed)) ModuleMessageSetParameterPayload
{
    uint8_t parameterId;
    ModuleParameterDataType dataType;
    union
    {
        int32_t intValue;
        float floatValue;
        uint8_t boolValue;
    } value;
};

struct __attribute__((packed)) ModuleMessageGetParameterPayload
{
    uint8_t parameterId;
};

struct __attribute__((packed)) ModuleMessageGetPropertiesPayload
{
    uint8_t requestId;
    Module module; // full descriptor
};

struct __attribute__((packed)) ModuleMessagePingPayload
{
    uint8_t magic; // 0x55
};

struct __attribute__((packed)) ModuleMessageResetPayload
{
    uint8_t magic; // 0xA5
};

struct __attribute__((packed)) ModuleMessageSetAutoupdatePayload
{
    uint8_t enable;      // 0/1
    uint16_t intervalMs; // 0 = on change only
};

static_assert(sizeof(ModuleMessageGetPropertiesPayload) <= MODULE_MAX_PAYLOAD, "GetProperties payload exceeds protocol maximum");
static_assert(sizeof(ModuleMessageResponsePayload) <= MODULE_MAX_PAYLOAD, "Response payload exceeds protocol maximum");

// --- Frame helpers ---
static constexpr uint8_t FRAME_START = 0xAA;
static uint8_t calcChecksum(const uint8_t *data, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += data[i];
    return static_cast<uint8_t>(sum & 0xFF);
}

void sendFrame(ModuleMessageId cmd, const uint8_t *payload, uint16_t len)
{
    Serial.printf("[TX] Sending frame: cmd=0x%02X, len=%u\n", cmd, len);
    if (len > MODULE_MAX_PAYLOAD)
    {
        Serial.printf("[TX-WARN] Payload truncated from %u to %u bytes\n", len, MODULE_MAX_PAYLOAD);
        len = MODULE_MAX_PAYLOAD;
    }
    uint8_t hdr[4] = {FRAME_START, static_cast<uint8_t>(cmd), static_cast<uint8_t>(len & 0xFF), static_cast<uint8_t>((len >> 8) & 0xFF)};
    uint8_t checksum = calcChecksum(hdr, 4);
    for (uint16_t i = 0; i < len; i++)
        checksum = static_cast<uint8_t>(checksum + payload[i]);

    Serial1.write(hdr, 4);
    if (len)
        Serial1.write(payload, len);
    Serial1.write(checksum);
    Serial.printf("[TX] Frame sent successfully, checksum=0x%02X\n", checksum);
}

// --- Demo parameter state ---
static int demoParam = 0;
static bool autoupdateEnabled = false;
static uint16_t autoupdateIntervalMs = 0;
static int lastSentDemoParam = 0;
static uint32_t lastPeriodicSentMs = 0;

// --- Handlers ---
void respondStatus(ModuleMessageId inRespTo, ModuleStatus status, const uint8_t *data, uint16_t dataLen)
{
    Serial.printf("[RESP] Responding to cmd=0x%02X with status=%u, dataLen=%u\n", inRespTo, status, dataLen);
    static uint8_t buf[4 + MODULE_MAX_PAYLOAD];
    ModuleMessageResponsePayload resp{};
    uint16_t copyLen = dataLen;
    if (copyLen > sizeof(resp.payload))
    {
        Serial.printf("[RESP-WARN] Response payload truncated from %u to %zu bytes\n", copyLen, sizeof(resp.payload));
        copyLen = sizeof(resp.payload);
    }
    resp.status = status;
    resp.inResponseTo = inRespTo;
    resp.payloadLength = copyLen;
    if (copyLen)
        memcpy(resp.payload, data, copyLen);

    memcpy(buf, &resp, 4 + resp.payloadLength);
    sendFrame(CMD_RESPONSE, buf, static_cast<uint16_t>(4 + resp.payloadLength));
}

void handlePing()
{
    Serial.println("[CMD] Handling PING command");
    ModuleMessagePingPayload p{0x55};
    // Keep PING consistent with the rest of the protocol: respond via CMD_RESPONSE.
    respondStatus(CMD_PING, MODULE_STATUS_OK, reinterpret_cast<const uint8_t *>(&p), sizeof(p));
}

void handleGetProperties(uint8_t requestId)
{
    Serial.printf("[CMD] Handling GET_PROPERTIES command, requestId=%u\n", requestId);
    ModuleMessageGetPropertiesPayload props{};
    props.requestId = requestId;
    props.module.protocol = PROTOCOL_UART;
    props.module.type = FADER;
    strncpy(props.module.name, "DemoModule", sizeof(props.module.name) - 1);
    strncpy(props.module.manufacturer, "DemoCo", sizeof(props.module.manufacturer) - 1);
    strncpy(props.module.fwVersion, "1.0.0", sizeof(props.module.fwVersion) - 1);
    props.module.compatibleHostVersion = 1;
    props.module.capabilities = 0x01; // MODULE_CAP_AUTOUPDATE
    props.module.physicalSizeRow = 1;
    props.module.physicalSizeCol = 1;
    props.module.portLocationRow = 0;
    props.module.portLocationCol = 0;

    props.module.parameterCount = 1;
    props.module.parameters[0].id = 0;
    strncpy(props.module.parameters[0].name, "DemoParam", sizeof(props.module.parameters[0].name) - 1);
    props.module.parameters[0].dataType = PARAM_TYPE_INT;
    props.module.parameters[0].value.intValue = demoParam;
    props.module.parameters[0].minMax.intMin = 0;
    props.module.parameters[0].minMax.intMax = 127;

    respondStatus(CMD_GET_PROPERTIES, MODULE_STATUS_OK,
                  reinterpret_cast<const uint8_t *>(&props), sizeof(props));
}

void handleSetParameter(const ModuleMessageSetParameterPayload &p)
{
    Serial.printf("[CMD] Handling SET_PARAMETER command, paramId=%u, dataType=%u\n", p.parameterId, p.dataType);
    if (p.parameterId != 0 || p.dataType != PARAM_TYPE_INT)
    {
        Serial.printf("[CMD-ERROR] SET_PARAMETER unsupported: paramId=%u, dataType=%u\n", p.parameterId, p.dataType);
        respondStatus(CMD_SET_PARAMETER, MODULE_STATUS_UNSUPPORTED, nullptr, 0);
        return;
    }
    demoParam = p.value.intValue;
    Serial.printf("[CMD] SET_PARAMETER success: demoParam=%d\n", demoParam);
    respondStatus(CMD_SET_PARAMETER, MODULE_STATUS_OK, nullptr, 0);
}

static void sendParamUpdateIfNeeded(bool force)
{
    if (!autoupdateEnabled)
        return;

    uint32_t now = millis();
    const bool changed = (demoParam != lastSentDemoParam);
    const bool periodic = (autoupdateIntervalMs > 0) && (now - lastPeriodicSentMs >= autoupdateIntervalMs);
    if (!force && !changed && !periodic)
        return;

    // Reuse GET_PARAMETER response payload format: [pid][value bytes]
    uint8_t buf[1 + sizeof(int32_t)];
    buf[0] = 0;
    int32_t v = (int32_t)demoParam;
    memcpy(&buf[1], &v, sizeof(v));
    respondStatus(CMD_GET_PARAMETER, MODULE_STATUS_OK, buf, (uint16_t)sizeof(buf));

    lastSentDemoParam = demoParam;
    lastPeriodicSentMs = now;
}

void handleGetParameter(const ModuleMessageGetParameterPayload &p)
{
    Serial.printf("[CMD] Handling GET_PARAMETER command, paramId=%u\n", p.parameterId);
    if (p.parameterId != 0)
    {
        Serial.printf("[CMD-ERROR] GET_PARAMETER invalid paramId=%u\n", p.parameterId);
        respondStatus(CMD_GET_PARAMETER, MODULE_STATUS_ERROR, nullptr, 0);
        return;
    }
    uint8_t buf[1 + sizeof(int)];
    buf[0] = 0; // parameterId
    memcpy(&buf[1], &demoParam, sizeof(int));
    Serial.printf("[CMD] GET_PARAMETER success: demoParam=%d\n", demoParam);
    respondStatus(CMD_GET_PARAMETER, MODULE_STATUS_OK, buf, static_cast<uint16_t>(1 + sizeof(int)));
}

void handleSetAutoupdate(const ModuleMessageSetAutoupdatePayload &p)
{
    Serial.printf("[CMD] Handling SET_AUTOUPDATE command: enable=%u intervalMs=%u\n", p.enable, p.intervalMs);
    autoupdateEnabled = p.enable != 0;
    autoupdateIntervalMs = p.intervalMs;
    // If enabling, emit a first update immediately so host has a starting value.
    sendParamUpdateIfNeeded(true);
    respondStatus(CMD_SET_AUTOUPDATE, MODULE_STATUS_OK, nullptr, 0);
}

void handleReset(const ModuleMessageResetPayload &p)
{
    Serial.println("[CMD] Handling RESET_MODULE command");
    (void)p;
    demoParam = 0;
    Serial.println("[CMD] RESET_MODULE success: parameters reset");
    respondStatus(CMD_RESET_MODULE, MODULE_STATUS_OK, nullptr, 0);
}

// --- Simple frame parser (timeout to resync) ---
static uint8_t rxBuf[MODULE_MAX_PAYLOAD + 5];
static uint16_t rxLen = 0;
static uint16_t rxExpected = 0;
static uint32_t lastByteMs = 0;
static constexpr uint32_t PARSER_TIMEOUT_MS = 50;

static void clearParser()
{
    rxLen = 0;
    rxExpected = 0;
    lastByteMs = 0;
}

static void resetParserError(const __FlashStringHelper *reason)
{
    if (rxLen > 0)
    {
        Serial.print(F("[RX-ERROR] Parser reset"));
        if (reason)
        {
            Serial.print(F(": "));
            Serial.print(reason);
        }
        Serial.printf(", discarding %u bytes\n", rxLen);
    }
    clearParser();
}

void processFrame()
{
    if (rxLen < 5)
        return;
    const uint8_t cmd = rxBuf[1];
    const uint16_t payloadLen = static_cast<uint16_t>(rxBuf[2]) | (static_cast<uint16_t>(rxBuf[3]) << 8);
    const uint8_t *payload = &rxBuf[4];

    Serial.printf("[RX] Processing frame: cmd=0x%02X, payloadLen=%u\n", cmd, payloadLen);

    switch (cmd)
    {
    case CMD_PING:
        handlePing();
        break;
    case CMD_GET_PROPERTIES:
        if (payloadLen >= 1)
            handleGetProperties(payload[0]);
        break;
    case CMD_SET_PARAMETER:
        if (payloadLen >= sizeof(ModuleMessageSetParameterPayload))
        {
            handleSetParameter(reinterpret_cast<const ModuleMessageSetParameterPayload &>(*payload));
        }
        break;
    case CMD_GET_PARAMETER:
        if (payloadLen >= sizeof(ModuleMessageGetParameterPayload))
        {
            handleGetParameter(reinterpret_cast<const ModuleMessageGetParameterPayload &>(*payload));
        }
        break;
    case CMD_RESET_MODULE:
        if (payloadLen >= sizeof(ModuleMessageResetPayload))
        {
            handleReset(reinterpret_cast<const ModuleMessageResetPayload &>(*payload));
        }
        break;
    case CMD_SET_AUTOUPDATE:
        if (payloadLen >= sizeof(ModuleMessageSetAutoupdatePayload))
        {
            handleSetAutoupdate(reinterpret_cast<const ModuleMessageSetAutoupdatePayload &>(*payload));
        }
        break;
    default:
        Serial.printf("[RX-ERROR] Unknown command: cmd=0x%02X\n", cmd);
        respondStatus(static_cast<ModuleMessageId>(cmd), MODULE_STATUS_UNSUPPORTED, nullptr, 0);
        break;
    }
}

void feedByte(uint8_t b)
{
    uint32_t now = millis();
    if (rxLen && (now - lastByteMs) > PARSER_TIMEOUT_MS)
    {
        Serial.println("[RX-ERROR] Parser timeout, resyncing");
        resetParserError(F("timeout"));
    }
    lastByteMs = now;

    if (rxLen == 0)
    {
        if (b == FRAME_START)
        {
            rxBuf[0] = b;
            rxLen = 1;
        }
        return;
    }

    if (rxLen >= sizeof(rxBuf))
    {
        Serial.println("[RX-ERROR] Buffer overflow, resetting parser");
        resetParserError(F("overflow"));
        return;
    }

    rxBuf[rxLen++] = b;

    if (rxLen == 4)
    {
        uint16_t payloadLen = static_cast<uint16_t>(rxBuf[2]) | (static_cast<uint16_t>(rxBuf[3]) << 8);
        if (payloadLen > MODULE_MAX_PAYLOAD)
        {
            Serial.printf("[RX-ERROR] Payload too large: %u > %u bytes\n", payloadLen, MODULE_MAX_PAYLOAD);
            resetParserError(F("payload too large"));
            return;
        }
        uint32_t total = 5u + payloadLen;
        rxExpected = (total <= sizeof(rxBuf)) ? total : 0;
        if (!rxExpected)
            resetParserError(F("expected length invalid"));
    }

    if (rxExpected && rxLen == rxExpected)
    {
        uint8_t checksum = rxBuf[rxLen - 1];
        uint8_t expected = calcChecksum(rxBuf, rxLen - 1);
        if (checksum == expected)
        {
            Serial.printf("[RX] Frame complete, checksum OK (0x%02X)\n", checksum);
            processFrame();
        }
        else
        {
            Serial.printf("[RX-ERROR] Checksum mismatch: got=0x%02X, expected=0x%02X\n", checksum, expected);
        }
        clearParser();
    }
}

// --- Arduino setup/loop ---
void setup()
{
    Serial.begin(115200);
    Serial1.setPins(17, 18, -1, -1); // RX, TX, RTS, CTS
    Serial1.begin(460800);           // matches host FIXED_BAUD
    Serial.println("Module client ready");
}

void loop()
{
    while (Serial1.available() > 0)
    {
        uint8_t b = static_cast<uint8_t>(Serial1.read());
        feedByte(b);
    }
    // In autoupdate mode, periodically push updates (or on change).
    sendParamUpdateIfNeeded(false);
    // Do module-specific sensor/logic work here
}