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

#include "InterruptSerialPIO.h"
#include "CoreMutex.h"
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include "pio_uart.pio.h"
#include <cstring>

static InterruptSerialPIO *g_pioInstances[2][4] = {};
static int rxProgramOffset[2] = {-1, -1};
static bool irqInit[2] = {false, false};
void (*InterruptSerialPIO::_messageSink)(const ModuleMessage &) = nullptr;

static uint8_t calcChecksum(const uint8_t *data, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++)
    {
        sum += data[i];
    }
    return static_cast<uint8_t>(sum & 0xFF);
}

static void __not_in_flash_func(pio_irq_common)(PIO pio)
{
    uint idx = pio_get_index(pio);
    for (int sm = 0; sm < 4; sm++)
    {
        InterruptSerialPIO *inst = g_pioInstances[idx][sm];
        if (inst)
        {
            inst->_handleIRQ();
        }
    }
}

static void __not_in_flash_func(pio0_irq)()
{
    pio_irq_common(pio0);
}

static void __not_in_flash_func(pio1_irq)()
{
    pio_irq_common(pio1);
}

InterruptSerialPIO::InterruptSerialPIO(pin_size_t tx, pin_size_t rx, size_t fifoSize)
{
    _tx = tx;
    _rx = rx;
    _fifoSize = fifoSize + 1; // Always one unused entry
    _queue = new uint8_t[_fifoSize];
    mutex_init(&_mutex);
}

InterruptSerialPIO::~InterruptSerialPIO()
{
    end();
    delete[] _queue;
}

void InterruptSerialPIO::setMessageSink(void (*handler)(const ModuleMessage &))
{
    _messageSink = handler;
}

static bool claim_rx_sm(PIO &outPio, int &outSm, uint &_offset)
{
    // Try pio0 then pio1
    int sm = pio_claim_unused_sm(pio0, false);
    if (sm >= 0)
    {
        outPio = pio0;
        outSm = sm;
        if (rxProgramOffset[0] < 0)
        {
            rxProgramOffset[0] = pio_add_program(pio0, &pio_rx_program);
        }
        _offset = rxProgramOffset[0];
        return true;
    }
    sm = pio_claim_unused_sm(pio1, false);
    if (sm >= 0)
    {
        outPio = pio1;
        outSm = sm;
        if (rxProgramOffset[1] < 0)
        {
            rxProgramOffset[1] = pio_add_program(pio1, &pio_rx_program);
        }
        _offset = rxProgramOffset[1];
        return true;
    }
    return false;
}

void InterruptSerialPIO::begin(unsigned long baud, uint16_t config)
{
    (void)config;
    (void)baud; // fixed
    _writer = 0;
    _reader = 0;
    _overflow = false;
    resetParser();

    if ((_tx == NOPIN) && (_rx == NOPIN))
    {
        return;
    }

    if (_tx != NOPIN)
    {
        pinMode(_tx, OUTPUT);
        digitalWrite(_tx, HIGH); // idle high
    }

    if (_rx != NOPIN)
    {
        if (!claim_rx_sm(_rxPIO, _rxSM, _rxOffset))
        {
            return; // No PIO available
        }

        pio_sm_config c = pio_rx_program_get_default_config(_rxOffset);
        sm_config_set_in_pins(&c, _rx);
        sm_config_set_jmp_pin(&c, _rx);
        sm_config_set_in_shift(&c, true, false, 32); // shift right, no autopush
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

        float div = static_cast<float>(clock_get_hz(clk_sys)) / static_cast<float>(FIXED_BAUD * 8); // 8x oversample
        sm_config_set_clkdiv(&c, div);

        pio_sm_init(_rxPIO, _rxSM, _rxOffset, &c);
        pio_sm_set_consecutive_pindirs(_rxPIO, _rxSM, _rx, 1, false);
        pio_gpio_init(_rxPIO, _rx);
        gpio_pull_up(_rx);
        pio_sm_clear_fifos(_rxPIO, _rxSM);

        // Enable IRQ for RX FIFO not empty
        switch (_rxSM)
        {
        case 0:
            pio_set_irq0_source_enabled(_rxPIO, pis_sm0_rx_fifo_not_empty, true);
            break;
        case 1:
            pio_set_irq0_source_enabled(_rxPIO, pis_sm1_rx_fifo_not_empty, true);
            break;
        case 2:
            pio_set_irq0_source_enabled(_rxPIO, pis_sm2_rx_fifo_not_empty, true);
            break;
        case 3:
            pio_set_irq0_source_enabled(_rxPIO, pis_sm3_rx_fifo_not_empty, true);
            break;
        }

        uint idx = pio_get_index(_rxPIO);
        if (!irqInit[idx])
        {
            if (idx == 0)
            {
                irq_set_exclusive_handler(PIO0_IRQ_0, pio0_irq);
                irq_set_enabled(PIO0_IRQ_0, true);
            }
            else
            {
                irq_set_exclusive_handler(PIO1_IRQ_0, pio1_irq);
                irq_set_enabled(PIO1_IRQ_0, true);
            }
            irqInit[idx] = true;
        }

        pio_sm_set_enabled(_rxPIO, _rxSM, true);
        g_pioInstances[idx][_rxSM] = this;
    }

    _running = true;
}

