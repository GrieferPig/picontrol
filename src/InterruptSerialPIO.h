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

#include <Arduino.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include "common.hpp"

class InterruptSerialPIO
{
public:
    static constexpr uint32_t FIXED_BAUD = 115200;

    volatile uint32_t lastByteReceivedTime;

    InterruptSerialPIO(pin_size_t tx, pin_size_t rx);
    ~InterruptSerialPIO();

    // Fixed 115200 8N1
    void begin(unsigned long baud = FIXED_BAUD);
    void end();

    // Metadata so parsed frames can include port info
    void setPortLocation(uint8_t row, uint8_t col)
    {
        _row = row;
        _col = col;
    }

    // Change pins
    void setPins(pin_size_t tx, pin_size_t rx)
    {
        _tx = tx;
        _rx = rx;
    }

    // Register a global sink invoked from IRQ when a frame is parsed
    static void setMessageSink(void (*handler)(ModuleMessage &));

    // TX
    size_t write(uint8_t c); // bit-banged TX
    size_t write(const uint8_t *buffer, size_t size);

    // ISR entry
    void _handleIRQ();

private:
    struct Parser
    {
        uint8_t buffer[MODULE_MAX_PAYLOAD + 5];
        uint16_t length = 0;
        uint16_t expectedLength = 0;
        bool syncing = false;
        uint32_t lastByteReceivedTime = 0;
    } _parser;

    static constexpr uint32_t PARSER_TIMEOUT_MS = 50;

    void resetParser();
    void processByte(uint8_t b);
    void emitFrame();

    bool _running = false;
    pin_size_t _tx, _rx;

    PIO _rxPIO;
    int _rxSM;
    uint _rxOffset;

    uint8_t _row = 0;
    uint8_t _col = 0;

    static void (*_messageSink)(ModuleMessage &);
};

#ifdef ARDUINO_NANO_RP2040_CONNECT
// NINA updates
extern InterruptSerialPIO Serial3;
#endif