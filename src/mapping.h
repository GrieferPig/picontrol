#pragma once
#include "module_mapping_config.h"

class MappingManager
{
private:
    static ModuleMapping mappings[32];
    static int mappingCount;

    static void clearMappings();

public:
    // Safe to call multiple times.
    static void init();

    // CRUD
    static void updateMapping(int r, int c, uint8_t pid, ActionType type, uint8_t d1, uint8_t d2);
    static void updateMappingCurve(int r, int c, uint8_t pid, const Curve &curve);
    static bool deleteMapping(int r, int c, uint8_t pid);
    static void clearAll();
    static void clearMappingsForPort(int r, int c);
    static void addMapping(int r, int c, const ModuleMapping &m);
    static const ModuleMapping *findMapping(int r, int c, uint8_t pid);

    // Execution
    static void applyMapping(const Port::State *port, uint8_t pid, ModuleParameterDataType dt, const ModuleParameterValue &cur);

    // Introspection (for config UI)
    static int count();
    static const ModuleMapping *getByIndex(int idx);
    static ModuleMapping *getAllMappings();

    // Utility
    static bool hexToCurve(uint8_t *data, Curve *outCurve);
    static inline bool curveToHex(const Curve &curve, uint8_t *outData);
};