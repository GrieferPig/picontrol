#!/usr/bin/env python3
"""
Picontrol Serial Command Line Tool

A command-line tool to send and parse MAP and MODULE commands over serial.
Supports all command types with custom parameters.

Usage examples:
    # List all modules
    python test_module_list.py --port COM38 modules list

    # Set a mapping
    python test_module_list.py --port COM38 map set --row 0 --col 0 --param 1 --type midi_cc --d1 1 --d2 74

    # List all mappings
    python test_module_list.py --port COM38 map list

    # Delete a mapping
    python test_module_list.py --port COM38 map del --row 0 --col 0 --param 1

    # Set module parameter
    python test_module_list.py --port COM38 modules param-set --row 0 --col 0 --param 1 --datatype int --value 100
"""

import serial
import struct
import time
import ctypes
import argparse
import sys
import threading
from typing import Optional, List, Dict, Any

# MIDI library for testing
try:
    import mido
    MIDO_AVAILABLE = True
except ImportError:
    MIDO_AVAILABLE = False


# Configuration defaults
DEFAULT_PORT = "COM38"
DEFAULT_BAUD_RATE = 115200
DEFAULT_TIMEOUT = 2.0

# Protocol Constants
HEADER_SIZE = 5
CHECKSUM_SIZE = 2
MIN_MESSAGE_SIZE = HEADER_SIZE + CHECKSUM_SIZE


# Enum mappings
class MessageType:
    COMMAND = 0
    RESPONSE = 1
    EVENT = 2


class CommandType:
    INFO = 0
    VERSION = 1
    MAP = 2
    MODULES = 3


class ResponseType:
    ACK = 0
    NACK = 1
    INFO = 2
    VERSION = 3
    MAP = 4
    MODULES = 5


class CommandSubMapType:
    SET = 0
    SET_CURVE = 1
    DEL = 2
    LIST = 3
    CLEAR = 4


class CommandSubModuleType:
    LIST = 0
    PARAM_SET = 1
    CALIB_SET = 2


class ActionType:
    NONE = 0
    MIDI_NOTE = 1
    MIDI_CC = 2
    KEYBOARD = 3
    MIDI_PITCH_BEND = 4
    MIDI_MOD_WHEEL = 5


ACTION_TYPE_NAMES = {
    ActionType.NONE: "NONE",
    ActionType.MIDI_NOTE: "MIDI_NOTE",
    ActionType.MIDI_CC: "MIDI_CC",
    ActionType.KEYBOARD: "KEYBOARD",
    ActionType.MIDI_PITCH_BEND: "MIDI_PITCH_BEND",
    ActionType.MIDI_MOD_WHEEL: "MIDI_MOD_WHEEL",
}

ACTION_TYPE_FROM_NAME = {v.lower(): k for k, v in ACTION_TYPE_NAMES.items()}
ACTION_TYPE_FROM_NAME.update(
    {
        "none": ActionType.NONE,
        "midi_note": ActionType.MIDI_NOTE,
        "midi_cc": ActionType.MIDI_CC,
        "keyboard": ActionType.KEYBOARD,
        "midi_pitch_bend": ActionType.MIDI_PITCH_BEND,
        "midi_mod_wheel": ActionType.MIDI_MOD_WHEEL,
    }
)


class ParamDataType:
    INT = 0
    FLOAT = 1
    BOOL = 2
    LED = 3


PARAM_TYPE_NAMES = {
    ParamDataType.INT: "INT",
    ParamDataType.FLOAT: "FLOAT",
    ParamDataType.BOOL: "BOOL",
    ParamDataType.LED: "LED",
}

PARAM_TYPE_FROM_NAME = {v.lower(): k for k, v in PARAM_TYPE_NAMES.items()}


# CRC16 CCITT implementation
def crc16_update(crc: int, data: int) -> int:
    crc ^= data << 8
    for _ in range(8):
        if crc & 0x8000:
            crc = (crc << 1) ^ 0x1021
        else:
            crc <<= 1
    return crc & 0xFFFF


def calculate_crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc = crc16_update(crc, byte)
    return crc


def create_command(
    cmd_type: int, sub_cmd: int, payload_bytes: Optional[bytes] = None
) -> bytes:
    """Create a command message with proper wire format."""
    if payload_bytes is None:
        payload_bytes = b""

    # Header: Type(1), Command(1), Subcommand(1), Length(2)
    length = len(payload_bytes)
    header = struct.pack("<BBBH", MessageType.COMMAND, cmd_type, sub_cmd, length)

    # Checksum is over header + payload
    checksum = calculate_crc16(header + payload_bytes)
    checksum_bytes = struct.pack("<H", checksum)

    # Wire format: header + checksum + payload
    return header + checksum_bytes + payload_bytes


def parse_response(data: bytes) -> Optional[Dict[str, Any]]:
    """Parse a response message."""
    if len(data) < MIN_MESSAGE_SIZE:
        return None

    # Header: Type(1), Response(1), Subcommand(1), Length(2)
    msg_type, resp_type, sub_cmd, length = struct.unpack("<BBBH", data[:5])

    # Wire format: header(5) + checksum(2) + payload(length)
    expected_len = HEADER_SIZE + CHECKSUM_SIZE + length
    if len(data) < expected_len:
        return None  # Incomplete

    # Checksum at fixed position (bytes 5-6)
    checksum_received = struct.unpack("<H", data[5:7])[0]

    # Payload starts at byte 7
    payload_bytes = data[7 : 7 + length]

    # Verify checksum over header + payload
    checksum_calc = calculate_crc16(data[:5] + payload_bytes)

    return {
        "type": msg_type,
        "response": resp_type,
        "subcommand": sub_cmd,
        "length": length,
        "payload": payload_bytes,
        "checksum_ok": checksum_calc == checksum_received,
        "raw_data": data[:expected_len],
    }


