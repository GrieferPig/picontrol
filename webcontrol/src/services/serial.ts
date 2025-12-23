import { useStore } from '../composables/useStore';
import { useLogger } from '../composables/useLogger';
import { useProtocol } from './protocol';

// eslint-disable-next-line @typescript-eslint/no-explicit-any
let port: any | null = null;
let reader: ReadableStreamDefaultReader<string> | null = null;
let writer: WritableStreamDefaultWriter<string> | null = null;
let inputBuffer = '';
let refreshIntervalId: number | null = null;

export function useSerial() {
    const { state } = useStore();
    const { add: logAdd } = useLogger();
    const { handleLine } = useProtocol();

    async function connect() {
        if (port) {
            await disconnect();
            return;
        }
        try {
            // eslint-disable-next-line @typescript-eslint/no-explicit-any
            port = await (navigator as any).serial.requestPort();
            await port.open({ baudRate: 115200 });

            const textEncoder = new TextEncoderStream();
            textEncoder.readable.pipeTo(port.writable);
            writer = textEncoder.writable.getWriter();

            const textDecoder = new TextDecoderStream();
            port.readable.pipeTo(textDecoder.writable);
            reader = textDecoder.readable.getReader();

            state.connection.connected = true;
            logAdd('Connected');
            readLoop();

            await send('map load');
            await send('modules list');
            await send('map list');

            // Start auto-refresh every 500ms
            refreshIntervalId = window.setInterval(async () => {
                if (port && writer) {
                    await send('modules list');
                }
            }, 500);
        } catch (err) {
            logAdd(`Connect failed: ${err}`);
            console.error(err);
            await disconnect();
        }
    }

    async function disconnect() {
        if (refreshIntervalId) {
            clearInterval(refreshIntervalId);
            refreshIntervalId = null;
        }
        try {
            if (reader) await reader.cancel();
        } catch (err) {
            console.warn(err);
        }
        reader = null;

        if (writer) {
            writer.releaseLock();
            writer = null;
        }

        if (port) {
            try {
                await port.close();
            } catch (err) {
                console.warn(err);
            }
            port = null;
        }
        state.connection.connected = false;
        logAdd('Disconnected');
    }

    async function send(cmd: string) {
        if (!writer) {
            logAdd('Cannot send: not connected');
            return;
        }
        await writer.write(cmd + '\n');
        logAdd(`TX: ${cmd}`);
    }

    async function readLoop() {
        while (port && port.readable && reader) {
            try {
                const { value, done } = await reader.read();
                if (done) break;
                if (value) {
                    inputBuffer += value;
                    processBuffer();
                }
            } catch (err) {
                logAdd(`Read error: ${err}`);
                break;
            }
        }
    }

    function processBuffer() {
        const lines = inputBuffer.split(/\r?\n/);
        inputBuffer = lines.pop() || '';
        for (const line of lines) {
            if (!line.trim()) continue;
            handleLine(line.trim());
        }
    }

    function isConnected() {
        return !!port;
    }

    return { connect, disconnect, send, isConnected };
}
