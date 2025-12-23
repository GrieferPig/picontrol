#include "usb_device.h"

#include <Adafruit_TinyUSB.h>
#include <Adafruit_TinyUSB_API.h>
#include <Adafruit_USBD_CDC.h>
#include "tusb.h"
#include <pico/multicore.h>
#include <pico/util/queue.h>

#include "mapping.h"
#include "runtime_config.h"
#include "runtime_query.h"

namespace
{
    // Instantiate interfaces so Adafruit TinyUSB builds a composite device.
    static Adafruit_USBD_MIDI g_midi;
    static Adafruit_USBD_HID g_hid;

    static uint8_t const g_hid_report_desc[] = {
        TUD_HID_REPORT_DESC_KEYBOARD()};

    struct LogChunk
    {
        uint16_t len;
        uint8_t data[256];
    };

    struct MidiMsg
    {
        uint8_t cable;
        uint8_t status;
        uint8_t data1;
        uint8_t data2;
    };

    struct HidKeyMsg
    {
        uint8_t modifier;
        uint8_t keycode; // 0 means release
    };

    // Small bounded queues; on overflow we drop newest.
    static queue_t g_logQ;
    static queue_t g_midiQ;
    static queue_t g_hidQ;
    static bool g_queuesInited = false;
    static bool g_usbStarted = false;

    static void initQueuesOnce()
    {
        if (g_queuesInited)
        {
            return;
        }

        // CDC logs can be bursty (e.g. modules list). Keep a larger queue to avoid
        // truncation when many small Print::write() calls happen quickly.
        queue_init(&g_logQ, sizeof(LogChunk), 256);
        queue_init(&g_midiQ, sizeof(MidiMsg), 64);
        queue_init(&g_hidQ, sizeof(HidKeyMsg), 64);
        g_queuesInited = true;
    }

    static inline uint8_t cinFromStatus(uint8_t status)
    {
        switch (status & 0xF0)
        {
        case 0x80:
            return 0x8; // Note Off
        case 0x90:
            return 0x9; // Note On
        case 0xA0:
            return 0xA; // Poly Key Pressure
        case 0xB0:
            return 0xB; // Control Change
        case 0xC0:
            return 0xC; // Program Change (2 bytes) - not used here
        case 0xD0:
            return 0xD; // Channel Pressure (2 bytes) - not used here
        case 0xE0:
            return 0xE; // Pitch Bend
        default:
            return 0x0; // Misc/unsupported
        }
    }

    class UsbSerialPrint final : public Print
    {
    public:
        size_t write(uint8_t b) override
        {
            const uint core = get_core_num() & 1u;
            Pending &p = g_pending[core];

            p.data[p.len++] = b;
            if (p.len >= sizeof(p.data) || b == '\n')
            {
                flush(core);
            }
            return 1;
        }

        size_t write(const uint8_t *buffer, size_t size) override
        {
            const uint core = get_core_num() & 1u;
            flush(core);
            return usb::enqueueCdcWrite(buffer, size);
        }

    private:
        struct Pending
        {
            uint8_t len = 0;
            uint8_t data[64]{};
        };

        static Pending g_pending[2];

        static void flush(uint core)
        {
            Pending &p = g_pending[core & 1u];
            if (p.len == 0)
                return;
            usb::enqueueCdcWrite(p.data, p.len);
            p.len = 0;
        }
    };

    UsbSerialPrint::Pending UsbSerialPrint::g_pending[2];

    static UsbSerialPrint g_usbSerial;
}

Print &UsbSerial = g_usbSerial;

namespace usb
{
    // Serial input circular buffer
    static uint8_t inputBuffer[256];
    static size_t inputPos = 0;
    void processCommand(char *cmd);

    static void printlnOk(const char *msg = nullptr)
    {
        if (msg && msg[0])
        {
            UsbSerial.print(F("ok "));
            UsbSerial.println(msg);
        }
        else
        {
            UsbSerial.println(F("ok"));
        }
    }