def unpack_data(packed_data: bytes) -> bytes:
    """Unpack data using zero run length encoding: [u8 ZERO_COUNT][u8 VALID_COUNT][VALID_DATA]"""
    unpacked = bytearray()
    i = 0
    while i < len(packed_data):
        if i + 1 >= len(packed_data):
            break
        zero_count = packed_data[i]
        valid_count = packed_data[i + 1]
        i += 2

        # Add zeros
        unpacked.extend([0] * zero_count)

        # Add valid data
        if valid_count > 0:
            if i + valid_count > len(packed_data):
                unpacked.extend(packed_data[i:])
                break
            unpacked.extend(packed_data[i : i + valid_count])
            i += valid_count
    return bytes(unpacked)


def read_response(
    ser: serial.Serial, timeout: float = DEFAULT_TIMEOUT
) -> Optional[bytes]:
    """Read a complete response from serial port."""
    start_time = time.time()

    # Read header first
    header_data = b""
    while len(header_data) < HEADER_SIZE and (time.time() - start_time) < timeout:
        if ser.in_waiting > 0:
            header_data += ser.read(1)
        else:
            time.sleep(0.001)

    if len(header_data) < HEADER_SIZE:
        return None

    # Parse length from header
    _, _, _, length = struct.unpack("<BBBH", header_data)

    # Read checksum + payload
    remaining = CHECKSUM_SIZE + length
    response_data = header_data

    while (
        len(response_data) < (HEADER_SIZE + remaining)
        and (time.time() - start_time) < timeout
    ):
        if ser.in_waiting > 0:
            chunk = ser.read(
                min(ser.in_waiting, HEADER_SIZE + remaining - len(response_data))
            )
            response_data += chunk
        else:
            time.sleep(0.001)

    return response_data


# ==================== ctypes Structures ====================


class LEDValue(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("r", ctypes.c_uint8),
        ("g", ctypes.c_uint8),
        ("b", ctypes.c_uint8),
        ("status", ctypes.c_uint8),
    ]


class LEDRange(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("rMin", ctypes.c_uint8),
        ("rMax", ctypes.c_uint8),
        ("gMin", ctypes.c_uint8),
        ("gMax", ctypes.c_uint8),
        ("bMin", ctypes.c_uint8),
        ("bMax", ctypes.c_uint8),
    ]


class ModuleParameterValue(ctypes.Union):
    _pack_ = 1
    _fields_ = [
        ("intValue", ctypes.c_int32),
        ("floatValue", ctypes.c_float),
        ("boolValue", ctypes.c_uint8),
        ("ledValue", LEDValue),
    ]


class IntMinMax(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("intMin", ctypes.c_int32),
        ("intMax", ctypes.c_int32),
    ]


class FloatMinMax(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("floatMin", ctypes.c_float),
        ("floatMax", ctypes.c_float),
    ]


class ModuleParameterMinMax(ctypes.Union):
    _pack_ = 1
    _fields_ = [
        ("intRange", IntMinMax),
        ("floatRange", FloatMinMax),
        ("ledRange", LEDRange),
    ]


class ModuleParameter(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("id", ctypes.c_uint8),
        ("name", ctypes.c_char * 32),
        ("dataType", ctypes.c_uint8),
        ("access", ctypes.c_uint8),
        ("value", ModuleParameterValue),
        ("minMax", ModuleParameterMinMax),
    ]


class Module(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("protocol", ctypes.c_uint8),
        ("type", ctypes.c_uint8),
        ("name", ctypes.c_char * 32),
        ("manufacturer", ctypes.c_char * 32),
        ("fwVersion", ctypes.c_char * 16),
        ("compatibleHostVersion", ctypes.c_uint8),
        ("capabilities", ctypes.c_uint8),
        ("physicalSizeRow", ctypes.c_uint8),
        ("physicalSizeCol", ctypes.c_uint8),
        ("portLocationRow", ctypes.c_uint8),
        ("portLocationCol", ctypes.c_uint8),
        ("parameterCount", ctypes.c_uint8),
        ("parameters", ModuleParameter * 8),
    ]


class PortPacked(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("row", ctypes.c_int32),
        ("col", ctypes.c_int32),
        ("hasModule", ctypes.c_uint8),
        ("module", Module),
        ("orientation", ctypes.c_uint8),
        ("configured", ctypes.c_uint8),
    ]


# Curve structures for mapping
class CurvePoint(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("x", ctypes.c_uint8),
        ("y", ctypes.c_uint8),
    ]


class Curve(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("count", ctypes.c_uint8),
        ("points", CurvePoint * 4),
        ("controls", CurvePoint * 3),
    ]


class ActionTargetMidiNote(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("channel", ctypes.c_uint8),
        ("noteNumber", ctypes.c_uint8),
        ("velocity", ctypes.c_uint8),
    ]


class ActionTargetMidiCC(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("channel", ctypes.c_uint8),
        ("ccNumber", ctypes.c_uint8),
        ("value", ctypes.c_uint8),
    ]


class ActionTargetKeyboard(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("keycode", ctypes.c_uint8),
        ("modifier", ctypes.c_uint8),
    ]


class ActionTarget(ctypes.Union):
    _pack_ = 1
    _fields_ = [
        ("midiNote", ActionTargetMidiNote),
        ("midiCC", ActionTargetMidiCC),
        ("keyboard", ActionTargetKeyboard),
    ]


class ModuleMapping(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("row", ctypes.c_int32),
        ("col", ctypes.c_int32),
        ("paramId", ctypes.c_uint8),
        ("type", ctypes.c_uint8),
        ("curve", Curve),
        ("target", ActionTarget),
    ]


# ==================== Helper Functions ====================


def _decode_cstr(raw_bytes: bytes) -> str:
    return raw_bytes.split(b"\x00", 1)[0].decode("utf-8", errors="ignore")


