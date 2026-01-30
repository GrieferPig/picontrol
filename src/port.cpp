#include "port.h"
#include <cstring>
#include "usb_device.h"
#include "mapping.h"

// Framing: 0xAA, commandId, payloadLenLo, payloadLenHi, payload, checksum
static constexpr uint8_t FRAME_START = 0xAA;
static constexpr uint32_t DETECTION_DEBOUNCE_MS = 10;
static constexpr uint32_t PING_INTERVAL_MS = 500;
static constexpr uint32_t RESPONSE_TIMEOUT_MS = 500;

namespace Port
{
    static State ports[MODULE_PORT_ROWS][MODULE_PORT_COLS];

    static ModuleMessage messageQueue[16];
    static volatile uint8_t messageHead = 0; // next write
    static volatile uint8_t messageTail = 0; // next read
    static volatile uint8_t messageCount = 0;

    static uint32_t lastDetectMs[MODULE_PORT_ROWS][MODULE_PORT_COLS];
    static uint32_t lastPingSentMs[MODULE_PORT_ROWS][MODULE_PORT_COLS];
    static volatile uint32_t lastHeardMs[MODULE_PORT_ROWS][MODULE_PORT_COLS];
    static uint32_t lastRxHighMs[MODULE_PORT_ROWS][MODULE_PORT_COLS];

    static const char *orientationToString(ModuleOrientation o)
    {
        switch (o)
        {
        case ModuleOrientation::UP:
            return "UP";
        case ModuleOrientation::RIGHT:
            return "RIGHT";
        case ModuleOrientation::DOWN:
            return "DOWN";
        case ModuleOrientation::LEFT:
            return "LEFT";
        default:
            return "?";
        }
    }

#ifdef DEBUG_MODULE_MESSAGES
    static const char *commandToStringTx(ModuleMessageId id)
    {
        switch (id)
        {
        case ModuleMessageId::CMD_PING:
            return "PING";
        case ModuleMessageId::CMD_GET_PROPERTIES:
            return "GET_PROPERTIES";
        case ModuleMessageId::CMD_SET_PARAMETER:
            return "SET_PARAMETER";
        case ModuleMessageId::CMD_GET_PARAMETER:
            return "GET_PARAMETER";
        case ModuleMessageId::CMD_RESET_MODULE:
            return "RESET_MODULE";
        case ModuleMessageId::CMD_SET_AUTOUPDATE:
            return "SET_AUTOUPDATE";
        case ModuleMessageId::CMD_GET_MAPPINGS:
            return "GET_MAPPINGS";
        case ModuleMessageId::CMD_SET_MAPPINGS:
            return "SET_MAPPINGS";
        case ModuleMessageId::CMD_RESPONSE:
            return "RESPONSE";
        default:
            return "UNKNOWN";
        }
    }

    static void printHexBytesTx(const uint8_t *data, uint16_t len, uint16_t maxBytes = 16)
    {
        if (!data || len == 0)
        {
            printf("<empty>");
            return;
        }
        uint16_t toPrint = len;
        if (toPrint > maxBytes)
            toPrint = maxBytes;
        for (uint16_t i = 0; i < toPrint; i++)
        {
            printf("%02X", data[i]);
            if (i + 1 < toPrint)
                printf(" ");
        }
        if (len > toPrint)
            printf(" ...");
    }
#endif

    static bool parseValueFromResponse(const Port::State *port, uint8_t pid, const ModuleMessageResponsePayload &resp, ModuleParameterValue &outValue)
    {
        if (!port || !port->hasModule)
            return false;
        if (pid >= port->module.parameterCount)
            return false;
        const ModuleParameterDataType dt = port->module.parameters[pid].dataType;

        const uint8_t *data = &resp.payload[1];
        const uint16_t len = (resp.payloadLength > 1) ? static_cast<uint16_t>(resp.payloadLength - 1) : 0;

        switch (dt)
        {
        case ModuleParameterDataType::PARAM_TYPE_BOOL:
            if (len < 1)
                return false;
            outValue.boolValue = data[0] ? 1 : 0;
            return true;
        case ModuleParameterDataType::PARAM_TYPE_INT:
            if (len < sizeof(int32_t))
                return false;
            memcpy(&outValue.intValue, data, sizeof(int32_t));
            return true;
        case ModuleParameterDataType::PARAM_TYPE_FLOAT:
            if (len < sizeof(float))
                return false;
            memcpy(&outValue.floatValue, data, sizeof(float));
            return true;
        case ModuleParameterDataType::PARAM_TYPE_LED:
            if (len < sizeof(LEDValue))
                return false;
            memcpy(&outValue.ledValue, data, sizeof(LEDValue));
            return true;
        default:
            return false;
        }
    }

