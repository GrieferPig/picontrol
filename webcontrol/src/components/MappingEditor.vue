<script setup lang="ts">
import { computed, ref, watch, nextTick } from 'vue';
import { useStore } from '../composables/useStore';
import { useRouter } from '../services/router';
import { midiNoteLabel, hidKeyLabel, noteNumberToParts, notePartsToNumber, formatKeyComboDisplay, hidKeycodeFromKeyboardEvent, hidModifierMaskFromEvent } from '../utils';

const { state } = useStore();
const { send } = useRouter();

const selectedParam = computed(() => {
    if (!state.selected || state.selected.pid == null) return null;
    const key = `${state.selected.r},${state.selected.c}`;
    const mod = state.modules[key];
    if (!mod) return null;
    return mod.params.find(p => p.id === state.selected!.pid);
});

const mapping = computed(() => {
    if (!state.selected || state.selected.pid == null) return null;
    return state.mappings.find(m => m.r === state.selected!.r && m.c === state.selected!.c && m.pid === state.selected!.pid);
});

const editType = ref(0);
const editCh = ref(1);
const editNoteIndex = ref(0);
const editOctave = ref(4);
const editCc = ref(0);
const capturedKeycode = ref(0);
const capturedModmask = ref(0);

const noteNames = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

watch(() => state.selected?.pid, () => {
    if (mapping.value) {
        editType.value = mapping.value.type;
        editCh.value = mapping.value.d1;
        if (mapping.value.type === 1) {
            const parts = noteNumberToParts(mapping.value.d2);
            editNoteIndex.value = parts.noteIndex;
            editOctave.value = parts.octave;
        } else if (mapping.value.type === 2) {
            editCc.value = mapping.value.d2;
        } else if (mapping.value.type === 3) {
            capturedKeycode.value = mapping.value.d1;
            capturedModmask.value = mapping.value.d2;
        }
    } else {
        editType.value = 0;
        editCh.value = 1;
        editNoteIndex.value = 0;
        editOctave.value = 4;
        editCc.value = 0;
        capturedKeycode.value = 0;
        capturedModmask.value = 0;
    }
}, { immediate: true });

const humanHint = computed(() => {
    const t = Number(editType.value);
    if (t === 1) {
        const nn = notePartsToNumber(editNoteIndex.value, editOctave.value);
        return `MIDI Note: ${midiNoteLabel(nn)} (Ch${editCh.value})`;
    } else if (t === 2) {
        return `MIDI CC: CC ${editCc.value} (Ch${editCh.value})`;
    } else if (t === 3) {
        return `Keyboard: ${formatKeyComboDisplay(capturedKeycode.value, capturedModmask.value)} (${hidKeyLabel(capturedKeycode.value)} mod ${capturedModmask.value})`;
    }
    return 'Unmapped';
});

const midiHint = computed(() => {
    const t = Number(editType.value);
    if (t === 1) {
        const nn = notePartsToNumber(editNoteIndex.value, editOctave.value);
        return `Note name: ${midiNoteLabel(nn)}`;
    } else if (t === 2) {
        return `Controller: CC ${editCc.value} [${editCc.value}]`;
    }
    return '';
});

const keyHint = computed(() => {
    const t = Number(editType.value);
    if (t === 3) {
        return `Key: ${hidKeyLabel(capturedKeycode.value)}  ModMask: ${capturedModmask.value}`;
    }
    return '';
});

const numHuman = computed(() => {
    const t = Number(editType.value);
    if (t === 1) {
        const nn = notePartsToNumber(editNoteIndex.value, editOctave.value);
        return `(${midiNoteLabel(nn)})`;
    } else if (t === 2) {
        return `([${editCc.value}])`;
    }
    return '';
});

const keyHuman = computed(() => {
    const t = Number(editType.value);
    if (t === 3) {
        return `${formatKeyComboDisplay(capturedKeycode.value, capturedModmask.value)}  •  ${hidKeyLabel(capturedKeycode.value)}  •  mod ${capturedModmask.value}`;
    }
    return '';
});

