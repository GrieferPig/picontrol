# piControl Codebase Guide

## Architecture Overview

piControl is a **dual-core Raspberry Pi Pico firmware + Vue web interface** for a modular MIDI/HID controller system. Physical control modules (faders, knobs, buttons) connect via UART and send parameter updates that get mapped to MIDI/keyboard actions.

### Core Components

1. **Firmware (PlatformIO/Arduino)** - RP2040 dual-core embedded system
2. **Web Interface (Vue 3 + TypeScript + Vite)** - Browser-based configuration UI via Web Serial API

## Critical Architecture Patterns

### Dual-Core Design (RP2040)

**Core 0** ([main.cpp](src/main.cpp)): USB tasks only (TinyUSB CDC, MIDI, HID)  
**Core 1** ([module_task.cpp](src/module_task.cpp)): Module communication, message routing, mapping execution

- Communication between cores uses **Pico SDK queues** (thread-safe, lock-free)
- Examples: `runtime_config`, `runtime_query` namespaces with `enqueue*/tryDequeue*` functions
- Never call TinyUSB APIs from Core 1 - queue messages to Core 0 instead

### Module Communication Protocol

Modules connect via **PIO-based UART** ([InterruptSerialPIO](src/InterruptSerialPIO.h)) at 115200 baud. Each physical port has dedicated GPIO pins defined in [boardconfig.h](src/boardconfig.h).

**Message structure** (see [common.hpp](src/common.hpp)):
- Binary framed protocol with `ModuleMessage` struct
- Commands: `PING`, `GET_PROPERTIES`, `SET_PARAMETER`, `GET_PARAMETER`, `SET_MAPPINGS`, etc.
- Responses include `ModuleStatus` (OK/ERROR/UNSUPPORTED)

**Key workflow**:
1. Module connects → `port_connected` event → firmware sends `GET_PROPERTIES`
2. Module responds with capabilities (type, parameters, name) → stored in `Port` struct
3. Web UI queries via `modules list` CDC command → firmware streams module data
4. User creates mappings → firmware syncs them to module flash via `SET_MAPPINGS`

### Mapping System

**Purpose**: Convert module parameter changes → MIDI/HID actions

[mapping.h](src/mapping.h) defines `ModuleMapping` with:
- Source: `row`, `col`, `parameterId` 
- Target: `ActionType` (MIDI_NOTE, MIDI_CC, KEYBOARD, PITCH_BEND, MOD_WHEEL)
- Optional: `Curve` for non-linear response (quadratic Bézier, see [curve.h](src/curve.h))

**Storage**: Mappings persist in Pico's LittleFS (512KB partition). Changes in web UI → `map set` CDC command → `MappingManager::updateMapping()` → `enqueueSyncMapping()` → Core 1 sends `SET_MAPPINGS` to module.

### Web Serial Command Interface

[usb_device.cpp](src/usb_device.cpp) `processCommand()` parses text commands from Web Serial:
- `modules list` - Enumerate all detected modules with parameters
- `map set r c pid type d1 d2` - Create/update mapping
- `map set_curve r c pid <hex>` - Set curve (hex-encoded binary)
- `map del r c pid` - Delete mapping
- `map list` - List all mappings
- `param set r c pid <value>` - Set module parameter value

Responses: `ok [data]` or `err [message]`

**Events** (sent asynchronously):
- `event port_connected r=X c=Y orientation=Z`
- `event module_found r=X c=Y type=T ... params=N ...`
- `event mappings_loaded r=X c=Y`

## Development Workflows

### Building & Flashing Firmware

```bash
# From workspace root (Windows - adjust paths for Unix)
C:\.platformio\penv\Scripts\platformio.exe run --target upload --environment pico
```

