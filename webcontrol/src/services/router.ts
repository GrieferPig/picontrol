import { useStore } from '../composables/useStore';
import { useLogger } from '../composables/useLogger';
import { useSerial } from './serial';
import { useProtocol } from './protocol';

export function useRouter() {
    const { state } = useStore();
    const { add: logAdd } = useLogger();
    const { send: serialSend } = useSerial();
    const { handleLine } = useProtocol();

    function upsertMapping(r: number, c: number, pid: number, type: number, d1: number, d2: number) {
        const idx = state.mappings.findIndex((m) => m.r === r && m.c === c && m.pid === pid);
        const entry = { r, c, pid, type, d1, d2 };
        if (idx >= 0) state.mappings[idx] = entry;
        else state.mappings.push(entry);
    }

    function deleteMapping(r: number, c: number, pid: number) {
        state.mappings = state.mappings.filter((m) => !(m.r === r && m.c === c && m.pid === pid));
    }

    function emitMapListFromState() {
        handleLine(`ok count=${state.mappings.length}`);
        state.mappings.forEach((m) => {
            handleLine(`map ${m.r} ${m.c} ${m.pid} ${m.type} ${m.d1} ${m.d2}`);
        });
        handleLine('ok');
    }

    async function send(cmd: string) {
        const mode = state.env.mode;
        if (mode === 'real') {
            await serialSend(cmd);
            return;
        }

        // Mock mode: interpret a subset of CLI.
        logAdd(`MOCK: ${cmd}`);
        const parts = cmd.trim().split(/\s+/);
        const [head, sub] = parts;
        if (head === 'map' && sub === 'set' && parts.length >= 8) {
            const r = parseInt(parts[2] || '0', 10);
            const c = parseInt(parts[3] || '0', 10);
            const pid = parseInt(parts[4] || '0', 10);
            const type = parseInt(parts[5] || '0', 10);
            const d1 = parseInt(parts[6] || '0', 10);
            const d2 = parseInt(parts[7] || '0', 10);
            upsertMapping(r, c, pid, type, d1, d2);
            emitMapListFromState();
            return;
        }
        if (head === 'map' && sub === 'del' && parts.length >= 5) {
            const r = parseInt(parts[2] || '0', 10);
            const c = parseInt(parts[3] || '0', 10);
            const pid = parseInt(parts[4] || '0', 10);
            deleteMapping(r, c, pid);
            emitMapListFromState();
            return;
        }
        if (head === 'map' && sub === 'list') {
            emitMapListFromState();
            return;
        }
        if (head === 'map' && sub === 'save') {
            logAdd('MOCK: map save (no-op)');
            handleLine('ok');
            return;
        }
        if (head === 'modules' && sub === 'list') {
            logAdd('MOCK: modules list (use "Load Mock Grid" to populate)');
            handleLine('ok');
            return;
        }
        if (head === 'map' && sub === 'load') {
            logAdd('MOCK: map load (no-op)');
            handleLine('ok');
            return;
        }

        logAdd(`MOCK: unsupported command: ${cmd}`);
    }

    return { send };
}
