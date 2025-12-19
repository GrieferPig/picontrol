#pragma once
#include "Arduino.h"
#include "InterruptSerialPIO.h"

// Module port configuration
#define MODULE_PORT_ROWS 3
#define MODULE_PORT_COLS 3

// GPIO pin map for each port (tx, rx). Use 0xFF to mark a missing slot.
static constexpr uint8_t PORT_PIN_UNUSED = 0xFF;

static constexpr uint8_t portTxPins[MODULE_PORT_ROWS][MODULE_PORT_COLS] = {
    {PORT_PIN_UNUSED, 26, 28},
    {20, 22, 24},
    {12, 16, 18}};

static constexpr uint8_t portRxPins[MODULE_PORT_ROWS][MODULE_PORT_COLS] = {
    {PORT_PIN_UNUSED, 27, 29},
    {21, 23, 25},
    {13, 17, 19}};

// Instantiate UARTs for all populated ports in a single translation unit.
// Declarations here; definitions live in boardconfig.cpp
extern InterruptSerialPIO port1b;
extern InterruptSerialPIO port1c;
extern InterruptSerialPIO port2a;
extern InterruptSerialPIO port2b;
extern InterruptSerialPIO port2c;
extern InterruptSerialPIO port3a;
extern InterruptSerialPIO port3b;
extern InterruptSerialPIO port3c;

extern InterruptSerialPIO *modulePorts[MODULE_PORT_ROWS][MODULE_PORT_COLS];