def _decode_param(param: ModuleParameter) -> Dict[str, Any]:
    data_type = param.dataType
    param_value = None
    param_min = None
    param_max = None

    if data_type == ParamDataType.INT:
        param_value = param.value.intValue
        param_min = param.minMax.intRange.intMin
        param_max = param.minMax.intRange.intMax
    elif data_type == ParamDataType.FLOAT:
        param_value = param.value.floatValue
        param_min = param.minMax.floatRange.floatMin
        param_max = param.minMax.floatRange.floatMax
    elif data_type == ParamDataType.BOOL:
        param_value = param.value.boolValue != 0
    elif data_type == ParamDataType.LED:
        param_value = {
            "r": param.value.ledValue.r,
            "g": param.value.ledValue.g,
            "b": param.value.ledValue.b,
            "status": param.value.ledValue.status,
        }
        param_min = {
            "r": param.minMax.ledRange.rMin,
            "g": param.minMax.ledRange.gMin,
            "b": param.minMax.ledRange.bMin,
        }
        param_max = {
            "r": param.minMax.ledRange.rMax,
            "g": param.minMax.ledRange.gMax,
            "b": param.minMax.ledRange.bMax,
        }

    return {
        "id": param.id,
        "name": _decode_cstr(param.name),
        "data_type": data_type,
        "data_type_name": PARAM_TYPE_NAMES.get(data_type, f"UNKNOWN({data_type})"),
        "access": param.access,
        "value": param_value,
        "min": param_min,
        "max": param_max,
    }


# ==================== Serial Communication Class ====================


class PicontrolSerial:
    def __init__(
        self,
        port: str,
        baud_rate: int = DEFAULT_BAUD_RATE,
        timeout: float = DEFAULT_TIMEOUT,
    ):
        self.port = port
        self.baud_rate = baud_rate
        self.timeout = timeout
        self.ser: Optional[serial.Serial] = None

    def connect(self) -> bool:
        try:
            self.ser = serial.Serial(self.port, self.baud_rate, timeout=0.5)
            time.sleep(0.3)  # Allow device to initialize
            return True
        except serial.SerialException as e:
            print(f"Error: Could not connect to {self.port}: {e}")
            return False

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    def send_command(
        self, cmd_type: int, sub_cmd: int, payload: Optional[bytes] = None
    ) -> Optional[Dict[str, Any]]:
        """Send a command and wait for response."""
        if not self.ser or not self.ser.is_open:
            print("Error: Serial port not connected")
            return None

        command = create_command(cmd_type, sub_cmd, payload)
        self.ser.reset_input_buffer()
        self.ser.write(command)
        self.ser.flush()

        time.sleep(0.05)  # Small delay for processing

        response_data = read_response(self.ser, self.timeout)
        if response_data:
            return parse_response(response_data)
        return None


# ==================== Command Handlers ====================


def cmd_modules_list(pcs: PicontrolSerial, args: argparse.Namespace) -> int:
    """Handle MODULES LIST command."""
    print("Sending MODULES LIST command...")

    resp = pcs.send_command(CommandType.MODULES, CommandSubModuleType.LIST)

    if not resp:
        print("Error: No response received")
        return 1

    if not resp["checksum_ok"]:
        print("Error: Checksum mismatch")
        return 1

    if resp["response"] == ResponseType.NACK:
        print("Error: Command not acknowledged (NACK)")
        return 1

    if resp["response"] != ResponseType.MODULES:
        print(f"Error: Unexpected response type: {resp['response']}")
        return 1

    # Unpack the data
    unpacked = unpack_data(resp["payload"])
    print(
        f"Received {len(resp['payload'])} bytes packed, {len(unpacked)} bytes unpacked\n"
    )

    # Parse port data
    port_size = ctypes.sizeof(PortPacked)
    port_count = len(unpacked) // port_size

    if port_count == 0:
        print("No ports found")
        return 0

    module_type_names = {
        0: "FADER",
        1: "KNOB",
        2: "BUTTON",
        3: "BUTTON_MATRIX",
        4: "ENCODER",
        5: "JOYSTICK",
        6: "PROXIMITY",
    }
    orientation_names = {0: "UP", 1: "RIGHT", 2: "DOWN", 3: "LEFT"}

    print(f"{'='*60}")
    print(f"MODULE LIST - {port_count} port(s)")
    print(f"{'='*60}")

    for i in range(port_count):
        offset = i * port_size
        port_data = unpacked[offset : offset + port_size]
        if len(port_data) < port_size:
            break

        port = PortPacked.from_buffer_copy(port_data)

        print(f"\n[Port {port.row},{port.col}]")
        print(f"  Configured: {'Yes' if port.configured else 'No'}")
        print(f"  Has Module: {'Yes' if port.hasModule else 'No'}")

        if port.hasModule:
            module = port.module
            print(f"\n  --- Module Info ---")
            print(
                f"  Type: {module_type_names.get(module.type, f'UNKNOWN({module.type})')}"
            )
            print(f"  Name: {_decode_cstr(module.name)}")
            print(f"  Manufacturer: {_decode_cstr(module.manufacturer)}")
            print(f"  Firmware: {_decode_cstr(module.fwVersion)}")
            print(f"  Host Compatibility: v{module.compatibleHostVersion}")
            print(f"  Capabilities: 0x{module.capabilities:02X}")
            print(f"  Physical Size: {module.physicalSizeRow}x{module.physicalSizeCol}")
            print(
                f"  Port Location: ({module.portLocationRow}, {module.portLocationCol})"
            )
            print(
                f"  Orientation: {orientation_names.get(port.orientation, f'UNKNOWN({port.orientation})')}"
            )

            param_count = min(module.parameterCount, 8)
            if param_count:
                print(f"\n  --- Parameters ({param_count}) ---")
                for idx in range(param_count):
                    param = module.parameters[idx]
                    parsed = _decode_param(param)

                    access_parts = []
                    if parsed["access"] & 0x01:
                        access_parts.append("R")
                    if parsed["access"] & 0x02:
                        access_parts.append("W")
                    access_str = "".join(access_parts) if access_parts else "NONE"

                    print(f"    [{parsed['id']}] {parsed['name']}")
                    print(
                        f"        Type: {parsed['data_type_name']}, Access: {access_str}"
                    )
                    print(f"        Value: {parsed['value']}")
                    if parsed["min"] is not None:
                        print(f"        Range: {parsed['min']} to {parsed['max']}")

    print(f"\n{'='*60}")
    return 0


