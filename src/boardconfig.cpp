#include "boardconfig.h"

// Definitions for the UART instances referenced via extern in boardconfig.h
InterruptSerialPIO port1b;
InterruptSerialPIO port1c;
InterruptSerialPIO port2a;
InterruptSerialPIO port2b;
InterruptSerialPIO port2c;
InterruptSerialPIO port3a;
InterruptSerialPIO port3b;
InterruptSerialPIO port3c;

// Module port pointers using the single-copy UART instances above
InterruptSerialPIO *modulePorts[MODULE_PORT_ROWS][MODULE_PORT_COLS] = {
    {nullptr, &port1b, &port1c},
    {&port2a, &port2b, &port2c},
    {&port3a, &port3b, &port3c}};

void initBoardSerial()
{
    ispio_init(&port1b, 26, 27);
    ispio_set_pio_sm(&port1b, pio0, 0);

    ispio_init(&port1c, 28, 29);
    ispio_set_pio_sm(&port1c, pio0, 1);

    ispio_init(&port2a, 20, 21);
    ispio_set_pio_sm(&port2a, pio0, 2);

    ispio_init(&port2b, 22, 23);
    ispio_set_pio_sm(&port2b, pio0, 3);

    ispio_init(&port2c, 24, 25);
    ispio_set_pio_sm(&port2c, pio1, 0);

    ispio_init(&port3a, 12, 13);
    ispio_set_pio_sm(&port3a, pio1, 1);

    ispio_init(&port3b, 16, 17);
    ispio_set_pio_sm(&port3b, pio1, 2);

    ispio_init(&port3c, 18, 19);
    ispio_set_pio_sm(&port3c, pio1, 3);
}
