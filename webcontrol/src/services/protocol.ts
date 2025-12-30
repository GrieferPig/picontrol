import { useStore } from '../composables/useStore';
import { useLogger } from '../composables/useLogger';
import type { ModuleParam } from '../types';

export function useProtocol() {
    const { state } = useStore();
    const { add: logAdd } = useLogger();

    function handleLine(line: string): { action?: string } | void {
        if (line.startsWith('event ')) {
            const evt = line.substring(6);

            if (evt.startsWith('port_connected ')) {
                const m = evt.match(/^port_connected\s+r=(\d+)\s+c=(\d+)\s+orientation=(\d+)/);
                if (m) {
                    const r = parseInt(m[1] || '0', 10);
                    const c = parseInt(m[2] || '0', 10);
                    const key = `${r},${c}`;
                    if (!state.ports.items[key]) {
                        state.ports.items[key] = { r, c, configured: false, hasModule: false, orientation: 0 };
                    }
                    state.ports.items[key].configured = true;
                    state.ports.items[key].hasModule = true; // Mark as present but unidentified
                    state.ports.items[key].orientation = parseInt(m[3] || '0', 10);

                    // Clear any old module data so it shows as unidentified
                    delete state.modules[key];
                }
                return;
            }

            if (evt.startsWith('port_disconnected ')) {
                const m = evt.match(/^port_disconnected\s+r=(\d+)\s+c=(\d+)/);
                if (m) {
                    const r = parseInt(m[1] || '0', 10);
                    const c = parseInt(m[2] || '0', 10);
                    const key = `${r},${c}`;
                    if (state.ports.items[key]) {
                        state.ports.items[key].configured = false;
                        state.ports.items[key].hasModule = false;
                    }
                    delete state.modules[key];
                }
                return;
            }

            if (evt.startsWith('module_ready ')) {
                // The module is ready to be queried.
                // We return a special signal so the caller can trigger a refresh.
                return { action: 'refresh_modules' };
            }

            if (evt.startsWith('mappings_loaded ')) {
                // Mappings were loaded from a module (or reloaded).
                // Trigger a map list refresh so UI reflects persisted mappings.
                return { action: 'refresh_mappings' };
            }

            if (evt.startsWith('module_found ')) {
                const m = evt.match(/^module_found\s+r=(\d+)\s+c=(\d+)\s+type=(\d+)\s+caps=(\d+)\s+name="([^"]*)"\s+mfg="([^"]*)"\s+fw="([^"]*)"\s+params=(\d+)(.*)$/);
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

                    if (state.ports.items[key]) {
                        state.ports.items[key].hasModule = true;
                    }

                    // Always overwrite on module_found
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
                return;
            }

            if (evt.startsWith('param_def ')) {
                const m = evt.match(/^param_def\s+r=(\d+)\s+c=(\d+)\s+pid=(\d+)\s+dt=(\d+)\s+name="([^"]*)"(.*)$/);
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
                    const access = 3; // Default for param_def

                    const key = `${r},${c}`;
                    if (!state.modules[key]) {
                        // Should not happen if module_found came first, but safety check
                        state.modules[key] = { r, c, type: 0, caps: 0, name: '(unknown)', mfg: '', fw: '', paramCount: 0, params: [], sizeR: 1, sizeC: 1, portLocR: 0, portLocC: 0 };
                    }
                    const mod = state.modules[key];
                    // Remove existing if any (redefinition)
                    mod.params = mod.params.filter(p => p.id !== pid);
                    mod.params.push({
                        id: pid,
                        dt,
                        access,
                        name,
                        min,
                        max,
                        value,
                    });
                    mod.params.sort((a, b) => a.id - b.id);
                }
                return;
            }

            if (evt.startsWith('param_changed ')) {
                const m = evt.match(/^param_changed\s+r=(\d+)\s+c=(\d+)\s+pid=(\d+)\s+value=(.*)$/);
                if (m) {
                    const r = parseInt(m[1] || '0', 10);
                    const c = parseInt(m[2] || '0', 10);
                    const pid = parseInt(m[3] || '0', 10);
                    const value = m[4] || '';

                    const key = `${r},${c}`;
                    const mod = state.modules[key];
                    if (mod) {
                        const p = mod.params.find(x => x.id === pid);
                        if (p) {
                            if (p.pendingUpdate && Date.now() - p.pendingUpdate < 2000) {
                                // ignore
                            } else {
                                p.value = value;
                            }
                        }
                    }
                }
                return;
            }
        }

        if (line.startsWith('ok count=')) {
            state.mappings = [];
            return;
        }

        if (line.startsWith('map ')) {
            const parts = line.split(' ');
            if (parts.length >= 7) {
                const entry: any = {
                    r: parseInt(parts[1] || '0', 10),
                    c: parseInt(parts[2] || '0', 10),
                    pid: parseInt(parts[3] || '0', 10),
                    type: parseInt(parts[4] || '0', 10),
                    d1: parseInt(parts[5] || '0', 10),
                    d2: parseInt(parts[6] || '0', 10),
                };

                // Parse curve=HEX
                const curvePart = parts.find(p => p.startsWith('curve='));
                if (curvePart) {
                    const hex = curvePart.substring(6);
                    if (hex && hex !== '00') {
                        // Parse hex string
                        // Format: count(1 char if <16), points(count*4 chars), controls((count-1)*4 chars)
                        // Wait, firmware prints count as HEX without leading zero if < 16.
                        // But points/controls are printed with leading zero (2 chars per byte).

                        // Actually, let's look at the hex string structure.
                        // If count is 2 (char '2'), then 4 bytes points (8 chars), 2 bytes controls (4 chars).
                        // Total 1 + 8 + 4 = 13 chars.
                        // If count is 10 (char 'A'), then ...

                        // Let's parse it carefully.
                        const countChar = hex.substring(0, 1); // First char is count?
                        // Wait, if count >= 16, it would be 2 chars?
                        // Firmware: UsbSerial.print(m->curve.count, HEX);
                        // If count is 16 (0x10), it prints "10".
                        // If count is 15 (0xF), it prints "F".
                        // So length of count part is variable?
                        // But max count is 4 (from firmware check: if (curve.count < 2 || curve.count > 4)).
                        // So count is always single digit hex (2, 3, 4).

                        const count = parseInt(hex.substring(0, 1), 16);
                        const dataHex = hex.substring(1);

                        const points = [];
                        const controls = [];

                        let ptr = 0;
                        for (let i = 0; i < count; i++) {
                            const x = parseInt(dataHex.substr(ptr, 2), 16);
                            const y = parseInt(dataHex.substr(ptr + 2, 2), 16);
                            points.push({ x, y });
                            ptr += 4;
                        }
                        for (let i = 0; i < count - 1; i++) {
                            const x = parseInt(dataHex.substr(ptr, 2), 16);
                            const y = parseInt(dataHex.substr(ptr + 2, 2), 16);
                            controls.push({ x, y });
                            ptr += 4;
                        }

                        entry.curve = { count, points, controls };
                    }
                }

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
            const m = line.match(/^param\s+r=(\d+)\s+c=(\d+)\s+pid=(\d+)\s+dt=(\d+)\s+access=(\d+)\s+name="([^"]*)"(.*)$/);
            if (m) {
                const r = parseInt(m[1] || '0', 10);
                const c = parseInt(m[2] || '0', 10);
                const pid = parseInt(m[3] || '0', 10);
                const dt = parseInt(m[4] || '0', 10);
                const access = parseInt(m[5] || '0', 10);
                const name = m[6] || '';
                const rest = m[7] || '';

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
                        existing.access = access;
                        existing.name = name;
                        existing.min = min;
                        existing.max = max;
                    }
                } else {
                    mod.params.push({
                        id: pid,
                        dt,
                        access,
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
