#pragma once
#include <stdint.h>
#include "common.hpp"

enum ActionType : uint8_t
{
    ACTION_NONE = 0,
    ACTION_MIDI_NOTE,
    ACTION_MIDI_CC,
    ACTION_KEYBOARD
};

enum ActionTriggerBoolean : uint8_t
{
    TRIGGER_ON_TRUE = 0,
    TRIGGER_ON_FALSE,
    TRIGGER_ON_CHANGE
};

enum ActionTriggerAnalog : uint8_t
{
    TRIGGER_NORMAL = 0,
    TRIGGER_INVERTED
};

union ActionTrigger
{
    ActionTriggerBoolean boolean;
    ActionTriggerAnalog analog;
};

struct ActionTargetMidiNote
{
    uint8_t channel; // 1-16
    uint8_t noteNumber;
    uint8_t velocity;
};

struct ActionTargetMidiCC
{
    uint8_t channel; // 1-16
    uint8_t ccNumber;
    uint8_t value;
};

struct ActionTargetKeyboard
{
    uint8_t keycode;
    uint8_t modifier; // bitmask
};

union ActionTarget
{
    ActionTargetMidiNote midiNote;
    ActionTargetMidiCC midiCC;
    ActionTargetKeyboard keyboard;
};

struct ModuleMapping
{
    // Key
    int row = -1;
    int col = -1;
    uint8_t paramId = 0;

    // Value
    ActionType type = ACTION_NONE;
    ActionTrigger trigger;
    ActionTarget target;
};