    static void printlnErr(const char *msg)
    {
        UsbSerial.print(F("err "));
        UsbSerial.println(msg ? msg : "");
    }

    static bool parseInt(const char *s, long &out)
    {
        if (!s || !s[0])
            return false;
        char *end = nullptr;
        out = strtol(s, &end, 10);
        return end && *end == '\0';
    }

    static bool parseU16(const char *s, uint16_t &out)
    {
        long v;
        if (!parseInt(s, v))
            return false;
        if (v < 0 || v > 65535)
            return false;
        out = (uint16_t)v;
        return true;
    }

    static bool parseU8(const char *s, uint8_t &out)
    {
        long v;
        if (!parseInt(s, v))
            return false;
        if (v < 0 || v > 255)
            return false;
        out = (uint8_t)v;
        return true;
    }

    void init()
    {
        MappingManager::init();
        runtime_config::init();
        runtime_query::init();
        initQueuesOnce();

        if (g_usbStarted)
        {
            return;
        }

        // 1. Force reset of SerialTinyUSB state.
        //    If SerialTinyUSB thinks it's already valid, it won't re-add itself.
        //    This ensures we start with a clean slate.
        SerialTinyUSB.end();

        // 2. Start TinyUSBDevice.
        //    - Clears configuration
        //    - Sets Device Class to MISC/IAD (Required for CDC composite on Windows)
        //    - Adds SerialTinyUSB interface
        //    - Initializes USB hardware (connects to host)
        TinyUSBDevice.begin();

        // 3. Set Device Strings (used by callbacks)
        TinyUSBDevice.setManufacturerDescriptor("picontrol");
        TinyUSBDevice.setProductDescriptor("picontrol (CDC+MIDI+HID)");

        // 4. Add other interfaces (MIDI + HID)
        //    We add these after begin() (which triggers HW init), relying on the
        //    standard Adafruit TinyUSB pattern where interfaces are appended
        //    to the descriptor before the host reads the full configuration.

        // MIDI
        g_midi.begin();

        // HID keyboard
        g_hid.setReportDescriptor(g_hid_report_desc, sizeof(g_hid_report_desc));
        g_hid.setPollInterval(2);
        g_hid.setBootProtocol(1); // 1: Keyboard
        g_hid.begin();

        g_usbStarted = true;
    }

    // CDC command handler (newline-delimited). Intended for WebSerial / debug.
    // Commands:
    //   info
    //   map set <r> <c> <pid> <type> <d1> <d2>
    //   map del <r> <c> <pid>
    //   map list
    //   map clear
    //   map save
    //   map load
    //   autoupdate set <r> <c> <0|1> [intervalMs]
    //   autoupdate all <0|1> [intervalMs]
    //   rot set <r> <c> <0|1>
    //   rot all <0|1>
    void processCommand(char *cmd)
    {
        if (!cmd)
            return;

        // Tokenize in-place.
        const int MAX_ARGS = 10;
        const char *argv[MAX_ARGS];
        int argc = 0;
        char *p = cmd;
        while (*p && argc < MAX_ARGS)
        {
            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p)
                break;
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
            if (*p)
                *p++ = 0;
        }

        if (argc == 0)
            return;

        if (strcmp(argv[0], "info") == 0)
        {
            printlnOk("fw=picontrol version=1.0.0 proto=1");
            return;
        }

        if (strcmp(argv[0], "version") == 0)
        {
            printlnOk("1.0.0");
            return;
        }

