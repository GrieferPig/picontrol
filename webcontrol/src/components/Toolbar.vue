<script setup lang="ts">
import { useStore } from '../composables/useStore';
import { useSerial } from '../services/serial';
import { useRouter } from '../services/router';
import { useLogger } from '../composables/useLogger';

const { state, reset } = useStore();
const { connect, disconnect, isConnected } = useSerial();
const { send } = useRouter();
const { add: logAdd } = useLogger();

async function toggleConnect() {
    if (state.env.mode === 'mock') {
        logAdd('Mock mode: Connect is disabled');
        return;
    }
    if (isConnected()) {
        await disconnect();
    } else {
        await connect();
    }
}

async function refresh() {
    await send('modules list');
    await send('map list');
}

function clear() {
    reset();
}

async function toggleMock(e: Event) {
    const checked = (e.target as HTMLInputElement).checked;
    if (checked) {
        if (isConnected()) {
            await disconnect();
        }
        state.env.mode = 'mock';
        state.connection.connected = false;
        logAdd('Mock mode enabled');
    } else {
        state.env.mode = 'real';
        logAdd('Mock mode disabled (real mode)');
    }
}
</script>

<template>
    <div class="toolbar">
        <button id="btnConnect" class="primary" @click="toggleConnect">
            {{ state.connection.connected ? 'Disconnect' : 'Connect Device' }}
        </button>
        <button id="btnRefresh" @click="refresh">Refresh</button>
        <button id="btnClear" class="ghost" @click="clear">Clear UI</button>
        <label class="toggle" title="When enabled, commands are handled locally (no WebSerial)">
            <input id="toggleMock" type="checkbox" :checked="state.env.mode === 'mock'" @change="toggleMock">
            Mock Mode
        </label>
        <div class="status" :class="{ connected: state.connection.connected || state.env.mode === 'mock' }">
            {{ state.env.mode === 'mock' ? 'Mock Mode' : (state.connection.connected ? 'Connected' : 'Disconnected') }}
        </div>
    </div>
</template>
