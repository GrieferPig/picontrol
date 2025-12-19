#include "mapping.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <cstring>

#include <pico/sync.h>

namespace
{
    static constexpr const char *MAPPING_FILE = "/mappings.bin";
    static constexpr uint32_t MAGIC_MAP1 = 0x3150414Du; // 'MAP1' little-endian

    static critical_section_t g_mapLock;
    static bool g_lockInited = false;
    static bool g_fsInited = false;

    static void initLockOnce()
    {
        if (g_lockInited)
            return;
        critical_section_init(&g_mapLock);
        g_lockInited = true;
    }

    static void initFsOnce()
    {
        if (g_fsInited)
            return;
        // LittleFS.begin() is safe to call multiple times on this core,
        // but we guard anyway.
        LittleFS.begin();
        g_fsInited = true;
    }

#pragma pack(push, 1)
    struct PersistHeaderV1
    {
        uint32_t magic;
        uint16_t version;
        uint16_t count;
        uint32_t checksum;
    };

    struct PersistMappingV1
    {
        int8_t row;
        int8_t col;
        uint8_t pid;
        uint8_t type;
        uint8_t d1;
        uint8_t d2;
    };
#pragma pack(pop)

    static uint32_t checksum32(const uint8_t *data, size_t len)
    {
        uint32_t sum = 0;
        for (size_t i = 0; i < len; i++)
        {
            sum = (sum * 33u) ^ data[i];
        }
        return sum;
    }

    static void fillTarget(ModuleMapping &m, ActionType type, uint8_t d1, uint8_t d2)
    {
        m.type = type;
        // Defaults
        m.trigger.boolean = TRIGGER_ON_CHANGE;

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

bool MappingManager::save()
{
    initLockOnce();
    initFsOnce();

    // Snapshot under lock.
    PersistMappingV1 snapshot[32];
    uint16_t countLocal = 0;

    critical_section_enter_blocking(&g_mapLock);
    countLocal = (uint16_t)mappingCount;
    if (countLocal > 32)
        countLocal = 32;

    for (uint16_t i = 0; i < countLocal; i++)
    {
        const ModuleMapping &m = mappings[i];
        snapshot[i].row = (int8_t)m.row;
        snapshot[i].col = (int8_t)m.col;
        snapshot[i].pid = m.paramId;
        snapshot[i].type = (uint8_t)m.type;

        // Extract d1/d2 based on type.
        uint8_t d1 = 0, d2 = 0;
        switch (m.type)
        {
        case ACTION_MIDI_NOTE:
            d1 = m.target.midiNote.channel;
            d2 = m.target.midiNote.noteNumber;
            break;
        case ACTION_MIDI_CC:
            d1 = m.target.midiCC.channel;
            d2 = m.target.midiCC.ccNumber;
            break;
        case ACTION_KEYBOARD:
            d1 = m.target.keyboard.keycode;
            d2 = m.target.keyboard.modifier;
            break;
        default:
            break;
        }
        snapshot[i].d1 = d1;
        snapshot[i].d2 = d2;
    }
    critical_section_exit(&g_mapLock);

    PersistHeaderV1 hdr{};
    hdr.magic = MAGIC_MAP1;
    hdr.version = 1;
    hdr.count = countLocal;
    hdr.checksum = checksum32(reinterpret_cast<const uint8_t *>(snapshot), sizeof(PersistMappingV1) * countLocal);

    const char *tmp = "/mappings.tmp";
    File f = LittleFS.open(tmp, "w");
    if (!f)
        return false;

    if (f.write(reinterpret_cast<const uint8_t *>(&hdr), sizeof(hdr)) != sizeof(hdr))
    {
        f.close();
        LittleFS.remove(tmp);
        return false;
    }

    size_t bodyLen = sizeof(PersistMappingV1) * countLocal;
    if (bodyLen && f.write(reinterpret_cast<const uint8_t *>(snapshot), bodyLen) != bodyLen)
    {
        f.close();
        LittleFS.remove(tmp);
        return false;
    }
    f.close();

    LittleFS.remove(MAPPING_FILE);
    return LittleFS.rename(tmp, MAPPING_FILE);
}

bool MappingManager::load()
{
    initLockOnce();
    initFsOnce();

    if (!LittleFS.exists(MAPPING_FILE))
    {
        clearMappings();
        return true;
    }

    File f = LittleFS.open(MAPPING_FILE, "r");
    if (!f)
        return false;

    PersistHeaderV1 hdr{};
    if (f.read(reinterpret_cast<uint8_t *>(&hdr), sizeof(hdr)) != sizeof(hdr))
    {
        f.close();
        return false;
    }

    if (hdr.magic != MAGIC_MAP1 || hdr.version != 1 || hdr.count > 32)
    {
        f.close();
        return false;
    }

    PersistMappingV1 buf[32];
    const size_t bodyLen = sizeof(PersistMappingV1) * hdr.count;
    if (bodyLen && f.read(reinterpret_cast<uint8_t *>(buf), bodyLen) != bodyLen)
    {
        f.close();
        return false;
    }
    f.close();

    const uint32_t actual = checksum32(reinterpret_cast<const uint8_t *>(buf), bodyLen);
    if (actual != hdr.checksum)
    {
        return false;
    }

    clearMappings();

    critical_section_enter_blocking(&g_mapLock);
    mappingCount = hdr.count;
    for (uint16_t i = 0; i < hdr.count; i++)
    {
        ModuleMapping &m = mappings[i];
        m.row = buf[i].row;
        m.col = buf[i].col;
        m.paramId = buf[i].pid;
        fillTarget(m, (ActionType)buf[i].type, buf[i].d1, buf[i].d2);
    }
    critical_section_exit(&g_mapLock);

    return true;
}

void MappingManager::updatePersistentStorage()
{
    // Kept for compatibility with older header plan; explicit save() is preferred.
    (void)save();
}
