<script setup lang="ts">
import { computed } from 'vue';
import { useStore } from '../composables/useStore';
import { useRouter } from '../services/router';
import { midiNoteLabel, hidKeyLabel } from '../utils';
import type { ModuleParam } from '../types';

const { state } = useStore();
const { send } = useRouter();

const selectedModule = computed(() => {
    if (!state.selected) return null;
    const key = `${state.selected.r},${state.selected.c}`;
    return state.modules[key];
});

const params = computed(() => {
    if (!selectedModule.value) return [];
    return (selectedModule.value.params || []).slice().sort((a, b) => a.id - b.id);
});

const slotMappings = computed(() => {
    if (!state.selected) return [];
    return state.mappings.filter((m) => m.r === state.selected!.r && m.c === state.selected!.c);
});

function getMappingInfo(pid: number) {
    const mapping = slotMappings.value.find((x) => x.pid === pid);
    if (!mapping) return 'Unmapped';
    if (mapping.type === 1) return `Note ${midiNoteLabel(mapping.d2)} (Ch${mapping.d1})`;
    if (mapping.type === 2) return `CC ${mapping.d2} (Ch${mapping.d1})`;
    if (mapping.type === 3) return `Key ${hidKeyLabel(mapping.d1)}`;
    if (mapping.type === 4) return `Pitch Bend (Ch${mapping.d1})`;
    if (mapping.type === 5) return `Mod Wheel (Ch${mapping.d1})`;
    return 'Unmapped';
}

function selectParam(pid: number) {
    if (state.selected) {
        state.selected.pid = pid;
    }
}

async function updateParam(p: ModuleParam, newVal: string | number) {
    if (!state.selected) return;
    const key = `${state.selected.r},${state.selected.c}`;
    const m = state.modules[key];
    if (m) {
        const pp = m.params.find(x => x.id === p.id);
        if (pp) {
            pp.value = newVal;
            pp.pendingUpdate = Date.now();
        }
    }
    await send(`param set ${state.selected.r} ${state.selected.c} ${p.id} ${p.dt} ${newVal}`);
}

function parseLEDValue(val: string | number | undefined): { r: number, g: number, b: number, status: number } {
    if (typeof val === 'string') {
        const parts = val.split(',').map(x => parseInt(x, 10));
        return { r: parts[0] || 0, g: parts[1] || 0, b: parts[2] || 0, status: parts[3] || 0 };
    }
    return { r: 0, g: 0, b: 0, status: 0 };
}

async function updateLED(p: ModuleParam, r: number, g: number, b: number, status: number) {
    const newVal = `${r},${g},${b},${status}`;
    await updateParam(p, newVal);
}

// Calibration functions
function canCalibrate(p: ModuleParam): boolean {
    // Only non-bool, read-only parameters can be calibrated
    return p.dt !== 2 && (p.access & 2) === 0;
}

async function startCalibration(p: ModuleParam) {
    if (!state.selected) return;
    
    // First, un-calibrate by setting extreme min/max values
    // This ensures the firmware sends unclamped raw sensor values
    const extremeMin = p.dt === 1 ? -1000000 : 0; // float vs int
    const extremeMax = p.dt === 1 ? 1000000 : 1023;
    
    await send(`calib set ${state.selected.r} ${state.selected.c} ${p.id} ${extremeMin} ${extremeMax}`);
    
    // Update UI range to reflect uncalibrated state
    p.min = extremeMin;
    p.max = extremeMax;
    
    // Start tracking calibration
    p.calibrating = true;
    p.calibMin = undefined;
    p.calibMax = undefined;
}

function stopCalibration(p: ModuleParam) {
    p.calibrating = false;
}

async function applyCalibration(p: ModuleParam) {
    if (!state.selected || p.calibMin === undefined || p.calibMax === undefined) return;
    
    await send(`calib set ${state.selected.r} ${state.selected.c} ${p.id} ${Math.floor(p.calibMin)} ${Math.floor(p.calibMax)}`);
    
    // Update min/max in UI
    p.min = Math.floor(p.calibMin);
    p.max = Math.floor(p.calibMax);
    p.calibrating = false;
    p.calibMin = undefined;
    p.calibMax = undefined;
}

