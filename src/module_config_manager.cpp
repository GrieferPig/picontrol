#include "module_config_manager.h"
#include "boardconfig.h"
#include <LittleFS.h>
#include <Arduino.h>

namespace
{
    static constexpr const char *CONFIG_FILE = "/modconfig.bin";
    static constexpr const char *PARAM_FILE = "/params.bin";
    static constexpr uint32_t MAGIC_CFG1 = 0x43464731u; // 'CFG1'
    static constexpr uint32_t MAGIC_PRM1 = 0x50524D31u; // 'PRM1'

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
    
    struct ParamEntry
    {
        uint8_t r;
        uint8_t c;
        uint8_t pid;
        uint8_t dt;
        ModuleParameterValue val;
    };

    static PortConfig configs[MODULE_PORT_ROWS][MODULE_PORT_COLS];
    static bool g_fsInited = false;
    
    // Simple in-memory cache for params (max 64 entries to save RAM)
    static constexpr int MAX_CACHED_PARAMS = 64;
    static ParamEntry paramCache[MAX_CACHED_PARAMS];
    static int paramCacheCount = 0;

    static void initFsOnce()
    {
        if (g_fsInited)
            return;
        LittleFS.begin();
        g_fsInited = true;
    }
    
    static void loadParams()
    {
        paramCacheCount = 0;
        if (!LittleFS.exists(PARAM_FILE)) return;
        File f = LittleFS.open(PARAM_FILE, "r");
        if (!f) return;
        
        PersistHeader h;
        if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) || h.magic != MAGIC_PRM1) {
            f.close();
            return;
        }
        
        while (f.available() && paramCacheCount < MAX_CACHED_PARAMS) {
            f.read((uint8_t*)&paramCache[paramCacheCount], sizeof(ParamEntry));
            paramCacheCount++;
        }
        f.close();
    }
    
    static void saveParams()
    {
        File f = LittleFS.open(PARAM_FILE, "w");
        if (!f) return;
        
        PersistHeader h = {MAGIC_PRM1, 1, 0};
        f.write((uint8_t*)&h, sizeof(h));
        for(int i=0; i<paramCacheCount; i++) {
            f.write((uint8_t*)&paramCache[i], sizeof(ParamEntry));
        }
        f.close();
    }
}

void ModuleConfigManager::init()
{
    memset(configs, 0, sizeof(configs));
    initFsOnce();
    load();
    loadParams();
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

void ModuleConfigManager::saveParamValue(int r, int c, uint8_t pid, ModuleParameterDataType dt, const ModuleParameterValue& val)
{
    // Update cache
    for(int i=0; i<paramCacheCount; i++) {
        if (paramCache[i].r == r && paramCache[i].c == c && paramCache[i].pid == pid) {
            paramCache[i].dt = (uint8_t)dt;
            paramCache[i].val = val;
            saveParams();
            return;
        }
    }
    
    if (paramCacheCount < MAX_CACHED_PARAMS) {
        paramCache[paramCacheCount].r = r;
        paramCache[paramCacheCount].c = c;
        paramCache[paramCacheCount].pid = pid;
        paramCache[paramCacheCount].dt = (uint8_t)dt;
        paramCache[paramCacheCount].val = val;
        paramCacheCount++;
        saveParams();
    }
}

bool ModuleConfigManager::getParamValue(int r, int c, uint8_t pid, ModuleParameterDataType dt, ModuleParameterValue& outVal)
{
    for(int i=0; i<paramCacheCount; i++) {
        if (paramCache[i].r == r && paramCache[i].c == c && paramCache[i].pid == pid) {
            // Type check? Or just cast?
            // If stored type matches requested type
            if (paramCache[i].dt == (uint8_t)dt) {
                outVal = paramCache[i].val;
                return true;
            }
        }
    }
    return false;
}

void ModuleConfigManager::clearAll()
{
    memset(configs, 0, sizeof(configs));
}
