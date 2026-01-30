import serial
import time
import struct

# Configuration
PORT = "COM38"
BAUD_RATE = 115200
DURATION_SECONDS = 5 * 60  # 5 minutes
FREQUENCY = 20  # messages per second

# Protocol Constants
HEADER_SIZE = 5
CHECKSUM_SIZE = 2


# Enum mappings (for logging/debug)
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


class CommandSubModuleType:
    LIST = 0
    PARAM_SET = 1
    CALIB_SET = 2


def crc16_update(crc, data):
    crc ^= data << 8
    for _ in range(8):
        if crc & 0x8000:
            crc = (crc << 1) ^ 0x1021
        else:
            crc <<= 1
    return crc & 0xFFFF


def calculate_crc16(data):
    crc = 0xFFFF  # Initial value
    for byte in data:
        crc = crc16_update(crc, byte)
    return crc


def create_message(msg_type, cmd_type, sub_cmd, payload_bytes):
    # Header: Type(1), Command(1), Subcommand(1), Length(2)
    length = len(payload_bytes)
    header = struct.pack("<BBBH", msg_type, cmd_type, sub_cmd, length)

    # Payload
    payload = bytes(payload_bytes)

    # Checksum is over header + payload (not including checksum itself)
    checksum = calculate_crc16(header + payload)

    checksum_bytes = struct.pack("<H", checksum)
    # Wire format: header + checksum + payload
    return header + checksum_bytes + payload


def parse_response(data):
    if len(data) < HEADER_SIZE + CHECKSUM_SIZE:
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
    }


def main():
    print(f"Starting Ping Test on {PORT} for {DURATION_SECONDS} seconds...")
    try:
        ser = serial.Serial(PORT, BAUD_RATE, timeout=0.1)
    except Exception as e:
        print(f"Failed to open port {PORT}: {e}")
        return

    stats = {
        "total_sent": 0,
        "acks_received": 0,
        "nacks_received": 0,
        "timeouts": 0,
        "checksum_errors_received": 0,
        "unexpected_response": 0,
        "success": 0,
    }

    start_time = time.time()

    try:
        while time.time() - start_time < DURATION_SECONDS:
            loop_start = time.time()

            # Send valid MODULES LIST command (ping-like command)
            msg_type = MessageType.COMMAND
            cmd_type = CommandType.MODULES
            sub_cmd = CommandSubModuleType.LIST
            payload = []

            message_bytes = create_message(msg_type, cmd_type, sub_cmd, payload)

            # Send
            ser.reset_input_buffer()
            ser.write(message_bytes)
            stats["total_sent"] += 1

            # Receive Response
            # MODULES LIST returns a response with packed data, not just ACK
            # Read header first
            response_data = ser.read(7)  # Minimal: header(5) + checksum(2)

            if len(response_data) == 0:
                stats["timeouts"] += 1
            else:
                # Try to parse
                # If data length > 0, we might need to read more.
                if len(response_data) >= 5:
                    _, _, _, r_len = struct.unpack("<BBBH", response_data[:5])
                    remaining = r_len + CHECKSUM_SIZE - (len(response_data) - 5)
                    if remaining > 0:
                        response_data += ser.read(remaining)

                resp = parse_response(response_data)

                if resp:
                    if not resp["checksum_ok"]:
                        stats["checksum_errors_received"] += 1
                    else:
                        if resp["type"] == MessageType.RESPONSE:
                            # For MODULES LIST, we expect a MODULES response (not ACK)
                            stats["success"] += 1
                        else:
                            stats["unexpected_response"] += 1
                else:
                    # Could not parse response
                    stats["unexpected_response"] += 1

            # Sleep to maintain frequency
            elapsed = time.time() - loop_start
            sleep_time = (1.0 / FREQUENCY) - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

            # Periodic status
            if stats["total_sent"] % 100 == 0:
                print(
                    f"Sent: {stats['total_sent']}, Success: {stats['success']}, Timeouts: {stats['timeouts']}"
                )

    except KeyboardInterrupt:
        print("\nTest stopped by user.")
    finally:
        ser.close()
        print("\n--- Final Statistics ---")
        print(f"Total Sent: {stats['total_sent']}")
        print(f"Successful Responses: {stats['success']}")
        print(f"Timeouts: {stats['timeouts']}")
        print(f"Checksum Errors: {stats['checksum_errors_received']}")
        print(f"Unexpected Responses: {stats['unexpected_response']}")
        if stats["total_sent"] > 0:
            print(
                f"Success Rate: {stats['success']} / {stats['total_sent']} ({stats['success']/stats['total_sent']*100:.2f}%)"
            )


if __name__ == "__main__":
    main()
