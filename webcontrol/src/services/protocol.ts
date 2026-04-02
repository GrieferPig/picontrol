/**
 * Binary protocol implementation for Picontrol device communication.
 *
 * Wire format (little-endian):
 *   Header:   type(1) + command(1) + subcommand(1) + length(2)  = 5 bytes
 *   Checksum: CRC16-CCITT (2 bytes, at byte offset 5-6)
 *   Data:     variable length (length bytes)
 *
 * CRC16 is calculated over header(5) + data(length), NOT including the checksum bytes.
 */

import { useStore } from '../composables/useStore';
import { useLogger } from '../composables/useLogger';
import type { Curve, Mapping, Module, ModuleParam, PortItem } from '../types';

// ── Enums matching firmware ─────────────────────────────────────────────────

export enum MessageType {
    COMMAND = 0,
    RESPONSE = 1,
    EVENT = 2,
}

export enum CommandType {
    INFO = 0,
    VERSION = 1,
    MAP = 2,
    MODULES = 3,
}

export enum ResponseType {
    ACK = 0,
    NACK = 1,
    INFO = 2,
    VERSION = 3,
    MAP = 4,
    MODULES = 5,
}

export enum EventType {
    PORT_STATUS_CHANGED = 0,
    MODULE_STATE_CHANGED = 1,
    MAPPINGS_LOADED = 2,
    MODULE_PARAM_CHANGED = 3,
}

export enum MapSubcommand {
    SET = 0,
    SET_CURVE = 1,
    DEL = 2,
    LIST = 3,
    CLEAR = 4,
}

export enum ModuleSubcommand {
    LIST = 0,
    PARAM_SET = 1,
    CALIB_SET = 2,
}

export enum ParamDataType {
    INT = 0,
    FLOAT = 1,
    BOOL = 2,
    LED = 3,
}

export enum ActionType {
    NONE = 0,
    MIDI_NOTE = 1,
    MIDI_CC = 2,
    KEYBOARD = 3,
    MIDI_PITCH_BEND = 4,
    MIDI_MOD_WHEEL = 5,
}

// ── Struct sizes (packed, matching firmware #pragma pack(push,1)) ───────────

export const SIZES = {
    HEADER: 5,
    CHECKSUM: 2,
    MIN_MESSAGE: 7, // HEADER + CHECKSUM
    LED_VALUE: 4,
    LED_RANGE: 6,
    PARAM_VALUE: 4,
    PARAM_MINMAX: 8,
    MODULE_PARAM: 47, // 1+32+1+1+4+8
    MODULE: 465, // 1+1+32+32+16+1+1+1+1+1+1+1 + 8*47
    PORT_STATE_PACKED: 476, // 4+4+1+465+1+1
    CURVE_POINT: 2,
    CURVE: 2, // int16_t h
    ACTION_TARGET: 3,
    MODULE_MAPPING: 15, // 4+4+1+1+2+3
} as const;

// ── CRC16-CCITT ─────────────────────────────────────────────────────────────

export function crc16Update(crc: number, byte: number): number {
    crc = ((crc ^ (byte << 8)) & 0xFFFF);
    for (let i = 0; i < 8; i++) {
        if (crc & 0x8000) {
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF;
        } else {
            crc = (crc << 1) & 0xFFFF;
        }
    }
    return crc;
}

export function calculateCrc16(data: Uint8Array, offset = 0, length?: number): number {
    const len = length ?? data.length - offset;
    let crc = 0xFFFF;
    for (let i = 0; i < len; i++) {
        crc = crc16Update(crc, data[offset + i]!);
    }
    return crc;
}

// ── Zero Run-Length Encoding / Decoding ─────────────────────────────────────

/**
 * Decode zero run-length encoded data.
 * Format: repeating blocks of [zeroCount(1), validCount(1), validBytes(validCount)]
 */
export function decodeZeroRLE(data: Uint8Array): Uint8Array {
    const result: number[] = [];
    let pos = 0;
    while (pos < data.length) {
        const zeroCount = data[pos++]!;
        if (pos >= data.length) {
            // Malformed: push zeros and stop
            for (let i = 0; i < zeroCount; i++) result.push(0);
            break;
        }
        const validCount = data[pos++]!;

        for (let i = 0; i < zeroCount; i++) result.push(0);

        for (let i = 0; i < validCount; i++) {
            if (pos < data.length) {
                result.push(data[pos++]!);
            }
        }
    }
    return new Uint8Array(result);
}