void InterruptSerialPIO::end()
{
    if (!_running)
    {
        return;
    }
    if (_rx != NOPIN)
    {
        uint idx = pio_get_index(_rxPIO);
        g_pioInstances[idx][_rxSM] = nullptr;
        pio_sm_set_enabled(_rxPIO, _rxSM, false);
        pio_sm_unclaim(_rxPIO, _rxSM);
    }
    _running = false;
}

int InterruptSerialPIO::peek()
{
    CoreMutex m(&_mutex);
    if (!_running || !m || (_rx == NOPIN))
    {
        return -1;
    }
    if (_writer != _reader)
    {
        return _queue[_reader];
    }
    return -1;
}

int InterruptSerialPIO::read()
{
    CoreMutex m(&_mutex);
    if (!_running || !m || (_rx == NOPIN))
    {
        return -1;
    }
    if (_writer != _reader)
    {
        auto ret = _queue[_reader];
        asm volatile("" ::: "memory");
        auto next_reader = (_reader + 1) % _fifoSize;
        asm volatile("" ::: "memory");
        _reader = next_reader;
        return ret;
    }
    return -1;
}

int InterruptSerialPIO::available()
{
    CoreMutex m(&_mutex);
    if (!_running || !m || (_rx == NOPIN))
    {
        return 0;
    }
    return (_fifoSize + _writer - _reader) % _fifoSize;
}

int InterruptSerialPIO::availableForWrite()
{
    // Bit-banged TX: always ready for one byte
    return 1;
}

void InterruptSerialPIO::flush()
{
    // Bit-banged, nothing buffered
}

size_t InterruptSerialPIO::write(uint8_t c)
{
    if (!_running || (_tx == NOPIN))
    {
        return 0;
    }

    uint32_t bitUs = static_cast<uint32_t>((1000000UL + (FIXED_BAUD / 2)) / FIXED_BAUD);
    noInterrupts();
    digitalWrite(_tx, LOW); // start
    delayMicroseconds(bitUs);
    for (int i = 0; i < 8; i++)
    {
        digitalWrite(_tx, (c >> i) & 0x01);
        delayMicroseconds(bitUs);
    }
    digitalWrite(_tx, HIGH); // stop
    delayMicroseconds(bitUs);
    interrupts();

    return 1;
}

void __not_in_flash_func(InterruptSerialPIO::_handleIRQ)()
{
    if (_rx == NOPIN)
    {
        return;
    }
    while (!pio_sm_is_rx_fifo_empty(_rxPIO, _rxSM))
    {
        uint8_t val = static_cast<uint8_t>((pio_sm_get_blocking(_rxPIO, _rxSM) >> 24) & 0xFF);

        auto next_writer = _writer + 1;
        if (next_writer == _fifoSize)
        {
            next_writer = 0;
        }
        if (next_writer != _reader)
        {
            _queue[_writer] = val;
            asm volatile("" ::: "memory");
            _writer = next_writer;
            processByte(val);
        }
        else
        {
            _overflow = true;
        }
    }
}

void InterruptSerialPIO::resetParser()
{
    _parser.length = 0;
    _parser.expectedLength = 0;
    _parser.syncing = false;
}

void InterruptSerialPIO::processByte(uint8_t b)
{
    Parser &p = _parser;
    if (!p.syncing)
    {
        if (b == 0xAA)
        {
            p.buffer[0] = b;
            p.length = 1;
            p.syncing = true;
        }
        return;
    }

    if (p.length >= sizeof(p.buffer))
    {
        resetParser();
        return;
    }

    p.buffer[p.length++] = b;

    if (p.length == 3)
    {
        uint8_t payloadLen = p.buffer[2];
        uint16_t total = 4 + payloadLen;
        if (total > sizeof(p.buffer))
        {
            resetParser();
            return;
        }
        p.expectedLength = total;
    }

    if (p.expectedLength > 0 && p.length == p.expectedLength)
    {
        uint8_t checksum = p.buffer[p.length - 1];
        if (checksum == calcChecksum(p.buffer, p.length - 1))
        {
            emitFrame();
        }
        resetParser();
    }
}

void InterruptSerialPIO::emitFrame()
{
    if (!_messageSink)
    {
        return;
    }

    if (_parser.length < 4)
    {
        return;
    }

    ModuleMessage msg = {};
    msg.moduleRow = _row;
    msg.moduleCol = _col;
    msg.commandId = static_cast<ModuleMessageId>(_parser.buffer[1]);
    msg.payloadLength = _parser.buffer[2];
    if (msg.payloadLength > sizeof(msg.payload))
    {
        msg.payloadLength = sizeof(msg.payload);
    }
    memcpy(msg.payload, &_parser.buffer[3], msg.payloadLength);
    _messageSink(msg);
}
