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
                <span>{{ p.name || 'Param ' + p.id }}
                    <input v-if="p.dt === 2" type="checkbox" class="param-value-input"
                        :checked="p.value == 1 || p.value === 'true' || p.value === '1'"
                        :disabled="(p.access & 2) === 0"
                        @click.stop
                        @change="(e) => updateParam(p, (e.target as HTMLInputElement).checked ? '1' : '0')"
                        style="width: auto; margin-left: 6px;"
                    >
                    <input v-else type="number" class="param-value-input"
                        :value="p.value"
                        :disabled="(p.access & 2) === 0"
                        :min="p.min" :max="p.max" :step="p.dt === 1 ? '0.01' : '1'"
                        @click.stop
                        @change="(e) => updateParam(p, (e.target as HTMLInputElement).value)"
                        style="width: 60px; padding: 2px 4px; font-size: 11px; background: #0c0f18; border: 1px solid #3a3f4b; border-radius: 4px; color: #d1d5db; margin-left: 6px;"
                    >
                </span>
                <span class="val">{{ getMappingInfo(p.id) }}</span>
            </div>
        </div>
    </div>
</template>
