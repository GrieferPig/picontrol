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
#include <hardware/clocks.h>
#include <hardware/sync.h>
#include <hardware/timer.h>
#include "pico/time.h"
#include "pio_uart.pio.h"
#include "hardware/structs/systick.h"
#include <cstring>

// Global state for IRQ dispatch
static InterruptSerialPIO *g_pioInstances[2][4] = {};
static int rxProgramOffset[2] = {-1, -1};
static bool irqInit[2] = {false, false};
static void (*g_messageSink)(ModuleMessage *) = NULL;

static const uint32_t PARSER_TIMEOUT_MS = 50;
static const uint NOPIN = 0xFFFFFFFF;

static uint8_t __not_in_flash_func(calcChecksum)(const uint8_t *data, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++)
    {
        sum += data[i];
    }
    return (uint8_t)(sum & 0xFF);
}

static inline void __not_in_flash_func(pio_irq_common)(PIO pio)
{
    uint idx = pio_get_index(pio);
    for (int sm = 0; sm < 4; sm++)
    {
        InterruptSerialPIO *inst = g_pioInstances[idx][sm];
        if (inst)
        {
            ispio_handle_irq(inst);
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

void ispio_init(InterruptSerialPIO *self, uint tx, uint rx)
{
    memset(self, 0, sizeof(InterruptSerialPIO));
    self->tx = tx;
    self->rx = rx;
    self->lastByteReceivedTime = 0;
    self->bitCycles = 0;
    self->running = false;
    self->rxSM = -1;
}

void ispio_deinit(InterruptSerialPIO *self)
{
    ispio_end(self);
}

void ispio_set_message_sink(void (*handler)(ModuleMessage *))
{
    g_messageSink = handler;
}

static bool claim_rx_sm(PIO *outPio, int *outSm, uint *outOffset)
{
    // Try pio0 then pio1
    int sm = pio_claim_unused_sm(pio0, false);
    if (sm >= 0)
    {
        *outPio = pio0;
        *outSm = sm;
        if (rxProgramOffset[0] < 0)
        {
            rxProgramOffset[0] = pio_add_program(pio0, &pio_rx_program);
        }
        *outOffset = rxProgramOffset[0];
        return true;
    }
    sm = pio_claim_unused_sm(pio1, false);
    if (sm >= 0)
    {
        *outPio = pio1;
        *outSm = sm;
        if (rxProgramOffset[1] < 0)
        {
            rxProgramOffset[1] = pio_add_program(pio1, &pio_rx_program);
        }
        *outOffset = rxProgramOffset[1];
        return true;
    }
    return false;
}

static inline void __not_in_flash_func(resetParser)(InterruptSerialPIO *self)
{
    self->parser.length = 0;
    self->parser.expectedLength = 0;
    self->parser.syncing = false;
    self->parser.lastByteReceivedTime = 0;
    self->lastByteReceivedTime = 0;
}

void ispio_begin(InterruptSerialPIO *self, unsigned long baud)
{
    (void)baud; // fixed
    resetParser(self);

    if ((self->tx == NOPIN) && (self->rx == NOPIN))
    {
        return;
    }

    if (self->tx != NOPIN)
    {
        gpio_init(self->tx);
        gpio_set_dir(self->tx, GPIO_OUT);
        gpio_put(self->tx, 1); // idle high
    }

    // Precompute cycles-per-bit for TX bit-banging
    if (self->tx != NOPIN)
    {
        self->bitCycles = clock_get_hz(clk_sys) / ISPIO_FIXED_BAUD;
    }

    if (self->rx != NOPIN)
    {
        if (self->rxSM < 0)
        {
            if (!claim_rx_sm(&self->rxPIO, &self->rxSM, &self->rxOffset))
            {
                return; // No PIO available
            }
        }
        else
        {
            uint idx = pio_get_index(self->rxPIO);
            if (rxProgramOffset[idx] < 0)
            {
                rxProgramOffset[idx] = pio_add_program(self->rxPIO, &pio_rx_program);
            }
            self->rxOffset = rxProgramOffset[idx];
            if (!pio_sm_is_claimed(self->rxPIO, self->rxSM))
            {
                pio_sm_claim(self->rxPIO, self->rxSM);
            }
        }

        pio_sm_config c = pio_rx_program_get_default_config(self->rxOffset);
        sm_config_set_in_pins(&c, self->rx);
        sm_config_set_jmp_pin(&c, self->rx);
        sm_config_set_in_shift(&c, true, false, 32); // shift right, no autopush
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

        float div = (float)clock_get_hz(clk_sys) / (float)(ISPIO_FIXED_BAUD * 8); // 8x oversample
        sm_config_set_clkdiv(&c, div);

        pio_sm_init(self->rxPIO, self->rxSM, self->rxOffset, &c);
        pio_sm_set_consecutive_pindirs(self->rxPIO, self->rxSM, self->rx, 1, false);
        pio_gpio_init(self->rxPIO, self->rx);
        // Bias RX low so a disconnected/floating module doesn't appear as UART-idle HIGH.
        // The module's TX should actively drive HIGH when present/idle.
        gpio_pull_down(self->rx);
        pio_sm_clear_fifos(self->rxPIO, self->rxSM);

        // Enable IRQ for RX FIFO not empty
        switch (self->rxSM)
        {
        case 0:
            pio_set_irq0_source_enabled(self->rxPIO, pis_sm0_rx_fifo_not_empty, true);
            break;
        case 1:
            pio_set_irq0_source_enabled(self->rxPIO, pis_sm1_rx_fifo_not_empty, true);
            break;
        case 2:
            pio_set_irq0_source_enabled(self->rxPIO, pis_sm2_rx_fifo_not_empty, true);
            break;
        case 3:
            pio_set_irq0_source_enabled(self->rxPIO, pis_sm3_rx_fifo_not_empty, true);
            break;
        }

        uint idx = pio_get_index(self->rxPIO);
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

        pio_sm_set_enabled(self->rxPIO, self->rxSM, true);
        g_pioInstances[idx][self->rxSM] = self;
    }

    self->running = true;
}

void ispio_end(InterruptSerialPIO *self)
{
    if (!self->running)
    {
        return;
    }
    if (self->rx != NOPIN)
    {
        uint idx = pio_get_index(self->rxPIO);
        g_pioInstances[idx][self->rxSM] = NULL;

        switch (self->rxSM)
        {
        case 0:
            pio_set_irq0_source_enabled(self->rxPIO, pis_sm0_rx_fifo_not_empty, false);
            pio_sm_clear_fifos(self->rxPIO, 0);
            break;
        case 1:
            pio_set_irq0_source_enabled(self->rxPIO, pis_sm1_rx_fifo_not_empty, false);
            pio_sm_clear_fifos(self->rxPIO, 1);
            break;
        case 2:
            pio_set_irq0_source_enabled(self->rxPIO, pis_sm2_rx_fifo_not_empty, false);
            pio_sm_clear_fifos(self->rxPIO, 2);
            break;
        case 3:
            pio_set_irq0_source_enabled(self->rxPIO, pis_sm3_rx_fifo_not_empty, false);
            pio_sm_clear_fifos(self->rxPIO, 3);
            break;
        }

        pio_sm_set_enabled(self->rxPIO, self->rxSM, false);
        if (!self->staticSM)
        {
            pio_sm_unclaim(self->rxPIO, self->rxSM);
        }
    }
    self->running = false;
}

void ispio_set_port_location(InterruptSerialPIO *self, uint8_t row, uint8_t col)
{
    self->row = row;
    self->col = col;
}

void ispio_set_pins(InterruptSerialPIO *self, uint tx, uint rx)
{
    self->tx = tx;
    self->rx = rx;
}

void ispio_set_pio_sm(InterruptSerialPIO *self, PIO pio, int sm)
{
    self->rxPIO = pio;
    self->rxSM = sm;
    self->staticSM = true;
}

// Host TX are small command payloads (~10 bytes) so bit-banging is acceptable
size_t __not_in_flash_func(ispio_write)(InterruptSerialPIO *self, uint8_t c)
{
    if (!self->running || (self->tx == NOPIN))
        return 0;

    // Ensure SysTick is running at CPU frequency
    // CSR bit 0: ENABLE, bit 2: CLKSOURCE (1=CPU)
    if ((systick_hw->csr & 0x5) != 0x5)
    {
        systick_hw->rvr = 0x00FFFFFF;
        systick_hw->cvr = 0;
        systick_hw->csr = 0x5;
    }

    // Calculate cycles per bit
    uint32_t bitCycles = self->bitCycles;

    // Pre-calculate masks
    uint32_t pinMask = 1ul << self->tx;

    uint32_t flags = save_and_disable_interrupts();

    uint32_t start = systick_hw->cvr;
    uint32_t target = bitCycles;

    // Start Bit (Low)
    sio_hw->gpio_clr = pinMask;
    while (((start - systick_hw->cvr) & 0xFFFFFF) < target)
        ;
    target += bitCycles;

    // DATA BITS
    for (int i = 0; i < 8; i++)
    {
        if (c & (1 << i))
            sio_hw->gpio_set = pinMask;
        else
            sio_hw->gpio_clr = pinMask;

        while (((start - systick_hw->cvr) & 0xFFFFFF) < target)
            ;
        target += bitCycles;
    }

    // STOP BIT (High)
    sio_hw->gpio_set = pinMask;
    while (((start - systick_hw->cvr) & 0xFFFFFF) < target)
        ;

    restore_interrupts_from_disabled(flags);
    return 1;
}

size_t ispio_write_buffer(InterruptSerialPIO *self, const uint8_t *buffer, size_t size)
{
    if (!buffer || size == 0)
    {
        return 0;
    }
    size_t written = 0;
    for (size_t i = 0; i < size; i++)
    {
        written += ispio_write(self, buffer[i]);
    }
    return written;
}

extern "C" ModuleMessage *allocateMessageFromIRQ();
extern "C" void commitMessageFromIRQ();

static void __not_in_flash_func(emitFrame)(InterruptSerialPIO *self)
{
    if (self->parser.length < 4)
    {
        return;
    }

    ModuleMessage *slot = allocateMessageFromIRQ();
    if (!slot)
    {
        return;
    }

    slot->moduleRow = self->row;
    slot->moduleCol = self->col;
    slot->commandId = (ModuleMessageId)self->parser.buffer[1];
    slot->payloadLength = (uint16_t)self->parser.buffer[2] | ((uint16_t)self->parser.buffer[3] << 8);
    if (slot->payloadLength > sizeof(slot->payload))
    {
        slot->payloadLength = sizeof(slot->payload);
    }
    memcpy(slot->payload, &self->parser.buffer[4], slot->payloadLength);

    commitMessageFromIRQ();

    if (g_messageSink)
    {
        g_messageSink(slot);
    }
}

static inline void __not_in_flash_func(processByte)(InterruptSerialPIO *self, uint8_t b)
{
    SerialParser *p = &self->parser;
    if (!p->syncing)
    {
        if (b == 0xAA)
        {
            p->buffer[0] = b;
            p->length = 1;
            p->syncing = true;
        }
        return;
    }

    if (p->length >= sizeof(p->buffer))
    {
        resetParser(self);
        return;
    }

    p->buffer[p->length++] = b;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    p->lastByteReceivedTime = now;
    self->lastByteReceivedTime = now;

    if (p->length == 4)
    {
        uint16_t payloadLen = (uint16_t)p->buffer[2] | ((uint16_t)p->buffer[3] << 8);
        if (payloadLen > MODULE_MAX_PAYLOAD)
        {
            resetParser(self);
            return;
        }
        uint32_t total = 5u + payloadLen; // 4-byte header + payload + checksum
        if (total > sizeof(p->buffer))
        {
            resetParser(self);
            return;
        }
        p->expectedLength = (uint16_t)total;
    }

    if (p->expectedLength > 0 && p->length == p->expectedLength)
    {
        uint8_t checksum = p->buffer[p->length - 1];
        if (checksum == calcChecksum(p->buffer, p->length - 1))
        {
            emitFrame(self);
        }
        resetParser(self);
    }
}

void inline __not_in_flash_func(ispio_handle_irq)(InterruptSerialPIO *self)
{
    if (self->rx == NOPIN)
    {
        return;
    }
    while (!pio_sm_is_rx_fifo_empty(self->rxPIO, self->rxSM))
    {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (self->parser.syncing && self->parser.lastByteReceivedTime && (now - self->parser.lastByteReceivedTime > PARSER_TIMEOUT_MS))
        {
            resetParser(self);
        }
        uint8_t val = (uint8_t)((pio_sm_get_blocking(self->rxPIO, self->rxSM) >> 24) & 0xFF);

        // Protocol parsing is the only RX consumer.
        processByte(self, val);
    }
}