/**
 * Encode data using zero run-length encoding.
 */
export function encodeZeroRLE(data: Uint8Array): Uint8Array {
    const result: number[] = [];
    let i = 0;
    while (i < data.length) {
        let zeroCount = 0;
        while (i < data.length && data[i] === 0 && zeroCount < 255) {
            zeroCount++;
            i++;
        }

        let validCount = 0;
        const validStart = i;
        while (i < data.length && data[i] !== 0 && validCount < 255) {
            validCount++;
            i++;
        }

        result.push(zeroCount, validCount);
        for (let j = 0; j < validCount; j++) {
            result.push(data[validStart + j]!);
        }
    }
    return new Uint8Array(result);
}

// ── Message Building ────────────────────────────────────────────────────────

export interface ParsedMessage {
    type: MessageType;
    command: number; // CommandType | ResponseType | EventType
    subcommand: number;
    length: number;
    checksum: number;
    data: Uint8Array;
}

/**
 * Build a binary command message ready for wire transmission.
 */
export function buildCommand(
    command: CommandType,
    subcommand: number,
    data?: Uint8Array,
): Uint8Array {
    const payload = data ?? new Uint8Array(0);
    const totalSize = SIZES.HEADER + SIZES.CHECKSUM + payload.length;
    const buf = new Uint8Array(totalSize);

    buf[0] = MessageType.COMMAND;
    buf[1] = command;
    buf[2] = subcommand;
    buf[3] = payload.length & 0xFF;
    buf[4] = (payload.length >> 8) & 0xFF;

    // Copy payload after header + checksum
    buf.set(payload, SIZES.HEADER + SIZES.CHECKSUM);

    // Calculate CRC over header + payload
    let crc = calculateCrc16(buf, 0, SIZES.HEADER);
    for (let i = 0; i < payload.length; i++) {
        crc = crc16Update(crc, payload[i]!);
    }

    buf[5] = crc & 0xFF;
    buf[6] = (crc >> 8) & 0xFF;

    return buf;
}

/**
 * Parse a complete binary message from a buffer.
 * Returns null if the buffer is too short or the checksum doesn't match.
 */
export function parseMessage(buf: Uint8Array): ParsedMessage | null {
    if (buf.length < SIZES.MIN_MESSAGE) return null;

    const type = buf[0] as MessageType;
    const command = buf[1]!;
    const subcommand = buf[2]!;
    const length = buf[3]! | (buf[4]! << 8);
    const checksum = buf[5]! | (buf[6]! << 8);

    const expectedTotal = SIZES.HEADER + SIZES.CHECKSUM + length;
    if (buf.length < expectedTotal) return null;

    const data = buf.slice(SIZES.HEADER + SIZES.CHECKSUM, expectedTotal);

    // Verify checksum
    let crc = calculateCrc16(buf, 0, SIZES.HEADER);
    for (let i = 0; i < data.length; i++) {
        crc = crc16Update(crc, data[i]!);
    }
    if (crc !== checksum) return null;

    return { type, command, subcommand, length, checksum, data };
}

/**
 * Try to find and extract a complete message from an accumulation buffer.
 * Returns { message, remaining } or null if no complete message yet.
 */
export function extractMessage(
    buf: Uint8Array,
): { message: ParsedMessage; remaining: Uint8Array } | null {
    if (buf.length < SIZES.MIN_MESSAGE) return null;

    const length = buf[3]! | (buf[4]! << 8);
    const expectedTotal = SIZES.HEADER + SIZES.CHECKSUM + length;

    if (expectedTotal > 8192) {
        // Sanity check: discard obviously corrupt data, skip one byte
        return extractMessage(buf.slice(1));
    }

    if (buf.length < expectedTotal) return null;

    const msgBuf = buf.slice(0, expectedTotal);
    const msg = parseMessage(msgBuf);

    if (msg) {
        return { message: msg, remaining: buf.slice(expectedTotal) };
    }

    // Checksum failed — skip one byte and retry
    return extractMessage(buf.slice(1));
}

// ── Convenience Builders ────────────────────────────────────────────────────

export function buildModulesListCmd(): Uint8Array {
    return buildCommand(CommandType.MODULES, ModuleSubcommand.LIST);
}

export function buildMapListCmd(): Uint8Array {
    return buildCommand(CommandType.MAP, MapSubcommand.LIST);
}

export function buildMapClearCmd(): Uint8Array {
    return buildCommand(CommandType.MAP, MapSubcommand.CLEAR);
}

