#include "usb_device.h"

#include <Adafruit_TinyUSB.h>
#include <Adafruit_TinyUSB_API.h>
#include <Adafruit_USBD_CDC.h>
#include <MIDI.h>
#include <tusb.h>
#include <pico/multicore.h>
#include <pico/util/queue.h>
#include <pico/sync.h>
#include "ipc.hpp"
#include "port.h"
#include "mapping.h"
#include "debug_printf.h"

static Adafruit_USBD_MIDI g_midi;
static Adafruit_USBD_HID g_hid;

static Adafruit_USBD_CDC g_cdc_bin;

// Arduino MIDI library instance using TinyUSB transport
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, g_midi, MIDI);

static uint8_t const g_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()};

static volatile bool g_usbStarted = false;

// HID message queue
static queue_t g_hidQ;
static bool g_queuesInited = false;

struct HidKeyMsg
{
    uint8_t modifier;
    uint8_t keycode;
    bool pressed;
};

namespace Message
{
    enum class MessageType : uint8_t
    {
        COMMAND = 0,
        RESPONSE,
        EVENT,
    };

    enum class CommandType : uint8_t
    {
        INFO = 0,
        VERSION,
        MAP,
        MODULES
    };

    enum class ResponseType : uint8_t
    {
        ACK = 0,
        NACK,
        INFO,
        VERSION,
        MAP,
        MODULES
    };

    enum class EventType : uint8_t
    {
        PORT_STATUS_CHANGED = 0,
        MODULE_STATE_CHANGED,
        MAPPINGS_LOADED,
        MODULE_PARAM_CHANGED,
    };

    enum class CommandSubMapType : uint8_t
    {
        SET = 0,
        SET_CURVE,
        DEL,
        LIST,
        CLEAR
    };

    enum class CommandSubModuleType : uint8_t
    {
        LIST = 0,
        PARAM_SET,
        CALIB_SET
    };

    struct __attribute__((packed)) Message
    {
        MessageType type;
        CommandType command;
        union
        {
            CommandSubMapType mapSub;
            CommandSubModuleType modulesSub;
        } subcommand;
        uint16_t length; // Only contain len(data)
        uint16_t checksum;
        uint8_t *data;
    };
}

namespace usb
{
    // Minimum message size: header(5) + checksum(2) = 7 bytes
    constexpr uint8_t MIN_MESSAGE_SIZE = 7;
    // serial input circular buffer
    constexpr uint32_t INPUT_BUF_SIZE = 512;
    static uint8_t inputBuffer[INPUT_BUF_SIZE];
    static size_t inputPos = 0;
    static uint64_t g_lastByteTime = 0;
    static size_t g_expectedLength = 0;
    static Message::Message g_currentMessage;

    // Output buffer for preparing data to be packed (e.g., module list)
    constexpr uint32_t OUTPUT_BUF_SIZE = 8192;
    static uint8_t outputBuffer[OUTPUT_BUF_SIZE];

    // Packed output buffer for sending responses (separate to avoid overwrite issues)
    static uint8_t packedOutputBuffer[OUTPUT_BUF_SIZE];

    static uint8_t desc_buffer[512];

    // Header size: type(1) + command(1) + subcommand(1) + length(2) = 5 bytes
    constexpr size_t HEADER_SIZE = 5;
    // Timeout in microseconds (50ms)
    constexpr uint64_t MESSAGE_TIMEOUT_US = 50000;

    static uint16_t crc16_update(uint16_t crc, uint8_t data)
    {
        crc ^= (static_cast<uint16_t>(data) << 8);
        for (int i = 0; i < 8; i++)
        {
            if (crc & 0x8000)
            {
                crc = (crc << 1) ^ 0x1021; // The polynomial
            }
            else
            {
                crc <<= 1;
            }
        }
        return crc;
    }

    static uint16_t calculate_crc16(const uint8_t *data, size_t length)
    {
        uint16_t crc = 0xFFFF; // Initial value
        for (size_t i = 0; i < length; i++)
        {
            crc = crc16_update(crc, data[i]);
        }
        return crc;
    }

