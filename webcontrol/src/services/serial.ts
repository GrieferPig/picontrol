import { useStore } from '../composables/useStore';
import { useLogger } from '../composables/useLogger';
import { useProtocol } from './protocol';
import { invoke } from '@tauri-apps/api/core';
import { listen, type UnlistenFn } from '@tauri-apps/api/event';
import {
    extractMessage,
    buildModulesListCmd,
    buildMapListCmd,
} from './protocol';

interface SerialPortInfo {
    name: string;
    port_type: string;
}

let connectedPortName: string | null = null;
let unlistenSerialData: UnlistenFn | null = null;
let unlistenSerialDisconnect: UnlistenFn | null = null;    // new listener for unexpected detach
let inputBuffer = new Uint8Array(0);
let pollIntervalId: number | null = null;
let refreshModulesRequested = false;
let refreshMappingsRequested = false;

/** Polling interval in ms for module/mapping list updates */
const POLL_INTERVAL_MS = 500;

export function useSerial() {
    const { state, reset } = useStore();
    const { add: logAdd } = useLogger();
    const { handleResponse } = useProtocol();

    function isTauriRuntime() {
        return typeof window !== 'undefined' && '__TAURI_INTERNALS__' in window;
    }

    async function connect() {
        if (connectedPortName) {
            await disconnect();
            return;
        }

        if (!isTauriRuntime()) {
            logAdd('Tauri runtime not detected. Use this app via `npm run tauri:dev`.');
            return;
        }

        try {
            const probeData = Array.from(buildModulesListCmd());
            const selectedPort = await invoke<SerialPortInfo | null>('find_responsive_serial_port', {
                probeData,
                baudRate: 115200,
                timeoutMs: 900,
                scanAttempts: 8,
                scanIntervalMs: 250,
            });

            if (!selectedPort) {
                logAdd('No responsive serial device found (firmware protocol check failed)');
                return;
            }

            await invoke('connect_serial', {
                portName: selectedPort.name,
                baudRate: 115200,
            });

            connectedPortName = selectedPort.name;

            unlistenSerialData = await listen<number[]>('serial-data', (event) => {
                const payload = event.payload;
                if (!Array.isArray(payload) || payload.length === 0) return;

                const value = Uint8Array.from(payload);
                const newBuf = new Uint8Array(inputBuffer.length + value.length);
                newBuf.set(inputBuffer, 0);
                newBuf.set(value, inputBuffer.length);
                inputBuffer = newBuf;
                processBuffer();
            });

            // Unexpected disconnect (device removed / error on port)
            unlistenSerialDisconnect = await listen('serial-disconnected', async () => {
                logAdd('Serial port disconnected (event)');
                // make sure our cleanup runs only once
                if (isConnected()) {
                    await disconnect();
                }
                // clear any state from the previous connection
                reset();
            });

            state.connection.connected = true;
            logAdd(`Connected to ${selectedPort.name} (native serial)`);

            // Initial data fetch
            await sendBinary(buildModulesListCmd());
            await sendBinary(buildMapListCmd());

            // Poll periodically since firmware does not send events on the binary CDC
            pollIntervalId = window.setInterval(async () => {
                if (!connectedPortName) return;
                await sendBinary(buildModulesListCmd());
                await sendBinary(buildMapListCmd());
            }, POLL_INTERVAL_MS);
        } catch (err) {
            logAdd(`Connect failed: ${err}`);
            console.error(err);
            await disconnect();
        }
    }

    async function disconnect() {
        if (pollIntervalId) {
            clearInterval(pollIntervalId);
            pollIntervalId = null;
        }

        if (unlistenSerialData) {
            unlistenSerialData();
            unlistenSerialData = null;
        }

        if (unlistenSerialDisconnect) {
            unlistenSerialDisconnect();
            unlistenSerialDisconnect = null;
        }

        if (connectedPortName) {
            try {
                await invoke('disconnect_serial');
            } catch (err) {
                console.warn(err);
            }
            connectedPortName = null;
        }

        inputBuffer = new Uint8Array(0);
        state.connection.connected = false;
        logAdd('Disconnected');
    }

    /**
     * Send a raw binary message to the device.
     */
    async function sendBinary(data: Uint8Array) {
        if (!connectedPortName) {
            logAdd('Cannot send: not connected');
            return;
        }
        await invoke('write_serial', { data: Array.from(data) });
    }

    /**
     * Process accumulated binary data, extracting and handling complete messages.
     */
    function processBuffer() {
        let result = extractMessage(inputBuffer);
        while (result) {
            const { message, remaining } = result;
            inputBuffer = new Uint8Array(remaining);

            const action = handleResponse(message);
            if (action && action.action === 'refresh_modules') {
                refreshModulesRequested = true;
            }
            if (action && action.action === 'refresh_mappings') {
                refreshMappingsRequested = true;
            }

            result = extractMessage(inputBuffer);
        }

        // Handle refresh requests triggered by events (future-proofing)
        if (refreshModulesRequested) {
            refreshModulesRequested = false;
            sendBinary(buildModulesListCmd());
        }
        if (refreshMappingsRequested) {
            refreshMappingsRequested = false;
            sendBinary(buildMapListCmd());
        }
    }

    function isConnected() {
        return !!connectedPortName;
    }

    return { connect, disconnect, sendBinary, isConnected };
}
