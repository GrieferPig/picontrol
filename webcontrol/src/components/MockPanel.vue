<script setup lang="ts">
import { useStore } from '../composables/useStore';
import { useMock } from '../services/mock';

const { state } = useStore();
const { emitScript, emitMockMappings, startPulse, stopPulse } = useMock();
</script>

<template>
    <div class="panel" v-if="state.env.mode === 'mock'">
        <h2>
            Mock & Test
            <small class="muted">No hardware? Load demo data.</small>
        </h2>
        <div class="button-row" style="margin-bottom:8px;">
            <button class="primary" @click="emitScript">Load Mock Grid</button>
            <button @click="startPulse">Start Value Stream</button>
        </div>
        <div class="button-row">
            <button class="ghost" @click="stopPulse">Stop Stream</button>
            <button class="ghost" @click="emitMockMappings">Mock Mappings</button>
        </div>
        <div class="hint-card" style="margin-top:10px;">
            The mock tool feeds the parser with the same lines firmware emits
            (<span class="muted">ok ports / module / param / map</span>) so you can test the UI without a
            device.
        </div>
    </div>
</template>