    // Send a response message with optional payload
    static void sendResponse(Message::ResponseType responseType, uint8_t subcommand = 0,
                             const uint8_t *data = nullptr, uint16_t dataLength = 0)
    {
        // Response format: type(1) + response(1) + subcommand(1) + length(2) + checksum(2) + data(length)
        constexpr size_t RESPONSE_HEADER_SIZE = 5;
        size_t totalSize = RESPONSE_HEADER_SIZE + sizeof(uint16_t) + dataLength;

        // Use static output buffer for responses
        if (totalSize > OUTPUT_BUF_SIZE)
        {
            dbg_printf("Response too large: %u bytes\n", totalSize);
            return; // Response too large
        }

        outputBuffer[0] = static_cast<uint8_t>(Message::MessageType::RESPONSE);
        outputBuffer[1] = static_cast<uint8_t>(responseType);
        outputBuffer[2] = subcommand;
        outputBuffer[3] = static_cast<uint8_t>(dataLength & 0xFF);        // Length LSB
        outputBuffer[4] = static_cast<uint8_t>((dataLength >> 8) & 0xFF); // Length MSB

        // Copy data payload directly
        size_t dataOffset = RESPONSE_HEADER_SIZE + sizeof(uint16_t); // After checksum slot
        if (dataLength > 0)
        {
            memcpy(&outputBuffer[dataOffset], data, dataLength);
        }

        // Calculate checksum over header + data (excluding checksum slot)
        // Checksum is calculated over: header(5) + data(dataLength)
        uint16_t checksum = calculate_crc16(outputBuffer, RESPONSE_HEADER_SIZE);
        for (uint16_t i = 0; i < dataLength; i++)
        {
            checksum = crc16_update(checksum, outputBuffer[dataOffset + i]);
        }
        // Insert checksum at fixed position (bytes 5-6)
        outputBuffer[5] = static_cast<uint8_t>(checksum & 0xFF);        // Checksum LSB
        outputBuffer[6] = static_cast<uint8_t>((checksum >> 8) & 0xFF); // Checksum MSB

        size_t totalLen = RESPONSE_HEADER_SIZE + sizeof(uint16_t) + dataLength;

        g_cdc_bin.write(outputBuffer, totalLen);
        g_cdc_bin.flush();
    }

    // Use zero run length encoding to pack data
    // Only to be used when sending resp to MODULES LIST
    static void sendResponsePacked(Message::ResponseType responseType, uint8_t subcommand = 0,
                                   const uint8_t *data = nullptr, uint16_t dataLength = 0)
    {
        // Response format: type(1) + response(1) + subcommand(1) + length(2) + checksum(2) + packed_data(length)
        // Uses packedOutputBuffer to avoid overwriting input data if it's in outputBuffer
        constexpr size_t RESPONSE_HEADER_SIZE = 5;

        packedOutputBuffer[0] = static_cast<uint8_t>(Message::MessageType::RESPONSE);
        packedOutputBuffer[1] = static_cast<uint8_t>(responseType);
        packedOutputBuffer[2] = subcommand;

        size_t packedOffset = RESPONSE_HEADER_SIZE + sizeof(uint16_t);

        size_t i = 0;
        while (i < dataLength)
        {
            // Count zeros
            uint8_t zeroCount = 0;
            while (i < dataLength && data[i] == 0 && zeroCount < 255)
            {
                zeroCount++;
                i++;
            }

            // Count valid (non-zero) bytes
            uint8_t validCount = 0;
            size_t validStart = i;
            while (i < dataLength && data[i] != 0 && validCount < 255)
            {
                validCount++;
                i++;
            }

            // Ensure we have space in packedOutputBuffer
            if (packedOffset + 2 + validCount > OUTPUT_BUF_SIZE)
            {
                dbg_printf("Packed response too large\n");
                return;
            }

            packedOutputBuffer[packedOffset++] = zeroCount;
            packedOutputBuffer[packedOffset++] = validCount;
            if (validCount > 0)
            {
                memcpy(&packedOutputBuffer[packedOffset], &data[validStart], validCount);
                packedOffset += validCount;
            }
        }

        uint16_t packedDataLen = packedOffset - (RESPONSE_HEADER_SIZE + sizeof(uint16_t));

        // Update length in header
        packedOutputBuffer[3] = static_cast<uint8_t>(packedDataLen & 0xFF);        // Length LSB
        packedOutputBuffer[4] = static_cast<uint8_t>((packedDataLen >> 8) & 0xFF); // Length MSB

        // Calculate checksum over header + packed data
        uint16_t checksum = calculate_crc16(packedOutputBuffer, RESPONSE_HEADER_SIZE);
        for (size_t k = (RESPONSE_HEADER_SIZE + sizeof(uint16_t)); k < packedOffset; k++)
        {
            checksum = crc16_update(checksum, packedOutputBuffer[k]);
        }

        // Insert checksum at fixed position (bytes 5-6)
        packedOutputBuffer[5] = static_cast<uint8_t>(checksum & 0xFF);        // Checksum LSB
        packedOutputBuffer[6] = static_cast<uint8_t>((checksum >> 8) & 0xFF); // Checksum MSB

        g_cdc_bin.write(packedOutputBuffer, packedOffset);
        g_cdc_bin.flush();
    }