Uses PlatformIO with [earlephilhower Arduino core](https://github.com/earlephilhower/arduino-pico). Build flags in [platformio.ini](platformio.ini):
- `-DUSE_TINYUSB` - Enable TinyUSB composite device
- `-DPICONTROL_USE_ADAFRUIT_TINYUSB` - Use Adafruit TinyUSB library wrappers

### Running Web Interface

```bash
cd webcontrol
bun install  # First time only
bun dev      # Hot reload on http://localhost:5173
```

**Browser setup**: Requires Chrome/Edge for Web Serial API. Click "Connect" → select Pico's CDC port → UI auto-syncs with firmware.

### Adding a New Module Type

1. **Define in [common.hpp](src/common.hpp)**: Add to `ModuleType` enum
2. **Handle in [module_task.cpp](src/module_task.cpp)**: Update `handleResponseMessage()` if special logic needed
3. **UI updates**: Add type-specific rendering in [Grid.vue](webcontrol/src/components/Grid.vue), [ParamPanel.vue](webcontrol/src/components/ParamPanel.vue)

### Adding a New Mapping Type

1. **Define action**: Add to `ActionType` enum in [module_mapping_config.h](src/module_mapping_config.h)
2. **Implement execution**: Edit `applyMappingAction()` in [module_task.cpp](src/module_task.cpp)
3. **USB queue call**: Use `usb::sendMidi*()` or `usb::sendKey*()` helpers from [usb_device.h](src/usb_device.h)
4. **UI support**: Update [MappingEditor.vue](webcontrol/src/components/MappingEditor.vue) mapping form

## Code Conventions

### Logging

**Never use `Serial.print()`** - it conflicts with hardware UART. Use `UsbSerial.print()` instead (routes to USB CDC).

```cpp
// ✅ Correct
UsbSerial.println("Module connected");

// ❌ Wrong - breaks hardware serial
Serial.println("Module connected");
```

### Queue-Based Cross-Core Communication

Pattern from [runtime_config.h](src/runtime_config.h):

```cpp
// Core 0 (USB) wants to trigger action on Core 1
bool enqueueAutoupdate(int row, int col, bool enable, uint16_t intervalMs);

// Core 1 polls for requests
bool tryDequeueAutoupdate(AutoupdateRequest &out);
```

Uses Pico SDK `queue_t` with `queue_try_add`/`queue_try_remove` (non-blocking).

### Module Parameter Types

[common.hpp](src/common.hpp) defines 3 types:
- `PARAM_TYPE_INT` - int32_t 
- `PARAM_TYPE_FLOAT` - float (IEEE 754)
- `PARAM_TYPE_BOOL` - uint8_t (0/1)

Access via union `ModuleParameterValue`. Check `ModuleParameter::dataType` before accessing union members.

### State Management (Web UI)

Single reactive store ([useStore.ts](webcontrol/src/composables/useStore.ts)):
- `state.modules` - Keyed by `"r,c"`, contains detected modules
- `state.ports` - Physical port configuration matrix
- `state.mappings` - All active parameter→action mappings
- `state.selected` - Currently selected module/param in UI

Protocol handler ([protocol.ts](webcontrol/src/services/protocol.ts)) parses CDC events/responses and mutates store.

## Key Files Reference

- [main.cpp](src/main.cpp) - Core 0 entry (USB only)
- [module_task.cpp](src/module_task.cpp) - Core 1 entry (setup1/loop1), module message router
- [port.cpp](src/port.cpp)/[port.h](src/port.h) - Port management, message queuing to/from modules
- [mapping.cpp](src/mapping.cpp) - Mapping persistence and lookup
- [usb_device.cpp](src/usb_device.cpp) - TinyUSB composite device, CDC command parser
- [boardconfig.h](src/boardconfig.h) - 3×3 port GPIO pin matrix (modify for different layouts)
- [webcontrol/src/services/serial.ts](webcontrol/src/services/serial.ts) - Web Serial connection handler
- [webcontrol/src/services/protocol.ts](webcontrol/src/services/protocol.ts) - CDC protocol parser

## Debugging Tips

- **Check terminal output**: PlatformIO uploads often show linker/build issues not visible in IDE
- **Web Serial logs**: [LogPanel.vue](webcontrol/src/components/LogPanel.vue) shows TX/RX - verify command format
- **Module not detected**: Check `boardconfig.h` pin assignments and physical wiring (TX↔RX crossover)
- **Crash/hang after module change**: Core 1 might be stuck waiting for response - add timeouts to `propsRequestAttempts` logic
