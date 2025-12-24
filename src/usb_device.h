#pragma once

#include <Arduino.h>
#include <cstdint>

// USB composite device helpers (TinyUSB): CDC serial + MIDI + HID keyboard.
// Design goal: all TinyUSB calls happen on core 0 via usb::task().

namespace usb
{
    void init();
    void task();

    // CDC status helpers
    bool cdcConnected();

    // Log/CDC write entrypoint used by UsbSerial (thread-safe across cores).
    size_t enqueueCdcWrite(const uint8_t *data, size_t len);

    // MIDI helpers (enqueue; core 0 sends)
    bool sendMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, uint8_t cable = 0);
    bool sendMidiNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, uint8_t cable = 0);
    bool sendMidiCC(uint8_t channel, uint8_t controller, uint8_t value, uint8_t cable = 0);

    // HID keyboard helpers (enqueue; core 0 sends)
    bool sendKeypress(uint8_t hidKeycode, uint8_t modifier = 0);
    bool sendKeyDown(uint8_t hidKeycode, uint8_t modifier = 0);
    bool sendKeyUp();
}

// Print-compatible logger routed to USB CDC.
// Replace Serial.print/println usage with UsbSerial.print/println.
extern Print &UsbSerial;