    void sendAck()
    {
        sendResponse(Message::ResponseType::ACK);
    }

    void sendNack()
    {
        sendResponse(Message::ResponseType::NACK);
    }

    void handleMap(Message::Message *msg)
    {
        switch (msg->subcommand.mapSub)
        {
        case Message::CommandSubMapType::SET:
        {
            // Payload format: row(1) + col(1) + paramId(1) + type(1) + d1(1) + d2(1)

            if (msg->length != 6)
            {
                sendNack();
                return; // Invalid length
            }
            uint8_t row = msg->data[0];
            uint8_t col = msg->data[1];
            uint8_t paramId = msg->data[2];
            uint8_t type = msg->data[3];
            uint8_t d1 = msg->data[4];
            uint8_t d2 = msg->data[5];
            MappingManager::updateMapping((int)row, (int)col, paramId, (ActionType)type, d1, d2);
            // Sync after update
            IPC::enqueueSyncMapping((int)row, (int)col);
            sendAck();
            return;
        }
        case Message::CommandSubMapType::SET_CURVE:
        {
            // Payload format: row(1) + col(1) + paramId(1) + curve(sizeof(Curve))
            if (msg->length != 3 + sizeof(Curve))
            {
                dbg_printf("SET_CURVE: expected length %d, got %d\n", (int)(3 + sizeof(Curve)), msg->length);
                sendNack();
                return; // Invalid length
            }
            uint8_t row = msg->data[0];
            uint8_t col = msg->data[1];
            uint8_t paramId = msg->data[2];
            Curve curve{};
            if (!MappingManager::hexToCurve(&msg->data[3], &curve))
            {
                sendNack();
                return; // Invalid curve data
            }
            MappingManager::updateMappingCurve((int)row, (int)col, paramId, curve);
            // Sync after update
            IPC::enqueueSyncMapping((int)row, (int)col);
            sendAck();
            return;
        }
        case Message::CommandSubMapType::DEL:
        {
            // Payload format: row(1) + col(1) + paramId(1)
            if (msg->length != 3)
            {
                sendNack();
                return; // Invalid length
            }
            uint8_t row = msg->data[0];
            uint8_t col = msg->data[1];
            uint8_t paramId = msg->data[2];
            bool deleted = MappingManager::deleteMapping((int)row, (int)col, paramId);
            if (deleted)
            {
                // Sync after delete
                IPC::enqueueSyncMapping((int)row, (int)col);
                sendAck();
            }
            else
            {
                sendNack();
            }
            return;
        }
        case Message::CommandSubMapType::LIST:
        {
            // no payload
            if (msg->length != 0)
            {
                sendNack();
                return; // Invalid length
            }
            uint8_t totalMappings = MappingManager::count();
            uint32_t responseSize = totalMappings * sizeof(ModuleMapping) + 1;
            if (responseSize > OUTPUT_BUF_SIZE)
            {
                sendNack();
                return; // Response too large (impossible but check anyway)
            }

            outputBuffer[0] = totalMappings;
            memcpy(&outputBuffer[1], MappingManager::getAllMappings(), totalMappings * sizeof(ModuleMapping));
            sendResponsePacked(Message::ResponseType::MAP, static_cast<uint8_t>(Message::CommandSubMapType::LIST),
                               outputBuffer, responseSize);
            return;
        }
        case Message::CommandSubMapType::CLEAR:
        {
            // no payload
            if (msg->length != 0)
            {
                sendNack();
                return; // Invalid length
            }
            MappingManager::clearAll();
            sendAck();
            return;
        }
        default:
            sendNack();
            return;
        }
    }

