#include <Arduino.h>
#include <cstring>
#include <cstddef>
#include <cstdarg>
#include <cstdio>

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

static constexpr uint32_t PROPS_RETRY_INTERVAL_MS = 50;
static constexpr uint8_t PROPS_MAX_ATTEMPTS = 10;

static constexpr uint32_t PARAM_POLL_INTERVAL_MS = 50;
static constexpr uint32_t POST_SET_PARAM_DELAY_MS = 20;
static constexpr uint32_t PARAM_EVENT_THROTTLE_MS = 100;
static bool autoupdateEnabled[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static uint32_t lastParamPollMs[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static uint8_t nextParamIndex[MODULE_PORT_ROWS][MODULE_PORT_COLS];

static bool hasPendingParamPoll[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static uint32_t pendingParamPollTime[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static uint8_t pendingParamPollId[MODULE_PORT_ROWS][MODULE_PORT_COLS];

// Cache last seen values to support edge-triggered actions.
static bool lastValueValid[MODULE_PORT_ROWS][MODULE_PORT_COLS][32];
static ModuleParameterValue lastValue[MODULE_PORT_ROWS][MODULE_PORT_COLS][32];

// Track last event time for throttling param_changed events
static uint32_t lastEventTimeMs[MODULE_PORT_ROWS][MODULE_PORT_COLS][32];

// Track pending throttled events that need to be sent after settling
static bool hasPendingEvent[MODULE_PORT_ROWS][MODULE_PORT_COLS][32];
static uint32_t lastChangeTimeMs[MODULE_PORT_ROWS][MODULE_PORT_COLS][32];
static ModuleParameterValue pendingEventValue[MODULE_PORT_ROWS][MODULE_PORT_COLS][32];
static ModuleParameterDataType pendingEventDataType[MODULE_PORT_ROWS][MODULE_PORT_COLS][32];

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
    case ModuleMessageId::CMD_GET_MAPPINGS:
        return "GET_MAPPINGS";
    case ModuleMessageId::CMD_SET_MAPPINGS:
        return "SET_MAPPINGS";
    case ModuleMessageId::CMD_SET_CALIB:
        return "SET_CALIB";
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

    auto u8ToU14 = [](uint8_t v) -> uint16_t
    {
        // Scale 0..255 to 0..16383
        return (uint16_t)(((uint32_t)v * 16383u + 127u) / 255u);
    };

    auto u8ToPitchBendSigned = [](uint8_t v) -> int16_t
    {
        // Map 0..255 into -8192..8191, forcing v=128 => 0 exactly.
        if (v <= 128)
        {
            const int32_t d = (int32_t)128 - (int32_t)v; // 0..128
            const int32_t neg = -(d * 8192) / 128;
            return (int16_t)neg;
        }
        else
        {
            const int32_t d = (int32_t)v - (int32_t)128; // 1..127
            const int32_t pos = (d * 8191) / 127;
            return (int16_t)pos;
        }
    };

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
        const uint8_t prevVel = hadPrev ? (mapPrev >> 1) : 0;

        // Note gating must NOT depend on the 50% threshold (which maps to vel>=64).
        // MIDI Note On with velocity 0 is treated as Note Off by many synths, so:
        // - Note On when vel > 0
        // - Note Off when vel == 0
        const bool curOn = (vel > 0);
        const bool prevOn = hadPrev ? (prevVel > 0) : false;

        if (!hadPrev || curOn != prevOn)
        {
            if (curOn)
                usb::sendMidiNoteOn(ch, note, vel);
            else
                usb::sendMidiNoteOff(ch, note, 0);
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
    case ACTION_MIDI_PITCH_BEND:
    {
        uint8_t ch = m->target.midiCC.channel;
        if (ch > 0)
            ch -= 1;

        const int16_t pb = u8ToPitchBendSigned(mapCur);
        const int16_t pbPrev = hadPrev ? u8ToPitchBendSigned(mapPrev) : 0;

        const uint16_t pb14 = (uint16_t)((int32_t)pb + 8192);
        const uint16_t pbPrev14 = (uint16_t)((int32_t)pbPrev + 8192);

        if (!hadPrev || pb14 != pbPrev14)
        {
            usb::sendMidiPitchBend(ch, pb14);
        }
        break;
    }
    case ACTION_MIDI_MOD_WHEEL:
    {
        uint8_t ch = m->target.midiCC.channel;
        if (ch > 0)
            ch -= 1;

        // CC1 (MSB) + CC33 (LSB) as 14-bit value
        const uint16_t v14 = u8ToU14(mapCur);
        const uint16_t prev14 = hadPrev ? u8ToU14(mapPrev) : 0;
        if (!hadPrev || v14 != prev14)
        {
            usb::sendMidiCC14(ch, 1, v14);
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
#ifdef DEBUG_MODULE_MESSAGES
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
#endif
}

void setup1()
{
    initBoardSerial();
    initPorts();
    runtime_config::init();
    runtime_query::init();
    MappingManager::init();
    // Mappings are now loaded from modules, so no local load/save needed here.
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
            // `modules list` can be very bursty and previously built each line from many
            // small Print::print() calls, which can overflow the CDC log queue and
            // produce corrupted/truncated output when many modules are present.
            // Instead, buffer the full response and emit it in one enqueue.

            static char outBuf[48 * 1024];
            size_t outPos = 0;
            bool truncated = false;

            auto appendf = [&](const char *fmt, ...)
            {
                if (truncated)
                    return;
                if (outPos >= sizeof(outBuf))
                {
                    truncated = true;
                    return;
                }

                va_list ap;
                va_start(ap, fmt);
                const int n = vsnprintf(outBuf + outPos, sizeof(outBuf) - outPos, fmt, ap);
                va_end(ap);

                if (n < 0)
                {
                    truncated = true;
                    return;
                }
                if ((size_t)n >= (sizeof(outBuf) - outPos))
                {
                    outPos = sizeof(outBuf);
                    truncated = true;
                    return;
                }
                outPos += (size_t)n;
            };

            // Header includes grid size so the UI can render empty ports.
            appendf("ok ports rows=%d cols=%d\n", MODULE_PORT_ROWS, MODULE_PORT_COLS);

            for (int r = 0; r < MODULE_PORT_ROWS; r++)
            {
                for (int c = 0; c < MODULE_PORT_COLS; c++)
                {
                    Port *p = getPort(r, c);
                    if (!p)
                        continue;

                    appendf(
                        "port r=%d c=%d configured=%d hasModule=%d orientation=%u\n",
                        r, c,
                        p->configured ? 1 : 0,
                        p->hasModule ? 1 : 0,
                        (uint8_t)getEffectiveOrientation(p));

                    if (!p->configured || !p->hasModule)
                        continue;

                    appendf(
                        "module r=%d c=%d type=%u caps=%u name=\"%s\" mfg=\"%s\" fw=\"%s\" params=%u szr=%u szc=%u plr=%u plc=%u\n",
                        r, c,
                        (uint8_t)p->module.type,
                        (unsigned)p->module.capabilities,
                        p->module.name,
                        p->module.manufacturer,
                        p->module.fwVersion,
                        (unsigned)p->module.parameterCount,
                        (unsigned)p->module.physicalSizeRow,
                        (unsigned)p->module.physicalSizeCol,
                        (unsigned)p->module.portLocationRow,
                        (unsigned)p->module.portLocationCol);

                    const uint8_t pc = p->module.parameterCount;
                    for (uint8_t pid = 0; pid < pc && pid < 32; pid++)
                    {
                        const ModuleParameter &mp = p->module.parameters[pid];

                        switch (mp.dataType)
                        {
                        case ModuleParameterDataType::PARAM_TYPE_BOOL:
                            appendf(
                                "param r=%d c=%d pid=%u dt=%u access=%u name=\"%s\" min=0 max=1 value=%d\n",
                                r, c,
                                (unsigned)mp.id,
                                (unsigned)mp.dataType,
                                (unsigned)mp.access,
                                mp.name,
                                (int)mp.value.boolValue);
                            break;
                        case ModuleParameterDataType::PARAM_TYPE_INT:
                            appendf(
                                "param r=%d c=%d pid=%u dt=%u access=%u name=\"%s\" min=%d max=%d value=%d\n",
                                r, c,
                                (unsigned)mp.id,
                                (unsigned)mp.dataType,
                                (unsigned)mp.access,
                                mp.name,
                                (int)mp.minMax.intMin,
                                (int)mp.minMax.intMax,
                                (int)mp.value.intValue);
                            break;
                        case ModuleParameterDataType::PARAM_TYPE_FLOAT:
                            appendf(
                                "param r=%d c=%d pid=%u dt=%u access=%u name=\"%s\" min=%.6f max=%.6f value=%.6f\n",
                                r, c,
                                (unsigned)mp.id,
                                (unsigned)mp.dataType,
                                (unsigned)mp.access,
                                mp.name,
                                (double)mp.minMax.floatMin,
                                (double)mp.minMax.floatMax,
                                (double)mp.value.floatValue);
                            break;
                        case ModuleParameterDataType::PARAM_TYPE_LED:
                            appendf(
                                "param r=%d c=%d pid=%u dt=%u access=%u name=\"%s\" min=%u,%u,%u,%u,%u,%u max=0,0,0,0,0,0 value=%u,%u,%u,%u\n",
                                r, c,
                                (unsigned)mp.id,
                                (unsigned)mp.dataType,
                                (unsigned)mp.access,
                                mp.name,
                                (unsigned)mp.minMax.ledRange.rMin,
                                (unsigned)mp.minMax.ledRange.rMax,
                                (unsigned)mp.minMax.ledRange.gMin,
                                (unsigned)mp.minMax.ledRange.gMax,
                                (unsigned)mp.minMax.ledRange.bMin,
                                (unsigned)mp.minMax.ledRange.bMax,
                                (unsigned)mp.value.ledValue.r,
                                (unsigned)mp.value.ledValue.g,
                                (unsigned)mp.value.ledValue.b,
                                (unsigned)mp.value.ledValue.status);
                            break;
                        default:
                            appendf(
                                "param r=%d c=%d pid=%u dt=%u access=%u name=\"%s\"\n",
                                r, c,
                                (unsigned)mp.id,
                                (unsigned)mp.dataType,
                                (unsigned)mp.access,
                                mp.name);
                            break;
                        }

                        if (truncated)
                            break;
                    }

                    if (truncated)
                        break;
                }

                if (truncated)
                    break;
            }

            if (truncated)
            {
                // Keep protocol sentinel line intact; add a soft warning line that the UI can ignore.
                appendf("warn modules_list truncated=1\n");
            }

            appendf("ok modules done\n");

            if (outPos > 0)
            {
                (void)usb::enqueueCdcWrite(reinterpret_cast<const uint8_t *>(outBuf), outPos);
            }
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
            case ModuleParameterDataType::PARAM_TYPE_LED:
            {
                // Parse LED value: "r,g,b,status" or similar
                // For simplicity, expect comma-separated values
                int r = 0, g = 0, b = 0, s = 0;
                if (sscanf(spreq.valueStr, "%d,%d,%d,%d", &r, &g, &b, &s) >= 3)
                {
                    val.ledValue.r = static_cast<uint8_t>(r);
                    val.ledValue.g = static_cast<uint8_t>(g);
                    val.ledValue.b = static_cast<uint8_t>(b);
                    val.ledValue.status = static_cast<uint8_t>(s);
                }
                break;
            }
            default:
                continue;
            }

            sendSetParameter(spreq.row, spreq.col, spreq.paramId, dt, val);

            // Schedule a delayed poll to confirm the value.
            // We wait a bit to allow the module to complete any blocking operations (like flash writes).
            hasPendingParamPoll[spreq.row][spreq.col] = true;
            pendingParamPollTime[spreq.row][spreq.col] = millis() + POST_SET_PARAM_DELAY_MS;
            pendingParamPollId[spreq.row][spreq.col] = spreq.paramId;
        }
    }

    // Handle set calibration requests from core0
    runtime_config::SetCalibRequest screq;
    while (runtime_config::tryDequeueSetCalib(screq))
    {
        Port *p = getPort(screq.row, screq.col);
        if (p && p->configured && p->hasModule)
        {
            ModuleMessageSetCalibPayload payload{};
            payload.parameterId = screq.paramId;
            payload.minValue = screq.minValue;
            payload.maxValue = screq.maxValue;

            sendMessage(screq.row, screq.col, ModuleMessageId::CMD_SET_CALIB,
                        (const uint8_t *)&payload, sizeof(payload));
        }
    }

    // Handle sync mapping requests from core0
    runtime_config::SyncMappingRequest smreq;
    while (runtime_config::tryDequeueSyncMapping(smreq))
    {
        auto syncOne = [&](int row, int col)
        {
            Port *p = getPort(row, col);
            if (!p || !p->configured || !p->hasModule)
                return;

            ModuleMessageSetMappingsPayload payload{};
            payload.count = 0;

            int total = MappingManager::count();
            for (int i = 0; i < total; i++)
            {
                const ModuleMapping *m = MappingManager::getByIndex(i);
                if (m && m->row == row && m->col == col)
                {
                    if (payload.count < 8)
                    {
                        WireModuleMapping &wm = payload.mappings[payload.count];
                        wm.paramId = m->paramId;
                        wm.type = (uint8_t)m->type;

                        // Curve
                        wm.curve.count = m->curve.count;
                        for (int k = 0; k < 4; k++)
                        {
                            wm.curve.points[k].x = m->curve.points[k].x;
                            wm.curve.points[k].y = m->curve.points[k].y;
                        }
                        for (int k = 0; k < 3; k++)
                        {
                            wm.curve.controls[k].x = m->curve.controls[k].x;
                            wm.curve.controls[k].y = m->curve.controls[k].y;
                        }

                        // Target
                        if (m->type == ACTION_MIDI_NOTE)
                        {
                            wm.target.midiNote.channel = m->target.midiNote.channel;
                            wm.target.midiNote.noteNumber = m->target.midiNote.noteNumber;
                            wm.target.midiNote.velocity = m->target.midiNote.velocity;
                        }
                        else if (m->type == ACTION_MIDI_CC)
                        {
                            wm.target.midiCC.channel = m->target.midiCC.channel;
                            wm.target.midiCC.ccNumber = m->target.midiCC.ccNumber;
                            wm.target.midiCC.value = m->target.midiCC.value;
                        }
                        else if (m->type == ACTION_MIDI_PITCH_BEND)
                        {
                            wm.target.midiCC.channel = m->target.midiCC.channel;
                            wm.target.midiCC.ccNumber = 0;
                            wm.target.midiCC.value = 0;
                        }
                        else if (m->type == ACTION_MIDI_MOD_WHEEL)
                        {
                            wm.target.midiCC.channel = m->target.midiCC.channel;
                            wm.target.midiCC.ccNumber = 1;
                            wm.target.midiCC.value = 0;
                        }
                        else if (m->type == ACTION_KEYBOARD)
                        {
                            wm.target.keyboard.keycode = m->target.keyboard.keycode;
                            wm.target.keyboard.modifier = m->target.keyboard.modifier;
                        }

                        payload.count++;
                    }
                }
            }

            sendSetMappings(row, col, payload);
        };

        if (smreq.applyToAll)
        {
            for (int rr = 0; rr < MODULE_PORT_ROWS; rr++)
            {
                for (int cc = 0; cc < MODULE_PORT_COLS; cc++)
                {
                    syncOne(rr, cc);
                }
            }
        }
        else
        {
            syncOne(smreq.row, smreq.col);
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
                hasPendingParamPoll[r][c] = false;
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

            // Check for pending parameter polls (scheduled after SET_PARAMETER)
            if (hasPendingParamPoll[r][c] && ((int32_t)(now - pendingParamPollTime[r][c]) >= 0))
            {
                sendGetParameter(r, c, pendingParamPollId[r][c]);
                hasPendingParamPoll[r][c] = false;
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

                // No longer restoring parameters from host persistence.
                // Module is responsible for its own state.

                if (wasNew)
                {
                    UsbSerial.print("event module_ready r=");
                    UsbSerial.print(port->row);
                    UsbSerial.print(" c=");
                    UsbSerial.println(port->col);

                    // Fetch mappings from module
                    sendGetMappings(port->row, port->col);
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

                UsbSerial.print("event mappings_loaded r=");
                UsbSerial.print(port->row);
                UsbSerial.print(" c=");
                UsbSerial.print(port->col);
                UsbSerial.print(" count=");
                UsbSerial.println(mappingsPayload.count);
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
                        if (!isValueInRange(port->module.parameters[pid], cur))
                        {
                            UsbSerial.print("event param_out_of_range r=");
                            UsbSerial.print(port->row);
                            UsbSerial.print(" c=");
                            UsbSerial.print(port->col);
                            UsbSerial.print(" pid=");
                            UsbSerial.println(pid);

                            ModuleParameterValue resetVal = getResetValue(port->module.parameters[pid]);
                            sendSetParameter(port->row, port->col, pid, dt, resetVal);
                            continue;
                        }

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

                            // Throttle param_changed events - skip if same parameter triggered within 100ms
                            uint32_t now = millis();
                            uint32_t lastEventTime = lastEventTimeMs[port->row][port->col][pid];
                            bool shouldSendEvent = (now - lastEventTime) >= PARAM_EVENT_THROTTLE_MS;

                            // Track this change time for debouncing
                            lastChangeTimeMs[port->row][port->col][pid] = now;

                            if (shouldSendEvent)
                            {
                                lastEventTimeMs[port->row][port->col][pid] = now;
                                hasPendingEvent[port->row][port->col][pid] = false;

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
                                case ModuleParameterDataType::PARAM_TYPE_LED:
                                    UsbSerial.print(cur.ledValue.r);
                                    UsbSerial.print(',');
                                    UsbSerial.print(cur.ledValue.g);
                                    UsbSerial.print(',');
                                    UsbSerial.print(cur.ledValue.b);
                                    UsbSerial.print(',');
                                    UsbSerial.println(cur.ledValue.status);
                                    break;
                                }
                            }
                            else
                            {
                                // Event was throttled - mark as pending to send after settling
                                hasPendingEvent[port->row][port->col][pid] = true;
                                pendingEventValue[port->row][port->col][pid] = cur;
                                pendingEventDataType[port->row][port->col][pid] = dt;
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

    // Send pending throttled events after 100ms of no change
    for (int r = 0; r < MODULE_PORT_ROWS; r++)
    {
        for (int c = 0; c < MODULE_PORT_COLS; c++)
        {
            Port *p = getPort(r, c);
            if (!p || !p->hasModule)
                continue;

            for (uint8_t pid = 0; pid < p->module.parameterCount && pid < 32; pid++)
            {
                if (hasPendingEvent[r][c][pid])
                {
                    uint32_t now = millis();
                    // If 100ms has elapsed since the last change, send the final value
                    if ((now - lastChangeTimeMs[r][c][pid]) >= PARAM_EVENT_THROTTLE_MS)
                    {
                        hasPendingEvent[r][c][pid] = false;
                        lastEventTimeMs[r][c][pid] = now;

                        ModuleParameterDataType dt = pendingEventDataType[r][c][pid];
                        ModuleParameterValue val = pendingEventValue[r][c][pid];

                        UsbSerial.print("event param_changed r=");
                        UsbSerial.print(r);
                        UsbSerial.print(" c=");
                        UsbSerial.print(c);
                        UsbSerial.print(" pid=");
                        UsbSerial.print(pid);
                        UsbSerial.print(" value=");
                        switch (dt)
                        {
                        case ModuleParameterDataType::PARAM_TYPE_BOOL:
                            UsbSerial.println(val.boolValue ? 1 : 0);
                            break;
                        case ModuleParameterDataType::PARAM_TYPE_INT:
                            UsbSerial.println(val.intValue);
                            break;
                        case ModuleParameterDataType::PARAM_TYPE_FLOAT:
                            UsbSerial.println(val.floatValue, 6);
                            break;
                        case ModuleParameterDataType::PARAM_TYPE_LED:
                            UsbSerial.print(val.ledValue.r);
                            UsbSerial.print(',');
                            UsbSerial.print(val.ledValue.g);
                            UsbSerial.print(',');
                            UsbSerial.print(val.ledValue.b);
                            UsbSerial.print(',');
                            UsbSerial.println(val.ledValue.status);
                            break;
                        }
                    }
                }
            }
        }
    }

    delay(10);
}
