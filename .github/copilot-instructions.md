# Copilot instructions (picontrol)

## Big picture
- This repo has two parts:
  - Firmware for Raspberry Pi Pico (PlatformIO + Earle Philhower Arduino core): `src/`
  - Web UI (Vue 3 + Vite, uses WebSerial): `webcontrol/`
- The device exposes a USB composite (CDC serial + MIDI + HID keyboard). All host control from the web UI is via newline-delimited CDC text commands.

## Firmware architecture (core0 vs core1)
- `src/main.cpp`: core0 owns USB. It calls `usb::task()` frequently.
- `src/module_task.cpp`: core1 owns “module grid” logic (`setup1()/loop1()`). It scans ports, polls modules, applies mappings, and prints status.
- Critical invariant: do not call TinyUSB/Adafruit TinyUSB APIs from core1. Route everything through `usb::` helpers in `src/usb_device.*`.
  - Use `UsbSerial` for logging from either core (it enqueues to a core0 queue).
  - For cross-core requests from CDC → core1, use the queue-based helpers:
    - `runtime_query::enqueueListModules()` / `tryDequeue()` (`src/runtime_query.*`)
    - `runtime_config::*` enqueue/tryDequeue APIs (`src/runtime_config.*`)

## Module/port protocol and data flow
- Physical ports are scanned and auto-configured in `src/port.cpp`:
  - Detection rule: exactly one of the two candidate pins is HIGH; that pin is module TX (host RX) and also determines `ModuleOrientation`.
  - Module on-wire framing: `0xAA, commandId, payloadLenLo, payloadLenHi, payload..., checksum(sum of prior bytes)`.
- On-wire structs are tightly packed (`#pragma pack(push, 1)`) in `src/common.hpp`. Keep them packed and update `static_assert`s when changing payload sizes.

## CDC text protocol (used by the web UI)
- Parsed on core0 in `usb::processCommand()` (`src/usb_device.cpp`). Commands are newline-delimited.
- Commands expected by the UI (`webcontrol/src/services/serial.ts` / `protocol.ts`):
  - `modules list` → queues an async dump; core1 prints:
    - `ok ports rows=<R> cols=<C>`
    - `port r=<r> c=<c> configured=<0|1> hasModule=<0|1> orientation=<n>`
    - `module r=<r> c=<c> type=<n> caps=<n> name="..." mfg="..." fw="..." params=<n> szr=<n> szc=<n> plr=<n> plc=<n>`
    - `param r=<r> c=<c> pid=<n> dt=<n> name="..." min=<...> max=<...> value=<...>`
    - `ok modules done`
  - Mapping commands (persisted): `map set ...`, `map del ...`, `map list`, `map clear`, `map save`, `map load`
  - Runtime controls: `autoupdate set ...` / `autoupdate all ...`, `rot set ...` / `rot all ...`, `param set r c pid datatype value`
- If you change any output line format, update the parsers in `webcontrol/src/services/protocol.ts`.

## Persistence
- Mapping persistence uses LittleFS and a binary file in `src/mapping.cpp`:
  - File: `/mappings.bin` with header/checksum (`MAGIC_MAP1`), max 32 entries.
- Rotation overrides persist via LittleFS in `src/module_config_manager.cpp`:
  - File: `/modconfig.bin` (`MAGIC_CFG1`)

## Webcontrol conventions
- Web UI runs in “real” mode (WebSerial) or “mock” mode (interprets a subset of commands) via `state.env.mode`.
  - Real transport: `webcontrol/src/services/serial.ts`
  - Command routing / mock emulation: `webcontrol/src/services/router.ts`
- Dev workflow uses Bun (see `webcontrol/README.md`): `bun install`, `bun dev`, `bun run build`.

## Common workflows
- Firmware (PlatformIO):
  - Build: `platformio run -e pico`
  - Upload: `platformio run -e pico -t upload`
- When adding new cross-core functionality:
  - Add a queue request type in `src/runtime_query.*` or `src/runtime_config.*`
  - Enqueue from `usb_device.cpp` (core0), handle in `module_task.cpp` (core1)
  - Emit CDC lines through `UsbSerial` and keep them parseable by the UI
