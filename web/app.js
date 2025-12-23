// --- Helpers ---
function midiNoteName(n) {
    if (typeof n !== 'number' || n < 0 || n > 127) return String(n);
    const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
    const name = names[n % 12];
    const octave = Math.floor(n / 12) - 1; // MIDI: C4 = 60
    return `${name}${octave}`;
}

function noteNumberToParts(n) {
    if (typeof n !== 'number' || n < 0 || n > 127) return { noteIndex: 0, octave: 4, noteNumber: 60 };
    return {
        noteIndex: n % 12,
        octave: Math.floor(n / 12) - 1,
        noteNumber: n,
    };
}

function notePartsToNumber(noteIndex, octave) {
    const idx = Math.max(0, Math.min(11, noteIndex | 0));
    const oct = Math.max(-1, Math.min(9, octave | 0));
    const n = (oct + 1) * 12 + idx;
    return Math.max(0, Math.min(127, n));
}

function hidModifierMaskFromEvent(e) {
    // USB HID modifier bits (common): 0x01 Ctrl, 0x02 Shift, 0x04 Alt, 0x08 GUI
    let m = 0;
    if (e.ctrlKey) m |= 0x01;
    if (e.shiftKey) m |= 0x02;
    if (e.altKey) m |= 0x04;
    if (e.metaKey) m |= 0x08;
    return m;
}

function hidKeycodeFromKeyboardEvent(e) {
    // Map common KeyboardEvent.code values to HID usage IDs.
    // Letters
    if (typeof e.code === 'string' && e.code.startsWith('Key') && e.code.length === 4) {
        const ch = e.code[3];
        const idx = ch.charCodeAt(0) - 'A'.charCodeAt(0);
        if (idx >= 0 && idx < 26) return 4 + idx;
    }
    // Digits
    if (typeof e.code === 'string' && e.code.startsWith('Digit') && e.code.length === 6) {
        const ch = e.code[5];
        if (ch >= '1' && ch <= '9') return 30 + (ch.charCodeAt(0) - '1'.charCodeAt(0));
        if (ch === '0') return 39;
    }

    const codeMap = {
        Enter: 40,
        Escape: 41,
        Backspace: 42,
        Tab: 43,
        Space: 44,
        Minus: 45,
        Equal: 46,
        BracketLeft: 47,
        BracketRight: 48,
        Backslash: 49,
        Semicolon: 51,
        Quote: 52,
        Backquote: 53,
        Comma: 54,
        Period: 55,
        Slash: 56,
        CapsLock: 57,
        ArrowRight: 79,
        ArrowLeft: 80,
        ArrowDown: 81,
        ArrowUp: 82,
        Delete: 76,
        Home: 74,
        End: 77,
        PageUp: 75,
        PageDown: 78,
    };
    if (e.code && codeMap[e.code] != null) return codeMap[e.code];
    return 0;
}

function formatKeyComboDisplay(keycode, modifierMask) {
    const parts = [];
    if (modifierMask & 0x01) parts.push('Ctrl');
    if (modifierMask & 0x02) parts.push('Shift');
    if (modifierMask & 0x04) parts.push('Alt');
    if (modifierMask & 0x08) parts.push('Meta');
    if (keycode) {
        const label = hidKeyLabel(keycode);
        // Drop the bracket portion for the combo display; keep it in hints.
        parts.push(label.replace(/\s\[\d+\]$/, ''));
    }
    return parts.join('+') || '';
}
function midiNoteLabel(n) {
    return `${midiNoteName(n)} [${n}]`;
}
function hidKeyLabel(code) {
    // Minimal HID Usage ID map (Keyboard/Keypad Page 0x07)
    const map = {
        4: 'A', 5: 'B', 6: 'C', 7: 'D', 8: 'E', 9: 'F', 10: 'G', 11: 'H', 12: 'I', 13: 'J', 14: 'K', 15: 'L', 16: 'M', 17: 'N', 18: 'O', 19: 'P', 20: 'Q', 21: 'R', 22: 'S', 23: 'T', 24: 'U', 25: 'V', 26: 'W', 27: 'X', 28: 'Y', 29: 'Z',
        30: '1', 31: '2', 32: '3', 33: '4', 34: '5', 35: '6', 36: '7', 37: '8', 38: '9', 39: '0',
        40: 'Enter', 41: 'Esc', 42: 'Backspace', 43: 'Tab', 44: 'Space', 45: '-', 46: '=', 47: '[', 48: ']', 49: '\\', 50: '#', 51: ';', 52: '\'', 53: '`', 54: ',', 55: '.', 56: '/',
        57: 'CapsLock',
    };
    const name = map[code] || `Keycode ${code}`;
    return `${name} [${code}]`;
}

// --- State Store ---
function createStore() {
    const state = {
        ports: { rows: 0, cols: 0, items: {} },
        modules: {},
        mappings: [],
        selected: null,
        connection: { connected: false },
        env: { mode: 'real' }, // 'real' | 'mock'
        moduleUi: {
            // key "r,c" => { sizeR, sizeC, rotate180 }
            overrides: {},
        },
    };
    const subscribers = new Set();

    const notify = () => subscribers.forEach((fn) => fn(state));

    return {
        getState: () => state,
        set(partial) {
            Object.assign(state, partial);
            notify();
        },
        update(updater) {
            updater(state);
            notify();
        },
        subscribe(fn) {
            subscribers.add(fn);
            return () => subscribers.delete(fn);
        },
        reset() {
            state.ports = { rows: 0, cols: 0, items: {} };
            state.modules = {};
            state.mappings = [];
            state.selected = null;
            notify();
        },
    };
}

// --- Logger ---
function createLogger(el) {
    const lines = [];
    function add(msg) {
        const stamp = new Date().toLocaleTimeString();
        const text = `[${stamp}] ${msg}`;
        lines.push(text);
        while (lines.length > 200) lines.shift();
        el.innerHTML = lines.map((l) => `<div>${l}</div>`).join('');
        el.scrollTop = el.scrollHeight;
    }
    return { add };
}