function handleKeyCombo(e: KeyboardEvent) {
    e.preventDefault();
    e.stopPropagation();
    if (e.key === 'Escape' || e.key === 'Backspace') {
        capturedKeycode.value = 0;
        capturedModmask.value = 0;
        return;
    }

    const keycode = hidKeycodeFromKeyboardEvent(e);
    const modmask = hidModifierMaskFromEvent(e);
    if (keycode) capturedKeycode.value = keycode;
    capturedModmask.value = modmask;
}

async function apply() {
    if (!state.selected || state.selected.pid == null) return;
    const t = Number(editType.value);
    let d1v = 0;
    let d2v = 0;
    if (t === 1 || t === 2) {
        d1v = Number(editCh.value);
        if (t === 1) {
            d2v = notePartsToNumber(editNoteIndex.value, editOctave.value);
        } else {
            d2v = Number(editCc.value);
        }
    } else if (t === 3) {
        d1v = capturedKeycode.value;
        d2v = capturedModmask.value;
    }
    await send(`map set ${state.selected.r} ${state.selected.c} ${state.selected.pid} ${t} ${d1v} ${d2v}`);
    await send('map list');
}

async function del() {
    if (!state.selected || state.selected.pid == null) return;
    await send(`map del ${state.selected.r} ${state.selected.c} ${state.selected.pid}`);
    await send('map list');
}
</script>

<template>
    <div class="panel">
        <h2>
            Mapping Editor
            <small class="muted" id="selectionLabel">{{ selectedParam ? `(${state.selected?.r},${state.selected?.c}) pid ${state.selected?.pid}` : 'Select a parameter' }}</small>
        </h2>
        <div v-if="!selectedParam" class="hint-card">Pick a parameter from the grid to edit its action.</div>
        <div v-else class="editor-panel" style="display:grid">
            <div class="form-group">
                <label>Selected parameter</label>
                <input type="text" :value="`(${state.selected?.r},${state.selected?.c}) ${selectedParam.name}`" readonly>
            </div>
            <div class="muted" style="font-size:12px;">{{ humanHint }}</div>
            <div class="form-group">
                <label>Action</label>
                <select v-model="editType">
                    <option :value="0">None</option>
                    <option :value="1">MIDI Note</option>
                    <option :value="2">MIDI CC</option>
                    <option :value="3">Keyboard</option>
                </select>
            </div>

            <div v-if="editType == 1 || editType == 2" class="field-row" style="display:grid">
                <div class="form-group">
                    <label>Channel (1-16)</label>
                    <input type="number" v-model="editCh" min="1" max="16">
                </div>
                <template v-if="editType == 1">
                    <div class="form-group">
                        <label>Note Name</label>
                        <select v-model="editNoteIndex">
                            <option v-for="(n, idx) in noteNames" :key="idx" :value="idx">{{ n }}</option>
                        </select>
                    </div>
                    <div class="form-group">
                        <label>Octave</label>
                        <input type="number" v-model="editOctave" min="-1" max="9">
                    </div>
                </template>
                <template v-if="editType == 2">
                    <div class="form-group">
                        <label>CC (0-127) <span class="muted">{{ numHuman }}</span></label>
                        <input type="number" v-model="editCc" min="0" max="127">
                    </div>
                </template>
            </div>
            <div v-if="editType == 1 || editType == 2" class="muted" style="font-size:12px;">{{ midiHint }}</div>

            <div v-if="editType == 3" class="field-row" style="display:grid">
                <div class="form-group">
                    <label>Press key combo</label>
                    <input type="text" placeholder="Click here then press keys" :value="formatKeyComboDisplay(capturedKeycode, capturedModmask)" readonly @keydown="handleKeyCombo">
                    <div class="muted" style="font-size:12px;">{{ keyHuman }}</div>
                </div>
            </div>
            <div v-if="editType == 3" class="muted" style="font-size:12px;">{{ keyHint }}</div>

            <div class="button-row">
                <button class="primary" @click="apply">Apply</button>
                <button style="background:#a33; border-color:#a33;" @click="del">Delete</button>
            </div>
        </div>
    </div>
</template>