async function setCalibManual(p: ModuleParam, minVal: number, maxVal: number) {
    if (!state.selected) return;
    
    await send(`calib set ${state.selected.r} ${state.selected.c} ${p.id} ${minVal} ${maxVal}`);
    
    // Update min/max in UI
    p.min = minVal;
    p.max = maxVal;
}
</script>

<template>
    <div class="panel">
        <h2>
            Parameter Control
            <small class="muted">{{ selectedModule ? `${selectedModule.name || 'Module'} (${selectedModule.r},${selectedModule.c})` : 'Select a module' }}</small>
        </h2>
        <div v-if="!selectedModule" class="hint-card">Select a module from the grid to view parameters.</div>
        <div v-else class="editor-panel" style="display:grid">
            <div v-for="p in params" :key="p.id" class="param-item" :class="{ selected: state.selected?.pid === p.id }" @click="selectParam(p.id)">
                <!-- Parameter name and value -->
                <div class="param-row">
                    <span class="param-name">{{ p.name || 'Param ' + p.id }}</span>
                    
                    <!-- Boolean input -->
                    <input v-if="p.dt === 2" type="checkbox" class="param-input-bool"
                        :checked="p.value == 1 || p.value === 'true' || p.value === '1'"
                        :disabled="(p.access & 2) === 0"
                        @click.stop
                        @change="(e) => updateParam(p, (e.target as HTMLInputElement).checked ? '1' : '0')"
                    />
                    
                    <!-- LED input -->
                    <div v-else-if="p.dt === 3" class="led-controls" @click.stop>
                        <input type="color" class="param-input-color"
                            :value="'#' + parseLEDValue(p.value).r.toString(16).padStart(2, '0') + parseLEDValue(p.value).g.toString(16).padStart(2, '0') + parseLEDValue(p.value).b.toString(16).padStart(2, '0')"
                            :disabled="(p.access & 2) === 0"
                            @change="(e) => {
                                const hex = (e.target as HTMLInputElement).value;
                                const r = parseInt(hex.substr(1, 2), 16);
                                const g = parseInt(hex.substr(3, 2), 16);
                                const b = parseInt(hex.substr(5, 2), 16);
                                updateLED(p, r, g, b, parseLEDValue(p.value).status);
                            }"
                        />
                        <label class="led-status-label">
                            <input type="checkbox" class="param-input-bool"
                                :checked="parseLEDValue(p.value).status === 1"
                                :disabled="(p.access & 2) === 0"
                                @change="(e) => {
                                    const led = parseLEDValue(p.value);
                                    updateLED(p, led.r, led.g, led.b, (e.target as HTMLInputElement).checked ? 1 : 0);
                                }"
                            />
                            <span>On</span>
                        </label>
                    </div>
                    
                    <!-- Numeric input (int/float) -->
                    <input v-else type="number" class="param-input-num"
                        :value="p.value"
                        :disabled="(p.access & 2) === 0"
                        :min="p.min" :max="p.max" :step="p.dt === 1 ? '0.01' : '1'"
                        @click.stop
                        @change="(e) => updateParam(p, (e.target as HTMLInputElement).value)"
                    />
                </div>
                
                <span class="val">{{ getMappingInfo(p.id) }}</span>
                
                <!-- Calibration UI for read-only non-bool parameters -->
                <div v-if="canCalibrate(p)" class="calib-section" @click.stop>
                    <div class="calib-header">
                        <span class="calib-label">Range: {{ p.min }} - {{ p.max }}</span>
                        <button v-if="!p.calibrating" class="btn-calib" @click="startCalibration(p)">
                            Calibrate
                        </button>
                    </div>
                    
                    <div v-if="p.calibrating" class="calib-active">
                        <div class="calib-instructions">
                            Move the sensor through its full range. Current: {{ p.value }}
                        </div>
                        <div class="calib-range">
                            <span>Min: {{ p.calibMin ?? 'N/A' }}</span>
                            <span>Max: {{ p.calibMax ?? 'N/A' }}</span>
                        </div>
                        <div class="calib-buttons">
                            <button class="btn-calib-apply" @click="applyCalibration(p)" :disabled="p.calibMin === undefined || p.calibMax === undefined">
                                Apply
                            </button>
                            <button class="btn-calib-cancel" @click="stopCalibration(p)">
                                Cancel
                            </button>
                        </div>
                    </div>
                    
                    <!-- Manual calibration edit -->
                    <div v-if="!p.calibrating" class="calib-manual">
                        <input type="number" class="calib-input" :value="p.min" @change="(e) => {
                            const minVal = parseInt((e.target as HTMLInputElement).value, 10);
                            const maxVal = typeof p.max === 'number' ? p.max : parseInt(p.max as string, 10);
                            setCalibManual(p, minVal, maxVal);
                        }" />
                        <span>to</span>
                        <input type="number" class="calib-input" :value="p.max" @change="(e) => {
                            const minVal = typeof p.min === 'number' ? p.min : parseInt(p.min as string, 10);
                            const maxVal = parseInt((e.target as HTMLInputElement).value, 10);
                            setCalibManual(p, minVal, maxVal);
                        }" />
                    </div>
                </div>
            </div>
        </div>
    </div>