// --- Serial Transport ---
function createSerialTransport({ onLine, onConnect, onDisconnect, log }) {
    let port = null;
    let reader = null;
    let writer = null;
    let inputBuffer = '';
    let refreshIntervalId = null;

    async function connect() {
        if (port) {
            await disconnect();
            return;
        }
        try {
            port = await navigator.serial.requestPort();
            await port.open({ baudRate: 115200 });

            const textEncoder = new TextEncoderStream();
            textEncoder.readable.pipeTo(port.writable);
            writer = textEncoder.writable.getWriter();

            const textDecoder = new TextDecoderStream();
            port.readable.pipeTo(textDecoder.writable);
            reader = textDecoder.readable.getReader();

            onConnect();
            log.add('Connected');
            readLoop();

            await send('map load');
            await send('modules list');
            await send('map list');

            // Start auto-refresh every 500ms
            refreshIntervalId = setInterval(async () => {
                if (port && writer) {
                    await send('modules list');
                }
            }, 500);
        } catch (err) {
            log.add(`Connect failed: ${err}`);
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
        onDisconnect();
        log.add('Disconnected');
    }

    async function send(cmd) {
        if (!writer) {
            log.add('Cannot send: not connected');
            return;
        }
        await writer.write(cmd + '\n');
        log.add(`TX: ${cmd}`);
    }

    async function readLoop() {
        while (port && port.readable) {
            try {
                const { value, done } = await reader.read();
                if (done) break;
                if (value) {
                    inputBuffer += value;
                    processBuffer();
                }
            } catch (err) {
                log.add(`Read error: ${err}`);
                break;
            }
        }
    }

    function processBuffer() {
        const lines = inputBuffer.split(/\r?\n/);
        inputBuffer = lines.pop();
        for (const line of lines) {
            if (!line.trim()) continue;
            onLine(line.trim());
        }
    }

    return { connect, disconnect, send, isConnected: () => !!port };
}

// --- Protocol Parser ---
function createProtocol({ store, log }) {
    function handleLine(line) {
        if (line.startsWith('ok count=')) {
            store.update((s) => {
                s.mappings = [];
            });
            return;
        }

        if (line.startsWith('map ')) {
            const parts = line.split(' ');
            if (parts.length >= 7) {
                const entry = {
                    r: parseInt(parts[1], 10),
                    c: parseInt(parts[2], 10),
                    pid: parseInt(parts[3], 10),
                    type: parseInt(parts[4], 10),
                    d1: parseInt(parts[5], 10),
                    d2: parseInt(parts[6], 10),
                };
                store.update((s) => s.mappings.push(entry));
            }
            return;
        }

        if (line.startsWith('ok ports rows=')) {
            const m = line.match(/^ok ports rows=(\d+)\s+cols=(\d+)/);
            if (m) {
                store.update((s) => {
                    s.ports.rows = parseInt(m[1], 10);
                    s.ports.cols = parseInt(m[2], 10);
                    s.ports.items = {};
                    s.modules = {};
                });
            }
            return;
        }

        if (line.startsWith('port ')) {
            const m = line.match(/^port\s+r=(\d+)\s+c=(\d+)\s+configured=(\d+)\s+hasModule=(\d+)\s+orientation=(\d+)/);
            if (m) {
                const r = parseInt(m[1], 10);
                const c = parseInt(m[2], 10);
                const key = `${r},${c}`;
                store.update((s) => {
                    s.ports.items[key] = {
                        r,
                        c,
                        configured: parseInt(m[3], 10) !== 0,
                        hasModule: parseInt(m[4], 10) !== 0,
                        orientation: parseInt(m[5], 10),
                    };
                });
            }
            return;
        }

        if (line.startsWith('module ')) {
            const m = line.match(/^module\s+r=(\d+)\s+c=(\d+)\s+type=(\d+)\s+caps=(\d+)\s+name="([^"]*)"\s+mfg="([^"]*)"\s+fw="([^"]*)"\s+params=(\d+)(.*)$/);
            if (m) {
                const r = parseInt(m[1], 10);
                const c = parseInt(m[2], 10);
                const key = `${r},${c}`;

                const rest = m[9] || '';
                const readField = (label) => {
                    const mm = rest.match(new RegExp(`\\s${label}=([^\\s]+)`));
                    return mm ? mm[1] : null;
                };
                const szr = readField('szr');
                const szc = readField('szc');
                const plr = readField('plr');
                const plc = readField('plc');

                store.update((s) => {
                    s.modules[key] = {
                        r,
                        c,
                        type: parseInt(m[3], 10),
                        caps: parseInt(m[4], 10),
                        name: m[5],
                        mfg: m[6],
                        fw: m[7],
                        paramCount: parseInt(m[8], 10),
                        params: [],
                        // Physical size and port location within module
                        sizeR: szr != null ? parseInt(szr, 10) : 1,
                        sizeC: szc != null ? parseInt(szc, 10) : 1,
                        portLocR: plr != null ? parseInt(plr, 10) : 0,
                        portLocC: plc != null ? parseInt(plc, 10) : 0,
                    };
                });
            }
            return;
        }

        if (line.startsWith('param ')) {
            const m = line.match(/^param\s+r=(\d+)\s+c=(\d+)\s+pid=(\d+)\s+dt=(\d+)\s+name="([^"]*)"(.*)$/);
            if (m) {
                const r = parseInt(m[1], 10);
                const c = parseInt(m[2], 10);
                const pid = parseInt(m[3], 10);
                const dt = parseInt(m[4], 10);
                const name = m[5];
                const rest = m[6] || '';

                const readField = (label) => {
                    const mm = rest.match(new RegExp(`\\s${label}=([^\\s]+)`));
                    return mm ? mm[1] : null;
                };

                const param = {
                    id: pid,
                    dt,
                    name,
                    min: readField('min'),
                    max: readField('max'),
                    value: readField('value'),
                };

                const key = `${r},${c}`;
                store.update((s) => {
                    if (!s.modules[key]) {
                        s.modules[key] = { r, c, type: 0, name: '(unknown)', mfg: '', fw: '', paramCount: 0, params: [] };
                    }
                    const mod = s.modules[key];
                    const idx = mod.params.findIndex((p) => p.id === pid);
                    if (idx >= 0) mod.params[idx] = param;
                    else mod.params.push(param);
                });
            }
            return;
        }

        if (line.startsWith('ok')) {
            return;
        }

        log.add(`Unparsed line: ${line}`);
    }

    return { handleLine };
}