        if (strcmp(argv[0], "map") == 0)
        {
            if (argc >= 2 && strcmp(argv[1], "set") == 0)
            {
                if (argc < 8)
                {
                    printlnErr("usage: map set r c pid type d1 d2");
                    return;
                }
                long r, c;
                uint8_t pid, type, d1, d2;
                if (!parseInt(argv[2], r) || !parseInt(argv[3], c) || !parseU8(argv[4], pid) ||
                    !parseU8(argv[5], type) || !parseU8(argv[6], d1) || !parseU8(argv[7], d2))
                {
                    printlnErr("bad args");
                    return;
                }
                MappingManager::updateMapping((int)r, (int)c, pid, (ActionType)type, d1, d2);
                printlnOk();
                return;
            }
            if (argc >= 2 && strcmp(argv[1], "del") == 0)
            {
                if (argc < 5)
                {
                    printlnErr("usage: map del r c pid");
                    return;
                }
                long r, c;
                uint8_t pid;
                if (!parseInt(argv[2], r) || !parseInt(argv[3], c) || !parseU8(argv[4], pid))
                {
                    printlnErr("bad args");
                    return;
                }
                const bool ok = MappingManager::deleteMapping((int)r, (int)c, pid);
                printlnOk(ok ? "deleted" : "notfound");
                return;
            }
            if (argc >= 2 && strcmp(argv[1], "list") == 0)
            {
                const int n = MappingManager::count();
                UsbSerial.print(F("ok count="));
                UsbSerial.println(n);
                for (int i = 0; i < n; i++)
                {
                    const ModuleMapping *m = MappingManager::getByIndex(i);
                    if (!m)
                        continue;

                    uint8_t d1 = 0, d2 = 0;
                    switch (m->type)
                    {
                    case ACTION_MIDI_NOTE:
                        d1 = m->target.midiNote.channel;
                        d2 = m->target.midiNote.noteNumber;
                        break;
                    case ACTION_MIDI_CC:
                        d1 = m->target.midiCC.channel;
                        d2 = m->target.midiCC.ccNumber;
                        break;
                    case ACTION_KEYBOARD:
                        d1 = m->target.keyboard.keycode;
                        d2 = m->target.keyboard.modifier;
                        break;
                    default:
                        break;
                    }

                    UsbSerial.print(F("map "));
                    UsbSerial.print(m->row);
                    UsbSerial.print(' ');
                    UsbSerial.print(m->col);
                    UsbSerial.print(' ');
                    UsbSerial.print(m->paramId);
                    UsbSerial.print(' ');
                    UsbSerial.print((uint8_t)m->type);
                    UsbSerial.print(' ');
                    UsbSerial.print(d1);
                    UsbSerial.print(' ');
                    UsbSerial.println(d2);
                }
                return;
            }
            if (argc >= 2 && strcmp(argv[1], "clear") == 0)
            {
                MappingManager::clearAll();
                printlnOk();
                return;
            }
            if (argc >= 2 && strcmp(argv[1], "save") == 0)
            {
                printlnOk(MappingManager::save() ? "saved" : "save_failed");
                return;
            }
            if (argc >= 2 && strcmp(argv[1], "load") == 0)
            {
                printlnOk(MappingManager::load() ? "loaded" : "load_failed");
                return;
            }

            printlnErr("unknown map cmd");
            return;
        }

        if (strcmp(argv[0], "autoupdate") == 0)
        {
            if (argc >= 2 && strcmp(argv[1], "set") == 0)
            {
                if (argc < 6)
                {
                    printlnErr("usage: autoupdate set r c 0|1 [intervalMs]");
                    return;
                }
                long r, c;
                uint8_t en;
                uint16_t interval = 0;
                if (!parseInt(argv[2], r) || !parseInt(argv[3], c) || !parseU8(argv[4], en))
                {
                    printlnErr("bad args");
                    return;
                }
                if (argc >= 6)
                {
                    // argv[5] exists if user provided it; but argc>=6 always here.
                    if (!parseU16(argv[5], interval))
                    {
                        // Allow omitted interval by passing empty (won't happen in tokenization)
                        interval = 0;
                    }
                }
                const bool ok = runtime_config::enqueueAutoupdate((int)r, (int)c, en != 0, interval);
                printlnOk(ok ? "queued" : "queue_full");
                return;
            }
            if (argc >= 2 && strcmp(argv[1], "all") == 0)
            {
                if (argc < 3)
                {
                    printlnErr("usage: autoupdate all 0|1 [intervalMs]");
                    return;
                }
                uint8_t en;
                uint16_t interval = 0;
                if (!parseU8(argv[2], en))
                {
                    printlnErr("bad args");
                    return;
                }
                if (argc >= 4)
                {
                    if (!parseU16(argv[3], interval))
                        interval = 0;
                }
                const bool ok = runtime_config::enqueueAutoupdateAll(en != 0, interval);
                printlnOk(ok ? "queued" : "queue_full");
                return;
            }
            printlnErr("unknown autoupdate cmd");
            return;
        }

