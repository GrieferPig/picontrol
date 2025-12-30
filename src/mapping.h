#pragma once
#include "module_mapping_config.h"

class MappingManager
{
private:
    static ModuleMapping mappings[32];
    static int mappingCount;

    static void clearMappings();
    static void updatePersistentStorage();

public:
    // Safe to call multiple times.
    static void init();

    // Persistence
    static bool load();
    static bool save();

    // CRUD
    static void updateMapping(int r, int c, uint8_t pid, ActionType type, uint8_t d1, uint8_t d2);
    static void updateMappingCurve(int r, int c, uint8_t pid, const Curve &curve);
    static bool deleteMapping(int r, int c, uint8_t pid);
    static void clearAll();
    static void clearMappingsForPort(int r, int c);
    static void addMapping(int r, int c, const ModuleMapping &m);
    static const ModuleMapping *findMapping(int r, int c, uint8_t pid);

    // Introspection (for config UI)
    static int count();
    static const ModuleMapping *getByIndex(int idx);
};