<script setup lang="ts">
import { computed } from 'vue';
import { useStore } from '../composables/useStore';
import { useRouter } from '../services/router';
import type { State } from '../types';

const { state } = useStore();
const { send } = useRouter();

const rows = computed(() => state.ports.rows);
const cols = computed(() => state.ports.cols);

function clamp(v: number, lo: number, hi: number) {
    return Math.max(lo, Math.min(hi, v));
}

function getPlacementForKey(s: State, key: string) {
    const mod = s.modules[key];
    if (!mod) return null;
    const portInfo = s.ports.items[key] || { configured: false, hasModule: false, orientation: 0 };
    const ovr = (s.moduleUi && s.moduleUi.overrides && s.moduleUi.overrides[key]) || {};

    const baseSizeR = Math.max(1, mod.sizeR || 1);
    const baseSizeC = Math.max(1, mod.sizeC || 1);
    const overrideSizeR = ovr.sizeR != null ? Math.max(1, Number(ovr.sizeR) || 1) : null;
    const overrideSizeC = ovr.sizeC != null ? Math.max(1, Number(ovr.sizeC) || 1) : null;
    let sizeR = overrideSizeR ?? baseSizeR;
    let sizeC = overrideSizeC ?? baseSizeC;

    let portLocR = mod.portLocR || 0;
    let portLocC = mod.portLocC || 0;

    const rotate180Cfg = !!ovr.rotate180;
    const rotate180Applied = s.env && s.env.mode === 'mock' ? rotate180Cfg : false;
    const orientation = rotate180Applied ? ((portInfo.orientation + 2) % 4) : portInfo.orientation;

    let effectivePortLocR = portLocR;
    let effectivePortLocC = portLocC;
    let effectiveSizeR = sizeR;
    let effectiveSizeC = sizeC;

    switch (orientation) {
        case 0: // UP
            break;
        case 1: // RIGHT
            effectiveSizeR = sizeC;
            effectiveSizeC = sizeR;
            effectivePortLocR = portLocC;
            effectivePortLocC = sizeR - 1 - portLocR;
            break;
        case 2: // DOWN
            effectivePortLocR = sizeR - 1 - portLocR;
            effectivePortLocC = sizeC - 1 - portLocC;
            break;
        case 3: // LEFT
            effectiveSizeR = sizeC;
            effectiveSizeC = sizeR;
            effectivePortLocR = sizeC - 1 - portLocC;
            effectivePortLocC = portLocR;
            break;
    }

    const anchorR = mod.r - effectivePortLocR;
    const anchorC = mod.c - effectivePortLocC;

    effectiveSizeR = clamp(effectiveSizeR, 1, s.ports.rows || effectiveSizeR);
    effectiveSizeC = clamp(effectiveSizeC, 1, s.ports.cols || effectiveSizeC);

    const r = s.ports.rows || 0;
    const c = s.ports.cols || 0;
    const overflow = anchorR < 0 || anchorC < 0 || anchorR + effectiveSizeR > r || anchorC + effectiveSizeC > c;

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

const coverage = computed(() => {
    const coveredBy: Record<string, string> = {};
    const anchors: Record<string, any> = {};
    const errors: string[] = [];
    const occupiedBy: Record<string, string> = {};

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
                if (rr < 0 || cc < 0 || rr >= rows.value || cc >= cols.value) continue;
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
});

const gridCells = computed(() => {
    const cells = [];
    for (let r = 0; r < rows.value; r++) {
        for (let c = 0; c < cols.value; c++) {
            cells.push({ r, c, key: `${r},${c}` });
        }
    }
    return cells;
});

function selectModule(r: number, c: number) {
    if (state.selected && state.selected.r === r && state.selected.c === c) {
        // already selected
    } else {
        state.selected = { r, c, pid: null };
    }
}

async function toggleRot180(key: string, place: any) {
    const cur = state;
    const currentVal = !!((cur.moduleUi && cur.moduleUi.overrides && cur.moduleUi.overrides[key]) || {}).rotate180;
    const nextVal = !currentVal;

    // In real mode, persist on the device; in mock mode, keep it local.
    if (cur.env && cur.env.mode === 'real') {
        await send(`rot set ${place.portR} ${place.portC} ${nextVal ? 1 : 0}`);
        await send('modules list');
    }

    if (!state.moduleUi.overrides[key]) state.moduleUi.overrides[key] = {};
    state.moduleUi.overrides[key].rotate180 = nextVal;
}

function getOriLabel(ori: number) {
    return ['UP', 'RIGHT', 'DOWN', 'LEFT'][ori] || String(ori);
}
</script>

<template>
    <div class="grid-shell">
        <div class="grid-header">
            <div class="badge"><span>Ports</span><strong id="portCount">{{ rows }}x{{ cols }}</strong></div>
            <div class="badge"><span>Mappings</span><strong id="mappingCount">{{ state.mappings.length }}</strong></div>
            <span class="hint" id="gridHint">{{ rows && cols ? 'Click a module to edit parameters' : 'Connect or load mock data' }}</span>
        </div>

        <div v-if="coverage.errors.length" style="grid-column:1/-1; text-align:center; color: var(--muted); padding: 24px;">
            {{ coverage.errors.join(' | ') }}
        </div>

        <div v-else-if="!rows || !cols" class="grid-view" style="display: block;">
            <div style="text-align:center; color: var(--muted); padding: 40px;">Connect or load mock data to view modules.</div>
        </div>

        <div v-else class="grid-view" :style="{ gridTemplateColumns: `repeat(${cols}, minmax(180px, 1fr))` }">
            <div v-for="cell in gridCells" :key="cell.key" class="module-card"
                :class="{
                    covered: !!coverage.coveredBy[cell.key],
                    noport: !state.ports.items[cell.key]?.configured,
                    empty: state.ports.items[cell.key]?.configured && (!state.ports.items[cell.key]?.hasModule || !state.modules[cell.key])
                }"
                @click="() => {
                    if (coverage.coveredBy[cell.key]) {
                        const a = coverage.anchors[coverage.coveredBy[cell.key]];
                        if (a) selectModule(a.portR, a.portC);
                    } else if (!state.ports.items[cell.key]?.configured || !state.ports.items[cell.key]?.hasModule || !state.modules[cell.key]) {
                        state.selected = null;
                    } else {
                        selectModule(cell.r, cell.c);
                    }
                }"
            >
                <template v-if="coverage.coveredBy[cell.key]">
                    <div>Covered<br><span class="loc">→ ({{ coverage.coveredBy[cell.key] }})</span></div>
                </template>

                <template v-else-if="!state.ports.items[cell.key]?.configured">
                    <div>No port<br><span class="loc">({{ cell.r }},{{ cell.c }})</span></div>
                </template>

                <template v-else-if="!state.ports.items[cell.key]?.hasModule || !state.modules[cell.key]">
                    <div>
                        <span v-if="state.ports.items[cell.key]?.hasModule">Unidentified</span>
                        <span v-else>Empty</span>
                        <br><span class="loc">({{ cell.r }},{{ cell.c }})</span>
                    </div>
                </template>

                <template v-else>
                    <div style="display:flex; justify-content:space-between; align-items:center; gap:6px;">
                        <div>
                            <h3>{{ state.modules[cell.key].name || 'Module' }}</h3>
                            <div class="module-meta">
                                {{ state.modules[cell.key].mfg }}
                                {{ state.modules[cell.key].fw ? '• ' + state.modules[cell.key].fw : '' }}
                                {{ (state.modules[cell.key].caps & 1) ? ' • AU' : '' }}
                                • {{ coverage.anchors[cell.key]?.sizeR }}x{{ coverage.anchors[cell.key]?.sizeC }}
                                • {{ getOriLabel(coverage.anchors[cell.key]?.orientation) }}
                            </div>
                        </div>
                        <span class="loc">({{ cell.r }},{{ cell.c }})</span>
                    </div>
                    <div style="display:flex; gap:6px; flex-wrap:wrap;">
                        <button class="mini" @click.stop="toggleRot180(cell.key, coverage.anchors[cell.key])">
                            Rot 180°: {{ coverage.anchors[cell.key]?.rotate180 ? 'On' : 'Off' }}
                        </button>
                    </div>
                </template>
            </div>
        </div>
    </div>
</template>