        if (strcmp(argv[0], "rot") == 0)
        {
            if (argc >= 2 && strcmp(argv[1], "set") == 0)
            {
                if (argc < 5)
                {
                    printlnErr("usage: rot set r c 0|1");
                    return;
                }
                long r, c;
                uint8_t en;
                if (!parseInt(argv[2], r) || !parseInt(argv[3], c) || !parseU8(argv[4], en))
                {
                    printlnErr("bad args");
                    return;
                }
                const bool ok = runtime_config::enqueueRotationOverride((int)r, (int)c, en != 0);
                printlnOk(ok ? "queued" : "queue_full");
                return;
            }
            if (argc >= 2 && strcmp(argv[1], "all") == 0)
            {
                if (argc < 3)
                {
                    printlnErr("usage: rot all 0|1");
                    return;
                }
                uint8_t en;
                if (!parseU8(argv[2], en))
                {
                    printlnErr("bad args");
                    return;
                }
                const bool ok = runtime_config::enqueueRotationOverrideAll(en != 0);
                printlnOk(ok ? "queued" : "queue_full");
                return;
            }

            printlnErr("unknown rot cmd");
            return;
        }

        if (strcmp(argv[0], "modules") == 0)
        {
            if (argc >= 2 && strcmp(argv[1], "list") == 0)
            {
                const bool ok = runtime_query::enqueueListModules();
                printlnOk(ok ? "queued" : "queue_full");
                return;
            }
            printlnErr("usage: modules list");
            return;
        }

        if (strcmp(argv[0], "param") == 0)
        {
            if (argc >= 2 && strcmp(argv[1], "set") == 0)
            {
                // param set <r> <c> <pid> <datatype> <value>
                // datatype: 0=int, 1=float, 2=bool
                if (argc < 7)
                {
                    printlnErr("usage: param set r c pid datatype value");
                    return;
                }
                long r, c;
                uint8_t pid, dt;
                if (!parseInt(argv[2], r) || !parseInt(argv[3], c) ||
                    !parseU8(argv[4], pid) || !parseU8(argv[5], dt))
                {
                    printlnErr("bad args");
                    return;
                }
                const bool ok = runtime_config::enqueueSetParameter((int)r, (int)c, pid, dt, argv[6]);
                printlnOk(ok ? "queued" : "queue_full");
                return;
            }
            printlnErr("usage: param set r c pid datatype value");
            return;
        }

