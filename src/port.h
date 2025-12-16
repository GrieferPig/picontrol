#pragma once

#include "common.hpp"
#include "boardconfig.h"

Port *getPort(int row, int col);

void initPorts();
void scanPorts();

bool sendMessage(int row, int col, ModuleMessageId commandId, const uint8_t *payload, uint8_t payloadLen);
bool getNextMessage(ModuleMessage &out);

// Typed helpers
bool sendPing(int row, int col);
bool sendGetProperties(int row, int col, uint8_t requestId = 0);
bool sendSetParameter(int row, int col, uint8_t parameterId, ModuleParameterDataType dataType, ModuleParameterValue value);
bool sendGetParameter(int row, int col, uint8_t parameterId);
bool sendResetModule(int row, int col);
bool sendResponse(int row, int col, ModuleMessageId inResponseTo, ModuleStatus status, const uint8_t *payload, uint8_t payloadLen);
