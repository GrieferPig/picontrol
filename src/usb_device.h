#pragma once

#include <Arduino.h>
#include <cstdint>

// USB composite device (TinyUSB): CDC serial + MIDI + HID keyboard.

namespace usb
{
    void init();
    void task();

    size_t enqueueCdcWrite(const uint8_t *data, size_t len);

    bool sendMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, uint8_t cable = 0);
    bool sendMidiNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, uint8_t cable = 0);
    bool sendMidiCC(uint8_t channel, uint8_t controller, uint8_t value, uint8_t cable = 0);
    bool sendMidiCC14(uint8_t channel, uint8_t controllerMsb, uint16_t value14, uint8_t cable = 0);
    bool sendMidiPitchBend(uint8_t channel, uint16_t value14, uint8_t cable = 0);

    bool sendKeypress(uint8_t hidKeycode, uint8_t modifier = 0);
    bool sendKeyDown(uint8_t hidKeycode, uint8_t modifier = 0);
    bool sendKeyUp(uint8_t hidKeycode);
}
