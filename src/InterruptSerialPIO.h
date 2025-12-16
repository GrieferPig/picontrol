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
#include "api/HardwareSerial.h"
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include "CoreMutex.h"
#include "common.hpp"

class InterruptSerialPIO : public arduino::HardwareSerial
{
public:
    static constexpr uint32_t FIXED_BAUD = 460800;

    InterruptSerialPIO(pin_size_t tx, pin_size_t rx, size_t fifoSize = 64);
    ~InterruptSerialPIO();

    // Fixed 460800 8N1
    void begin(unsigned long baud = FIXED_BAUD) override { begin(baud, SERIAL_8N1); }
    void begin(unsigned long baud, uint16_t config) override;
    void end() override;

    // Metadata so parsed frames can include port info
    void setPortLocation(uint8_t row, uint8_t col)
    {
        _row = row;
        _col = col;
    }

    // Register a global sink invoked from IRQ when a frame is parsed
    static void setMessageSink(void (*handler)(const ModuleMessage &));

    // Minimal HardwareSerial surface
    int peek() override;
    int read() override;
    int available() override;
    int availableForWrite() override;
    void flush() override;
    size_t write(uint8_t c) override; // bit-banged TX
    using Print::write;
    operator bool() override { return _running; }

    // ISR entry
    void _handleIRQ();

private:
    struct Parser
    {
        uint8_t buffer[260];
        uint8_t length = 0;
        uint8_t expectedLength = 0;
        bool syncing = false;
    } _parser;

    void resetParser();
    void processByte(uint8_t b);
    void emitFrame();

    bool _running = false;
    pin_size_t _tx, _rx;
    mutex_t _mutex;
    bool _overflow;

    PIO _rxPIO;
    int _rxSM;
    uint _rxOffset;

    size_t _fifoSize;
    uint32_t _writer;
    uint32_t _reader;
    uint8_t *_queue;

    uint8_t _row = 0;
    uint8_t _col = 0;

    static void (*_messageSink)(const ModuleMessage &);
};

#ifdef ARDUINO_NANO_RP2040_CONNECT
// NINA updates
extern InterruptSerialPIO Serial3;
#endif