<script setup lang="ts">
import { useStore } from '../composables/useStore';
import { useSerial } from '../services/serial';
import { useRouter } from '../services/router';
import { useLogger } from '../composables/useLogger';

const { state, reset } = useStore();
const { connect, disconnect, isConnected } = useSerial();
const { listModules, listMappings } = useRouter();
const { add: logAdd } = useLogger();

async function toggleConnect() {
    if (isConnected()) {
        await disconnect();
    } else {
        await connect();
    }
}

async function refresh() {
    await listModules();
    await listMappings();
}

function clear() {
    reset();
}
</script>

<template>
    <div class="toolbar">
        <button id="btnConnect" class="primary" @click="toggleConnect">
            {{ state.connection.connected ? 'Disconnect' : 'Connect Device' }}
        </button>
        <button id="btnRefresh" @click="refresh">Refresh</button>
        <button id="btnClear" class="ghost" @click="clear">Clear UI</button>
        <div class="status" :class="{ connected: state.connection.connected }">
            {{ state.connection.connected ? 'Connected' : 'Disconnected' }}
        </div>
    </div>
</template>
