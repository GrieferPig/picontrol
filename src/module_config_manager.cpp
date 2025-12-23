#include "module_config_manager.h"
#include "boardconfig.h"
#include <LittleFS.h>
#include <Arduino.h>

namespace
{
    static constexpr const char *CONFIG_FILE = "/modconfig.bin";
    static constexpr uint32_t MAGIC_CFG1 = 0x43464731u; // 'CFG1'

    struct PersistHeader
    {
        uint32_t magic;
        uint16_t version;
        uint16_t reserved;
    };

    struct PortConfig
    {
        uint8_t rotated180; // 0 or 1
        uint8_t reserved[3];
    };

    static PortConfig configs[MODULE_PORT_ROWS][MODULE_PORT_COLS];
    static bool g_fsInited = false;

    static void initFsOnce()
    {
        if (g_fsInited)
            return;
        LittleFS.begin();
        g_fsInited = true;
    }
}

void ModuleConfigManager::init()
{
    memset(configs, 0, sizeof(configs));
    initFsOnce();
    load();
}

bool ModuleConfigManager::load()
{
    initFsOnce();
    if (!LittleFS.exists(CONFIG_FILE))
        return false;
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f)
        return false;

    PersistHeader h;
    if (f.read((uint8_t *)&h, sizeof(h)) != sizeof(h))
    {
        f.close();
        return false;
    }
    if (h.magic != MAGIC_CFG1)
    {
        f.close();
        return false;
    }

    f.read((uint8_t *)configs, sizeof(configs));
    f.close();
    return true;
}

bool ModuleConfigManager::save()
{
    initFsOnce();
    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f)
        return false;

    PersistHeader h = {MAGIC_CFG1, 1, 0};
    f.write((uint8_t *)&h, sizeof(h));
    f.write((uint8_t *)configs, sizeof(configs));
    f.close();
    return true;
}

void ModuleConfigManager::setRotation(int r, int c, bool rotated180)
{
    if (r >= 0 && r < MODULE_PORT_ROWS && c >= 0 && c < MODULE_PORT_COLS)
    {
        configs[r][c].rotated180 = rotated180 ? 1 : 0;
    }
}

bool ModuleConfigManager::isRotated180(int r, int c)
{
    if (r >= 0 && r < MODULE_PORT_ROWS && c >= 0 && c < MODULE_PORT_COLS)
    {
        return configs[r][c].rotated180 != 0;
    }
    return false;
}

void ModuleConfigManager::clearAll()
{
    memset(configs, 0, sizeof(configs));
}