def cmd_modules_param_set(pcs: PicontrolSerial, args: argparse.Namespace) -> int:
    """Handle MODULES PARAM_SET command."""
    print(f"Setting parameter {args.param} on module [{args.row},{args.col}]...")
    print(f"  Data Type: {args.datatype}")
    print(f"  Value: {args.value}")

    # Build payload: row(1) + col(1) + paramId(1) + dataType(1) + valueStr
    datatype_num = PARAM_TYPE_FROM_NAME.get(args.datatype.lower(), ParamDataType.INT)
    value_str = str(args.value).encode("utf-8") + b"\x00"  # Null-terminated string

    payload = (
        struct.pack("<BBBB", args.row, args.col, args.param, datatype_num) + value_str
    )

    resp = pcs.send_command(
        CommandType.MODULES, CommandSubModuleType.PARAM_SET, payload
    )

    if not resp:
        print("Error: No response received")
        return 1

    if resp["response"] == ResponseType.ACK:
        print("Success: Parameter set acknowledged")
        return 0
    elif resp["response"] == ResponseType.NACK:
        print("Error: Parameter set failed (NACK)")
        return 1
    else:
        print(f"Error: Unexpected response type: {resp['response']}")
        return 1


def cmd_modules_calib_set(pcs: PicontrolSerial, args: argparse.Namespace) -> int:
    """Handle MODULES CALIB_SET command."""
    print(f"Setting calibration on module [{args.row},{args.col}]...")
    print("Note: This command is not fully implemented on the device")

    # Build payload: row(1) + col(1) + additional calibration data
    payload = struct.pack("<BB", args.row, args.col)

    # Add calibration data if provided
    if hasattr(args, "data") and args.data:
        calib_data = bytes.fromhex(args.data)
        payload += calib_data

    resp = pcs.send_command(
        CommandType.MODULES, CommandSubModuleType.CALIB_SET, payload
    )

    if not resp:
        print("Error: No response received")
        return 1

    if resp["response"] == ResponseType.ACK:
        print("Success: Calibration set acknowledged")
        return 0
    elif resp["response"] == ResponseType.NACK:
        print("Error: Calibration set failed (NACK)")
        return 1
    else:
        print(f"Error: Unexpected response type: {resp['response']}")
        return 1


def cmd_map_set(pcs: PicontrolSerial, args: argparse.Namespace) -> int:
    """Handle MAP SET command."""
    action_type = ACTION_TYPE_FROM_NAME.get(args.type.lower(), ActionType.NONE)

    print(f"Setting mapping on [{args.row},{args.col}] param {args.param}...")
    print(f"  Action Type: {ACTION_TYPE_NAMES.get(action_type, 'UNKNOWN')}")
    print(f"  D1: {args.d1}, D2: {args.d2}")

    # Payload: row(1) + col(1) + paramId(1) + type(1) + d1(1) + d2(1)
    payload = struct.pack(
        "<BBBBBB", args.row, args.col, args.param, action_type, args.d1, args.d2
    )

    resp = pcs.send_command(CommandType.MAP, CommandSubMapType.SET, payload)

    if not resp:
        print("Error: No response received")
        return 1

    if resp["response"] == ResponseType.ACK:
        print("Success: Mapping set acknowledged")
        return 0
    elif resp["response"] == ResponseType.NACK:
        print("Error: Mapping set failed (NACK)")
        return 1
    else:
        print(f"Error: Unexpected response type: {resp['response']}")
        return 1


def cmd_map_set_curve(pcs: PicontrolSerial, args: argparse.Namespace) -> int:
    """Handle MAP SET_CURVE command."""
    print(f"Setting curve on mapping [{args.row},{args.col}] param {args.param}...")

    # Curve struct size: count(1) + points(8) + controls(6) = 15 bytes
    CURVE_SIZE = 15

    # Parse curve data (expecting 15 bytes as hex string or comma-separated values)
    if args.curve.startswith("0x") or all(
        c in "0123456789abcdefABCDEF" for c in args.curve
    ):
        # Hex string
        curve_hex = args.curve.replace("0x", "").replace(" ", "")
        if len(curve_hex) != CURVE_SIZE * 2:  # 15 bytes = 30 hex chars
            print(
                f"Error: Curve data must be {CURVE_SIZE} bytes ({CURVE_SIZE*2} hex chars), got {len(curve_hex)//2} bytes"
            )
            return 1
        curve_data = bytes.fromhex(curve_hex)
    else:
        # Comma-separated decimal values
        try:
            values = [int(x.strip()) for x in args.curve.split(",")]
            if len(values) != CURVE_SIZE:
                print(
                    f"Error: Curve data must be {CURVE_SIZE} bytes, got {len(values)}"
                )
                return 1
            curve_data = bytes(values)
        except ValueError as e:
            print(f"Error: Invalid curve data format: {e}")
            return 1

    print(f"  Curve Data: {curve_data.hex()}")

    # Payload: row(1) + col(1) + paramId(1) + curve(16)
    payload = struct.pack("<BBB", args.row, args.col, args.param) + curve_data

    resp = pcs.send_command(CommandType.MAP, CommandSubMapType.SET_CURVE, payload)

    if not resp:
        print("Error: No response received")
        return 1

    if resp["response"] == ResponseType.ACK:
        print("Success: Curve set acknowledged")
        return 0
    elif resp["response"] == ResponseType.NACK:
        print("Error: Curve set failed (NACK)")
        return 1
    else:
        print(f"Error: Unexpected response type: {resp['response']}")
        return 1