    void handleModules(Message::Message *msg)
    {
        switch (msg->subcommand.modulesSub)
        {
        case Message::CommandSubModuleType::LIST:
        {
            // Convert port states to packed format for transmission
            static PortStatePacked packedPorts[MODULE_PORT_ROWS * MODULE_PORT_COLS];
            Port::State *ports = Port::getAll();

            for (int i = 0; i < MODULE_PORT_ROWS * MODULE_PORT_COLS; i++)
            {
                Port::toPackedState(ports[i], packedPorts[i]);
            }

            sendResponsePacked(Message::ResponseType::MODULES, static_cast<uint8_t>(Message::CommandSubModuleType::LIST),
                               reinterpret_cast<uint8_t *>(packedPorts),
                               sizeof(PortStatePacked) * MODULE_PORT_ROWS * MODULE_PORT_COLS);
            break;
        }
        case Message::CommandSubModuleType::PARAM_SET:
        {
            if (msg->length < 4)
            {
                sendNack();
                return; // Invalid length
            }
            uint8_t row = msg->data[0];
            uint8_t col = msg->data[1];
            uint8_t paramId = msg->data[2];
            uint8_t dataType = msg->data[3];
            const char *valueStr = reinterpret_cast<const char *>(&msg->data[4]);

            Port::State *p = Port::get(row, col);
            if (!p || !p->configured || !p->hasModule)
            {
                sendNack();
                return; // Invalid port
            }

            IPC::enqueueSetParameter(row, col, paramId, dataType, valueStr);
            sendAck();
            break;
        }

        case Message::CommandSubModuleType::CALIB_SET:
        {
            if (msg->length < 4)
            {
                sendNack();
                return; // Invalid length
            }
            uint8_t row = msg->data[0];
            uint8_t col = msg->data[1];
            // TODO: Implement calibration setting
            // For now, acknowledge the command but don't do anything
            sendAck();
            return;
            break;
        }

        default:
            sendNack();
            return;
            break;
        }
    }

    void init()
    {
        if (g_usbStarted)
        {
            return;
        }
        TinyUSBDevice.setConfigurationBuffer(desc_buffer, sizeof(desc_buffer));

        if (!TinyUSBDevice.isInitialized())
        {
            TinyUSBDevice.begin(0);
        }
        if (!g_queuesInited)
        {
            queue_init(&g_hidQ, sizeof(HidKeyMsg), 32);
            g_queuesInited = true;
        }
        Serial.begin(115200);

        g_cdc_bin.setStringDescriptor("Picontrol Config Interface");
        g_cdc_bin.begin(115200);

        // MIDI
        g_midi.setStringDescriptor("Picontrol MIDI Interface");
        MIDI.begin(MIDI_CHANNEL_OMNI);

        // HID keyboard
        g_hid.setBootProtocol(HID_ITF_PROTOCOL_KEYBOARD);
        g_hid.setPollInterval(2);
        g_hid.setReportDescriptor(g_hid_report_desc, sizeof(g_hid_report_desc));
        g_hid.setStringDescriptor("Picontrol HID Keyboard Interface");
        g_hid.begin();

        if (TinyUSBDevice.mounted())
        {
            TinyUSBDevice.detach();
            delay(10);
            TinyUSBDevice.attach();
        }

        g_usbStarted = true;
    }

