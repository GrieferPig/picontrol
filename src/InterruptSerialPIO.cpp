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
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include "pio_uart.pio.h"
#include <cstring>

static InterruptSerialPIO *g_pioInstances[2][4] = {};
static int rxProgramOffset[2] = {-1, -1};
static bool irqInit[2] = {false, false};
void (*InterruptSerialPIO::_messageSink)(ModuleMessage &) = nullptr;

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

InterruptSerialPIO::InterruptSerialPIO(pin_size_t tx, pin_size_t rx)
{
    _tx = tx;
    _rx = rx;
    lastByteReceivedTime = 0;
}

InterruptSerialPIO::~InterruptSerialPIO()
{
    end();
}

void InterruptSerialPIO::setMessageSink(void (*handler)(ModuleMessage &))
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

void InterruptSerialPIO::begin(unsigned long baud)
{
    (void)baud; // fixed
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
        // Bias RX low so a disconnected/floating module doesn't appear as UART-idle HIGH.
        // The module's TX should actively drive HIGH when present/idle.
        gpio_pull_down(_rx);
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

// Host TX are small command payloads (~10 bytes) so bit-banging is acceptable
size_t __not_in_flash_func(InterruptSerialPIO::write)(uint8_t c)
{
    if (!_running || (_tx == NOPIN))
        return 0;

    // Calculate cycles per bit
    const uint32_t bitCycles = clock_get_hz(clk_sys) / FIXED_BAUD;

    // Pre-calculate masks
    uint32_t pinMask = 1ul << _tx;

    noInterrupts();

    // Start Bit (Low)
    sio_hw->gpio_clr = pinMask;

    // START BIT
    sio_hw->gpio_clr = pinMask;
    busy_wait_at_least_cycles(bitCycles);

    // DATA BITS
    for (int i = 0; i < 8; i++)
    {
        if (c & (1 << i))
            sio_hw->gpio_set = pinMask;
        else
            sio_hw->gpio_clr = pinMask;
        busy_wait_at_least_cycles(bitCycles);
    }

    // STOP BIT (High)
    sio_hw->gpio_set = pinMask;
    busy_wait_at_least_cycles(bitCycles);

    interrupts();
    return 1;
}

size_t InterruptSerialPIO::write(const uint8_t *buffer, size_t size)
{
    if (!buffer || size == 0)
    {
        return 0;
    }
    size_t written = 0;
    for (size_t i = 0; i < size; i++)
    {
        written += write(buffer[i]);
    }
    return written;
}

void __not_in_flash_func(InterruptSerialPIO::_handleIRQ)()
{
    if (_rx == NOPIN)
    {
        return;
    }
    while (!pio_sm_is_rx_fifo_empty(_rxPIO, _rxSM))
    {
        uint32_t now = millis();
        if (_parser.syncing && _parser.lastByteReceivedTime && (now - _parser.lastByteReceivedTime > PARSER_TIMEOUT_MS))
        {
            resetParser();
        }
        uint8_t val = static_cast<uint8_t>((pio_sm_get_blocking(_rxPIO, _rxSM) >> 24) & 0xFF);

        // Protocol parsing is the only RX consumer.
        processByte(val);
    }
}

void InterruptSerialPIO::resetParser()
{
    _parser.length = 0;
    _parser.expectedLength = 0;
    _parser.syncing = false;
    _parser.lastByteReceivedTime = 0;
    lastByteReceivedTime = 0;
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
    uint32_t now = millis();
    p.lastByteReceivedTime = now;
    lastByteReceivedTime = now;

    if (p.length == 4)
    {
        uint16_t payloadLen = static_cast<uint16_t>(p.buffer[2]) | (static_cast<uint16_t>(p.buffer[3]) << 8);
        if (payloadLen > MODULE_MAX_PAYLOAD)
        {
            resetParser();
            return;
        }
        uint32_t total = 5u + payloadLen; // 4-byte header + payload + checksum
        if (total > sizeof(p.buffer))
        {
            resetParser();
            return;
        }
        p.expectedLength = static_cast<uint16_t>(total);
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

extern ModuleMessage *allocateMessageFromIRQ();
extern void commitMessageFromIRQ();

void InterruptSerialPIO::emitFrame()
{
    if (_parser.length < 4)
    {
        return;
    }

    ModuleMessage *slot = allocateMessageFromIRQ();
    if (!slot)
    {
        return;
    }

    slot->moduleRow = _row;
    slot->moduleCol = _col;
    slot->commandId = static_cast<ModuleMessageId>(_parser.buffer[1]);
    slot->payloadLength = static_cast<uint16_t>(_parser.buffer[2]) | (static_cast<uint16_t>(_parser.buffer[3]) << 8);
    if (slot->payloadLength > sizeof(slot->payload))
    {
        slot->payloadLength = sizeof(slot->payload);
    }
    memcpy(slot->payload, &_parser.buffer[4], slot->payloadLength);

    commitMessageFromIRQ();

    if (_messageSink)
    {
        _messageSink(*slot);
    }
}