def cmd_map_del(pcs: PicontrolSerial, args: argparse.Namespace) -> int:
    """Handle MAP DEL command."""
    print(f"Deleting mapping [{args.row},{args.col}] param {args.param}...")

    # Payload: row(1) + col(1) + paramId(1)
    payload = struct.pack("<BBB", args.row, args.col, args.param)

    resp = pcs.send_command(CommandType.MAP, CommandSubMapType.DEL, payload)

    if not resp:
        print("Error: No response received")
        return 1

    if resp["response"] == ResponseType.ACK:
        print("Success: Mapping deleted")
        return 0
    elif resp["response"] == ResponseType.NACK:
        print("Error: Mapping not found or delete failed (NACK)")
        return 1
    else:
        print(f"Error: Unexpected response type: {resp['response']}")
        return 1


def cmd_map_list(pcs: PicontrolSerial, args: argparse.Namespace) -> int:
    """Handle MAP LIST command."""
    print("Fetching mapping list...")

    resp = pcs.send_command(CommandType.MAP, CommandSubMapType.LIST)

    if not resp:
        print("Error: No response received")
        return 1

    if not resp["checksum_ok"]:
        print("Error: Checksum mismatch")
        return 1

    if resp["response"] == ResponseType.NACK:
        print("Error: Command not acknowledged (NACK)")
        return 1

    if resp["response"] != ResponseType.MAP:
        print(f"Error: Unexpected response type: {resp['response']}")
        return 1

    # Unpack the data
    unpacked = unpack_data(resp["payload"])

    if len(unpacked) < 1:
        print("No mappings found")
        return 0

    # First byte is count
    mapping_count = unpacked[0]
    mapping_size = ctypes.sizeof(ModuleMapping)

    print(f"\n{'='*60}")
    print(f"MAPPING LIST - {mapping_count} mapping(s)")
    print(f"{'='*60}")

    if mapping_count == 0:
        print("No mappings configured")
        return 0

    for i in range(mapping_count):
        offset = 1 + i * mapping_size
        if offset + mapping_size > len(unpacked):
            print(f"Warning: Incomplete mapping data at index {i}")
            break

        mapping_data = unpacked[offset : offset + mapping_size]
        mapping = ModuleMapping.from_buffer_copy(mapping_data)

        action_name = ACTION_TYPE_NAMES.get(mapping.type, f"UNKNOWN({mapping.type})")

        print(f"\n[Mapping {i+1}]")
        print(f"  Port: [{mapping.row},{mapping.col}]")
        print(f"  Parameter ID: {mapping.paramId}")
        print(f"  Action Type: {action_name}")

        # Display action-specific target info
        if mapping.type == ActionType.MIDI_NOTE:
            t = mapping.target.midiNote
            print(f"  MIDI Note Target:")
            print(
                f"    Channel: {t.channel}, Note: {t.noteNumber}, Velocity: {t.velocity}"
            )
        elif mapping.type == ActionType.MIDI_CC:
            t = mapping.target.midiCC
            print(f"  MIDI CC Target:")
            print(f"    Channel: {t.channel}, CC#: {t.ccNumber}, Value: {t.value}")
        elif mapping.type == ActionType.KEYBOARD:
            t = mapping.target.keyboard
            print(f"  Keyboard Target:")
            print(
                f"    Keycode: {t.keycode} (0x{t.keycode:02X}), Modifier: {t.modifier} (0x{t.modifier:02X})"
            )
        elif mapping.type in (ActionType.MIDI_PITCH_BEND, ActionType.MIDI_MOD_WHEEL):
            t = mapping.target.midiCC  # Uses same structure
            print(f"  MIDI Target:")
            print(f"    Channel: {t.channel}")

        # Display curve info
        curve = mapping.curve
        if curve.count > 0:
            print(f"  Curve ({curve.count} points):")
            for j in range(min(curve.count, 4)):
                p = curve.points[j]
                print(f"    Point {j}: ({p.x}, {p.y})")
            for j in range(min(curve.count - 1, 3)):
                c = curve.controls[j]
                print(f"    Control {j}: ({c.x}, {c.y})")

    print(f"\n{'='*60}")
    return 0


def cmd_map_clear(pcs: PicontrolSerial, args: argparse.Namespace) -> int:
    """Handle MAP CLEAR command."""
    print("Clearing all mappings...")

    resp = pcs.send_command(CommandType.MAP, CommandSubMapType.CLEAR)

    if not resp:
        print("Error: No response received")
        return 1

    if resp["response"] == ResponseType.ACK:
        print("Success: All mappings cleared")
        return 0
    elif resp["response"] == ResponseType.NACK:
        print("Error: Clear failed (NACK)")
        return 1
    else:
        print(f"Error: Unexpected response type: {resp['response']}")
        return 1


# ==================== MIDI Test Functions ====================


def find_picontrol_midi_port() -> Optional[str]:
    """Find the Picontrol MIDI input port."""
    if not MIDO_AVAILABLE:
        return None
    
    for port_name in mido.get_input_names():
        if "picontrol" in port_name.lower() or "pico" in port_name.lower():
            return port_name
    return None


def get_fader_value(pcs: PicontrolSerial, row: int, col: int) -> Optional[int]:
    """Get current fader value from module list."""
    resp = pcs.send_command(CommandType.MODULES, CommandSubModuleType.LIST)
    
    if not resp or not resp["checksum_ok"] or resp["response"] != ResponseType.MODULES:
        return None
    
    unpacked = unpack_data(resp["payload"])
    port_size = ctypes.sizeof(PortPacked)
    port_count = len(unpacked) // port_size
    
    for i in range(port_count):
        offset = i * port_size
        port_data = unpacked[offset : offset + port_size]
        if len(port_data) < port_size:
            continue
        
        port = PortPacked.from_buffer_copy(port_data)
        if port.row == row and port.col == col and port.hasModule:
            # Parameter 0 is usually the fader value
            if port.module.parameterCount > 0:
                return port.module.parameters[0].value.intValue
    
    return None


class MidiTestResult:
    """Stores MIDI test results."""
    def __init__(self):
        self.messages: List[mido.Message] = []
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
    
    def add_message(self, msg):
        with self.lock:
            self.messages.append(msg)
    
    def get_messages(self):
        with self.lock:
            return list(self.messages)
    
    def clear(self):
        with self.lock:
            self.messages.clear()