export function buildMapSetCmd(
    row: number, col: number, paramId: number,
    actionType: number, d1: number, d2: number,
): Uint8Array {
    const data = new Uint8Array([row, col, paramId, actionType, d1, d2]);
    return buildCommand(CommandType.MAP, MapSubcommand.SET, data);
}

export function buildMapSetCurveCmd(
    row: number, col: number, paramId: number, curve: Curve,
): Uint8Array {
    // Curve is sent as the packed struct: count(1) + points(4*2) + controls(3*2) = 15 bytes
    const data = new Uint8Array(3 + SIZES.CURVE);
    data[0] = row;
    data[1] = col;
    data[2] = paramId;
    serializeCurve(curve, data, 3);
    return buildCommand(CommandType.MAP, MapSubcommand.SET_CURVE, data);
}

export function buildMapDelCmd(row: number, col: number, paramId: number): Uint8Array {
    const data = new Uint8Array([row, col, paramId]);
    return buildCommand(CommandType.MAP, MapSubcommand.DEL, data);
}

export function buildParamSetCmd(
    row: number, col: number, paramId: number,
    dataType: number, valueStr: string,
): Uint8Array {
    const encoder = new TextEncoder();
    const valueBytes = encoder.encode(valueStr);
    const data = new Uint8Array(4 + valueBytes.length);
    data[0] = row;
    data[1] = col;
    data[2] = paramId;
    data[3] = dataType;
    data.set(valueBytes, 4);
    return buildCommand(CommandType.MODULES, ModuleSubcommand.PARAM_SET, data);
}

export function buildCalibSetCmd(
    row: number, col: number, paramId: number,
    minValue: number, maxValue: number,
): Uint8Array {
    // Payload: row(1) + col(1) + paramId(1) + minValue(4,i32LE) + maxValue(4,i32LE)
    const data = new Uint8Array(11);
    const view = new DataView(data.buffer);
    data[0] = row;
    data[1] = col;
    data[2] = paramId;
    view.setInt32(3, minValue, true);
    view.setInt32(7, maxValue, true);
    return buildCommand(CommandType.MODULES, ModuleSubcommand.CALIB_SET, data);
}

// ── Curve Serialization / Deserialization ───────────────────────────────────

export function serializeCurve(curve: Curve, outBuf: Uint8Array, offset = 0): void {
    // Write h as int16 LE (2 bytes)
    const h = curve.h;
    outBuf[offset] = h & 0xFF;
    outBuf[offset + 1] = (h >> 8) & 0xFF;
}

export function deserializeCurve(data: Uint8Array, offset = 0): Curve {
    // Read h as int16 LE (2 bytes)
    const lo = data[offset]!;
    const hi = data[offset + 1]!;
    let h = lo | (hi << 8);
    // Sign-extend from 16-bit
    if (h >= 0x8000) h -= 0x10000;
    return { h };
}

// ── Binary Struct Parsers ───────────────────────────────────────────────────

function readCString(data: Uint8Array, offset: number, maxLen: number): string {
    let end = offset;
    while (end < offset + maxLen && data[end] !== 0) end++;
    return new TextDecoder().decode(data.slice(offset, end));
}

export interface ParsedModuleParameter {
    id: number;
    name: string;
    dataType: number;
    access: number;
    value: { raw: Uint8Array; int?: number; float?: number; bool?: number; led?: { r: number; g: number; b: number; status: number } };
    minMax: { raw: Uint8Array; intMin?: number; intMax?: number; floatMin?: number; floatMax?: number; ledRange?: { rMin: number; rMax: number; gMin: number; gMax: number; bMin: number; bMax: number } };
}

