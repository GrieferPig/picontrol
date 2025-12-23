export interface PortItem {
    r: number;
    c: number;
    configured: boolean;
    hasModule: boolean;
    orientation: number;
}

export interface ModuleParam {
    id: number;
    dt: number;
    name: string;
    min?: string | number;
    max?: string | number;
    value?: string | number;
    pendingUpdate?: number;
}

export interface Module {
    r: number;
    c: number;
    type: number;
    caps: number;
    name: string;
    mfg: string;
    fw: string;
    paramCount: number;
    params: ModuleParam[];
    sizeR: number;
    sizeC: number;
    portLocR: number;
    portLocC: number;
}

export interface Mapping {
    r: number;
    c: number;
    pid: number;
    type: number;
    d1: number;
    d2: number;
}

export interface ModuleUiOverride {
    sizeR?: number;
    sizeC?: number;
    rotate180?: boolean;
}

export interface State {
    ports: {
        rows: number;
        cols: number;
        items: Record<string, PortItem>;
    };
    modules: Record<string, Module>;
    mappings: Mapping[];
    selected: { r: number; c: number; pid?: number | null } | null;
    connection: { connected: boolean };
    env: { mode: 'real' | 'mock' };
    moduleUi: {
        overrides: Record<string, ModuleUiOverride>;
    };
}