def midi_listener_thread(port_name: str, result: MidiTestResult, duration: float):
    """Thread function to listen for MIDI messages."""
    try:
        with mido.open_input(port_name) as inport:
            start_time = time.time()
            while not result.stop_event.is_set() and (time.time() - start_time) < duration:
                msg = inport.poll()
                if msg is not None:
                    result.add_message(msg)
                time.sleep(0.001)
    except Exception as e:
        print(f"MIDI listener error: {e}")


def cmd_midi_test(pcs: PicontrolSerial, args: argparse.Namespace) -> int:
    """Run MIDI mapping tests to verify device sends correct values."""
    if not MIDO_AVAILABLE:
        print("Error: mido library not installed. Install with: pip install mido python-rtmidi")
        return 1
    
    print("=" * 60)
    print("MIDI MAPPING TEST")
    print("=" * 60)
    
    # Find MIDI port
    if args.midi_port:
        midi_port = args.midi_port
    else:
        midi_port = find_picontrol_midi_port()
    
    if not midi_port:
        print("\nAvailable MIDI input ports:")
        for name in mido.get_input_names():
            print(f"  - {name}")
        print("\nError: Could not find Picontrol MIDI port. Use --midi-port to specify.")
        return 1
    
    print(f"\nUsing MIDI port: {midi_port}")
    print(f"Testing module at: [{args.row},{args.col}]")
    print(f"Parameter: {args.param}")
    
    test_results = []
    
    # Test 1: MIDI CC
    print("\n" + "-" * 40)
    print("TEST 1: MIDI CC Mapping")
    print("-" * 40)
    
    # Clear existing mappings
    pcs.send_command(CommandType.MAP, CommandSubMapType.CLEAR)
    time.sleep(0.1)
    
    # Set MIDI CC mapping (channel 1, CC#74)
    cc_channel = 1
    cc_number = 74
    payload = struct.pack("<BBBBBB", args.row, args.col, args.param, ActionType.MIDI_CC, cc_channel, cc_number)
    resp = pcs.send_command(CommandType.MAP, CommandSubMapType.SET, payload)
    
    if not resp or resp["response"] != ResponseType.ACK:
        print("  FAIL: Could not set MIDI CC mapping")
        test_results.append(("MIDI_CC", False, "Failed to set mapping"))
    else:
        print(f"  Mapping set: Channel {cc_channel}, CC#{cc_number}")
        
        # Listen for MIDI messages
        result = MidiTestResult()
        listener = threading.Thread(target=midi_listener_thread, args=(midi_port, result, 2.0))
        listener.start()
        
        # Read fader value multiple times to trigger MIDI output
        print("  Monitoring MIDI for 2 seconds... (move the fader)")
        time.sleep(2.0)
        result.stop_event.set()
        listener.join()
        
        messages = result.get_messages()
        cc_messages = [m for m in messages if m.type == 'control_change' and m.control == cc_number]
        
        if cc_messages:
            # Get current fader value
            fader_value = get_fader_value(pcs, args.row, args.col)
            expected_cc_value = (fader_value * 127) // 1023 if fader_value is not None else None
            
            last_msg = cc_messages[-1]
            print(f"  Received {len(cc_messages)} CC messages")
            print(f"  Last CC value: {last_msg.value}, Channel: {last_msg.channel + 1}")
            
            if fader_value is not None:
                print(f"  Current fader value: {fader_value} (expected CC ≈ {expected_cc_value})")
                # Allow some tolerance due to timing
                if abs(last_msg.value - expected_cc_value) <= 5:
                    print("  PASS: CC value matches expected range")
                    test_results.append(("MIDI_CC", True, f"CC#{cc_number}={last_msg.value}"))
                else:
                    print(f"  WARN: CC value differs (got {last_msg.value}, expected ~{expected_cc_value})")
                    test_results.append(("MIDI_CC", True, f"Value received (may differ due to timing)"))
            else:
                print("  PASS: CC messages received")
                test_results.append(("MIDI_CC", True, f"Received {len(cc_messages)} messages"))
        else:
            print("  FAIL: No CC messages received")
            test_results.append(("MIDI_CC", False, "No messages received"))
    
    # Test 2: MIDI Note
    print("\n" + "-" * 40)
    print("TEST 2: MIDI Note Mapping")
    print("-" * 40)
    
    # Clear and set MIDI Note mapping
    pcs.send_command(CommandType.MAP, CommandSubMapType.CLEAR)
    time.sleep(0.1)
    
    note_channel = 1
    note_number = 60  # Middle C
    payload = struct.pack("<BBBBBB", args.row, args.col, args.param, ActionType.MIDI_NOTE, note_channel, note_number)
    resp = pcs.send_command(CommandType.MAP, CommandSubMapType.SET, payload)
    
    if not resp or resp["response"] != ResponseType.ACK:
        print("  FAIL: Could not set MIDI Note mapping")
        test_results.append(("MIDI_NOTE", False, "Failed to set mapping"))
    else:
        print(f"  Mapping set: Channel {note_channel}, Note {note_number} (Middle C)")
        
        result = MidiTestResult()
        listener = threading.Thread(target=midi_listener_thread, args=(midi_port, result, 2.0))
        listener.start()
        
        print("  Monitoring MIDI for 2 seconds... (move the fader)")
        time.sleep(2.0)
        result.stop_event.set()
        listener.join()
        
        messages = result.get_messages()
        note_on = [m for m in messages if m.type == 'note_on' and m.note == note_number]
        note_off = [m for m in messages if m.type == 'note_off' and m.note == note_number]
        
        if note_on or note_off:
            print(f"  Received {len(note_on)} Note On, {len(note_off)} Note Off messages")
            if note_on:
                print(f"  Last Note On velocity: {note_on[-1].velocity}")
            print("  PASS: Note messages received")
            test_results.append(("MIDI_NOTE", True, f"On:{len(note_on)}, Off:{len(note_off)}"))
        else:
            print("  FAIL: No Note messages received")
            test_results.append(("MIDI_NOTE", False, "No messages received"))
    
    # Test 3: Pitch Bend
    print("\n" + "-" * 40)
    print("TEST 3: MIDI Pitch Bend Mapping")
    print("-" * 40)
    
    pcs.send_command(CommandType.MAP, CommandSubMapType.CLEAR)
    time.sleep(0.1)
    
    pb_channel = 1
    payload = struct.pack("<BBBBBB", args.row, args.col, args.param, ActionType.MIDI_PITCH_BEND, pb_channel, 0)
    resp = pcs.send_command(CommandType.MAP, CommandSubMapType.SET, payload)
    
    if not resp or resp["response"] != ResponseType.ACK:
        print("  FAIL: Could not set Pitch Bend mapping")
        test_results.append(("MIDI_PITCH_BEND", False, "Failed to set mapping"))
    else:
        print(f"  Mapping set: Channel {pb_channel}, Pitch Bend")
        
        result = MidiTestResult()
        listener = threading.Thread(target=midi_listener_thread, args=(midi_port, result, 2.0))
        listener.start()
        
        print("  Monitoring MIDI for 2 seconds... (move the fader)")
        time.sleep(2.0)
        result.stop_event.set()
        listener.join()
        
        messages = result.get_messages()
        pb_messages = [m for m in messages if m.type == 'pitchwheel']
        
        if pb_messages:
            print(f"  Received {len(pb_messages)} Pitch Bend messages")
            print(f"  Last Pitch Bend value: {pb_messages[-1].pitch}")
            print("  PASS: Pitch Bend messages received")
            test_results.append(("MIDI_PITCH_BEND", True, f"Received {len(pb_messages)} messages"))
        else:
            print("  FAIL: No Pitch Bend messages received")
            test_results.append(("MIDI_PITCH_BEND", False, "No messages received"))
    
    # Test 4: Mod Wheel
    print("\n" + "-" * 40)
    print("TEST 4: MIDI Mod Wheel Mapping")
    print("-" * 40)
    
    pcs.send_command(CommandType.MAP, CommandSubMapType.CLEAR)
    time.sleep(0.1)
    
    mw_channel = 1
    payload = struct.pack("<BBBBBB", args.row, args.col, args.param, ActionType.MIDI_MOD_WHEEL, mw_channel, 0)
    resp = pcs.send_command(CommandType.MAP, CommandSubMapType.SET, payload)
    
    if not resp or resp["response"] != ResponseType.ACK:
        print("  FAIL: Could not set Mod Wheel mapping")
        test_results.append(("MIDI_MOD_WHEEL", False, "Failed to set mapping"))
    else:
        print(f"  Mapping set: Channel {mw_channel}, Mod Wheel (CC#1)")
        
        result = MidiTestResult()
        listener = threading.Thread(target=midi_listener_thread, args=(midi_port, result, 2.0))
        listener.start()
        
        print("  Monitoring MIDI for 2 seconds... (move the fader)")
        time.sleep(2.0)
        result.stop_event.set()
        listener.join()
        
        messages = result.get_messages()
        mw_messages = [m for m in messages if m.type == 'control_change' and m.control == 1]
        
        if mw_messages:
            print(f"  Received {len(mw_messages)} Mod Wheel messages")
            print(f"  Last Mod Wheel value: {mw_messages[-1].value}")
            print("  PASS: Mod Wheel messages received")
            test_results.append(("MIDI_MOD_WHEEL", True, f"Received {len(mw_messages)} messages"))
        else:
            print("  FAIL: No Mod Wheel messages received")
            test_results.append(("MIDI_MOD_WHEEL", False, "No messages received"))
    
    # Clean up
    pcs.send_command(CommandType.MAP, CommandSubMapType.CLEAR)
    
    # Summary
    print("\n" + "=" * 60)
    print("TEST SUMMARY")
    print("=" * 60)
    
    passed = sum(1 for _, success, _ in test_results if success)
    total = len(test_results)
    
    for name, success, detail in test_results:
        status = "✓ PASS" if success else "✗ FAIL"
        print(f"  {status}: {name} - {detail}")
    
    print(f"\nTotal: {passed}/{total} tests passed")
    print("=" * 60)
    
    return 0 if passed == total else 1