export function parseModuleParameter(data: Uint8Array, offset: number): ParsedModuleParameter {
    const id = data[offset]!;
    const name = readCString(data, offset + 1, 32);
    const dataType = data[offset + 33]!;
    const access = data[offset + 34]!;

    const valueOffset = offset + 35;
    const valueRaw = data.slice(valueOffset, valueOffset + 4);
    const valueView = new DataView(data.buffer, data.byteOffset + valueOffset, 4);

    const value: ParsedModuleParameter['value'] = { raw: valueRaw };
    switch (dataType) {
        case ParamDataType.INT:
            value.int = valueView.getInt32(0, true);
            break;
        case ParamDataType.FLOAT:
            value.float = valueView.getFloat32(0, true);
            break;
        case ParamDataType.BOOL:
            value.bool = data[valueOffset]!;
            break;
        case ParamDataType.LED:
            value.led = {
                r: data[valueOffset]!,
                g: data[valueOffset + 1]!,
                b: data[valueOffset + 2]!,
                status: data[valueOffset + 3]!,
            };
            break;
    }

    const minMaxOffset = offset + 39;
    const minMaxRaw = data.slice(minMaxOffset, minMaxOffset + 8);
    const minMaxView = new DataView(data.buffer, data.byteOffset + minMaxOffset, 8);

    const minMax: ParsedModuleParameter['minMax'] = { raw: minMaxRaw };
    switch (dataType) {
        case ParamDataType.INT:
            minMax.intMin = minMaxView.getInt32(0, true);
            minMax.intMax = minMaxView.getInt32(4, true);
            break;
        case ParamDataType.FLOAT:
            minMax.floatMin = minMaxView.getFloat32(0, true);
            minMax.floatMax = minMaxView.getFloat32(4, true);
            break;
        case ParamDataType.LED:
            minMax.ledRange = {
                rMin: data[minMaxOffset]!,
                rMax: data[minMaxOffset + 1]!,
                gMin: data[minMaxOffset + 2]!,
                gMax: data[minMaxOffset + 3]!,
                bMin: data[minMaxOffset + 4]!,
                bMax: data[minMaxOffset + 5]!,
            };
            break;
    }

    return { id, name, dataType, access, value, minMax };
}

export interface ParsedModule {
    protocol: number;
    type: number;
    name: string;
    manufacturer: string;
    fwVersion: string;
    compatibleHostVersion: number;
    capabilities: number;
    physicalSizeRow: number;
    physicalSizeCol: number;
    portLocationRow: number;
    portLocationCol: number;
    parameterCount: number;
    parameters: ParsedModuleParameter[];
}

export function parseModule(data: Uint8Array, offset: number): ParsedModule {
    const protocol = data[offset]!;
    const type = data[offset + 1]!;
    const name = readCString(data, offset + 2, 32);
    const manufacturer = readCString(data, offset + 34, 32);
    const fwVersion = readCString(data, offset + 66, 16);
    const compatibleHostVersion = data[offset + 82]!;
    const capabilities = data[offset + 83]!;
    const physicalSizeRow = data[offset + 84]!;
    const physicalSizeCol = data[offset + 85]!;
    const portLocationRow = data[offset + 86]!;
    const portLocationCol = data[offset + 87]!;
    const parameterCount = data[offset + 88]!;

    const parameters: ParsedModuleParameter[] = [];
    const paramsOffset = offset + 89;
    for (let i = 0; i < Math.min(parameterCount, 8); i++) {
        parameters.push(parseModuleParameter(data, paramsOffset + i * SIZES.MODULE_PARAM));
    }

    return {
        protocol, type, name, manufacturer, fwVersion,
        compatibleHostVersion, capabilities,
        physicalSizeRow, physicalSizeCol, portLocationRow, portLocationCol,
        parameterCount, parameters,
    };
}

export interface ParsedPortState {
    row: number;
    col: number;
    hasModule: boolean;
    module: ParsedModule;
    orientation: number;
    configured: boolean;
}

export function parsePortStatePacked(data: Uint8Array, offset: number): ParsedPortState {
    const view = new DataView(data.buffer, data.byteOffset + offset, SIZES.PORT_STATE_PACKED);
    const row = view.getInt32(0, true);
    const col = view.getInt32(4, true);
    const hasModule = data[offset + 8]! !== 0;
    const mod = parseModule(data, offset + 9);
    const orientation = data[offset + 9 + SIZES.MODULE]!;
    const configured = data[offset + 9 + SIZES.MODULE + 1]! !== 0;

    return { row, col, hasModule, module: mod, orientation, configured };
}

export function parseModuleMapping(data: Uint8Array, offset: number): Mapping {
    const view = new DataView(data.buffer, data.byteOffset + offset, SIZES.MODULE_MAPPING);
    const r = view.getInt32(0, true);
    const c = view.getInt32(4, true);
    const pid = data[offset + 8]!;
    const type = data[offset + 9]!;
    const curve = deserializeCurve(data, offset + 10);

    const targetOffset = offset + 10 + SIZES.CURVE;
    const d1 = data[targetOffset]!;
    const d2 = data[targetOffset + 1]!;

    return { r, c, pid, type, d1, d2, curve };
}

// ── Response Handlers (store-updating) ──────────────────────────────────────