// --- Rendering ---
function createRenderer({ store, transport, log }) {
    const els = {
        grid: document.getElementById('grid'),
        portCount: document.getElementById('portCount'),
        mappingCount: document.getElementById('mappingCount'),
        gridHint: document.getElementById('gridHint'),
        editor: document.getElementById('editor'),
        editorEmpty: document.getElementById('editorEmpty'),
        selectionLabel: document.getElementById('selectionLabel'),
        status: document.getElementById('status'),
        paramPanel: document.getElementById('paramPanel'),
        paramPanelEmpty: document.getElementById('paramPanelEmpty'),
        paramPanelLabel: document.getElementById('paramPanelLabel'),
    };

    let lastEditorKey = null;
    let forceEditor = false;

    function clamp(v, lo, hi) {
        return Math.max(lo, Math.min(hi, v));
    }

    let invalidPlacementNotified = false;

    function getPlacementForKey(state, key) {
        const mod = state.modules[key];
        if (!mod) return null;
        const portInfo = state.ports.items[key] || { configured: false, hasModule: false, orientation: 0 };
        const ovr = (state.moduleUi && state.moduleUi.overrides && state.moduleUi.overrides[key]) || {};

        const baseSizeR = Math.max(1, mod.sizeR || 1);
        const baseSizeC = Math.max(1, mod.sizeC || 1);
        const overrideSizeR = ovr.sizeR != null ? Math.max(1, parseInt(ovr.sizeR, 10) || 1) : null;
        const overrideSizeC = ovr.sizeC != null ? Math.max(1, parseInt(ovr.sizeC, 10) || 1) : null;
        let sizeR = overrideSizeR ?? baseSizeR;
        let sizeC = overrideSizeC ?? baseSizeC;

        // Port location within module (0-indexed from top-left of module in UP orientation)
        let portLocR = mod.portLocR || 0;
        let portLocC = mod.portLocC || 0;

        const rotate180Cfg = !!ovr.rotate180;
        // In real mode, firmware reports effective orientation already; don't double-apply.
        const rotate180Applied = state.env && state.env.mode === 'mock' ? rotate180Cfg : false;
        const orientation = rotate180Applied ? ((portInfo.orientation + 2) % 4) : portInfo.orientation;

        // Transform port location and size based on orientation
        // orientation: 0=UP, 1=RIGHT, 2=DOWN, 3=LEFT
        let effectivePortLocR = portLocR;
        let effectivePortLocC = portLocC;
        let effectiveSizeR = sizeR;
        let effectiveSizeC = sizeC;

        switch (orientation) {
            case 0: // UP - no transform
                break;
            case 1: // RIGHT - 90° CW: swap dims, transform port loc
                effectiveSizeR = sizeC;
                effectiveSizeC = sizeR;
                effectivePortLocR = portLocC;
                effectivePortLocC = sizeR - 1 - portLocR;
                break;
            case 2: // DOWN - 180°: flip port loc within same dims
                effectivePortLocR = sizeR - 1 - portLocR;
                effectivePortLocC = sizeC - 1 - portLocC;
                break;
            case 3: // LEFT - 90° CCW: swap dims, transform port loc
                effectiveSizeR = sizeC;
                effectiveSizeC = sizeR;
                effectivePortLocR = sizeC - 1 - portLocC;
                effectivePortLocC = portLocR;
                break;
        }

        // Compute anchor cell: where the module's top-left corner is in the grid
        // The port is at (mod.r, mod.c), and portLoc tells us where in the module the port is
        const anchorR = mod.r - effectivePortLocR;
        const anchorC = mod.c - effectivePortLocC;

        effectiveSizeR = clamp(effectiveSizeR, 1, state.ports.rows || effectiveSizeR);
        effectiveSizeC = clamp(effectiveSizeC, 1, state.ports.cols || effectiveSizeC);

        const rows = state.ports.rows || 0;
        const cols = state.ports.cols || 0;
        const overflow = anchorR < 0 || anchorC < 0 || anchorR + effectiveSizeR > rows || anchorC + effectiveSizeC > cols;

        return {
            key,
            r: anchorR,
            c: anchorC,
            sizeR: effectiveSizeR,
            sizeC: effectiveSizeC,
            orientation,
            rotate180: rotate180Cfg,
            rotate180Applied,
            portR: mod.r,
            portC: mod.c,
            overflow,
            name: mod.name || key,
        };
    }

    function buildCoverage(state) {
        const coveredBy = {}; // cellKey -> anchorKey
        const anchors = {}; // anchorKey -> placement
        const rows = state.ports.rows || 0;
        const cols = state.ports.cols || 0;
        const errors = [];
        const occupiedBy = {}; // cellKey -> anchorKey

        for (const key of Object.keys(state.modules || {})) {
            const place = getPlacementForKey(state, key);
            if (!place) continue;
            if (place.overflow) {
                errors.push(`Module ${place.name} at (${place.portR},${place.portC}) exceeds grid after rotation/offset`);
                continue;
            }
            anchors[key] = place;

            for (let rr = place.r; rr < place.r + place.sizeR; rr++) {
                for (let cc = place.c; cc < place.c + place.sizeC; cc++) {
                    if (rr < 0 || cc < 0 || rr >= rows || cc >= cols) continue;
                    const cellKey = `${rr},${cc}`;
                    const existing = occupiedBy[cellKey];
                    if (existing && existing !== key) {
                        const a = anchors[existing];
                        errors.push(`Overlap at (${cellKey}): ${place.name} conflicts with ${(a && a.name) ? a.name : existing}`);
                        continue;
                    }
                    occupiedBy[cellKey] = key;

                    if (cellKey === key) continue;
                    coveredBy[cellKey] = key;
                }
            }
        }
        return { coveredBy, anchors, errors };
    }

    function getLayoutErrors(state) {
        const { errors } = buildCoverage(state);
        return errors || [];
    }

    function renderStatus() {
        const st = store.getState();
        const connected = st.connection.connected;
        const mode = st.env.mode;

        if (mode === 'mock') {
            els.status.textContent = 'Mock Mode';
            els.status.className = 'status connected';
            document.getElementById('btnConnect').textContent = 'Connect Device';
            return;
        }

        els.status.textContent = connected ? 'Connected' : 'Disconnected';
        els.status.className = connected ? 'status connected' : 'status';
        document.getElementById('btnConnect').textContent = connected ? 'Disconnect' : 'Connect Device';
    }

    function renderGrid() {
        const { ports, modules, mappings, selected } = store.getState();
        const rows = ports.rows;
        const cols = ports.cols;

        const { coveredBy, anchors, errors } = buildCoverage(store.getState());

        if (errors && errors.length) {
            const msg = errors.join(' | ');
            if (!invalidPlacementNotified) {
                alert(`Layout invalid after rotation/offset: ${msg}`);
                invalidPlacementNotified = true;
            }
            els.grid.innerHTML = `<div style="grid-column:1/-1; text-align:center; color: var(--muted); padding: 24px;">${msg}</div>`;
            return;
        } else {
            invalidPlacementNotified = false;
        }

        els.portCount.textContent = `${rows}x${cols}`;
        els.mappingCount.textContent = mappings.length;
        els.gridHint.textContent = rows && cols ? 'Click a parameter to edit' : 'Connect or load mock data';

        els.grid.innerHTML = '';
        if (!rows || !cols) {
            els.grid.innerHTML = '<div style="grid-column:1/-1; text-align:center; color: var(--muted); padding: 40px;">Connect or load mock data to view modules.</div>';
            return;
        }

        els.grid.style.gridTemplateColumns = `repeat(${cols}, minmax(180px, 1fr))`;

        for (let r = 0; r < rows; r++) {
            for (let c = 0; c < cols; c++) {
                const key = `${r},${c}`;
                const portInfo = ports.items[key] || { configured: false, hasModule: false, orientation: 0 };
                const mod = modules[key];
                const card = document.createElement('div');
                card.className = 'module-card';

                const anchorKey = coveredBy[key];
                if (anchorKey) {
                    card.classList.add('covered');
                    card.innerHTML = `<div>Covered<br><span class="loc">→ (${anchorKey})</span></div>`;
                    card.onclick = () => {
                        const a = anchors[anchorKey];
                        const m = store.getState().modules[anchorKey];
                        const firstPid = m && m.params && m.params.length ? m.params.slice().sort((x, y) => x.id - y.id)[0].id : null;
                        if (a && firstPid != null) selectParam(a.r, a.c, firstPid, null, null);
                    };
                    els.grid.appendChild(card);
                    continue;
                }

                if (!portInfo.configured) {
                    card.classList.add('noport');
                    card.innerHTML = `<div>No port<br><span class="loc">(${r},${c})</span></div>`;
                    card.onclick = () => {
                        store.update((s) => {
                            s.selected = null;
                        });
                        renderGrid();
                    };
                    els.grid.appendChild(card);
                    continue;
                }

                if (!portInfo.hasModule || !mod) {
                    card.classList.add('empty');
                    card.innerHTML = `<div>Empty<br><span class="loc">(${r},${c})</span></div>`;
                    card.onclick = () => {
                        store.update((s) => {
                            s.selected = null;
                        });
                        renderGrid();
                    };
                    els.grid.appendChild(card);
                    continue;
                }

                const caps = mod.caps || 0;
                const capStr = (caps & 1) ? ' • AU' : '';

                const place = anchors[key] || getPlacementForKey(store.getState(), key) || { sizeR: 1, sizeC: 1, orientation: portInfo.orientation, rotate180: false };
                const oriLabel = ['UP', 'RIGHT', 'DOWN', 'LEFT'][place.orientation] || String(place.orientation);

                card.innerHTML = `
          <div style="display:flex; justify-content:space-between; align-items:center; gap:6px;">
            <div>
              <h3>${mod.name || 'Module'}</h3>
              <div class="module-meta">${mod.mfg || ''} ${mod.fw ? '• ' + mod.fw : ''}${capStr} • ${place.sizeR}x${place.sizeC} • ${oriLabel}</div>
            </div>
            <span class="loc">(${r},${c})</span>
          </div>
                    <div style="display:flex; gap:6px; flex-wrap:wrap;">
                        <button class="mini" data-action="rot180">Rot 180°: ${place.rotate180 ? 'On' : 'Off'}</button>
                    </div>
          <div class="param-list" id="params-${r}-${c}"></div>
        `;

                const btnRot = card.querySelector('button[data-action="rot180"]');
                if (btnRot) {
                    btnRot.onclick = async (e) => {
                        e.stopPropagation();

                        // Simulate the change first; if it makes layout invalid, reject it.
                        const cur = store.getState();
                        const currentVal = !!((cur.moduleUi && cur.moduleUi.overrides && cur.moduleUi.overrides[key]) || {}).rotate180;
                        const nextVal = !currentVal;
                        const next = {
                            ...cur,
                            moduleUi: {
                                ...(cur.moduleUi || {}),
                                overrides: {
                                    ...((cur.moduleUi && cur.moduleUi.overrides) || {}),
                                    [key]: {
                                        ...(((cur.moduleUi && cur.moduleUi.overrides && cur.moduleUi.overrides[key]) || {})),
                                        rotate180: nextVal,
                                    },
                                },
                            },
                        };

                        // Validate as if the rotation override affects layout.
                        // In real mode, firmware already reports effective orientation. To simulate
                        // overrides locally we first normalize orientations back to the base
                        // (pre-override) value using the *current* overrides.
                        const basePorts = { ...((cur.ports && cur.ports.items) || {}) };
                        const curOverrides = ((cur.moduleUi && cur.moduleUi.overrides) || {});
                        for (const k of Object.keys(basePorts)) {
                            const p = basePorts[k];
                            if (!p) continue;
                            const o = curOverrides[k];
                            if (o && o.rotate180) {
                                basePorts[k] = { ...p, orientation: ((p.orientation + 2) % 4) };
                            }
                        }

                        const nextValidate = {
                            ...next,
                            env: { ...(next.env || {}), mode: 'mock' },
                            ports: { ...(next.ports || {}), items: basePorts },
                        };

                        const errs = getLayoutErrors(nextValidate);
                        if (errs.length) {
                            alert(`Layout invalid after rotation/offset: ${errs.join(' | ')}`);
                            return;
                        }

                        // In real mode, persist on the device; in mock mode, keep it local.
                        if (cur.env && cur.env.mode === 'real') {
                            await transport.send(`rot set ${place.portR} ${place.portC} ${nextVal ? 1 : 0}`);
                            await transport.send('modules list');
                        }

                        store.update((s) => {
                            s.moduleUi = s.moduleUi || { overrides: {} };
                            s.moduleUi.overrides = s.moduleUi.overrides || {};
                            const o = s.moduleUi.overrides[key] || {};
                            o.rotate180 = nextVal;
                            s.moduleUi.overrides[key] = o;
                        });
                    };
                }


                const paramsContainer = card.querySelector('.param-list');
                const slotMappings = mappings.filter((m) => m.r === r && m.c === c);
                const params = (mod.params || []).slice().sort((a, b) => a.id - b.id);

                params.forEach((p) => {
                    const mapping = slotMappings.find((x) => x.pid === p.id);
                    const item = document.createElement('div');
                    item.className = 'param-item';
                    if (selected && selected.r === r && selected.c === c && selected.pid === p.id) {
                        item.classList.add('selected');
                    }

                    let info = 'Unmapped';
                    if (mapping) {
                        if (mapping.type === 1) info = `Note ${midiNoteLabel(mapping.d2)} (Ch${mapping.d1})`;
                        else if (mapping.type === 2) info = `CC ${mapping.d2} (Ch${mapping.d1})`;
                        else if (mapping.type === 3) info = `Key ${hidKeyLabel(mapping.d1)}`;
                    }

                    // Create editable value input
                    const valStr = p.value != null ? p.value : '';
                    const valInput = document.createElement('input');
                    valInput.type = p.dt === 2 ? 'checkbox' : 'number';
                    valInput.className = 'param-value-input';
                    valInput.style.cssText = 'width: 60px; padding: 2px 4px; font-size: 11px; background: #0c0f18; border: 1px solid #3a3f4b; border-radius: 4px; color: #d1d5db; margin-left: 6px;';

                    if (p.dt === 2) {
                        // Bool type: checkbox
                        valInput.checked = p.value == 1 || p.value === 'true' || p.value === '1';
                        valInput.style.width = 'auto';
                    } else {
                        valInput.value = valStr;
                        if (p.min != null) valInput.min = p.min;
                        if (p.max != null) valInput.max = p.max;
                        if (p.dt === 1) valInput.step = '0.01'; // Float
                    }

                    // Stop click propagation to prevent selecting the param when editing value
                    valInput.onclick = (e) => e.stopPropagation();

                    // Send param set command on change
                    valInput.onchange = async (e) => {
                        e.stopPropagation();
                        let newVal;
                        if (p.dt === 2) {
                            newVal = valInput.checked ? '1' : '0';
                        } else {
                            newVal = valInput.value;
                        }
                        await transport.send(`param set ${r} ${c} ${p.id} ${p.dt} ${newVal}`);
                    };

                    item.innerHTML = `<span>${p.name || 'Param ' + p.id}</span><span class="val">${info}</span>`;
                    // Insert value input before the mapping info
                    const nameSpan = item.querySelector('span');
                    nameSpan.appendChild(valInput);

                    item.onclick = () => selectParam(r, c, p.id, mapping, p);
                    paramsContainer.appendChild(item);
                });

                els.grid.appendChild(card);
            }
        }
    }

    function selectParam(r, c, pid, mapping, paramInfo) {
        store.update((s) => {
            s.selected = { r, c, pid };
        });
        lastEditorKey = `${r},${c}:${pid}`;
        renderEditor(mapping, paramInfo);
        renderGrid();
    }

    function renderEditor(mapping, paramInfo) {
        const { selected } = store.getState();
        if (!selected) {
            els.editor.style.display = 'none';
            els.editorEmpty.style.display = 'block';
            els.selectionLabel.textContent = 'Select a parameter';
            return;
        }

        els.editor.style.display = 'grid';
        els.editorEmpty.style.display = 'none';
        els.selectionLabel.textContent = `(${selected.r},${selected.c}) pid ${selected.pid}`;

        const name = paramInfo && paramInfo.name ? paramInfo.name : `Param ${selected.pid}`;
        const mappingType = mapping ? mapping.type : 0;
        const d1 = mapping ? mapping.d1 : 1;
        const d2 = mapping ? mapping.d2 : 0;

        const noteParts = noteNumberToParts(d2);
        const noteNames = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
        const noteOptions = noteNames
            .map((n, idx) => `<option value="${idx}" ${idx === noteParts.noteIndex ? 'selected' : ''}>${n}</option>`)
            .join('');

        const initialHint = (() => {
            if (!mapping) return 'Unmapped';
            if (mapping.type === 1) return `MIDI Note: ${midiNoteLabel(mapping.d2)} (Ch${mapping.d1})`;
            if (mapping.type === 2) return `MIDI CC: CC ${mapping.d2} (Ch${mapping.d1})`;
            if (mapping.type === 3) return `Keyboard: ${hidKeyLabel(mapping.d1)} (mod ${mapping.d2})`;
            return 'Unmapped';
        })();

        els.editor.innerHTML = `
      <div class="form-group">
        <label>Selected parameter</label>
        <input type="text" id="editTargetName" value="(${selected.r},${selected.c}) ${name}" readonly>
      </div>
            <div class="muted" id="editHumanHint" style="font-size:12px;">${initialHint}</div>
      <div class="form-group">
        <label>Action</label>
        <select id="editType">
          <option value="0">None</option>
          <option value="1">MIDI Note</option>
          <option value="2">MIDI CC</option>
          <option value="3">Keyboard</option>
        </select>
      </div>
      <div id="groupMidi" class="field-row" style="display:none;">
        <div class="form-group">
          <label>Channel (1-16)</label>
          <input type="number" id="editCh" min="1" max="16" value="${d1}">
        </div>
                <div class="form-group" id="groupMidiNoteName" style="display:none;">
                    <label>Note Name</label>
                    <select id="editNoteName">${noteOptions}</select>
                </div>
                <div class="form-group" id="groupMidiNoteOct" style="display:none;">
                    <label>Octave</label>
                    <input type="number" id="editOctave" min="-1" max="9" value="${noteParts.octave}">
                </div>
                <div class="form-group" id="groupMidiCc" style="display:none;">
                    <label>CC (0-127) <span class="muted" id="editNumHuman"></span></label>
                    <input type="number" id="editCc" min="0" max="127" value="${d2}">
                </div>
      </div>
            <div class="muted" id="editMidiHint" style="display:none; font-size:12px;"></div>
      <div id="groupKey" class="field-row" style="display:none;">
        <div class="form-group">
                    <label>Press key combo</label>
                    <input type="text" id="editKeyCombo" placeholder="Click here then press keys" value="${formatKeyComboDisplay(d1, d2)}" readonly>
                    <div class="muted" id="editKeyHuman" style="font-size:12px;"></div>
                </div>
      </div>
            <div class="muted" id="editKeyHint" style="display:none; font-size:12px;"></div>
      <div class="button-row">
        <button id="btnApply" class="primary">Apply</button>
        <button id="btnDelete" style="background:#a33; border-color:#a33;">Delete</button>
      </div>
    `;

        const typeSelect = document.getElementById('editType');
        typeSelect.value = mappingType;

        const syncEditorFields = () => {
            const t = parseInt(typeSelect.value, 10);
            document.getElementById('groupMidi').style.display = t === 1 || t === 2 ? 'grid' : 'none';
            document.getElementById('groupKey').style.display = t === 3 ? 'grid' : 'none';
            document.getElementById('editMidiHint').style.display = t === 1 || t === 2 ? 'block' : 'none';
            document.getElementById('editKeyHint').style.display = t === 3 ? 'block' : 'none';

            const showNote = t === 1;
            const showCc = t === 2;
            const nn = document.getElementById('groupMidiNoteName');
            const no = document.getElementById('groupMidiNoteOct');
            const cc = document.getElementById('groupMidiCc');
            if (nn) nn.style.display = showNote ? 'block' : 'none';
            if (no) no.style.display = showNote ? 'block' : 'none';
            if (cc) cc.style.display = showCc ? 'block' : 'none';
        };
        typeSelect.onchange = syncEditorFields;
        syncEditorFields();

        let capturedKeycode = d1;
        let capturedModmask = d2;

        const updateHints = () => {
            const t = parseInt(typeSelect.value, 10);
            const human = document.getElementById('editHumanHint');
            const midiHint = document.getElementById('editMidiHint');
            const keyHint = document.getElementById('editKeyHint');
            const numHuman = document.getElementById('editNumHuman');
            const keyHuman = document.getElementById('editKeyHuman');

            if (t === 1) {
                const ch = parseInt(document.getElementById('editCh').value || '1', 10);
                const noteIndex = parseInt(document.getElementById('editNoteName').value || '0', 10);
                const octave = parseInt(document.getElementById('editOctave').value || '4', 10);
                const nn = notePartsToNumber(noteIndex, octave);
                const s = `MIDI Note: ${midiNoteLabel(nn)} (Ch${ch})`;
                human.textContent = s;
                midiHint.textContent = `Note name: ${midiNoteLabel(nn)}`;
                if (numHuman) numHuman.textContent = `(${midiNoteLabel(nn)})`;
                if (keyHuman) keyHuman.textContent = '';
            } else if (t === 2) {
                const ch = parseInt(document.getElementById('editCh').value || '1', 10);
                const cc = parseInt(document.getElementById('editCc').value || '0', 10);
                const s = `MIDI CC: CC ${cc} (Ch${ch})`;
                human.textContent = s;
                midiHint.textContent = `Controller: CC ${cc} [${cc}]`;
                if (numHuman) numHuman.textContent = `([${cc}])`;
                if (keyHuman) keyHuman.textContent = '';
            } else if (t === 3) {
                const key = capturedKeycode | 0;
                const mod = capturedModmask | 0;
                const s = `Keyboard: ${formatKeyComboDisplay(key, mod)} (${hidKeyLabel(key)} mod ${mod})`;
                human.textContent = s;
                keyHint.textContent = `Key: ${hidKeyLabel(key)}  ModMask: ${mod}`;
                if (keyHuman) keyHuman.textContent = `${formatKeyComboDisplay(key, mod)}  •  ${hidKeyLabel(key)}  •  mod ${mod}`;
                if (numHuman) numHuman.textContent = '';
            } else {
                human.textContent = 'Unmapped';
                if (numHuman) numHuman.textContent = '';
                if (keyHuman) keyHuman.textContent = '';
            }
        };

        // Update hints on any relevant input changes.
        typeSelect.onchange = () => {
            syncEditorFields();
            updateHints();
        };
        ['editCh', 'editNoteName', 'editOctave', 'editCc'].forEach((id) => {
            const el = document.getElementById(id);
            if (el) el.oninput = updateHints;
        });

        const combo = document.getElementById('editKeyCombo');
        if (combo) {
            combo.addEventListener('keydown', (e) => {
                e.preventDefault();
                e.stopPropagation();
                if (e.key === 'Escape' || e.key === 'Backspace') {
                    capturedKeycode = 0;
                    capturedModmask = 0;
                    combo.value = '';
                    updateHints();
                    return;
                }

                const keycode = hidKeycodeFromKeyboardEvent(e);
                const modmask = hidModifierMaskFromEvent(e);
                if (keycode) capturedKeycode = keycode;
                capturedModmask = modmask;
                combo.value = formatKeyComboDisplay(capturedKeycode, capturedModmask);
                updateHints();
            });
        }
        updateHints();

        document.getElementById('btnApply').onclick = async () => {
            const t = parseInt(typeSelect.value, 10);
            let d1v = 0;
            let d2v = 0;
            if (t === 1 || t === 2) {
                d1v = parseInt(document.getElementById('editCh').value || '0', 10);
                if (t === 1) {
                    const noteIndex = parseInt(document.getElementById('editNoteName').value || '0', 10);
                    const octave = parseInt(document.getElementById('editOctave').value || '4', 10);
                    d2v = notePartsToNumber(noteIndex, octave);
                } else {
                    d2v = parseInt(document.getElementById('editCc').value || '0', 10);
                }
            } else if (t === 3) {
                d1v = capturedKeycode | 0;
                d2v = capturedModmask | 0;
            }
            await transport.send(`map set ${selected.r} ${selected.c} ${selected.pid} ${t} ${d1v} ${d2v}`);
            await transport.send('map list');
            forceEditor = true; // refresh editor fields from mapping after map list arrives
        };

        document.getElementById('btnDelete').onclick = async () => {
            await transport.send(`map del ${selected.r} ${selected.c} ${selected.pid}`);
            await transport.send('map list');
            forceEditor = true;
        };
    }

    function renderAll() {
        renderStatus();
        renderGrid();
        const st = store.getState();
        if (st.selected && st.selected.pid != null) {
            const { selected, modules, mappings } = st;
            const keyParam = `${selected.r},${selected.c}:${selected.pid}`;
            if (forceEditor || keyParam !== lastEditorKey) {
                const keyPort = `${selected.r},${selected.c}`;
                const mod = modules[keyPort];
                const param = mod ? (mod.params || []).find((p) => p.id === selected.pid) : null;
                const mapping = mappings.find((m) => m.r === selected.r && m.c === selected.c && m.pid === selected.pid);
                renderEditor(mapping, param);
                lastEditorKey = keyParam;
                forceEditor = false;
            }
        } else {
            renderEditor(null, null);
        }
    }

    return { renderAll, forceEditorRefresh: () => { forceEditor = true; } };
}