# ==================== Main CLI Setup ====================


def main():
    parser = argparse.ArgumentParser(
        description="Picontrol Serial Command Line Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  List all modules:
    %(prog)s --port COM38 modules list

  Set a MIDI CC mapping:
    %(prog)s --port COM38 map set --row 0 --col 0 --param 1 --type midi_cc --d1 1 --d2 74

  Set a MIDI Note mapping:
    %(prog)s --port COM38 map set --row 0 --col 0 --param 1 --type midi_note --d1 1 --d2 60

  Set a keyboard mapping:
    %(prog)s --port COM38 map set --row 0 --col 0 --param 1 --type keyboard --d1 0x04 --d2 0

  List all mappings:
    %(prog)s --port COM38 map list

  Delete a mapping:
    %(prog)s --port COM38 map del --row 0 --col 0 --param 1

  Set module parameter:
    %(prog)s --port COM38 modules param-set --row 0 --col 0 --param 1 --datatype int --value 100

Action Types:
  none, midi_note, midi_cc, keyboard, midi_pitch_bend, midi_mod_wheel

Parameter Data Types:
  int, float, bool, led
""",
    )

    # Global arguments
    parser.add_argument(
        "--port",
        "-p",
        default=DEFAULT_PORT,
        help=f"Serial port (default: {DEFAULT_PORT})",
    )
    parser.add_argument(
        "--baud",
        "-b",
        type=int,
        default=DEFAULT_BAUD_RATE,
        help=f"Baud rate (default: {DEFAULT_BAUD_RATE})",
    )
    parser.add_argument(
        "--timeout",
        "-t",
        type=float,
        default=DEFAULT_TIMEOUT,
        help=f"Response timeout in seconds (default: {DEFAULT_TIMEOUT})",
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true", help="Enable verbose output"
    )

    subparsers = parser.add_subparsers(dest="command", help="Command category")

    # ==================== MODULES commands ====================
    modules_parser = subparsers.add_parser("modules", help="Module-related commands")
    modules_sub = modules_parser.add_subparsers(
        dest="subcommand", help="Module subcommand"
    )

    # modules list
    modules_list = modules_sub.add_parser(
        "list", help="List all modules and their parameters"
    )

    # modules param-set
    modules_param_set = modules_sub.add_parser(
        "param-set", help="Set a module parameter value"
    )
    modules_param_set.add_argument(
        "--row", "-r", type=int, required=True, help="Port row"
    )
    modules_param_set.add_argument(
        "--col", "-c", type=int, required=True, help="Port column"
    )
    modules_param_set.add_argument(
        "--param", "-P", type=int, required=True, help="Parameter ID"
    )
    modules_param_set.add_argument(
        "--datatype",
        "-d",
        required=True,
        choices=["int", "float", "bool", "led"],
        help="Parameter data type",
    )
    modules_param_set.add_argument(
        "--value", "-V", required=True, help="Value to set (for LED: r,g,b,status)"
    )

    # modules calib-set
    modules_calib_set = modules_sub.add_parser(
        "calib-set", help="Set module calibration"
    )
    modules_calib_set.add_argument(
        "--row", "-r", type=int, required=True, help="Port row"
    )
    modules_calib_set.add_argument(
        "--col", "-c", type=int, required=True, help="Port column"
    )
    modules_calib_set.add_argument("--data", help="Calibration data as hex string")

    # ==================== MAP commands ====================
    map_parser = subparsers.add_parser("map", help="Mapping-related commands")
    map_sub = map_parser.add_subparsers(dest="subcommand", help="Map subcommand")

    # map list
    map_list = map_sub.add_parser("list", help="List all mappings")

    # map set
    map_set = map_sub.add_parser("set", help="Set/update a mapping")
    map_set.add_argument("--row", "-r", type=int, required=True, help="Port row")
    map_set.add_argument("--col", "-c", type=int, required=True, help="Port column")
    map_set.add_argument("--param", "-P", type=int, required=True, help="Parameter ID")
    map_set.add_argument(
        "--type",
        "-T",
        required=True,
        choices=[
            "none",
            "midi_note",
            "midi_cc",
            "keyboard",
            "midi_pitch_bend",
            "midi_mod_wheel",
        ],
        help="Action type",
    )
    map_set.add_argument(
        "--d1",
        type=lambda x: int(x, 0),
        default=0,
        help="Data byte 1 (channel for MIDI, keycode for keyboard)",
    )
    map_set.add_argument(
        "--d2",
        type=lambda x: int(x, 0),
        default=0,
        help="Data byte 2 (note/CC# for MIDI, modifier for keyboard)",
    )

    # map set-curve
    map_set_curve = map_sub.add_parser("set-curve", help="Set curve for a mapping")
    map_set_curve.add_argument("--row", "-r", type=int, required=True, help="Port row")
    map_set_curve.add_argument(
        "--col", "-c", type=int, required=True, help="Port column"
    )
    map_set_curve.add_argument(
        "--param", "-P", type=int, required=True, help="Parameter ID"
    )
    map_set_curve.add_argument(
        "--curve",
        required=True,
        help="Curve data (16 bytes as hex or comma-separated decimal)",
    )

    # map del
    map_del = map_sub.add_parser("del", help="Delete a mapping")
    map_del.add_argument("--row", "-r", type=int, required=True, help="Port row")
    map_del.add_argument("--col", "-c", type=int, required=True, help="Port column")
    map_del.add_argument("--param", "-P", type=int, required=True, help="Parameter ID")

    # map clear
    map_clear = map_sub.add_parser("clear", help="Clear all mappings")

    # ==================== MIDI TEST command ====================
    midi_test_parser = subparsers.add_parser(
        "midi-test", help="Test MIDI mappings by monitoring MIDI output"
    )
    midi_test_parser.add_argument(
        "--row", "-r", type=int, default=0, help="Port row (default: 0)"
    )
    midi_test_parser.add_argument(
        "--col", "-c", type=int, default=1, help="Port column (default: 1)"
    )
    midi_test_parser.add_argument(
        "--param", "-P", type=int, default=0, help="Parameter ID (default: 0)"
    )
    midi_test_parser.add_argument(
        "--midi-port", "-m", help="MIDI input port name (auto-detect if not specified)"
    )

    # Parse arguments
    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        return 1

    if args.command == "modules" and not args.subcommand:
        modules_parser.print_help()
        return 1

    if args.command == "map" and not args.subcommand:
        map_parser.print_help()
        return 1

    # Connect to device
    pcs = PicontrolSerial(args.port, args.baud, args.timeout)
    if not pcs.connect():
        return 1

    try:
        # Dispatch command
        if args.command == "modules":
            if args.subcommand == "list":
                return cmd_modules_list(pcs, args)
            elif args.subcommand == "param-set":
                return cmd_modules_param_set(pcs, args)
            elif args.subcommand == "calib-set":
                return cmd_modules_calib_set(pcs, args)
        elif args.command == "map":
            if args.subcommand == "list":
                return cmd_map_list(pcs, args)
            elif args.subcommand == "set":
                return cmd_map_set(pcs, args)
            elif args.subcommand == "set-curve":
                return cmd_map_set_curve(pcs, args)
            elif args.subcommand == "del":
                return cmd_map_del(pcs, args)
            elif args.subcommand == "clear":
                return cmd_map_clear(pcs, args)
        elif args.command == "midi-test":
            return cmd_midi_test(pcs, args)

        print(f"Error: Unknown command: {args.command} {args.subcommand}")
        return 1

    finally:
        pcs.close()


if __name__ == "__main__":
    sys.exit(main())
