#pragma once
#include <stdint.h>
#include "common.hpp"

class ModuleConfigManager
{
public:
    static void init();
    static bool load();
    static bool save();

    static void setRotation(int r, int c, bool rotated180);
    static bool isRotated180(int r, int c);
    static void clearAll();

    // Parameter persistence
    static void saveParamValue(int r, int c, uint8_t pid, ModuleParameterDataType dt, const ModuleParameterValue& val);
    static bool getParamValue(int r, int c, uint8_t pid, ModuleParameterDataType dt, ModuleParameterValue& outVal);
};
