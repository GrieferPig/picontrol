#include "port.h"
#include <cstring>
#include "usb_device.h"

// Framing: 0xAA, commandId, payloadLenLo, payloadLenHi, payload..., checksum(sum of all prior bytes)
static constexpr uint8_t FRAME_START = 0xAA;
static constexpr uint32_t DETECTION_DEBOUNCE_MS = 10;
static constexpr uint32_t PING_INTERVAL_MS = 500;
static constexpr uint32_t RESPONSE_TIMEOUT_MS = 500;

static Port ports[MODULE_PORT_ROWS][MODULE_PORT_COLS];

static ModuleMessage messageQueue[16];
static volatile uint8_t messageHead = 0; // next write
static volatile uint8_t messageTail = 0; // next read
static volatile uint8_t messageCount = 0;

static uint32_t lastDetectMs[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static uint32_t lastPingSentMs[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static volatile uint32_t lastHeardMs[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static uint32_t lastRxHighMs[MODULE_PORT_ROWS][MODULE_PORT_COLS];

static const __FlashStringHelper *orientationToString(ModuleOrientation o)
{
    switch (o)
    {
    case ModuleOrientation::UP:
        return F("UP");
    case ModuleOrientation::RIGHT:
        return F("RIGHT");
    case ModuleOrientation::DOWN:
        return F("DOWN");
    case ModuleOrientation::LEFT:
        return F("LEFT");
    default:
        return F("?");
    }
}

static void ensureDetectionPinModes(int r, int c)
{
    if (portTxPins[r][c] != PORT_PIN_UNUSED)
    {
        pinMode(portTxPins[r][c], INPUT_PULLDOWN);
    }
    if (portRxPins[r][c] != PORT_PIN_UNUSED)
    {
        pinMode(portRxPins[r][c], INPUT_PULLDOWN);
    }
}

static void logPortInsertion(int r, int c, const Port &port)
{
    UsbSerial.print(F("[PORT] Insert r="));
    UsbSerial.print(r);
    UsbSerial.print(F(" c="));
    UsbSerial.print(c);
    UsbSerial.print(F(" hostTX="));
    UsbSerial.print(port.txPin);
    UsbSerial.print(F(" hostRX="));
    UsbSerial.print(port.rxPin);
    UsbSerial.print(F(" orientation="));
    UsbSerial.println(orientationToString(port.orientation));
}

static void logPortRemoval(int r, int c, const Port &port)
{
    UsbSerial.print(F("[PORT] Remove r="));
    UsbSerial.print(r);
    UsbSerial.print(F(" c="));
    UsbSerial.print(c);
    UsbSerial.print(F(" hostTX="));
    UsbSerial.print(port.txPin);
    UsbSerial.print(F(" hostRX="));
    UsbSerial.println(port.rxPin);
}

static uint8_t calcChecksum(const uint8_t *data, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++)
    {
        sum += data[i];
    }
    return static_cast<uint8_t>(sum & 0xFF);
}

ModuleMessage *allocateMessageFromIRQ()
{
    uint8_t cap = sizeof(messageQueue) / sizeof(messageQueue[0]);
    if (messageCount == cap)
    {
        // Drop oldest to make room
        messageTail = (messageTail + 1) % cap;
        messageCount--;
    }
    return &messageQueue[messageHead];
}

void commitMessageFromIRQ()
{
    uint8_t cap = sizeof(messageQueue) / sizeof(messageQueue[0]);
    messageHead = (messageHead + 1) % cap;
    messageCount++;
}

static void messageSinkFromIRQ(ModuleMessage &msg)
{
    Port *p = getPort(msg.moduleRow, msg.moduleCol);
    if (p)
    {
        p->hasModule = true;
        // Any valid frame counts as proof-of-life.
        lastHeardMs[msg.moduleRow][msg.moduleCol] = millis();
    }
}

static void removePort(int r, int c)
{
    Port &port = ports[r][c];
    if (!port.configured || port.serial == nullptr)
    {
        return;
    }

    logPortRemoval(r, c, port);

    port.serial->end();
    port.serial = nullptr;
    port.configured = false;
    port.hasModule = false;
    port.module = {};

    lastPingSentMs[r][c] = 0;
    lastHeardMs[r][c] = 0;
    lastRxHighMs[r][c] = 0;

    if (port.txPin != PORT_PIN_UNUSED)
    {
        pinMode(port.txPin, INPUT_PULLDOWN);
    }
    if (port.rxPin != PORT_PIN_UNUSED)
    {
        pinMode(port.rxPin, INPUT_PULLDOWN);
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
    uint32_t now = millis();
    if (now - lastDetectMs[r][c] < DETECTION_DEBOUNCE_MS)
    {
        return;
    }

    // Ensure pins are configured as inputs before we read them.
    // Other subsystems may have repurposed pins in-between scans.
    ensureDetectionPinModes(r, c);

    const uint8_t candA = portTxPins[r][c];
    const uint8_t candB = portRxPins[r][c];
    int stateA = digitalRead(candA);
    int stateB = digitalRead(candB);

    // Detection rule: exactly one pin is HIGH.
    // The HIGH pin is the module's TX, therefore it's the host RX.
    uint8_t hostRxPin = PORT_PIN_UNUSED;
    uint8_t hostTxPin = PORT_PIN_UNUSED;

    if (stateA == HIGH && stateB != HIGH)
    {
        port.orientation = ModuleOrientation::UP;
        hostRxPin = candA;
        hostTxPin = candB;
    }
    else if (stateB == HIGH && stateA != HIGH)
    {
        port.orientation = ModuleOrientation::RIGHT;
        hostRxPin = candB;
        hostTxPin = candA;
    }
    else
    {
        return;
    }

    port.rxPin = hostRxPin;
    port.txPin = hostTxPin;

    InterruptSerialPIO *serial = modulePorts[r][c];
    if (!serial)
    {
        return;
    }

    // Apply pin swap
    serial->setPins(port.txPin, port.rxPin);

    serial->setPortLocation(r, c);
    serial->begin(InterruptSerialPIO::FIXED_BAUD);
    port.serial = serial;
    port.configured = true;
    lastDetectMs[r][c] = now;

    // Start liveness tracking.
    lastPingSentMs[r][c] = 0;
    lastHeardMs[r][c] = now;
    lastRxHighMs[r][c] = digitalRead(port.rxPin) == HIGH ? now : 0;

    logPortInsertion(r, c, port);
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
            ports[r][c].module = {};
            lastDetectMs[r][c] = 0;
            lastPingSentMs[r][c] = 0;
            lastHeardMs[r][c] = 0;
            lastRxHighMs[r][c] = 0;

            ports[r][c].txPin = portTxPins[r][c];
            ports[r][c].rxPin = portRxPins[r][c];

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
    uint32_t now = millis();
    for (int r = 0; r < MODULE_PORT_ROWS; r++)
    {
        for (int c = 0; c < MODULE_PORT_COLS; c++)
        {
            configurePortIfDetected(r, c);

            Port &port = ports[r][c];
            if (!port.configured || port.serial == nullptr)
            {
                continue;
            }

            if (now - lastPingSentMs[r][c] >= PING_INTERVAL_MS)
            {
                if (sendPing(r, c))
                {
                    lastPingSentMs[r][c] = now;
                }
            }

            // Track whether RX has been observed HIGH (UART idle) recently.
            // This is sampled opportunistically during scans.
            if (digitalRead(port.rxPin) == HIGH)
            {
                lastRxHighMs[r][c] = now;
            }

            uint32_t heard;
            uint32_t rxHigh;
            noInterrupts();
            heard = lastHeardMs[r][c];
            interrupts();

            rxHigh = lastRxHighMs[r][c];

            // A port is considered removed if:
            // - we have not received any valid frame (any response) in RESPONSE_TIMEOUT_MS, AND
            // - the RX line has not been observed HIGH (UART idle) at any point during that same window.
            const bool noRecentResponse = heard && (now - heard) > RESPONSE_TIMEOUT_MS;
            const bool rxNeverHighInWindow = (rxHigh == 0) || ((now - rxHigh) > RESPONSE_TIMEOUT_MS);

            if (noRecentResponse && rxNeverHighInWindow)
            {
                UsbSerial.print(F("[PORT] Remove (no response + RX low) r="));
                UsbSerial.print(r);
                UsbSerial.print(F(" c="));
                UsbSerial.print(c);
                UsbSerial.print(F(" heardAgeMs="));
                UsbSerial.print((uint32_t)(now - heard));
                UsbSerial.print(F(" rxHighAgeMs="));
                UsbSerial.println(rxHigh ? (uint32_t)(now - rxHigh) : 0);
                removePort(r, c);
            }
        }
    }
}

bool sendMessage(int row, int col, ModuleMessageId commandId, const uint8_t *payload, uint16_t payloadLen)
{
    Port *port = getPort(row, col);
    if (!port || !port->configured || port->serial == nullptr)
    {
        return false;
    }

    if (payloadLen > MODULE_MAX_PAYLOAD)
    {
        payloadLen = MODULE_MAX_PAYLOAD;
    }

    uint8_t frameHeader[4];
    frameHeader[0] = FRAME_START;
    frameHeader[1] = static_cast<uint8_t>(commandId);
    frameHeader[2] = static_cast<uint8_t>(payloadLen & 0xFF);
    frameHeader[3] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);

    uint8_t checksum = calcChecksum(frameHeader, sizeof(frameHeader));
    for (uint16_t i = 0; i < payloadLen; i++)
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
    return sendMessage(row, col, ModuleMessageId::CMD_GET_PROPERTIES, &requestId, sizeof(requestId));
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

bool sendSetAutoupdate(int row, int col, bool enable, uint16_t intervalMs)
{
    ModuleMessageSetAutoupdatePayload payload{};
    payload.enable = enable ? 1 : 0;
    payload.intervalMs = intervalMs;
    return sendMessage(row, col, ModuleMessageId::CMD_SET_AUTOUPDATE, reinterpret_cast<uint8_t *>(&payload), sizeof(payload));
}

bool sendResponse(int row, int col, ModuleMessageId inResponseTo, ModuleStatus status, const uint8_t *payload, uint16_t payloadLen)
{
    uint16_t copyLen = payloadLen;
    if (copyLen > sizeof(ModuleMessageResponsePayload::payload))
    {
        copyLen = sizeof(ModuleMessageResponsePayload::payload);
    }

    static uint8_t buffer[4 + sizeof(ModuleMessageResponsePayload::payload)];
    buffer[0] = static_cast<uint8_t>(status);
    buffer[1] = static_cast<uint8_t>(inResponseTo);
    buffer[2] = static_cast<uint8_t>(copyLen & 0xFF);
    buffer[3] = static_cast<uint8_t>((copyLen >> 8) & 0xFF);
    if (copyLen > 0 && payload != nullptr)
    {
        memcpy(&buffer[4], payload, copyLen);
    }

    return sendMessage(row, col, ModuleMessageId::CMD_RESPONSE, buffer, static_cast<uint16_t>(4 + copyLen));
}

bool getNextMessage(ModuleMessage &out)
{
    noInterrupts();
    if (messageCount == 0)
    {
        interrupts();
        return false;
    }
    uint8_t idx = messageTail;
    uint8_t cap = sizeof(messageQueue) / sizeof(messageQueue[0]);
    // Copy while interrupts are disabled to prevent the ISR from overwriting
    // the slot after we logically free it.
    out.moduleRow = messageQueue[idx].moduleRow;
    out.moduleCol = messageQueue[idx].moduleCol;
    out.commandId = messageQueue[idx].commandId;
    out.payloadLength = messageQueue[idx].payloadLength;
    if (out.payloadLength > sizeof(out.payload))
    {
        out.payloadLength = sizeof(out.payload);
    }
    if (out.payloadLength > 0)
    {
        memcpy(out.payload, messageQueue[idx].payload, out.payloadLength);
    }

    messageTail = (messageTail + 1) % cap;
    messageCount--;
    interrupts();
    return true;
}