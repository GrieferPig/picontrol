#include "mapping.h"

#include <Arduino.h>
#include <cstring>

#include <pico/sync.h>

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
}

ModuleMapping MappingManager::mappings[32];
int MappingManager::mappingCount = 0;

void MappingManager::init()
{
    initLockOnce();
}

bool MappingManager::load()
{
    // No-op: Mappings are loaded from modules
    return true;
}

bool MappingManager::save()
{
    // No-op: Mappings are saved to modules
    return true;
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

void MappingManager::updatePersistentStorage()
{
    // Kept for compatibility with older header plan; explicit save() is preferred.
    (void)save();
}
