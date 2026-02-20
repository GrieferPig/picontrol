import { reactive } from 'vue';

const logs = reactive<string[]>([]);

export function useLogger() {
    function add(msg: string) {
        const stamp = new Date().toLocaleTimeString();
        const text = `[${stamp}] ${msg}`;
        logs.push(text);
        if (logs.length > 200) logs.shift();
    }

    return {
        logs,
        add,
    };
}
