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

// Instantiate UARTs for all populated ports. Missing slots remain nullptr.
static InterruptSerialPIO port1b(26, 27);
static InterruptSerialPIO port1c(28, 29);
static InterruptSerialPIO port2a(20, 21);
static InterruptSerialPIO port2b(22, 23);
static InterruptSerialPIO port2c(24, 25);
static InterruptSerialPIO port3a(12, 13);
static InterruptSerialPIO port3b(16, 17);
static InterruptSerialPIO port3c(18, 19);

static InterruptSerialPIO *modulePorts[MODULE_PORT_ROWS][MODULE_PORT_COLS] = {
    {nullptr, &port1b, &port1c},
    {&port2a, &port2b, &port2c},
    {&port3a, &port3b, &port3c}};