        printlnErr("unknown cmd");
    }

    void task()
    {
        // Must be called frequently from core 0.
        TinyUSBDevice.task();

        // Flush CDC log queue
        if (tud_cdc_connected())
        {
            LogChunk chunk;
            while (queue_try_peek(&g_logQ, &chunk))
            {
                if (chunk.len == 0)
                {
                    (void)queue_try_remove(&g_logQ, &chunk);
                    continue;
                }

                // Avoid partial writes: tud_cdc_write() can return short if the
                // USB TX buffer is full. If we drop the remainder, log lines get
                // randomly truncated (e.g. missing param lines in modules list).
                if (tud_cdc_write_available() < chunk.len)
                {
                    break;
                }

                (void)queue_try_remove(&g_logQ, &chunk);
                tud_cdc_write(chunk.data, chunk.len);
            }
            tud_cdc_write_flush();
        }
        else
        {
            // If host is not connected, just drain to avoid unbounded growth
            LogChunk chunk;
            while (queue_try_remove(&g_logQ, &chunk))
            {
            }
        }

        // Serial input handling
        if (tud_cdc_available())
        {
            uint8_t buf[64];
            uint32_t count = tud_cdc_read(buf, sizeof(buf));
            for (uint32_t i = 0; i < count; i++)
            {
                char c = (char)buf[i];
                if (c == '\n' || c == '\r')
                {
                    if (inputPos > 0)
                    {
                        inputBuffer[inputPos] = 0; // Null terminate
                        processCommand((char *)inputBuffer);
                        inputPos = 0;
                    }
                }
                else if (inputPos < sizeof(inputBuffer) - 1)
                {
                    inputBuffer[inputPos++] = c;
                }
            }
        }

        // Drain MIDI
        if (tud_midi_mounted())
        {
            MidiMsg m;
            while (queue_try_remove(&g_midiQ, &m))
            {
                uint8_t cin = cinFromStatus(m.status);
                uint8_t packet[4] = {(uint8_t)((m.cable << 4) | cin), m.status, m.data1, m.data2};
                tud_midi_stream_write(0, packet, 4);
            }
        }

        // Drain HID keyboard (one message per task tick to ensure host sees press/release)
        if (tud_hid_ready())
        {
            HidKeyMsg k;
            if (queue_try_remove(&g_hidQ, &k))
            {
                uint8_t keycodes[6] = {0, 0, 0, 0, 0, 0};
                if (k.keycode)
                {
                    keycodes[0] = k.keycode;
                }
                tud_hid_keyboard_report(0, k.modifier, keycodes);
            }
        }
    }

    bool cdcConnected()
    {
        return tud_cdc_connected();
    }

    size_t enqueueCdcWrite(const uint8_t *data, size_t len)
    {
        initQueuesOnce();

        size_t written = 0;
        while (written < len)
        {
            LogChunk chunk;
            size_t n = len - written;
            if (n > sizeof(chunk.data))
                n = sizeof(chunk.data);

            chunk.len = (uint16_t)n;
            memcpy(chunk.data, data + written, n);

            if (!queue_try_add(&g_logQ, &chunk))
            {
                // Drop if queue is full.
                break;
            }
            written += n;
        }
        return written;
    }

    static bool enqueueMidi3(uint8_t cable, uint8_t status, uint8_t d1, uint8_t d2)
    {
        initQueuesOnce();

        MidiMsg m{cable, status, d1, d2};
        return queue_try_add(&g_midiQ, &m);
    }

    bool sendMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, uint8_t cable)
    {
        if (channel > 15)
            channel &= 0x0F;
        return enqueueMidi3(cable, (uint8_t)(0x90 | channel), (uint8_t)(note & 0x7F), (uint8_t)(velocity & 0x7F));
    }

    bool sendMidiNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, uint8_t cable)
    {
        if (channel > 15)
            channel &= 0x0F;
        return enqueueMidi3(cable, (uint8_t)(0x80 | channel), (uint8_t)(note & 0x7F), (uint8_t)(velocity & 0x7F));
    }

    bool sendMidiCC(uint8_t channel, uint8_t controller, uint8_t value, uint8_t cable)
    {
        if (channel > 15)
            channel &= 0x0F;
        return enqueueMidi3(cable, (uint8_t)(0xB0 | channel), (uint8_t)(controller & 0x7F), (uint8_t)(value & 0x7F));
    }

    bool sendKeypress(uint8_t hidKeycode, uint8_t modifier)
    {
        initQueuesOnce();

        HidKeyMsg press{modifier, hidKeycode};
        HidKeyMsg release{0, 0};

        // Best effort: if press enqueues but release doesn't, host may see stuck key.
        // So require both.
        if (!queue_try_add(&g_hidQ, &press))
            return false;
        if (!queue_try_add(&g_hidQ, &release))
            return false;
        return true;
    }
}
