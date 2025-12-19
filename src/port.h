#pragma once

#include "common.hpp"
#include "boardconfig.h"

Port *getPort(int row, int col);

// ISR-facing queue helpers (do not use outside interrupt context)
ModuleMessage *allocateMessageFromIRQ();
void commitMessageFromIRQ();

void initPorts();
void scanPorts();

bool sendMessage(int row, int col, ModuleMessageId commandId, const uint8_t *payload, uint16_t payloadLen);
bool getNextMessage(ModuleMessage &out);

// Typed helpers
bool sendPing(int row, int col);
bool sendGetProperties(int row, int col, uint8_t requestId = 0);
bool sendSetParameter(int row, int col, uint8_t parameterId, ModuleParameterDataType dataType, ModuleParameterValue value);
bool sendGetParameter(int row, int col, uint8_t parameterId);
bool sendResetModule(int row, int col);
bool sendSetAutoupdate(int row, int col, bool enable, uint16_t intervalMs = 0);
bool sendResponse(int row, int col, ModuleMessageId inResponseTo, ModuleStatus status, const uint8_t *payload, uint16_t payloadLen);
