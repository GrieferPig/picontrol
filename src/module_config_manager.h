#pragma once
#include <stdint.h>

class ModuleConfigManager
{
public:
    static void init();
    static bool load();
    static bool save();

    static void setRotation(int r, int c, bool rotated180);
    static bool isRotated180(int r, int c);
    static void clearAll();
};
