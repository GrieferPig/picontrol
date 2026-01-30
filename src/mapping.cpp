#include "mapping.h"

#include <Arduino.h>
#include <cstring>

#include <pico/sync.h>
#include "usb_device.h"

namespace
{
    static critical_section_t g_mapLock;
    static bool g_lockInited = false;

    static void initLockOnce()
    {
        if (g_lockInited)
            return;
        critical_section_init(&g_mapLock);
        g_lockInited = true;
    }

    static void fillDefaultCurve(Curve &c)
    {
        c.count = 2;
        c.points[0] = {0, 0};
        c.points[1] = {255, 255};
        c.controls[0] = {127, 127};
        // Clear others
        c.points[2] = {0, 0};
        c.points[3] = {0, 0};
        c.controls[1] = {0, 0};
        c.controls[2] = {0, 0};
    }

    static void fillTarget(ModuleMapping &m, ActionType type, uint8_t d1, uint8_t d2)
    {
        m.type = type;
        // Initialize curve to default linear if empty (count=0)
        if (m.curve.count == 0)
        {
            fillDefaultCurve(m.curve);
        }

        switch (type)
        {
        case ACTION_MIDI_NOTE:
            m.target.midiNote.channel = d1; // 1-16
            m.target.midiNote.noteNumber = d2;
            m.target.midiNote.velocity = 127;
            break;
        case ACTION_MIDI_CC:
            m.target.midiCC.channel = d1; // 1-16
            m.target.midiCC.ccNumber = d2;
            m.target.midiCC.value = 0;
            break;
        case ACTION_MIDI_PITCH_BEND:
            // Store channel in midiCC channel slot; pitch bend has no extra per-mapping params.
            m.target.midiCC.channel = d1; // 1-16
            m.target.midiCC.ccNumber = 0;
            m.target.midiCC.value = 0;
            break;
        case ACTION_MIDI_MOD_WHEEL:
            // Store channel in midiCC channel slot; mod wheel is CC1 (14-bit: CC1+33).
            m.target.midiCC.channel = d1; // 1-16
            m.target.midiCC.ccNumber = 1;
            m.target.midiCC.value = 0;
            break;
        case ACTION_KEYBOARD:
            m.target.keyboard.keycode = d1;
            m.target.keyboard.modifier = d2;
            break;
        case ACTION_NONE:
        default:
            memset(&m.target, 0, sizeof(m.target));
            break;
        }
    }

    static uint8_t normalizeToU8(const Port::State *port, uint8_t pid, ModuleParameterDataType dt, const ModuleParameterValue &v)
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
}

ModuleMapping MappingManager::mappings[32];
int MappingManager::mappingCount = 0;

void MappingManager::init()
{
    initLockOnce();
}

void MappingManager::clearMappings()
{
    initLockOnce();
    critical_section_enter_blocking(&g_mapLock);
    mappingCount = 0;
    for (int i = 0; i < 32; i++)
    {
        mappings[i] = {};
        mappings[i].row = -1;
        mappings[i].col = -1;
    }
    critical_section_exit(&g_mapLock);
}

void MappingManager::clearAll()
{
    clearMappings();
}

void MappingManager::clearMappingsForPort(int r, int c)
{
    initLockOnce();
    critical_section_enter_blocking(&g_mapLock);

    int i = 0;
    while (i < mappingCount)
    {
        if (mappings[i].row == r && mappings[i].col == c)
        {
            // release usb actions
            switch (mappings[i].type)
            {
            case ACTION_KEYBOARD:
                usb::sendKeyUp(mappings[i].target.keyboard.keycode);
                break;
            case ACTION_MIDI_CC:
                usb::sendMidiCC(mappings[i].target.midiCC.channel - 1,
                                mappings[i].target.midiCC.ccNumber,
                                0);
                break;
            case ACTION_MIDI_MOD_WHEEL:
                usb::sendMidiCC14(mappings[i].target.midiCC.channel - 1,
                                  1, // CC1 (Mod wheel)
                                  0);
                break;
            case ACTION_MIDI_PITCH_BEND:
                usb::sendMidiPitchBend(mappings[i].target.midiCC.channel - 1,
                                       8192); // center
                break;
            case ACTION_MIDI_NOTE:
                usb::sendMidiNoteOff(mappings[i].target.midiNote.channel - 1,
                                     mappings[i].target.midiNote.noteNumber,
                                     0);
                break;
            default:
                break;
            }
            // swap-remove
            if (i != mappingCount - 1)
            {
                mappings[i] = mappings[mappingCount - 1];
            }
            mappingCount--;
            // Don't increment i, check the swapped element
        }
        else
        {
            i++;
        }
    }

    critical_section_exit(&g_mapLock);
}