function paramValueToString(param: ParsedModuleParameter): string | undefined {
    switch (param.dataType) {
        case ParamDataType.INT:
            return param.value.int?.toString();
        case ParamDataType.FLOAT:
            return param.value.float?.toFixed(6);
        case ParamDataType.BOOL:
            return param.value.bool?.toString();
        case ParamDataType.LED:
            if (param.value.led) {
                const l = param.value.led;
                return `${l.r},${l.g},${l.b},${l.status}`;
            }
            return undefined;
        default:
            return undefined;
    }
}

function paramMinToString(param: ParsedModuleParameter): string | undefined {
    switch (param.dataType) {
        case ParamDataType.INT:
            return param.minMax.intMin?.toString();
        case ParamDataType.FLOAT:
            return param.minMax.floatMin?.toFixed(6);
        default:
            return undefined;
    }
}

function paramMaxToString(param: ParsedModuleParameter): string | undefined {
    switch (param.dataType) {
        case ParamDataType.INT:
            return param.minMax.intMax?.toString();
        case ParamDataType.FLOAT:
            return param.minMax.floatMax?.toFixed(6);
        default:
            return undefined;
    }
}

export function useProtocol() {
    const { state } = useStore();
    const { add: logAdd } = useLogger();

    /**
     * Handle a parsed binary response message.
     * Returns optional action signals for the caller.
     */
    function handleResponse(msg: ParsedMessage): { action?: string } | void {
        if (msg.type === MessageType.RESPONSE) {
            const respType = msg.command as ResponseType;

            if (respType === ResponseType.ACK) {
                return;
            }
            if (respType === ResponseType.NACK) {
                logAdd('Device responded NACK');
                return;
            }

            if (respType === ResponseType.MODULES && msg.subcommand === ModuleSubcommand.LIST) {
                // Decode zero-RLE packed data
                const decoded = decodeZeroRLE(msg.data);
                handleModulesList(decoded);
                return;
            }

            if (respType === ResponseType.MAP && msg.subcommand === MapSubcommand.LIST) {
                const decoded = decodeZeroRLE(msg.data);
                handleMapList(decoded);
                return;
            }

            return;
        }

        if (msg.type === MessageType.EVENT) {
            // Events are defined but not yet sent by the firmware.
            // Handle them here for future-proofing.
            const evtType = msg.command as EventType;
            switch (evtType) {
                case EventType.PORT_STATUS_CHANGED:
                    return { action: 'refresh_modules' };
                case EventType.MODULE_STATE_CHANGED:
                    return { action: 'refresh_modules' };
                case EventType.MAPPINGS_LOADED:
                    return { action: 'refresh_mappings' };
                case EventType.MODULE_PARAM_CHANGED:
                    handleParamChangedEvent(msg.data);
                    return;
            }
        }
    }

    function handleModulesList(data: Uint8Array): void {
        // Data is an array of PortStatePacked structs
        const portCount = Math.floor(data.length / SIZES.PORT_STATE_PACKED);

        // Determine grid dimensions from the data
        let maxRow = 0, maxCol = 0;
        const parsedPorts: ParsedPortState[] = [];
        for (let i = 0; i < portCount; i++) {
            const ps = parsePortStatePacked(data, i * SIZES.PORT_STATE_PACKED);
            parsedPorts.push(ps);
            if (ps.row > maxRow) maxRow = ps.row;
            if (ps.col > maxCol) maxCol = ps.col;
        }

        state.ports.rows = maxRow + 1;
        state.ports.cols = maxCol + 1;

        // Build a new items map
        const newItems: Record<string, PortItem> = {};
        const newModules: Record<string, Module> = {};

        for (const ps of parsedPorts) {
            const key = `${ps.row},${ps.col}`;
            newItems[key] = {
                r: ps.row,
                c: ps.col,
                configured: ps.configured,
                hasModule: ps.hasModule,
                orientation: ps.orientation,
            };

            if (ps.hasModule) {
                const mod = ps.module;
                const params: ModuleParam[] = mod.parameters.map(p => {
                    // Preserve UI state (pendingUpdate, calibrating, etc.) from existing data
                    const existingMod = state.modules[key];
                    const existingParam = existingMod?.params.find(ep => ep.id === p.id);

                    const param: ModuleParam = {
                        id: p.id,
                        dt: p.dataType,
                        access: p.access,
                        name: p.name,
                        min: paramMinToString(p),
                        max: paramMaxToString(p),
                        value: paramValueToString(p),
                    };

                    // Preserve pending update and calibration state
                    if (existingParam) {
                        if (existingParam.pendingValue !== undefined && existingParam.pendingUpdate) {
                            const firmwareVal = paramValueToString(p);
                            const elapsed = Date.now() - existingParam.pendingUpdate;
                            const timedOut = elapsed > 10000;
                            // For float params, compare numerically to handle "0.5" vs "0.500000" format differences
                            let confirmed: boolean;
                            if (p.dataType === ParamDataType.FLOAT) {
                                const fwNum = parseFloat(firmwareVal ?? '');
                                const pendNum = parseFloat(String(existingParam.pendingValue));
                                confirmed = !isNaN(fwNum) && !isNaN(pendNum) && Math.abs(fwNum - pendNum) < 1e-4;
                            } else {
                                confirmed = firmwareVal === String(existingParam.pendingValue);
                            }
                            if (!confirmed && !timedOut) {
                                // Firmware hasn't reflected the new value yet — keep showing what we sent
                                param.value = existingParam.value;
                                param.pendingValue = existingParam.pendingValue;
                                param.pendingUpdate = existingParam.pendingUpdate;
                            }
                            // else: confirmed or timed out — use firmware value and don't carry pendingValue forward
                        }

                        // Preserve optimistic/manual calibration edits (min/max) recently made in the UI.
                        // Keep the user's manual min/max until firmware confirms (timeout ~10s).
                        if (existingParam.pendingCalibUpdate) {
                            const elapsedCalib = Date.now() - existingParam.pendingCalibUpdate;
                            const timedOutCalib = elapsedCalib > 10000;
                            if (!timedOutCalib) {
                                param.min = existingParam.min;
                                param.max = existingParam.max;
                                param.pendingCalibMin = existingParam.pendingCalibMin;
                                param.pendingCalibMax = existingParam.pendingCalibMax;
                                param.pendingCalibUpdate = existingParam.pendingCalibUpdate;
                            }
                        }

                        if (existingParam.calibrating) {
                            param.calibrating = existingParam.calibrating;
                            param.calibMin = existingParam.calibMin;
                            param.calibMax = existingParam.calibMax;

                            // Track min/max during calibration
                            if (p.dataType !== ParamDataType.BOOL && (p.access & 2) === 0) {
                                let numVal: number | undefined;
                                if (p.dataType === ParamDataType.INT) numVal = p.value.int;
                                else if (p.dataType === ParamDataType.FLOAT) numVal = p.value.float;

                                if (numVal !== undefined && !isNaN(numVal)) {
                                    if (param.calibMin === undefined || numVal < param.calibMin) {
                                        param.calibMin = numVal;
                                    }
                                    if (param.calibMax === undefined || numVal > param.calibMax) {
                                        param.calibMax = numVal;
                                    }
                                }
                            }
                        }
                    }
                    return param;
                });

                newModules[key] = {
                    r: ps.row,
                    c: ps.col,
                    type: mod.type,
                    caps: mod.capabilities,
                    name: mod.name,
                    mfg: mod.manufacturer,
                    fw: mod.fwVersion,
                    paramCount: mod.parameterCount,
                    params,
                    sizeR: mod.physicalSizeRow,
                    sizeC: mod.physicalSizeCol,
                    portLocR: mod.portLocationRow,
                    portLocC: mod.portLocationCol,
                };
            }
        }

        // Update atomically
        state.ports.items = newItems;
        // Only update modules that changed; preserve entries not in this response
        for (const key of Object.keys(newModules)) {
            state.modules[key] = newModules[key]!;
        }
        // Remove modules for ports that no longer have modules
        for (const key of Object.keys(state.modules)) {
            if (newItems[key] && !newItems[key]!.hasModule) {
                delete state.modules[key];
            }
        }
    }

    function handleMapList(data: Uint8Array): void {
        if (data.length < 1) {
            state.mappings = [];
            return;
        }
        const count = data[0]!;
        const mappings: Mapping[] = [];
        for (let i = 0; i < count; i++) {
            const offset = 1 + i * SIZES.MODULE_MAPPING;
            if (offset + SIZES.MODULE_MAPPING > data.length) break;
            mappings.push(parseModuleMapping(data, offset));
        }
        state.mappings = mappings;
    }

    function handleParamChangedEvent(data: Uint8Array): void {
        // Future: parse event payload for param change notification
        if (data.length < 3) return;
        // TODO: implement when firmware sends these events
    }

    return { handleResponse };
}
