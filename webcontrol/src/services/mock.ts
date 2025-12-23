import { useStore } from '../composables/useStore';
import { useLogger } from '../composables/useLogger';
import { useProtocol } from './protocol';

let intervalId: number | null = null;

const sampleModules = [
    {
        r: 1,
        c: 2,
        type: 0,
        caps: 3, // AUTOUPDATE | ROTATION_AWARE
        name: 'Fader X',
        mfg: 'piControl',
        fw: '1.2.0',
        sizeR: 2,
        sizeC: 1,
        portLocR: 1,
        portLocC: 0,
        params: [
            { id: 0, dt: 0, name: 'Level', min: 0, max: 127, value: 64 },
            { id: 1, dt: 2, name: 'Mute', min: 0, max: 1, value: 0 },
        ],
    },
    {
        r: 1,
        c: 1,
        type: 1,
        caps: 1,
        name: 'Knob Y',
        mfg: 'piControl',
        fw: '1.0.5',
        sizeR: 1,
        sizeC: 1,
        portLocR: 0,
        portLocC: 0,
        params: [
            { id: 0, dt: 1, name: 'Pan', min: -1.0, max: 1.0, value: 0.0 },
            { id: 1, dt: 0, name: 'Gain', min: 0, max: 100, value: 35 },
        ],
    },
    {
        r: 2,
        c: 2,
        type: 2,
        caps: 0,
        name: 'Button Z',
        mfg: 'piControl',
        fw: '1.0.0',
        sizeR: 1,
        sizeC: 1,
        portLocR: 0,
        portLocC: 0,
        params: [
            { id: 0, dt: 2, name: 'Pressed', min: 0, max: 1, value: 0 },
        ],
    },
];

export function useMock() {
    const { state } = useStore();
    const { add: logAdd } = useLogger();
    const { handleLine } = useProtocol();

    function emitScript() {
        handleLine('ok ports rows=3 cols=3');

        const isCovered = (r: number, c: number) => {
            for (const m of sampleModules) {
                const sr = Math.max(1, m.sizeR || 1);
                const sc = Math.max(1, m.sizeC || 1);
                const plr = m.portLocR || 0;
                const plc = m.portLocC || 0;
                const anchorR = m.r - plr;
                const anchorC = m.c - plc;
                if (r >= anchorR && r < anchorR + sr && c >= anchorC && c < anchorC + sc) {
                    return true;
                }
            }
            return false;
        };

        for (let r = 0; r < 3; r++) {
            for (let c = 0; c < 3; c++) {
                const mod = sampleModules.find((m) => m.r === r && m.c === c);
                const configured = !(r === 0 && c === 0);
                const hasModule = configured && isCovered(r, c);
                handleLine(`port r=${r} c=${c} configured=${configured ? 1 : 0} hasModule=${hasModule ? 1 : 0} orientation=0`);
                if (!mod) continue;
                const plr = mod.portLocR || 0;
                const plc = mod.portLocC || 0;
                handleLine(`module r=${r} c=${c} type=${mod.type} caps=${mod.caps} name="${mod.name}" mfg="${mod.mfg}" fw="${mod.fw}" params=${mod.params.length} szr=${mod.sizeR || 1} szc=${mod.sizeC || 1} plr=${plr} plc=${plc}`);
                mod.params.forEach((p) => {
                    handleLine(`param r=${r} c=${c} pid=${p.id} dt=${p.dt} name="${p.name}" min=${p.min} max=${p.max} value=${p.value}`);
                });
            }
        }
        handleLine('ok modules done');
        logAdd('Mock grid loaded');
    }

    function emitMockMappings() {
        handleLine('ok count=3');
        handleLine('map 1 2 0 1 1 64');
        handleLine('map 1 1 0 2 3 16');
        handleLine('map 2 2 0 3 4 0');
        logAdd('Mock mappings injected');
    }

    function startPulse() {
        if (intervalId) return;
        intervalId = window.setInterval(() => {
            sampleModules.forEach((mod) => {
                mod.params.forEach((p: any) => {
                    if (p.dt === 0) {
                        const next = Math.max(Number(p.min), Math.min(Number(p.max), (Number(p.value) || 0) + (Math.random() > 0.5 ? 5 : -5)));
                        p.value = next;
                    }
                    if (p.dt === 1) {
                        const next = (Math.random() * (Number(p.max) - Number(p.min)) + Number(p.min)).toFixed(2);
                        p.value = next;
                    }
                    if (p.dt === 2) {
                        p.value = Math.random() > 0.7 ? 1 : 0;
                    }
                    handleLine(`param r=${mod.r} c=${mod.c} pid=${p.id} dt=${p.dt} name="${p.name}" min=${p.min} max=${p.max} value=${p.value}`);
                });
            });
        }, 900);
        logAdd('Mock value stream started');
    }

    function stopPulse() {
        if (intervalId) {
            clearInterval(intervalId);
            intervalId = null;
            logAdd('Mock value stream stopped');
        }
    }

    return { emitScript, emitMockMappings, startPulse, stopPulse };
}