// --- Mock Tool ---
function createMockTool({ pushLine, store, log }) {
    let intervalId = null;

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
            // Port is at row 1 within the 2x1 module (bottom cell)
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

    function emitScript() {
        pushLine('ok ports rows=3 cols=3');

        // Compute which cells are covered by each module using portLocation
        const isCovered = (r, c) => {
            for (const m of sampleModules) {
                const sr = Math.max(1, m.sizeR || 1);
                const sc = Math.max(1, m.sizeC || 1);
                const plr = m.portLocR || 0;
                const plc = m.portLocC || 0;
                // Anchor is top-left of module
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
                const configured = !(r === 0 && c === 0); // mimic missing physical port
                const hasModule = configured && isCovered(r, c);
                pushLine(`port r=${r} c=${c} configured=${configured ? 1 : 0} hasModule=${hasModule ? 1 : 0} orientation=0`);
                if (!mod) continue;
                const plr = mod.portLocR || 0;
                const plc = mod.portLocC || 0;
                pushLine(`module r=${r} c=${c} type=${mod.type} caps=${mod.caps} name="${mod.name}" mfg="${mod.mfg}" fw="${mod.fw}" params=${mod.params.length} szr=${mod.sizeR || 1} szc=${mod.sizeC || 1} plr=${plr} plc=${plc}`);
                mod.params.forEach((p) => {
                    pushLine(`param r=${r} c=${c} pid=${p.id} dt=${p.dt} name="${p.name}" min=${p.min} max=${p.max} value=${p.value}`);
                });
            }
        }
        pushLine('ok modules done');
        log.add('Mock grid loaded');
    }

    function emitMockMappings() {
        pushLine('ok count=3');
        pushLine('map 1 2 0 1 1 64');
        pushLine('map 1 1 0 2 3 16');
        pushLine('map 2 2 0 3 4 0');
        log.add('Mock mappings injected');
    }

    function startPulse() {
        if (intervalId) return;
        intervalId = setInterval(() => {
            sampleModules.forEach((mod) => {
                mod.params.forEach((p) => {
                    if (p.dt === 0) {
                        const next = Math.max(p.min, Math.min(p.max, (p.value || 0) + (Math.random() > 0.5 ? 5 : -5)));
                        p.value = next;
                    }
                    if (p.dt === 1) {
                        const next = (Math.random() * (p.max - p.min) + p.min).toFixed(2);
                        p.value = next;
                    }
                    if (p.dt === 2) {
                        p.value = Math.random() > 0.7 ? 1 : 0;
                    }
                    pushLine(`param r=${mod.r} c=${mod.c} pid=${p.id} dt=${p.dt} name="${p.name}" min=${p.min} max=${p.max} value=${p.value}`);
                });
            });
        }, 900);
        log.add('Mock value stream started');
    }

    function stopPulse() {
        if (intervalId) {
            clearInterval(intervalId);
            intervalId = null;
            log.add('Mock value stream stopped');
        }
    }

    return { emitScript, emitMockMappings, startPulse, stopPulse };
}

