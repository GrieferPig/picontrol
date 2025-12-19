#include "boardconfig.h"

// Definitions for the UART instances referenced via extern in boardconfig.h
InterruptSerialPIO port1b(26, 27);
InterruptSerialPIO port1c(28, 29);
InterruptSerialPIO port2a(20, 21);
InterruptSerialPIO port2b(22, 23);
InterruptSerialPIO port2c(24, 25);
InterruptSerialPIO port3a(12, 13);
InterruptSerialPIO port3b(16, 17);
InterruptSerialPIO port3c(18, 19);

// Module port pointers using the single-copy UART instances above
InterruptSerialPIO *modulePorts[MODULE_PORT_ROWS][MODULE_PORT_COLS] = {
    {nullptr, &port1b, &port1c},
    {&port2a, &port2b, &port2c},
    {&port3a, &port3b, &port3c}};
