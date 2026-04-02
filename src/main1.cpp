#include <Arduino.h>
#include "port.h"
#include "ipc.hpp"
#include "mapping.h"
#include "debug_printf.h"

void setup1()
{
    // Core 1 setup
    dbg_printf("Picontrol: core1 started\n");
    Port::init();
    dbg_printf("Ports initialized\n");
}
void loop1()
{
    // Core 1 loop
    Port::task();

    // Check for IPC messages

    IPC::SyncMappingRequest smreq;
    while (IPC::tryDequeueSyncMapping(smreq))
    {
        auto syncOne = [&](int row, int col)
        {
            Port::State *p = Port::get(row, col);
            if (!p || !p->configured || !p->hasModule)
                return;

            ModuleMessageSetMappingsPayload payload{};
            payload.count = 0;

            int total = MappingManager::count();
            for (int i = 0; i < total; i++)
            {
                const ModuleMapping *m = MappingManager::getByIndex(i);
                if (m && m->row == row && m->col == col)
                {
                    if (payload.count < 8)
                    {
                        WireModuleMapping &wm = payload.mappings[payload.count];
                        wm.paramId = m->paramId;
                        wm.type = (uint8_t)m->type;

                        // Curve: convert host Curve to wire format
                        wm.curve = curveToWireCurve(m->curve);

                        // Target
                        if (m->type == ACTION_MIDI_NOTE)
                        {
                            wm.target.midiNote.channel = m->target.midiNote.channel;
                            wm.target.midiNote.noteNumber = m->target.midiNote.noteNumber;
                            wm.target.midiNote.velocity = m->target.midiNote.velocity;
                        }
                        else if (m->type == ACTION_MIDI_CC)
                        {
                            wm.target.midiCC.channel = m->target.midiCC.channel;
                            wm.target.midiCC.ccNumber = m->target.midiCC.ccNumber;
                            wm.target.midiCC.value = m->target.midiCC.value;
                        }
                        else if (m->type == ACTION_MIDI_PITCH_BEND)
                        {
                            wm.target.midiCC.channel = m->target.midiCC.channel;
                            wm.target.midiCC.ccNumber = 0;
                            wm.target.midiCC.value = 0;
                        }
                        else if (m->type == ACTION_MIDI_MOD_WHEEL)
                        {
                            wm.target.midiCC.channel = m->target.midiCC.channel;
                            wm.target.midiCC.ccNumber = 1;
                            wm.target.midiCC.value = 0;
                        }
                        else if (m->type == ACTION_KEYBOARD)
                        {
                            wm.target.keyboard.keycode = m->target.keyboard.keycode;
                            wm.target.keyboard.modifier = m->target.keyboard.modifier;
                        }

                        payload.count++;
                    }
                }
            }

            Port::sendSetMappings(row, col, payload);
        };

        if (smreq.applyToAll)
        {
            for (int rr = 0; rr < MODULE_PORT_ROWS; rr++)
            {
                for (int cc = 0; cc < MODULE_PORT_COLS; cc++)
                {
                    syncOne(rr, cc);
                }
            }
        }
        else
        {
            syncOne(smreq.row, smreq.col);
        }
    }

    // Handle set parameter requests from core0
    IPC::SetParameterRequest spreq;
    while (IPC::tryDequeueSetParameter(spreq))
    {
        Port::State *p = Port::get(spreq.row, spreq.col);
        if (p && p->configured && p->hasModule)
        {
            ModuleParameterDataType dt = static_cast<ModuleParameterDataType>(spreq.dataType);
            ModuleParameterValue val{};

            switch (dt)
            {
            case ModuleParameterDataType::PARAM_TYPE_INT:
                val.intValue = static_cast<int32_t>(strtol(spreq.valueStr, nullptr, 10));
                break;
            case ModuleParameterDataType::PARAM_TYPE_FLOAT:
                val.floatValue = strtof(spreq.valueStr, nullptr);
                break;
            case ModuleParameterDataType::PARAM_TYPE_BOOL:
                val.boolValue = (spreq.valueStr[0] == '1' || spreq.valueStr[0] == 't' || spreq.valueStr[0] == 'T') ? 1 : 0;
                break;
            case ModuleParameterDataType::PARAM_TYPE_LED:
            {
                // Parse LED value: "r,g,b,status" or similar
                // For simplicity, expect comma-separated values
                int r = 0, g = 0, b = 0, s = 0;
                if (sscanf(spreq.valueStr, "%d,%d,%d,%d", &r, &g, &b, &s) >= 3)
                {
                    val.ledValue.r = static_cast<uint8_t>(r);
                    val.ledValue.g = static_cast<uint8_t>(g);
                    val.ledValue.b = static_cast<uint8_t>(b);
                    val.ledValue.status = static_cast<uint8_t>(s);
                }
                break;
            }
            default:
                continue;
            }

            Port::sendSetParameter(spreq.row, spreq.col, spreq.paramId, dt, val);
        }
    }

    // Handle set calibration requests from core0
    IPC::SetCalibRequest screq;
    while (IPC::tryDequeueSetCalib(screq))
    {
        Port::State *p = Port::get(screq.row, screq.col);
        if (p && p->configured && p->hasModule)
        {
            ModuleMessageSetCalibPayload payload{};
            payload.parameterId = screq.paramId;
            payload.minValue = screq.minValue;
            payload.maxValue = screq.maxValue;

            Port::sendMessage(screq.row, screq.col, ModuleMessageId::CMD_SET_CALIB,
                              (const uint8_t *)&payload, sizeof(payload));
        }
    }
}