// --- Command Router (real vs mock) ---
function createCommandRouter({ store, transport, protocol, log }) {
    function upsertMapping(r, c, pid, type, d1, d2) {
        store.update((s) => {
            const idx = s.mappings.findIndex((m) => m.r === r && m.c === c && m.pid === pid);
            const entry = { r, c, pid, type, d1, d2 };
            if (idx >= 0) s.mappings[idx] = entry;
            else s.mappings.push(entry);
        });
    }

    function deleteMapping(r, c, pid) {
        store.update((s) => {
            s.mappings = s.mappings.filter((m) => !(m.r === r && m.c === c && m.pid === pid));
        });
    }

    function emitMapListFromState() {
        const { mappings } = store.getState();
        protocol.handleLine(`ok count=${mappings.length}`);
        mappings.forEach((m) => {
            protocol.handleLine(`map ${m.r} ${m.c} ${m.pid} ${m.type} ${m.d1} ${m.d2}`);
        });
        protocol.handleLine('ok');
    }

    async function send(cmd) {
        const mode = store.getState().env.mode;
        if (mode === 'real') {
            await transport.send(cmd);
            return;
        }

        // Mock mode: interpret a subset of CLI.
        log.add(`MOCK: ${cmd}`);
        const parts = cmd.trim().split(/\s+/);
        const [head, sub] = parts;
        if (head === 'map' && sub === 'set' && parts.length >= 8) {
            const r = parseInt(parts[2], 10);
            const c = parseInt(parts[3], 10);
            const pid = parseInt(parts[4], 10);
            const type = parseInt(parts[5], 10);
            const d1 = parseInt(parts[6], 10);
            const d2 = parseInt(parts[7], 10);
            upsertMapping(r, c, pid, type, d1, d2);
            emitMapListFromState();
            return;
        }
        if (head === 'map' && sub === 'del' && parts.length >= 5) {
            const r = parseInt(parts[2], 10);
            const c = parseInt(parts[3], 10);
            const pid = parseInt(parts[4], 10);
            deleteMapping(r, c, pid);
            emitMapListFromState();
            return;
        }
        if (head === 'map' && sub === 'list') {
            emitMapListFromState();
            return;
        }
        if (head === 'map' && sub === 'save') {
            log.add('MOCK: map save (no-op)');
            protocol.handleLine('ok');
            return;
        }
        if (head === 'modules' && sub === 'list') {
            log.add('MOCK: modules list (use "Load Mock Grid" to populate)');
            protocol.handleLine('ok');
            return;
        }
        if (head === 'map' && sub === 'load') {
            log.add('MOCK: map load (no-op)');
            protocol.handleLine('ok');
            return;
        }

        log.add(`MOCK: unsupported command: ${cmd}`);
    }

    return { send };
}