void MappingManager::addMapping(int r, int c, const ModuleMapping &m)
{
    initLockOnce();
    critical_section_enter_blocking(&g_mapLock);

    // Check if exists
    for (int i = 0; i < mappingCount; i++)
    {
        if (mappings[i].row == r && mappings[i].col == c && mappings[i].paramId == m.paramId)
        {
            mappings[i] = m;
            mappings[i].row = r; // Ensure correct coords
            mappings[i].col = c;
            critical_section_exit(&g_mapLock);
            return;
        }
    }

    if (mappingCount < 32)
    {
        mappings[mappingCount] = m;
        mappings[mappingCount].row = r;
        mappings[mappingCount].col = c;
        mappingCount++;
    }

    critical_section_exit(&g_mapLock);
}

int MappingManager::count()
{
    initLockOnce();
    critical_section_enter_blocking(&g_mapLock);
    int c = mappingCount;
    critical_section_exit(&g_mapLock);
    return c;
}

const ModuleMapping *MappingManager::getByIndex(int idx)
{
    initLockOnce();
    critical_section_enter_blocking(&g_mapLock);
    const ModuleMapping *out = nullptr;
    if (idx >= 0 && idx < mappingCount)
    {
        out = &mappings[idx];
    }
    critical_section_exit(&g_mapLock);
    return out;
}

const ModuleMapping *MappingManager::findMapping(int r, int c, uint8_t pid)
{
    initLockOnce();
    critical_section_enter_blocking(&g_mapLock);
    for (int i = 0; i < mappingCount; i++)
    {
        const ModuleMapping &m = mappings[i];
        if (m.row == r && m.col == c && m.paramId == pid)
        {
            const ModuleMapping *out = &mappings[i];
            critical_section_exit(&g_mapLock);
            return out;
        }
    }
    critical_section_exit(&g_mapLock);
    return nullptr;
}

void MappingManager::applyMapping(const Port::State *port, uint8_t pid, ModuleParameterDataType dt, const ModuleParameterValue &cur)
{
    if (!port)
        return;

    const ModuleMapping *m = MappingManager::findMapping(port->row, port->col, pid);
    if (!m || m->type == ACTION_NONE)
        return;

    // Normalize inputs to 0-255
    uint8_t rawCur = normalizeToU8(port, pid, dt, cur);
    // Apply Curve
    uint8_t mapCur = CurveEvaluator::eval(m->curve, rawCur);

    // Boolean logic based on 50% threshold of MAPPED value
    const bool curBool = (mapCur >= 128);

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

        // Note gating must NOT depend on the 50% threshold (which maps to vel>=64).
        // MIDI Note On with velocity 0 is treated as Note Off by many synths, so:
        // - Note On when vel > 0
        // - Note Off when vel == 0
        const bool curOn = (vel > 0);

        if (curOn)
            usb::sendMidiNoteOn(ch, note, vel);
        else
            usb::sendMidiNoteOff(ch, note, 0);

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

        usb::sendMidiCC(ch, cc, value);

        break;
    }
    case ACTION_MIDI_PITCH_BEND:
    {
        uint8_t ch = m->target.midiCC.channel;
        if (ch > 0)
            ch -= 1;

        const int16_t pb = u8ToPitchBendSigned(mapCur);

        const uint16_t pb14 = (uint16_t)((int32_t)pb + 8192);

        usb::sendMidiPitchBend(ch, pb14);

        break;
    }
    case ACTION_MIDI_MOD_WHEEL:
    {
        uint8_t ch = m->target.midiCC.channel;
        if (ch > 0)
            ch -= 1;

        // CC1 (MSB) + CC33 (LSB) as 14-bit value
        const uint16_t v14 = u8ToU14(mapCur);

        usb::sendMidiCC14(ch, 1, v14);

        break;
    }
    case ACTION_KEYBOARD:
    {
        // Fire KeyDown on rising edge, KeyUp on falling edge.
        // This supports holding keys.
        if (curBool)
        {
            usb::sendKeyDown(m->target.keyboard.keycode, m->target.keyboard.modifier);
        }
        else
        {
            usb::sendKeyUp(m->target.keyboard.keycode);
        }
        break;
    }
    default:
        break;
    }
}