    // Serial command processing task
    void processMessage(uint8_t *messageBuffer, size_t length)
    {
        if (!messageBuffer)
        {
            return;
        }
        if (length < usb::MIN_MESSAGE_SIZE)
        {
            dbg_printf("Message too short\n");
            sendNack();
            return; // too short to be valid
        }

        // Parse message header manually (don't cast directly because of pointer member)
        Message::Message msg;
        msg.type = static_cast<Message::MessageType>(messageBuffer[0]);
        msg.command = static_cast<Message::CommandType>(messageBuffer[1]);
        msg.subcommand.mapSub = static_cast<Message::CommandSubMapType>(messageBuffer[2]);
        msg.length = static_cast<uint16_t>(messageBuffer[3]) | (static_cast<uint16_t>(messageBuffer[4]) << 8);
        msg.checksum = static_cast<uint16_t>(messageBuffer[5]) | (static_cast<uint16_t>(messageBuffer[6]) << 8);
        msg.data = &messageBuffer[HEADER_SIZE + sizeof(uint16_t)]; // Payload starts at byte 7

        // Calculate checksum over header(5) + payload (excluding checksum bytes)
        uint16_t calculatedChecksum = calculate_crc16(messageBuffer, HEADER_SIZE);
        // Continue CRC over payload portion (starts at byte 7)
        for (size_t i = HEADER_SIZE + sizeof(uint16_t); i < length; i++)
        {
            calculatedChecksum = crc16_update(calculatedChecksum, messageBuffer[i]);
        }
        if (calculatedChecksum != msg.checksum)
        {
            dbg_printf("Checksum mismatch: calc=%04X recv=%04X\n", calculatedChecksum, msg.checksum);
            sendNack();
            return; // checksum mismatch
        }
        // Only COMMAND messages are allowed for incoming messages
        if (msg.type != Message::MessageType::COMMAND)
        {
            dbg_printf("Invalid message type\n");
            sendNack();
            return;
        }

        switch (msg.command)
        {
        case Message::CommandType::MAP:
            dbg_printf("MAP command received\n");
            handleMap(&msg);
            break;
        case Message::CommandType::MODULES:
            dbg_printf("MODULES command received\n");
            handleModules(&msg);
            break;
        default:
            dbg_printf("Unknown command received\n");
            sendNack();
            return;
            // Unknown command
            break;
        }
    }