// --- Wire Up ---
(function main() {
    const store = createStore();
    const logger = createLogger(document.getElementById('log'));
    const protocol = createProtocol({ store, log: logger });
    const transport = createSerialTransport({
        onLine: protocol.handleLine,
        onConnect: () => store.update((s) => { s.connection.connected = true; }),
        onDisconnect: () => store.update((s) => { s.connection.connected = false; }),
        log: logger,
    });
    const router = createCommandRouter({ store, transport, protocol, log: logger });
    const renderer = createRenderer({ store, transport: { send: router.send }, log: logger });
    const mock = createMockTool({ pushLine: protocol.handleLine, store, log: logger });
    const mockPanel = document.getElementById('mockPanel');

    store.subscribe(renderer.renderAll);
    renderer.renderAll();

    document.getElementById('btnConnect').onclick = () => {
        if (store.getState().env.mode === 'mock') {
            logger.add('Mock mode: Connect is disabled');
            return;
        }
        transport.connect();
    };
    document.getElementById('btnRefresh').onclick = async () => {
        await router.send('modules list');
        await router.send('map list');
    };
    document.getElementById('btnSave').onclick = () => router.send('map save');
    document.getElementById('btnClear').onclick = () => store.reset();

    document.getElementById('btnMockLoad').onclick = () => mock.emitScript();
    document.getElementById('btnMockMappings').onclick = () => mock.emitMockMappings();
    document.getElementById('btnMockPulse').onclick = () => mock.startPulse();
    document.getElementById('btnMockStop').onclick = () => mock.stopPulse();

    const toggle = document.getElementById('toggleMock');
    if (toggle) {
        const syncMockPanel = () => {
            if (mockPanel) mockPanel.style.display = toggle.checked ? 'block' : 'none';
        };

        toggle.onchange = async () => {
            const enable = !!toggle.checked;
            if (enable) {
                if (transport.isConnected()) {
                    await transport.disconnect();
                }
                store.update((s) => {
                    s.env.mode = 'mock';
                    s.connection.connected = false;
                });
                logger.add('Mock mode enabled');
            } else {
                store.update((s) => {
                    s.env.mode = 'real';
                });
                logger.add('Mock mode disabled (real mode)');
            }
            syncMockPanel();
            renderer.renderAll();
        };

        syncMockPanel();
    }

    // Expose for quick debugging in devtools.
    window.piControl = { store, transport, router, mock, renderer };
})();
