/*
    Serial-over-PIO for the Raspberry Pi Pico RP2040

    Copyright (c) 2021 Earle F. Philhower, III <earlephilhower@yahoo.com>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hardware/pio.h"
#include "common.hpp"

#ifdef __cplusplus
extern "C"
{
#endif

#define ISPIO_FIXED_BAUD 115200

    typedef struct
    {
        uint8_t buffer[MODULE_MAX_PAYLOAD + 5];
        uint16_t length;
        uint16_t expectedLength;
        bool syncing;
        uint32_t lastByteReceivedTime;
    } SerialParser;

    typedef struct InterruptSerialPIO
    {
        volatile uint32_t lastByteReceivedTime;
        bool running;
        uint tx;
        uint rx;
        uint32_t bitCycles;
        PIO rxPIO;
        int rxSM;
        uint rxOffset;
        uint8_t row;
        uint8_t col;
        bool staticSM;
        SerialParser parser;
    } InterruptSerialPIO;

    void ispio_init(InterruptSerialPIO *self, uint tx, uint rx);
    void ispio_deinit(InterruptSerialPIO *self);
    void ispio_begin(InterruptSerialPIO *self, unsigned long baud);
    void ispio_end(InterruptSerialPIO *self);
    void ispio_set_port_location(InterruptSerialPIO *self, uint8_t row, uint8_t col);
    void ispio_set_pins(InterruptSerialPIO *self, uint tx, uint rx);
    void ispio_set_pio_sm(InterruptSerialPIO *self, PIO pio, int sm);
    void ispio_set_message_sink(void (*handler)(ModuleMessage *));
    size_t ispio_write(InterruptSerialPIO *self, uint8_t c);
    size_t ispio_write_buffer(InterruptSerialPIO *self, const uint8_t *buffer, size_t size);
    void ispio_handle_irq(InterruptSerialPIO *self);

#ifdef __cplusplus
}
#endif