void MappingManager::updateMapping(int r, int c, uint8_t pid, ActionType type, uint8_t d1, uint8_t d2)
{
    initLockOnce();
    critical_section_enter_blocking(&g_mapLock);

    for (int i = 0; i < mappingCount; i++)
    {
        if (mappings[i].row == r && mappings[i].col == c && mappings[i].paramId == pid)
        {
            fillTarget(mappings[i], type, d1, d2);
            critical_section_exit(&g_mapLock);
            return;
        }
    }

    if (mappingCount < 32)
    {
        ModuleMapping &m = mappings[mappingCount++];
        m.row = r;
        m.col = c;
        m.paramId = pid;
        fillTarget(m, type, d1, d2);
    }

    critical_section_exit(&g_mapLock);
}

void MappingManager::updateMappingCurve(int r, int c, uint8_t pid, const Curve &curve)
{
    initLockOnce();
    critical_section_enter_blocking(&g_mapLock);

    for (int i = 0; i < mappingCount; i++)
    {
        if (mappings[i].row == r && mappings[i].col == c && mappings[i].paramId == pid)
        {
            mappings[i].curve = curve;
            critical_section_exit(&g_mapLock);
            return;
        }
    }
    // If not found, we don't create it just for curve.
    // User must create mapping first.
    critical_section_exit(&g_mapLock);
}

bool MappingManager::deleteMapping(int r, int c, uint8_t pid)
{
    initLockOnce();
    critical_section_enter_blocking(&g_mapLock);

    for (int i = 0; i < mappingCount; i++)
    {
        if (mappings[i].row == r && mappings[i].col == c && mappings[i].paramId == pid)
        {
            // swap-remove
            if (i != mappingCount - 1)
            {
                mappings[i] = mappings[mappingCount - 1];
            }
            mappingCount--;
            critical_section_exit(&g_mapLock);
            return true;
        }
    }

    critical_section_exit(&g_mapLock);
    return false;
}

bool MappingManager::hexToCurve(uint8_t *data, Curve *outCurve)
{
    // Format: count(1), points(count*2), controls(count-1*2)
    if (data[0] < 2 || data[0] > 4)
    {
        return false;
    }
    outCurve->count = data[0];
    size_t ptr = 1;
    for (int i = 0; i < outCurve->count; i++)
    {
        outCurve->points[i].x = data[ptr++];
        outCurve->points[i].y = data[ptr++];
    }
    for (int i = 0; i < outCurve->count - 1; i++)
    {
        outCurve->controls[i].x = data[ptr++];
        outCurve->controls[i].y = data[ptr++];
    }
    return true;
}

bool MappingManager::curveToHex(const Curve &curve, uint8_t *outData)
{
    if (curve.count < 2 || curve.count > 4)
    {
        return false;
    }
    outData[0] = curve.count;
    size_t ptr = 1;
    for (int i = 0; i < curve.count; i++)
    {
        outData[ptr++] = curve.points[i].x;
        outData[ptr++] = curve.points[i].y;
    }
    for (int i = 0; i < curve.count - 1; i++)
    {
        outData[ptr++] = curve.controls[i].x;
        outData[ptr++] = curve.controls[i].y;
    }
    return true;
}

ModuleMapping *MappingManager::getAllMappings()
{
    ModuleMapping *out = mappings;
    return out;
}