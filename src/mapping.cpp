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
        c.h = 16384; // Linear (0.5 in Q15)
    }

    static void fillTarget(ModuleMapping &m, ActionType type, uint8_t d1, uint8_t d2)
    {
        m.type = type;
        // Initialize curve to default linear if h=0 (uninitialized)
        if (m.curve.h == 0)
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

    static uint16_t normalizeToU10(const Port::State *port, uint8_t pid, ModuleParameterDataType dt, const ModuleParameterValue &v)
    {
        if (!port || !port->hasModule || pid >= port->module.parameterCount)
            return 0;
        const ModuleParameter &p = port->module.parameters[pid];

        if (dt == ModuleParameterDataType::PARAM_TYPE_BOOL)
        {
            return v.boolValue ? 1023 : 0;
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

            // (val - min) * 1023 / (max - min)
            return (uint16_t)(((int64_t)(val - mn) * 1023) / (mx - mn));
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

            return (uint16_t)((val - mn) * 1023.0f / (mx - mn));
        }
        return 0;
    }

    static void releaseMappingAction(const ModuleMapping &m)
    {
        switch (m.type)
        {
        case ACTION_KEYBOARD:
            usb::sendKeyUp(m.target.keyboard.keycode);
            break;
        case ACTION_MIDI_CC:
            usb::sendMidiCC(m.target.midiCC.channel - 1,
                            m.target.midiCC.ccNumber,
                            0);
            break;
        case ACTION_MIDI_MOD_WHEEL:
            usb::sendMidiCC14(m.target.midiCC.channel - 1,
                              1, // CC1 (Mod wheel)
                              0);
            break;
        case ACTION_MIDI_PITCH_BEND:
            usb::sendMidiPitchBend(m.target.midiCC.channel - 1,
                                   8192); // center
            break;
        case ACTION_MIDI_NOTE:
            usb::sendMidiNoteOff(m.target.midiNote.channel - 1,
                                 m.target.midiNote.noteNumber,
                                 0);
            break;
        default:
            break;
        }
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
            releaseMappingAction(mappings[i]);
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
            releaseMappingAction(mappings[i]);
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

    if (m->type == ACTION_MIDI_PITCH_BEND)
    {
        uint16_t rawCur10 = normalizeToU10(port, pid, dt, cur);
        uint16_t mapCur10 = CurveEvaluator::eval10(m->curve, rawCur10);

        auto u10ToPitchBendSigned = [](uint16_t v) -> int16_t
        {
            // Map 0..1023 into -8192..8191, forcing v=512 => 0 exactly.
            if (v <= 512)
            {
                const int32_t d = (int32_t)512 - (int32_t)v; // 0..512
                const int32_t neg = -(d * 8192) / 512;
                return (int16_t)neg;
            }
            else
            {
                const int32_t d = (int32_t)v - (int32_t)512; // 1..511
                const int32_t pos = (d * 8191) / 511;
                return (int16_t)pos;
            }
        };

        uint8_t ch = m->target.midiCC.channel;
        if (ch > 0)
            ch -= 1;

        const int16_t pb = u10ToPitchBendSigned(mapCur10);
        const uint16_t pb14 = (uint16_t)((int32_t)pb + 8192);

        usb::sendMidiPitchBend(ch, pb14);
        return;
    }

    // Normalize inputs to 0-255
    uint8_t rawCur = normalizeToU8(port, pid, dt, cur);
    // Apply Curve
    uint8_t mapCur = CurveEvaluator::eval(m->curve, rawCur);
    printf("Curve: h=%d, input: %d\n", m->curve.h, rawCur);
    printf("Eval result: %d\n", mapCur);

    // Boolean logic based on 50% threshold of MAPPED value
    const bool curBool = (mapCur >= 128);

    auto u8ToU14 = [](uint8_t v) -> uint16_t
    {
        // Scale 0..255 to 0..16383
        return (uint16_t)(((uint32_t)v * 16383u + 127u) / 255u);
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
            releaseMappingAction(mappings[i]);
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
            releaseMappingAction(mappings[i]);
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
    // Format: h (int16_t little-endian, 2 bytes)
    outCurve->h = static_cast<int16_t>(data[0] | (data[1] << 8));
    return true;
}

bool MappingManager::curveToHex(const Curve &curve, uint8_t *outData)
{
    // Format: h (int16_t little-endian, 2 bytes)
    outData[0] = static_cast<uint8_t>(curve.h & 0xFF);
    outData[1] = static_cast<uint8_t>((curve.h >> 8) & 0xFF);
    return true;
}

ModuleMapping *MappingManager::getAllMappings()
{
    ModuleMapping *out = mappings;
    return out;
}