#include <Arduino.h>
#include <cstring>
#include <cstddef>

#include "port.h"
#include "runtime_config.h"
#include "runtime_query.h"
#include "usb_device.h"
#include "mapping.h"
#include "module_config_manager.h"

// Everything module-oriented runs on core 1.

static uint32_t lastPingMs[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static uint32_t connectionTime[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static bool propsRequested[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static uint8_t propsRequestAttempts[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static uint32_t lastPropsRequestMs[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static constexpr uint32_t DEMO_PARAM_DELAY_MS = 2000;
static constexpr uint32_t DEMO_GET_PARAM_DELAY_MS = 2600;
static constexpr uint32_t DEMO_RESET_DELAY_MS = 3200;

static constexpr uint32_t PROPS_RETRY_INTERVAL_MS = 50;
static constexpr uint8_t PROPS_MAX_ATTEMPTS = 10;

static constexpr uint32_t PARAM_POLL_INTERVAL_MS = 50;
static bool autoupdateEnabled[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static uint32_t lastParamPollMs[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static uint8_t nextParamIndex[MODULE_PORT_ROWS][MODULE_PORT_COLS];

// Cache last seen values to support edge-triggered actions.
static bool lastValueValid[MODULE_PORT_ROWS][MODULE_PORT_COLS][32];
static ModuleParameterValue lastValue[MODULE_PORT_ROWS][MODULE_PORT_COLS][32];

static const char *commandToString(ModuleMessageId id)
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
    case ModuleMessageId::CMD_RESPONSE:
        return "RESPONSE";
    default:
        return "UNKNOWN";
    }
}

static bool parseValueFromResponse(const Port *port, uint8_t pid, const ModuleMessageResponsePayload &resp, ModuleParameterValue &outValue)
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
    default:
        return false;
    }
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
    default:
        return false;
    }
}

static ModuleOrientation getEffectiveOrientation(const Port *port)
{
    if (!port)
        return ModuleOrientation::UP;
    ModuleOrientation o = port->orientation;
    if (ModuleConfigManager::isRotated180(port->row, port->col))
    {
        o = static_cast<ModuleOrientation>((static_cast<uint8_t>(o) + 2u) % 4u);
    }
    return o;
}

// Returns true if the module is rotation-aware and currently rotated 180°.
static bool shouldFlipValue(const Port *port)
{
    if (!port || !port->hasModule)
        return false;
    // Check rotation-aware capability
    if ((port->module.capabilities & MODULE_CAP_ROTATION_AWARE) == 0)
        return false;
    // Orientation DOWN (2) means 180° rotation
    return getEffectiveOrientation(port) == ModuleOrientation::DOWN;
}

// Flip a value within its min/max range (mirror around midpoint).
static ModuleParameterValue flipValue(const Port *port, uint8_t pid, ModuleParameterDataType dt, const ModuleParameterValue &v)
{
    ModuleParameterValue result = v;
    if (!port || !port->hasModule || pid >= port->module.parameterCount)
        return result;

    const ModuleParameter &p = port->module.parameters[pid];

    switch (dt)
    {
    case ModuleParameterDataType::PARAM_TYPE_BOOL:
        // Booleans are not flipped per requirement
        break;
    case ModuleParameterDataType::PARAM_TYPE_INT:
    {
        const int32_t mn = p.minMax.intMin;
        const int32_t mx = p.minMax.intMax;
        // Flip: result = max - (value - min) = max + min - value
        result.intValue = mx + mn - v.intValue;
        break;
    }
    case ModuleParameterDataType::PARAM_TYPE_FLOAT:
    {
        const float mn = p.minMax.floatMin;
        const float mx = p.minMax.floatMax;
        // Flip: result = max - (value - min) = max + min - value
        result.floatValue = mx + mn - v.floatValue;
        break;
    }
    default:
        break;
    }
    return result;
}

static uint8_t normalizeToU8(const Port *port, uint8_t pid, ModuleParameterDataType dt, const ModuleParameterValue &v)
{
    if (!port || !port->hasModule || pid >= port->module.parameterCount)
        return 0;
    const ModuleParameter &p = port->module.parameters[pid];

    if (dt == ModuleParameterDataType::PARAM_TYPE_BOOL)
    {
        return v.boolValue ? 255 : 0;
    }
    if (dt == ModuleParameterDataType::PARAM_TYPE_INT)
    {
        const int32_t mn = p.minMax.intMin;
        const int32_t mx = p.minMax.intMax;
        if (mx <= mn)
            return 0;

        int32_t val = v.intValue;
        if (val < mn)
            val = mn;
        if (val > mx)
            val = mx;

        // (val - min) * 255 / (max - min)
        return (uint8_t)(((int64_t)(val - mn) * 255) / (mx - mn));
    }
    if (dt == ModuleParameterDataType::PARAM_TYPE_FLOAT)
    {
        const float mn = p.minMax.floatMin;
        const float mx = p.minMax.floatMax;
        if (mx <= mn)
            return 0;

        float val = v.floatValue;
        if (val < mn)
            val = mn;
        if (val > mx)
            val = mx;

        return (uint8_t)((val - mn) * 255.0f / (mx - mn));
    }
    return 0;
}

static void applyMappingToUsb(const Port *port, uint8_t pid, ModuleParameterDataType dt, const ModuleParameterValue &cur, const ModuleParameterValue *prev)
{
    if (!port)
        return;

    const ModuleMapping *m = MappingManager::findMapping(port->row, port->col, pid);
    if (!m || m->type == ACTION_NONE)
        return;

    // Apply rotation-aware flipping if needed
    ModuleParameterValue effectiveCur = cur;
    ModuleParameterValue effectivePrev{};
    if (shouldFlipValue(port))
    {
        effectiveCur = flipValue(port, pid, dt, cur);
        if (prev)
            effectivePrev = flipValue(port, pid, dt, *prev);
    }
    else if (prev)
    {
        effectivePrev = *prev;
    }

    // Normalize inputs to 0-255
    uint8_t rawCur = normalizeToU8(port, pid, dt, effectiveCur);
    uint8_t rawPrev = prev ? normalizeToU8(port, pid, dt, effectivePrev) : 0;

    // Apply Curve
    uint8_t mapCur = CurveEvaluator::eval(m->curve, rawCur);
    uint8_t mapPrev = prev ? CurveEvaluator::eval(m->curve, rawPrev) : 0;

    const bool hadPrev = (prev != nullptr);

    // Boolean logic based on 50% threshold of MAPPED value
    const bool curBool = (mapCur >= 128);
    const bool prevBool = hadPrev ? (mapPrev >= 128) : false;

    switch (m->type)
    {
    case ACTION_MIDI_NOTE:
    {
        // Channel is stored as 1-16 in mapping; usb expects 0-15.
        uint8_t ch = m->target.midiNote.channel;
        if (ch > 0)
            ch -= 1;
        const uint8_t note = m->target.midiNote.noteNumber;
        // Use curve output as velocity (0-255 -> 0-127)
        const uint8_t vel = mapCur >> 1;

        if (!hadPrev || curBool != prevBool)
        {
            if (curBool)
                usb::sendMidiNoteOn(ch, note, vel);
            else
                usb::sendMidiNoteOff(ch, note, vel);
        }
        break;
    }
    case ACTION_MIDI_CC:
    {
        uint8_t ch = m->target.midiCC.channel;
        if (ch > 0)
            ch -= 1;
        const uint8_t cc = m->target.midiCC.ccNumber;
        // Map 0-255 to 0-127
        const uint8_t value = mapCur >> 1;

        // Send if changed (or first time)
        if (!hadPrev || (mapCur >> 1) != (mapPrev >> 1))
        {
            usb::sendMidiCC(ch, cc, value);
        }
        break;
    }
    case ACTION_KEYBOARD:
    {
        // Fire KeyDown on rising edge, KeyUp on falling edge.
        // This supports holding keys.
        if (!hadPrev || curBool != prevBool)
        {
            if (curBool)
            {
                usb::sendKeyDown(m->target.keyboard.keycode, m->target.keyboard.modifier);
            }
            else
            {
                usb::sendKeyUp();
            }
        }
        break;
    }
    default:
        break;
    }
}

static void printHexBytes(const uint8_t *data, uint16_t len, uint16_t maxBytes = 16)
{
    if (!data || len == 0)
    {
        UsbSerial.print("<empty>");
        return;
    }
    uint16_t toPrint = len;
    if (toPrint > maxBytes)
        toPrint = maxBytes;
    for (uint16_t i = 0; i < toPrint; i++)
    {
        if (data[i] < 0x10)
            UsbSerial.print('0');
        UsbSerial.print(data[i], HEX);
        if (i + 1 < toPrint)
            UsbSerial.print(' ');
    }
    if (len > toPrint)
        UsbSerial.print(" ...");
}

static void printMessageHuman(const ModuleMessage &msg, Port *port)
{
    UsbSerial.print("[MSG] Port ");
    UsbSerial.print(msg.moduleRow);
    UsbSerial.print(',');
    UsbSerial.print(msg.moduleCol);
    UsbSerial.print(" type=");
    UsbSerial.print(commandToString(msg.commandId));
    UsbSerial.print(" (0x");
    UsbSerial.print(static_cast<uint8_t>(msg.commandId), HEX);
    UsbSerial.print(") len=");
    UsbSerial.print(msg.payloadLength);

    switch (msg.commandId)
    {
    case ModuleMessageId::CMD_PING:
        if (msg.payloadLength >= sizeof(ModuleMessagePingPayload))
        {
            ModuleMessagePingPayload p{};
            memcpy(&p, msg.payload, sizeof(p));
            UsbSerial.print(" magic=0x");
            UsbSerial.print(p.magic, HEX);
        }
        break;
    case ModuleMessageId::CMD_GET_PROPERTIES:
        if (msg.payloadLength >= 1)
        {
            UsbSerial.print(" requestId=");
            UsbSerial.print(msg.payload[0]);
        }
        break;
    case ModuleMessageId::CMD_SET_PARAMETER:
        if (msg.payloadLength >= sizeof(ModuleMessageSetParameterPayload))
        {
            ModuleMessageSetParameterPayload p{};
            memcpy(&p, msg.payload, sizeof(p));
            UsbSerial.print(" paramId=");
            UsbSerial.print(p.parameterId);
            UsbSerial.print(" type=");
            UsbSerial.print(static_cast<uint8_t>(p.dataType));
            UsbSerial.print(" value=");
            switch (p.dataType)
            {
            case ModuleParameterDataType::PARAM_TYPE_INT:
                UsbSerial.print(p.value.intValue);
                break;
            case ModuleParameterDataType::PARAM_TYPE_FLOAT:
                UsbSerial.print(p.value.floatValue);
                break;
            case ModuleParameterDataType::PARAM_TYPE_BOOL:
                UsbSerial.print(static_cast<int>(p.value.boolValue));
                break;
            default:
                UsbSerial.print("?");
                break;
            }
        }
        break;
    case ModuleMessageId::CMD_GET_PARAMETER:
        if (msg.payloadLength >= sizeof(ModuleMessageGetParameterPayload))
        {
            ModuleMessageGetParameterPayload p{};
            memcpy(&p, msg.payload, sizeof(p));
            UsbSerial.print(" paramId=");
            UsbSerial.print(p.parameterId);
        }
        break;
    case ModuleMessageId::CMD_RESET_MODULE:
        if (msg.payloadLength >= sizeof(ModuleMessageResetPayload))
        {
            ModuleMessageResetPayload p{};
            memcpy(&p, msg.payload, sizeof(p));
            UsbSerial.print(" magic=0x");
            UsbSerial.print(p.magic, HEX);
        }
        break;
    case ModuleMessageId::CMD_RESPONSE:
        if (msg.payloadLength >= 4)
        {
            ModuleMessageResponsePayload resp{};
            uint16_t copyLen = msg.payloadLength;
            if (copyLen > sizeof(resp))
                copyLen = sizeof(resp);
            memcpy(&resp, msg.payload, copyLen);

            UsbSerial.print(" status=");
            UsbSerial.print(static_cast<uint8_t>(resp.status));
            UsbSerial.print(" inRespTo=");
            UsbSerial.print(commandToString(resp.inResponseTo));
            UsbSerial.print(" payloadBytes=");
            UsbSerial.print(resp.payloadLength);

            if (resp.inResponseTo == ModuleMessageId::CMD_GET_PROPERTIES &&
                resp.status == ModuleStatus::MODULE_STATUS_OK &&
                resp.payloadLength >= (uint16_t)(1u + offsetof(Module, parameterCount) + 1u))
            {
                ModuleMessageGetPropertiesPayload props{};
                const uint16_t n = (resp.payloadLength > sizeof(props)) ? (uint16_t)sizeof(props) : resp.payloadLength;
                memcpy(&props, resp.payload, n);

                // Clamp parameterCount to what actually fits in this payload.
                if (props.module.parameterCount > 32)
                    props.module.parameterCount = 32;
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

                UsbSerial.print(" name=\"");
                UsbSerial.print(props.module.name);
                UsbSerial.print("\" mfg=\"");
                UsbSerial.print(props.module.manufacturer);
                UsbSerial.print("\" fw=\"");
                UsbSerial.print(props.module.fwVersion);
                UsbSerial.print("\" params=");
                UsbSerial.print(props.module.parameterCount);
            }
            else if (resp.inResponseTo == ModuleMessageId::CMD_GET_PARAMETER &&
                     resp.status == ModuleStatus::MODULE_STATUS_OK &&
                     resp.payloadLength >= 1)
            {
                uint8_t pid = resp.payload[0];
                UsbSerial.print(" paramId=");
                UsbSerial.print(pid);
                UsbSerial.print(" valueBytes=");
                if (resp.payloadLength > 1)
                {
                    printHexBytes(&resp.payload[1], static_cast<uint16_t>(resp.payloadLength - 1));
                }
                else
                {
                    UsbSerial.print("<none>");
                }
            }
            else
            {
                UsbSerial.print(" data=");
                printHexBytes(resp.payload, resp.payloadLength);
            }
        }
        break;
    default:
        break;
    }

    if (port && port->hasModule)
    {
        UsbSerial.print(" module=\"");
        UsbSerial.print(port->module.name);
        UsbSerial.print("\"");
    }
    UsbSerial.println();
}

void setup1()
{
    initPorts();
    runtime_config::init();
    runtime_query::init();
    MappingManager::init();
    // Load persisted mappings on boot so default mappings are active
    // even without a web panel issuing 'map load'. If no mapping file exists,
    // this will clear to an empty default.
    if (!MappingManager::load())
    {
        // Ensure a mapping file exists for subsequent boots.
        MappingManager::save();
    }
    ModuleConfigManager::init();
    UsbSerial.println("piControl: core1 module scan ready");
}

void loop1()
{
    scanPorts();

    uint32_t now = millis();

    // Serve async queries from core0.
    runtime_query::Request q;
    while (runtime_query::tryDequeue(q))
    {
        if (q.type == runtime_query::RequestType::LIST_MODULES)
        {
            // Header includes grid size so the UI can render empty ports.
            UsbSerial.print("ok ports rows=");
            UsbSerial.print(MODULE_PORT_ROWS);
            UsbSerial.print(" cols=");
            UsbSerial.println(MODULE_PORT_COLS);

            for (int r = 0; r < MODULE_PORT_ROWS; r++)
            {
                for (int c = 0; c < MODULE_PORT_COLS; c++)
                {
                    Port *p = getPort(r, c);
                    if (!p)
                        continue;

                    // Always emit port line.
                    UsbSerial.print("port r=");
                    UsbSerial.print(r);
                    UsbSerial.print(" c=");
                    UsbSerial.print(c);
                    UsbSerial.print(" configured=");
                    UsbSerial.print(p->configured ? 1 : 0);
                    UsbSerial.print(" hasModule=");
                    UsbSerial.print(p->hasModule ? 1 : 0);
                    UsbSerial.print(" orientation=");
                    UsbSerial.println((uint8_t)getEffectiveOrientation(p));

                    if (!p->configured || !p->hasModule)
                        continue;

                    UsbSerial.print("module r=");
                    UsbSerial.print(r);
                    UsbSerial.print(" c=");
                    UsbSerial.print(c);
                    UsbSerial.print(" type=");
                    UsbSerial.print((uint8_t)p->module.type);
                    UsbSerial.print(" caps=");
                    UsbSerial.print(p->module.capabilities);
                    UsbSerial.print(" name=\"");
                    UsbSerial.print(p->module.name);
                    UsbSerial.print("\" mfg=\"");
                    UsbSerial.print(p->module.manufacturer);
                    UsbSerial.print("\" fw=\"");
                    UsbSerial.print(p->module.fwVersion);
                    UsbSerial.print("\" params=");
                    UsbSerial.print(p->module.parameterCount);
                    UsbSerial.print(" szr=");
                    UsbSerial.print(p->module.physicalSizeRow);
                    UsbSerial.print(" szc=");
                    UsbSerial.print(p->module.physicalSizeCol);
                    UsbSerial.print(" plr=");
                    UsbSerial.print(p->module.portLocationRow);
                    UsbSerial.print(" plc=");
                    UsbSerial.println(p->module.portLocationCol);

                    const uint8_t pc = p->module.parameterCount;
                    for (uint8_t pid = 0; pid < pc && pid < 32; pid++)
                    {
                        const ModuleParameter &mp = p->module.parameters[pid];
                        UsbSerial.print("param r=");
                        UsbSerial.print(r);
                        UsbSerial.print(" c=");
                        UsbSerial.print(c);
                        UsbSerial.print(" pid=");
                        UsbSerial.print(mp.id);
                        UsbSerial.print(" dt=");
                        UsbSerial.print((uint8_t)mp.dataType);
                        UsbSerial.print(" access=");
                        UsbSerial.print(mp.access);
                        UsbSerial.print(" name=\"");
                        UsbSerial.print(mp.name);
                        UsbSerial.print("\"");

                        // Min/max/value are type-dependent.
                        switch (mp.dataType)
                        {
                        case ModuleParameterDataType::PARAM_TYPE_BOOL:
                            UsbSerial.print(" min=0 max=1 value=");
                            UsbSerial.print((int)mp.value.boolValue);
                            break;
                        case ModuleParameterDataType::PARAM_TYPE_INT:
                            UsbSerial.print(" min=");
                            UsbSerial.print(mp.minMax.intMin);
                            UsbSerial.print(" max=");
                            UsbSerial.print(mp.minMax.intMax);
                            UsbSerial.print(" value=");
                            UsbSerial.print(mp.value.intValue);
                            break;
                        case ModuleParameterDataType::PARAM_TYPE_FLOAT:
                            UsbSerial.print(" min=");
                            UsbSerial.print(mp.minMax.floatMin, 6);
                            UsbSerial.print(" max=");
                            UsbSerial.print(mp.minMax.floatMax, 6);
                            UsbSerial.print(" value=");
                            UsbSerial.print(mp.value.floatValue, 6);
                            break;
                        default:
                            break;
                        }
                        UsbSerial.println();
                    }
                }
            }

            UsbSerial.println("ok modules done");
        }
    }

    // Apply any host requests coming from core0 (CDC config).
    runtime_config::AutoupdateRequest req;
    while (runtime_config::tryDequeueAutoupdate(req))
    {
        if (req.applyToAll)
        {
            for (int r = 0; r < MODULE_PORT_ROWS; r++)
            {
                for (int c = 0; c < MODULE_PORT_COLS; c++)
                {
                    Port *p = getPort(r, c);
                    if (!p || !p->configured)
                        continue;
                    sendSetAutoupdate(r, c, req.enable != 0, req.intervalMs);
                    autoupdateEnabled[r][c] = req.enable != 0;
                }
            }
        }
        else
        {
            Port *p = getPort(req.row, req.col);
            if (p && p->configured)
            {
                sendSetAutoupdate(req.row, req.col, req.enable != 0, req.intervalMs);
                autoupdateEnabled[req.row][req.col] = req.enable != 0;
            }
        }
    }

    runtime_config::RotationOverrideRequest rreq;
    bool rotationChanged = false;
    while (runtime_config::tryDequeueRotationOverride(rreq))
    {
        if (rreq.applyToAll)
        {
            for (int r = 0; r < MODULE_PORT_ROWS; r++)
            {
                for (int c = 0; c < MODULE_PORT_COLS; c++)
                {
                    Port *p = getPort(r, c);
                    if (!p || !p->configured)
                        continue;
                    ModuleConfigManager::setRotation(r, c, rreq.rotated180 != 0);
                    rotationChanged = true;
                }
            }
        }
        else
        {
            Port *p = getPort(rreq.row, rreq.col);
            if (p && p->configured)
            {
                ModuleConfigManager::setRotation(rreq.row, rreq.col, rreq.rotated180 != 0);
                rotationChanged = true;
            }
        }
    }
    if (rotationChanged)
    {
        ModuleConfigManager::save();
    }

    // Handle set parameter requests from core0
    runtime_config::SetParameterRequest spreq;
    while (runtime_config::tryDequeueSetParameter(spreq))
    {
        Port *p = getPort(spreq.row, spreq.col);
        if (p && p->configured && p->hasModule)
        {
            ModuleParameterDataType dt = static_cast<ModuleParameterDataType>(spreq.dataType);
            ModuleParameterValue val{};

            switch (dt)
            {
            case ModuleParameterDataType::PARAM_TYPE_INT:
                val.intValue = static_cast<int32_t>(strtol(spreq.valueStr, nullptr, 10));
                break;
            case ModuleParameterDataType::PARAM_TYPE_FLOAT:
                val.floatValue = strtof(spreq.valueStr, nullptr);
                break;
            case ModuleParameterDataType::PARAM_TYPE_BOOL:
                val.boolValue = (spreq.valueStr[0] == '1' || spreq.valueStr[0] == 't' || spreq.valueStr[0] == 'T') ? 1 : 0;
                break;
            default:
                continue;
            }

            sendSetParameter(spreq.row, spreq.col, spreq.paramId, dt, val);

            // Save to persistence if it's a writable parameter
            if (spreq.paramId < p->module.parameterCount)
            {
                const ModuleParameter &mp = p->module.parameters[spreq.paramId];
                if (mp.access & ModuleParameterAccess::ACCESS_WRITE)
                {
                    ModuleConfigManager::saveParamValue(spreq.row, spreq.col, spreq.paramId, dt, val);
                }
            }

            sendGetParameter(spreq.row, spreq.col, spreq.paramId);
        }
    }

    for (int r = 0; r < MODULE_PORT_ROWS; r++)
    {
        for (int c = 0; c < MODULE_PORT_COLS; c++)
        {
            Port *p = getPort(r, c);
            if (!p || !p->configured)
            {
                propsRequested[r][c] = false;
                propsRequestAttempts[r][c] = 0;
                lastPropsRequestMs[r][c] = 0;
                connectionTime[r][c] = 0;
                lastPingMs[r][c] = 0;
                autoupdateEnabled[r][c] = false;
                lastParamPollMs[r][c] = 0;
                nextParamIndex[r][c] = 0;
                for (int i = 0; i < 32; i++)
                {
                    lastValueValid[r][c][i] = false;
                }
                continue;
            }

            // Failsafe: keep asking for properties while the port is configured but
            // the module hasn't answered yet. This helps recover from missed frames
            // during hot-plug or UART bring-up.
            if (!p->hasModule && propsRequestAttempts[r][c] < PROPS_MAX_ATTEMPTS)
            {
                if (propsRequestAttempts[r][c] == 0 || (now - lastPropsRequestMs[r][c] >= PROPS_RETRY_INTERVAL_MS))
                {
                    sendGetProperties(r, c, propsRequestAttempts[r][c]);
                    propsRequested[r][c] = true;
                    if (propsRequestAttempts[r][c] == 0)
                        connectionTime[r][c] = now;
                    propsRequestAttempts[r][c]++;
                    lastPropsRequestMs[r][c] = now;
                }
            }
            else if (p->hasModule)
            {
                // Once module is known, stop retrying.
                propsRequested[r][c] = true;
                propsRequestAttempts[r][c] = PROPS_MAX_ATTEMPTS;
            }

            // Polling mode: ask for one parameter per tick.
            if (!autoupdateEnabled[r][c] && p->hasModule && p->module.parameterCount > 0)
            {
                if (now - lastParamPollMs[r][c] >= PARAM_POLL_INTERVAL_MS)
                {
                    uint8_t pid = nextParamIndex[r][c];
                    if (pid >= p->module.parameterCount)
                        pid = 0;
                    sendGetParameter(r, c, pid);
                    nextParamIndex[r][c] = static_cast<uint8_t>(pid + 1);
                    lastParamPollMs[r][c] = now;
                }
            }
        }
    }

    static ModuleMessage msg;
    while (getNextMessage(msg))
    {
        Port *port = getPort(msg.moduleRow, msg.moduleCol);

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
                if (props.module.parameterCount > 32)
                    props.module.parameterCount = 32;
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

                // Restore persisted values for writable parameters
                for (uint8_t i = 0; i < port->module.parameterCount; i++)
                {
                    const ModuleParameter &mp = port->module.parameters[i];
                    if (mp.access & ModuleParameterAccess::ACCESS_WRITE)
                    {
                        ModuleParameterValue savedVal;
                        if (ModuleConfigManager::getParamValue(port->row, port->col, i, mp.dataType, savedVal))
                        {
                            sendSetParameter(port->row, port->col, i, mp.dataType, savedVal);
                        }
                    }
                }

                if (wasNew)
                {
                    UsbSerial.print("event module_ready r=");
                    UsbSerial.print(port->row);
                    UsbSerial.print(" c=");
                    UsbSerial.println(port->col);
                }

                // Prefer module-driven updates if the module advertises support.
                if ((port->module.capabilities & MODULE_CAP_AUTOUPDATE) != 0)
                {
                    autoupdateEnabled[port->row][port->col] = true;
                    sendSetAutoupdate(port->row, port->col, true, 0);
                }
                else
                {
                    autoupdateEnabled[port->row][port->col] = false;
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
                    if (parseValueFromResponse(port, pid, resp, cur))
                    {
                        ModuleParameterValue *prevPtr = nullptr;
                        ModuleParameterValue prev{};
                        if (lastValueValid[port->row][port->col][pid])
                        {
                            prev = lastValue[port->row][port->col][pid];
                            prevPtr = &prev;
                        }

                        if (!prevPtr || !valueEquals(dt, cur, *prevPtr))
                        {
                            // Update cached state so 'modules list' returns current values
                            port->module.parameters[pid].value = cur;

                            applyMappingToUsb(port, pid, dt, cur, prevPtr);
                            lastValue[port->row][port->col][pid] = cur;
                            lastValueValid[port->row][port->col][pid] = true;

                            UsbSerial.print("event param_changed r=");
                            UsbSerial.print(port->row);
                            UsbSerial.print(" c=");
                            UsbSerial.print(port->col);
                            UsbSerial.print(" pid=");
                            UsbSerial.print(pid);
                            UsbSerial.print(" value=");
                            switch (dt)
                            {
                            case ModuleParameterDataType::PARAM_TYPE_BOOL:
                                UsbSerial.println(cur.boolValue ? 1 : 0);
                                break;
                            case ModuleParameterDataType::PARAM_TYPE_INT:
                                UsbSerial.println(cur.intValue);
                                break;
                            case ModuleParameterDataType::PARAM_TYPE_FLOAT:
                                UsbSerial.println(cur.floatValue, 6);
                                break;
                            }
                        }
                    }
                }
            }
        }
#ifdef DEBUG_MODULE_MESSAGES
        printMessageHuman(msg, port);
#endif
    }

    delay(10);
}