    bool sendMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, uint8_t cable)
    {
        (void)cable;
        uint8_t ch = (channel & 0x0F) + 1;
        MIDI.sendNoteOn((uint8_t)(note & 0x7F), (uint8_t)(velocity & 0x7F), ch);
        return true;
    }

    bool sendMidiNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, uint8_t cable)
    {
        (void)cable;
        uint8_t ch = (channel & 0x0F) + 1;
        MIDI.sendNoteOff((uint8_t)(note & 0x7F), (uint8_t)(velocity & 0x7F), ch);
        return true;
    }

    bool sendMidiCC(uint8_t channel, uint8_t controller, uint8_t value, uint8_t cable)
    {
        (void)cable;
        uint8_t ch = (channel & 0x0F) + 1;
        MIDI.sendControlChange((uint8_t)(controller & 0x7F), (uint8_t)(value & 0x7F), ch);
        return true;
    }

    bool sendMidiCC14(uint8_t channel, uint8_t controllerMsb, uint16_t value14, uint8_t cable)
    {
        (void)cable;
        if (value14 > 16383)
            value14 = 16383;

        // 14-bit CC uses controller N (MSB) and controller N+32 (LSB)
        const uint8_t msb = (uint8_t)((value14 >> 7) & 0x7F);
        const uint8_t lsb = (uint8_t)(value14 & 0x7F);
        const uint8_t ctrlMsb = (uint8_t)(controllerMsb & 0x7F);
        const uint8_t ctrlLsb = (uint8_t)((controllerMsb + 32) & 0x7F);

        uint8_t ch = (channel & 0x0F) + 1;
        MIDI.sendControlChange(ctrlMsb, msb, ch);
        MIDI.sendControlChange(ctrlLsb, lsb, ch);
        return true;
    }

    bool sendMidiPitchBend(uint8_t channel, uint16_t value14, uint8_t cable)
    {
        (void)cable;
        if (value14 > 16383)
            value14 = 16383;

        uint8_t ch = (channel & 0x0F) + 1;
        int16_t bend = (int16_t)value14 - 8192; // MIDI library uses -8192..8191
        MIDI.sendPitchBend(bend, ch);
        return true;
    }

    bool sendKeypress(uint8_t hidKeycode, uint8_t modifier)
    {
        HidKeyMsg press{modifier, hidKeycode, true};
        HidKeyMsg release{0, hidKeycode, false};

        // If press enqueues but release doesn't, host may see stuck key
        // Hence both or none must succeed
        if (!queue_try_add(&g_hidQ, &press))
            return false;
        if (!queue_try_add(&g_hidQ, &release))
            return false;
        return true;
    }

    bool sendKeyDown(uint8_t hidKeycode, uint8_t modifier)
    {
        HidKeyMsg press{modifier, hidKeycode, true};
        return queue_try_add(&g_hidQ, &press);
    }

    bool sendKeyUp(uint8_t hidKeycode)
    {
        HidKeyMsg release{0, hidKeycode, false};
        return queue_try_add(&g_hidQ, &release);
    }

    void task()
    {
        uint64_t timestamp = to_us_since_boot(get_absolute_time());

        // Check for timeout (50ms) - clear current message if no data received
        if (inputPos > 0 && (timestamp - g_lastByteTime) > MESSAGE_TIMEOUT_US)
        {
            inputPos = 0;
            g_expectedLength = 0;
        }

        // Drain Serial
        if (g_cdc_bin)
        {
            // Parse incoming serial data
            while (g_cdc_bin.available())
            {
                uint8_t byte = g_cdc_bin.read();
                g_lastByteTime = timestamp;

                // Store byte in buffer
                if (inputPos < INPUT_BUF_SIZE)
                {
                    inputBuffer[inputPos++] = byte;
                }
                else
                {
                    // Buffer overflow - reset
                    inputPos = 0;
                    g_expectedLength = 0;
                    continue;
                }

                // Once we have the header, calculate expected total length
                if (inputPos >= HEADER_SIZE && g_expectedLength == 0)
                {
                    // Parse header fields
                    g_currentMessage.type = static_cast<Message::MessageType>(inputBuffer[0]);
                    g_currentMessage.command = static_cast<Message::CommandType>(inputBuffer[1]);
                    // subcommand at index 2 (will be parsed based on command type)
                    g_currentMessage.subcommand.mapSub = static_cast<Message::CommandSubMapType>(inputBuffer[2]);
                    // length is little-endian uint16_t at index 3-4
                    g_currentMessage.length = static_cast<uint16_t>(inputBuffer[3]) |
                                              (static_cast<uint16_t>(inputBuffer[4]) << 8);

                    // Expected total: header + data (length bytes) + checksum (2 bytes)
                    g_expectedLength = HEADER_SIZE + g_currentMessage.length + sizeof(uint16_t);

                    // Sanity check - if expected length exceeds buffer, reset
                    if (g_expectedLength > INPUT_BUF_SIZE)
                    {
                        inputPos = 0;
                        g_expectedLength = 0;
                        continue;
                    }
                }

                // Check if we have received the complete message
                if (g_expectedLength > 0 && inputPos >= g_expectedLength)
                {
                    // Wire format: header(5) + checksum(2) + payload(length)
                    // Extract checksum from bytes 5-6 (right after header)
                    g_currentMessage.checksum = static_cast<uint16_t>(inputBuffer[HEADER_SIZE]) |
                                                (static_cast<uint16_t>(inputBuffer[HEADER_SIZE + 1]) << 8);

                    // Set data pointer to the payload portion (starts at byte 7)
                    g_currentMessage.data = &inputBuffer[HEADER_SIZE + sizeof(uint16_t)];

                    processMessage(inputBuffer, g_expectedLength);

                    // Reset
                    inputPos = 0;
                    g_expectedLength = 0;
                }
            }
        }

        // Drain HID keyboard
        if (g_hid.ready())
        {
            HidKeyMsg k;
            static uint8_t active_keys[6] = {0};
            static uint8_t active_modifier = 0;

            if (queue_try_remove(&g_hidQ, &k))
            {
                if (k.pressed)
                {
                    active_modifier = k.modifier;
                    bool found = false;
                    for (int i = 0; i < 6; i++)
                    {
                        if (active_keys[i] == k.keycode)
                        {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        for (int i = 0; i < 6; i++)
                        {
                            if (active_keys[i] == 0)
                            {
                                active_keys[i] = k.keycode;
                                break;
                            }
                        }
                    }
                }
                else
                {
                    if (k.keycode == 0)
                    {
                        memset(active_keys, 0, 6);
                        active_modifier = 0;
                    }
                    else
                    {
                        for (int i = 0; i < 6; i++)
                        {
                            if (active_keys[i] == k.keycode)
                            {
                                active_keys[i] = 0;
                            }
                        }
                        // Pack
                        uint8_t packed[6] = {0};
                        int p = 0;
                        for (int i = 0; i < 6; i++)
                        {
                            if (active_keys[i] != 0)
                            {
                                packed[p++] = active_keys[i];
                            }
                        }
                        memcpy(active_keys, packed, 6);

                        if (p == 0)
                        {
                            active_modifier = 0;
                        }
                    }
                }

                g_hid.keyboardReport(0, active_modifier, active_keys);
            }
        }
    }
}