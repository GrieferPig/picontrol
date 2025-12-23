import { reactive } from 'vue';
import type { State } from '../types';

const state = reactive<State>({
    ports: { rows: 0, cols: 0, items: {} },
    modules: {},
    mappings: [],
    selected: null,
    connection: { connected: false },
    env: { mode: 'real' },
    moduleUi: {
        overrides: {},
    },
});

export function useStore() {
    function reset() {
        state.ports = { rows: 0, cols: 0, items: {} };
        state.modules = {};
        state.mappings = [];
        state.selected = null;
    }

    return {
        state,
        reset,
    };
}