</template>

<style scoped>
.param-item {
    display: flex;
    flex-direction: column;
    padding: 12px;
    border-bottom: 1px solid #2a2f3b;
    cursor: pointer;
    gap: 8px;
}
.param-item:hover {
    background: #1c1f2b;
}
.param-item.selected {
    background: #2c3f5b;
}
.param-row {
    display: flex;
    align-items: center;
    gap: 8px;
}
.param-name {
    font-weight: 500;
    flex: 1;
}
.param-input-num {
    width: 70px;
    padding: 4px 8px;
    font-size: 12px;
    background: #0c0f18;
    border: 1px solid #3a3f4b;
    border-radius: 4px;
    color: #d1d5db;
    transition: border-color 0.2s;
}
.param-input-num:focus {
    border-color: #3b82f6;
    outline: none;
}
.param-input-bool {
    width: 16px;
    height: 16px;
    accent-color: #3b82f6;
    cursor: pointer;
}
.param-input-color {
    width: 50px;
    height: 30px;
    border: 1px solid #3a3f4b;
    border-radius: 4px;
    cursor: pointer;
    background: #0c0f18;
}
.led-controls {
    display: flex;
    align-items: center;
    gap: 8px;
}
.led-status-label {
    display: flex;
    align-items: center;
    gap: 4px;
    font-size: 12px;
    color: #9ca3af;
    cursor: pointer;
}
.val {
    font-size: 11px;
    color: #6b7280;
}
.calib-section {
    margin-top: 4px;
    padding: 8px;
    background: #0f1219;
    border-radius: 4px;
    font-size: 11px;
}
.calib-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 4px;
}
.calib-label {
    color: #9ca3af;
}
.btn-calib {
    padding: 3px 8px;
    font-size: 11px;
    background: #374151;
    border: 1px solid #4b5563;
    border-radius: 3px;
    color: #d1d5db;
    cursor: pointer;
    transition: background 0.2s;
}
.btn-calib:hover {
    background: #4b5563;
}
.calib-active {
    padding: 8px;
    background: #1a1f2e;
    border-radius: 4px;
    border: 1px solid #3b82f6;
}
.calib-instructions {
    color: #3b82f6;
    font-weight: 500;
    margin-bottom: 6px;
}
.calib-range {
    display: flex;
    justify-content: space-between;
    color: #d1d5db;
    margin-bottom: 8px;
}
.calib-buttons {
    display: flex;
    gap: 6px;
}
.btn-calib-apply, .btn-calib-cancel {
    padding: 4px 10px;
    font-size: 11px;
    border-radius: 3px;
    cursor: pointer;
    border: 1px solid;
    transition: all 0.2s;
}
.btn-calib-apply {
    background: #10b981;
    border-color: #059669;
    color: white;
}
.btn-calib-apply:hover:not(:disabled) {
    background: #059669;
}
.btn-calib-apply:disabled {
    opacity: 0.5;
    cursor: not-allowed;
}
.btn-calib-cancel {
    background: #374151;
    border-color: #4b5563;
    color: #d1d5db;
}
.btn-calib-cancel:hover {
    background: #4b5563;
}
.calib-manual {
    display: flex;
    align-items: center;
    gap: 6px;
    margin-top: 6px;
}
.calib-input {
    width: 60px;
    padding: 3px 6px;
    font-size: 11px;
    background: #0c0f18;
    border: 1px solid #3a3f4b;
    border-radius: 3px;
    color: #d1d5db;
}
.calib-input:focus {
    border-color: #3b82f6;
    outline: none;
}
</style>