    static bool isValueInRange(const ModuleParameter &p, const ModuleParameterValue &v)
    {
        switch (p.dataType)
        {
        case ModuleParameterDataType::PARAM_TYPE_INT:
            return v.intValue >= p.minMax.intMin && v.intValue <= p.minMax.intMax;
        case ModuleParameterDataType::PARAM_TYPE_FLOAT:
            return v.floatValue >= p.minMax.floatMin && v.floatValue <= p.minMax.floatMax;
        default:
            return true;
        }
    }

    static ModuleParameterValue getResetValue(const ModuleParameter &p)
    {
        ModuleParameterValue v{};
        switch (p.dataType)
        {
        case ModuleParameterDataType::PARAM_TYPE_INT:
            v.intValue = p.minMax.intMin;
            break;
        case ModuleParameterDataType::PARAM_TYPE_FLOAT:
            v.floatValue = p.minMax.floatMin;
            break;
        case ModuleParameterDataType::PARAM_TYPE_BOOL:
            v.boolValue = 0;
            break;
        default:
            break;
        }
        return v;
    }

    static bool valueEquals(ModuleParameterDataType dt, const ModuleParameterValue &a, const ModuleParameterValue &b)
    {
        switch (dt)
        {
        case ModuleParameterDataType::PARAM_TYPE_BOOL:
            return a.boolValue == b.boolValue;
        case ModuleParameterDataType::PARAM_TYPE_INT:
            return a.intValue == b.intValue;
        case ModuleParameterDataType::PARAM_TYPE_FLOAT:
            return memcmp(&a.floatValue, &b.floatValue, sizeof(float)) == 0;
        case ModuleParameterDataType::PARAM_TYPE_LED:
            return memcmp(&a.ledValue, &b.ledValue, sizeof(LEDValue)) == 0;
        default:
            return false;
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

    static void logPortInsertion(int r, int c, const State &port)
    {
#ifdef DEBUG_MODULE_MESSAGES
        printf("[PORT] Insert r=%d c=%d hostTX=%d hostRX=%d orientation=%s\n", r, c, port.txPin, port.rxPin, orientationToString(port.orientation));
#endif
    }

    static void logPortRemoval(int r, int c, const State &port)
    {
#ifdef DEBUG_MODULE_MESSAGES
        printf("[PORT] Remove r=%d c=%d hostTX=%d hostRX=%d\n", r, c, port.txPin, port.rxPin);
#endif
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

    extern "C" ModuleMessage *allocateMessageFromIRQ()
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

    extern "C" void commitMessageFromIRQ()
    {
        uint8_t cap = sizeof(messageQueue) / sizeof(messageQueue[0]);
        messageHead = (messageHead + 1) % cap;
        messageCount++;
    }

    static void messageSinkFromIRQ(ModuleMessage *msg)
    {
        State *p = get(msg->moduleRow, msg->moduleCol);
        if (p)
        {
            // Do NOT set hasModule = true here.
            // hasModule implies we have successfully fetched the module properties (name, params, etc).
            // That logic is handled in module_task.cpp upon receiving CMD_GET_PROPERTIES response.

            // Any valid frame counts as proof-of-life.
            lastHeardMs[msg->moduleRow][msg->moduleCol] = millis();
        }
    }

    static void removePort(int r, int c)
    {
        State &port = ports[r][c];
        if (!port.configured || port.serial == nullptr)
        {
            return;
        }

        logPortRemoval(r, c, port);

        ispio_end(port.serial);
        port.serial = nullptr;
        port.configured = false;
        port.hasModule = false;
        port.module = {};

        MappingManager::clearMappingsForPort(r, c);

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

        printf("event port_disconnected r=%d c=%d\n", r, c);
    }

    static void configurePortIfDetected(int r, int c)
    {
        if (portTxPins[r][c] == PORT_PIN_UNUSED || portRxPins[r][c] == PORT_PIN_UNUSED)
        {
            return;
        }
        State &port = ports[r][c];
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
        ispio_set_pins(serial, port.txPin, port.rxPin);

        ispio_set_port_location(serial, r, c);
        ispio_begin(serial, ISPIO_FIXED_BAUD);
        port.serial = serial;
        port.configured = true;
        lastDetectMs[r][c] = now;

        // Start liveness tracking.
        lastPingSentMs[r][c] = 0;
        lastHeardMs[r][c] = now;
        lastRxHighMs[r][c] = digitalRead(port.rxPin) == HIGH ? now : 0;

        logPortInsertion(r, c, port);

        // Get module properties
        sendGetProperties(r, c);
    }

    State *get(int row, int col)
    {
        if (row < 0 || col < 0 || row >= MODULE_PORT_ROWS || col >= MODULE_PORT_COLS)
        {
            return nullptr;
        }
        return &ports[row][col];
    }

    State *getAll()
    {
        return &ports[0][0];
    }

    void init()
    {
        initBoardSerial();
        ispio_set_message_sink(messageSinkFromIRQ);

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

    void task()
    {
        uint32_t now = millis();
        // Scan ports for insertion/removal
        for (int r = 0; r < MODULE_PORT_ROWS; r++)
        {
            for (int c = 0; c < MODULE_PORT_COLS; c++)
            {
                // Check insertion
                configurePortIfDetected(r, c);

                // Check removal
                State &port = ports[r][c];
                if (!port.configured || port.serial == nullptr)
                {
                    continue;
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
                    removePort(r, c);
                }
            }
        }

        // Process any queued messages
        ModuleMessage msg;
        while (Port::getNextMessage(msg))
        {
            Port::State *port = Port::get(msg.moduleRow, msg.moduleCol);

            if (msg.commandId == ModuleMessageId::CMD_RESPONSE && msg.payloadLength >= 4)
            {
                ModuleMessageResponsePayload resp{};
                uint16_t copyLen = msg.payloadLength;
                if (copyLen > sizeof(resp))
                    copyLen = sizeof(resp);
                memcpy(&resp, msg.payload, copyLen);

                if (port && resp.status == ModuleStatus::MODULE_STATUS_OK &&
                    resp.inResponseTo == ModuleMessageId::CMD_GET_PROPERTIES &&
                    resp.payloadLength >= (uint16_t)(1u + offsetof(Module, parameterCount) + 1u))
                {
                    ModuleMessageGetPropertiesPayload props{};
                    const uint16_t n = (resp.payloadLength > sizeof(props)) ? (uint16_t)sizeof(props) : resp.payloadLength;
                    memcpy(&props, resp.payload, n);

                    // Clamp parameterCount to what actually fits in this payload.
                    if (props.module.parameterCount > 8)
                        props.module.parameterCount = 8;
                    const size_t headerBytes = 1u + offsetof(Module, parameters);
                    if (resp.payloadLength < headerBytes)
                    {
                        props.module.parameterCount = 0;
                    }
                    else
                    {
                        const size_t availParamBytes = (size_t)resp.payloadLength - headerBytes;
                        const uint8_t maxParams = (uint8_t)(availParamBytes / sizeof(ModuleParameter));
                        if (props.module.parameterCount > maxParams)
                            props.module.parameterCount = maxParams;
                    }

                    port->module = props.module;
                    bool wasNew = !port->hasModule;
                    port->hasModule = true;

                    // Check for auto update capability
                    if (port->module.capabilities & MODULE_CAP_AUTOUPDATE)
                    {
                        // Enable auto update with on-change only
                        sendSetAutoupdate(port->row, port->col, 1, 0);
                    }

                    if (wasNew)
                    {
                        // Fetch mappings from module
                        sendGetMappings(port->row, port->col);
                    }
                }
                else if (port && resp.status == ModuleStatus::MODULE_STATUS_OK &&
                         resp.inResponseTo == ModuleMessageId::CMD_GET_MAPPINGS &&
                         resp.payloadLength >= 1)
                {
                    ModuleMessageGetMappingsPayload mappingsPayload{};
                    uint16_t copyLen = resp.payloadLength;
                    if (copyLen > sizeof(mappingsPayload))
                        copyLen = sizeof(mappingsPayload);
                    memcpy(&mappingsPayload, resp.payload, copyLen);

                    MappingManager::clearMappingsForPort(port->row, port->col);

                    for (int i = 0; i < mappingsPayload.count && i < 8; i++)
                    {
                        const WireModuleMapping &wm = mappingsPayload.mappings[i];
                        ModuleMapping m{};
                        m.row = port->row;
                        m.col = port->col;
                        m.paramId = wm.paramId;
                        m.type = (ActionType)wm.type;

                        // Curve
                        m.curve.count = wm.curve.count;
                        for (int k = 0; k < 4; k++)
                        {
                            m.curve.points[k].x = wm.curve.points[k].x;
                            m.curve.points[k].y = wm.curve.points[k].y;
                        }
                        for (int k = 0; k < 3; k++)
                        {
                            m.curve.controls[k].x = wm.curve.controls[k].x;
                            m.curve.controls[k].y = wm.curve.controls[k].y;
                        }

                        // Target
                        if (m.type == ACTION_MIDI_NOTE)
                        {
                            m.target.midiNote.channel = wm.target.midiNote.channel;
                            m.target.midiNote.noteNumber = wm.target.midiNote.noteNumber;
                            m.target.midiNote.velocity = wm.target.midiNote.velocity;
                        }
                        else if (m.type == ACTION_MIDI_CC)
                        {
                            m.target.midiCC.channel = wm.target.midiCC.channel;
                            m.target.midiCC.ccNumber = wm.target.midiCC.ccNumber;
                            m.target.midiCC.value = wm.target.midiCC.value;
                        }
                        else if (m.type == ACTION_MIDI_PITCH_BEND)
                        {
                            m.target.midiCC.channel = wm.target.midiCC.channel;
                            m.target.midiCC.ccNumber = 0;
                            m.target.midiCC.value = 0;
                        }
                        else if (m.type == ACTION_MIDI_MOD_WHEEL)
                        {
                            m.target.midiCC.channel = wm.target.midiCC.channel;
                            m.target.midiCC.ccNumber = 1;
                            m.target.midiCC.value = 0;
                        }
                        else if (m.type == ACTION_KEYBOARD)
                        {
                            m.target.keyboard.keycode = wm.target.keyboard.keycode;
                            m.target.keyboard.modifier = wm.target.keyboard.modifier;
                        }

                        MappingManager::addMapping(port->row, port->col, m);
                    }
                }

                // Treat any OK GET_PARAMETER response as a (possibly unsolicited) parameter update.
                if (port && resp.status == ModuleStatus::MODULE_STATUS_OK &&
                    resp.inResponseTo == ModuleMessageId::CMD_GET_PARAMETER && resp.payloadLength >= 1)
                {
                    const uint8_t pid = resp.payload[0];
                    if (port->hasModule && pid < port->module.parameterCount)
                    {
                        const ModuleParameterDataType dt = port->module.parameters[pid].dataType;
                        ModuleParameterValue cur{};
                        // Try to parse the value
                        if (parseValueFromResponse(port, pid, resp, cur))
                        {
                            if (!isValueInRange(port->module.parameters[pid], cur))
                            {
                                printf("warn Param r=%d c=%d pid=%d value out of range, resetting\n", port->row, port->col, pid);
                                ModuleParameterValue resetVal = getResetValue(port->module.parameters[pid]);
                                sendSetParameter(port->row, port->col, pid, dt, resetVal);
                                continue;
                            }

                            // Update module parameter cache
                            ModuleParameterValue &cached = port->module.parameters[pid].value;
                            if (!valueEquals(dt, cached, cur))
                            {
                                cached = cur;
                                MappingManager::applyMapping(port, pid, dt, cur);
                            }
                        }
                    }
                }
            }
        }
    }

    bool sendMessage(int row, int col, ModuleMessageId commandId, const uint8_t *payload, uint16_t payloadLen)
    {
        State *port = get(row, col);
        if (!port || !port->configured || port->serial == nullptr)
        {
            return false;
        }

        if (payloadLen > MODULE_MAX_PAYLOAD)
        {
            payloadLen = MODULE_MAX_PAYLOAD;
        }

#ifdef DEBUG_MODULE_MESSAGES
        printf("[TX] Port %d,%d cmd=%s (0x%02X) len=%u data=",
               row, col, commandToStringTx(commandId), static_cast<uint8_t>(commandId), payloadLen);
        printHexBytesTx(payload, payloadLen);
        printf("\n");
#endif

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

        ispio_write_buffer(port->serial, frameHeader, sizeof(frameHeader));
        if (payloadLen > 0)
        {
            ispio_write_buffer(port->serial, payload, payloadLen);
        }
        ispio_write(port->serial, checksum);
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

    bool sendSetMappings(int row, int col, const ModuleMessageSetMappingsPayload &payload)
    {
        return sendMessage(row, col, ModuleMessageId::CMD_SET_MAPPINGS, (const uint8_t *)&payload, sizeof(payload));
    }

    bool sendGetMappings(int row, int col)
    {
        return sendMessage(row, col, ModuleMessageId::CMD_GET_MAPPINGS, nullptr, 0);
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

        if (messageCount >= 16)
        {
            printf("[WARN]: messageCount >= 16, may overflow");
        }

        messageTail = (messageTail + 1) % cap;
        messageCount--;
        interrupts();
#ifdef DEBUG_MODULE_MESSAGES
        printf("[RX] Port %u,%u cmd=%s (0x%02X) len=%u data=",
               out.moduleRow,
               out.moduleCol,
               commandToStringTx(out.commandId),
               static_cast<uint8_t>(out.commandId),
               out.payloadLength);
        printHexBytesTx(out.payload, out.payloadLength);
        printf("\n");
#endif
        return true;
    }
}