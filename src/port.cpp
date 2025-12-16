#include "port.h"
#include <cstring>

// Framing: 0xAA, commandId, payloadLen, payload..., checksum(sum of all prior bytes)
static constexpr uint8_t FRAME_START = 0xAA;
static constexpr uint32_t DETECTION_DEBOUNCE_MS = 10;

static Port ports[MODULE_PORT_ROWS][MODULE_PORT_COLS];

static ModuleMessage messageQueue[8];
static volatile uint8_t messageHead = 0;
static volatile uint8_t messageTail = 0;

static uint32_t lastDetectMs[MODULE_PORT_ROWS][MODULE_PORT_COLS];

static uint8_t calcChecksum(const uint8_t *data, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++)
    {
        sum += data[i];
    }
    return static_cast<uint8_t>(sum & 0xFF);
}

static void queueMessage(const ModuleMessage &msg)
{
    uint8_t next = (messageHead + 1) % (sizeof(messageQueue) / sizeof(messageQueue[0]));
    if (next == messageTail)
    {
        // Drop oldest on overflow to keep ISR short
        messageTail = (messageTail + 1) % (sizeof(messageQueue) / sizeof(messageQueue[0]));
    }
    messageQueue[messageHead] = msg;
    messageHead = next;
}

static void messageSinkFromIRQ(const ModuleMessage &msg)
{
    queueMessage(msg);
    Port *p = getPort(msg.moduleRow, msg.moduleCol);
    if (p)
    {
        p->hasModule = true;
    }
}

static void configurePortIfDetected(int r, int c)
{
    if (portTxPins[r][c] == PORT_PIN_UNUSED || portRxPins[r][c] == PORT_PIN_UNUSED)
    {
        return;
    }
    Port &port = ports[r][c];
    if (port.configured)
    {
        return;
    }

    int txState = digitalRead(portTxPins[r][c]);
    int rxState = digitalRead(portRxPins[r][c]);

    uint32_t now = millis();
    if (now - lastDetectMs[r][c] < DETECTION_DEBOUNCE_MS)
    {
        return;
    }

    if (txState == HIGH && rxState == LOW)
    {
        port.orientation = ModuleOrientation::UP; // vertical
    }
    else if (rxState == HIGH && txState == LOW)
    {
        port.orientation = ModuleOrientation::RIGHT; // horizontal
    }
    else
    {
        return;
    }

    InterruptSerialPIO *serial = modulePorts[r][c];
    if (!serial)
    {
        return;
    }

    serial->setPortLocation(r, c);
    serial->begin(InterruptSerialPIO::FIXED_BAUD);
    port.serial = serial;
    port.configured = true;
    lastDetectMs[r][c] = now;
}

static void detectRemoval(int r, int c)
{
    Port &port = ports[r][c];
    if (!port.configured || port.serial == nullptr)
    {
        return;
    }
    if (digitalRead(portTxPins[r][c]) == LOW && digitalRead(portRxPins[r][c]) == LOW)
    {
        port.serial->end();
        port.serial = nullptr;
        port.configured = false;
        port.hasModule = false;
    }
}

Port *getPort(int row, int col)
{
    if (row < 0 || col < 0 || row >= MODULE_PORT_ROWS || col >= MODULE_PORT_COLS)
    {
        return nullptr;
    }
    return &ports[row][col];
}

void initPorts()
{
    InterruptSerialPIO::setMessageSink(messageSinkFromIRQ);

    for (int r = 0; r < MODULE_PORT_ROWS; r++)
    {
        for (int c = 0; c < MODULE_PORT_COLS; c++)
        {
            ports[r][c].row = r;
            ports[r][c].col = c;
            ports[r][c].hasModule = false;
            ports[r][c].configured = false;
            ports[r][c].serial = nullptr;
            ports[r][c].orientation = ModuleOrientation::UP;
            lastDetectMs[r][c] = 0;

            if (portTxPins[r][c] != PORT_PIN_UNUSED)
            {
                pinMode(portTxPins[r][c], INPUT_PULLDOWN);
            }
            if (portRxPins[r][c] != PORT_PIN_UNUSED)
            {
                pinMode(portRxPins[r][c], INPUT_PULLDOWN);
            }
        }
    }
}

