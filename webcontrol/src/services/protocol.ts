import { useStore } from '../composables/useStore';
import { useLogger } from '../composables/useLogger';
import type { ModuleParam } from '../types';

export function useProtocol() {
    const { state } = useStore();
    const { add: logAdd } = useLogger();

    function handleLine(line: string) {
        if (line.startsWith('ok count=')) {
            state.mappings = [];
            return;
        }

        if (line.startsWith('map ')) {
            const parts = line.split(' ');
            if (parts.length >= 7) {
                const entry = {
                    r: parseInt(parts[1] || '0', 10),
                    c: parseInt(parts[2] || '0', 10),
                    pid: parseInt(parts[3] || '0', 10),
                    type: parseInt(parts[4] || '0', 10),
                    d1: parseInt(parts[5] || '0', 10),
                    d2: parseInt(parts[6] || '0', 10),
                };
                state.mappings.push(entry);
            }
            return;
        }

        if (line.startsWith('ok ports rows=')) {
            const m = line.match(/^ok ports rows=(\d+)\s+cols=(\d+)/);
            if (m) {
                state.ports.rows = parseInt(m[1] || '0', 10);
                state.ports.cols = parseInt(m[2] || '0', 10);
                state.ports.items = {};
                state.modules = {};
            }
            return;
        }

        if (line.startsWith('port ')) {
            const m = line.match(/^port\s+r=(\d+)\s+c=(\d+)\s+configured=(\d+)\s+hasModule=(\d+)\s+orientation=(\d+)/);
            if (m) {
                const r = parseInt(m[1] || '0', 10);
                const c = parseInt(m[2] || '0', 10);
                const key = `${r},${c}`;
                state.ports.items[key] = {
                    r,
                    c,
                    configured: parseInt(m[3] || '0', 10) !== 0,
                    hasModule: parseInt(m[4] || '0', 10) !== 0,
                    orientation: parseInt(m[5] || '0', 10),
                };
            }
            return;
        }

        if (line.startsWith('module ')) {
            const m = line.match(/^module\s+r=(\d+)\s+c=(\d+)\s+type=(\d+)\s+caps=(\d+)\s+name="([^"]*)"\s+mfg="([^"]*)"\s+fw="([^"]*)"\s+params=(\d+)(.*)$/);
            if (m) {
                const r = parseInt(m[1] || '0', 10);
                const c = parseInt(m[2] || '0', 10);
                const key = `${r},${c}`;

                const rest = m[9] || '';
                const readField = (label: string) => {
                    const mm = rest.match(new RegExp(`\\s${label}=([^\\s]+)`));
                    return mm ? mm[1] : null;
                };
                const szr = readField('szr');
                const szc = readField('szc');
                const plr = readField('plr');
                const plc = readField('plc');

                const type = parseInt(m[3] || '0', 10);
                const caps = parseInt(m[4] || '0', 10);
                const name = m[5] || '';
                const mfg = m[6] || '';
                const fw = m[7] || '';
                const paramCount = parseInt(m[8] || '0', 10);
                const sizeR = szr != null ? parseInt(szr, 10) : 1;
                const sizeC = szc != null ? parseInt(szc, 10) : 1;
                const portLocR = plr != null ? parseInt(plr, 10) : 0;
                const portLocC = plc != null ? parseInt(plc, 10) : 0;

                if (state.modules[key]) {
                    const mod = state.modules[key];
                    if (mod.type !== type) {
                        mod.params = [];
                    }
                    mod.type = type;
                    mod.caps = caps;
                    mod.name = name;
                    mod.mfg = mfg;
                    mod.fw = fw;
                    mod.paramCount = paramCount;
                    mod.sizeR = sizeR;
                    mod.sizeC = sizeC;
                    mod.portLocR = portLocR;
                    mod.portLocC = portLocC;
                } else {
                    state.modules[key] = {
                        r,
                        c,
                        type,
                        caps,
                        name,
                        mfg,
                        fw,
                        paramCount,
                        params: [],
                        sizeR,
                        sizeC,
                        portLocR,
                        portLocC,
                    };
                }
            }
            return;
        }

        if (line.startsWith('param ')) {
            const m = line.match(/^param\s+r=(\d+)\s+c=(\d+)\s+pid=(\d+)\s+dt=(\d+)\s+name="([^"]*)"(.*)$/);
            if (m) {
                const r = parseInt(m[1] || '0', 10);
                const c = parseInt(m[2] || '0', 10);
                const pid = parseInt(m[3] || '0', 10);
                const dt = parseInt(m[4] || '0', 10);
                const name = m[5] || '';
                const rest = m[6] || '';

                const readField = (label: string) => {
                    const mm = rest.match(new RegExp(`\\s${label}=([^\\s]+)`));
                    return mm ? mm[1] : null;
                };

                const min = readField('min') || undefined;
                const max = readField('max') || undefined;
                const value = readField('value') || undefined;

                const key = `${r},${c}`;
                if (!state.modules[key]) {
                    state.modules[key] = { r, c, type: 0, caps: 0, name: '(unknown)', mfg: '', fw: '', paramCount: 0, params: [], sizeR: 1, sizeC: 1, portLocR: 0, portLocC: 0 };
                }
                const mod = state.modules[key];
                const idx = mod.params.findIndex((p) => p.id === pid);
                if (idx >= 0) {
                    const existing = mod.params[idx];
                    if (existing) {
                        if (existing.pendingUpdate && Date.now() - existing.pendingUpdate < 2000) {
                            // Keep existing value if pending update
                        } else {
                            existing.value = value;
                        }
                        existing.dt = dt;
                        existing.name = name;
                        existing.min = min;
                        existing.max = max;
                    }
                } else {
                    mod.params.push({
                        id: pid,
                        dt,
                        name,
                        min,
                        max,
                        value,
                    });
                }
            }
            return;
        }

        if (line.startsWith('ok')) {
            return;
        }

        logAdd(`Unparsed line: ${line}`);
    }

    return { handleLine };
}
