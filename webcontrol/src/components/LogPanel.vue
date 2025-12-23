<script setup lang="ts">
import { useLogger } from '../composables/useLogger';
import { watch, ref, nextTick } from 'vue';

const { logs } = useLogger();
const logContainer = ref<HTMLElement | null>(null);

watch(logs, () => {
    nextTick(() => {
        if (logContainer.value) {
            logContainer.value.scrollTop = logContainer.value.scrollHeight;
        }
    });
}, { deep: true });
</script>

<template>
    <div class="panel">
        <h2>Log</h2>
        <div class="log" ref="logContainer">
            <div v-for="(log, index) in logs" :key="index">{{ log }}</div>
        </div>
    </div>
</template>