void scanPorts()
{
    for (int r = 0; r < MODULE_PORT_ROWS; r++)
    {
        for (int c = 0; c < MODULE_PORT_COLS; c++)
        {
            configurePortIfDetected(r, c);
            detectRemoval(r, c);
        }
    }
}

bool sendMessage(int row, int col, ModuleMessageId commandId, const uint8_t *payload, uint8_t payloadLen)
{
    Port *port = getPort(row, col);
    if (!port || !port->configured || port->serial == nullptr)
    {
        return false;
    }

    uint8_t frameHeader[3];
    frameHeader[0] = FRAME_START;
    frameHeader[1] = static_cast<uint8_t>(commandId);
    frameHeader[2] = payloadLen;

    uint8_t checksum = calcChecksum(frameHeader, sizeof(frameHeader));
    for (uint8_t i = 0; i < payloadLen; i++)
    {
        checksum = static_cast<uint8_t>(checksum + payload[i]);
    }

    port->serial->write(frameHeader, sizeof(frameHeader));
    if (payloadLen > 0)
    {
        port->serial->write(payload, payloadLen);
    }
    port->serial->write(checksum);
    return true;
}

bool sendPing(int row, int col)
{
    ModuleMessagePingPayload payload{0x55};
    return sendMessage(row, col, ModuleMessageId::CMD_PING, reinterpret_cast<uint8_t *>(&payload), sizeof(payload));
}

bool sendGetProperties(int row, int col, uint8_t requestId)
{
    ModuleMessageGetPropertiesPayload payload{requestId};
    return sendMessage(row, col, ModuleMessageId::CMD_GET_PROPERTIES, reinterpret_cast<uint8_t *>(&payload), sizeof(payload));
}

bool sendSetParameter(int row, int col, uint8_t parameterId, ModuleParameterDataType dataType, ModuleParameterValue value)
{
    ModuleMessageSetParameterPayload payload{};
    payload.parameterId = parameterId;
    payload.dataType = dataType;
    payload.value = value;
    return sendMessage(row, col, ModuleMessageId::CMD_SET_PARAMETER, reinterpret_cast<uint8_t *>(&payload), sizeof(payload));
}

bool sendGetParameter(int row, int col, uint8_t parameterId)
{
    ModuleMessageGetParameterPayload payload{parameterId};
    return sendMessage(row, col, ModuleMessageId::CMD_GET_PARAMETER, reinterpret_cast<uint8_t *>(&payload), sizeof(payload));
}

bool sendResetModule(int row, int col)
{
    ModuleMessageResetPayload payload{0xA5};
    return sendMessage(row, col, ModuleMessageId::CMD_RESET_MODULE, reinterpret_cast<uint8_t *>(&payload), sizeof(payload));
}

bool sendResponse(int row, int col, ModuleMessageId inResponseTo, ModuleStatus status, const uint8_t *payload, uint8_t payloadLen)
{
    uint8_t copyLen = payloadLen;
    if (copyLen > sizeof(ModuleMessageResponsePayload::payload))
    {
        copyLen = sizeof(ModuleMessageResponsePayload::payload);
    }

    uint8_t buffer[3 + sizeof(ModuleMessageResponsePayload::payload)];
    buffer[0] = static_cast<uint8_t>(status);
    buffer[1] = static_cast<uint8_t>(inResponseTo);
    buffer[2] = copyLen;
    if (copyLen > 0 && payload != nullptr)
    {
        memcpy(&buffer[3], payload, copyLen);
    }

    return sendMessage(row, col, ModuleMessageId::CMD_RESPONSE, buffer, static_cast<uint8_t>(3 + copyLen));
}

bool getNextMessage(ModuleMessage &out)
{
    if (messageHead == messageTail)
    {
        return false;
    }
    out = messageQueue[messageTail];
    messageTail = (messageTail + 1) % (sizeof(messageQueue) / sizeof(messageQueue[0]));
    return true;
}