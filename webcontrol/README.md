# webcontrol (Tauri)

Desktop control UI for PiControl using Vue 3 + Tauri.

## Architecture

- Frontend: existing Vue app in `src/`
- Backend: Rust in `src-tauri/`
- Serial transport: native Rust `serialport` crate (no Web Serial)

## Prerequisites

- Node.js 20+
- Rust toolchain (`rustup`)
- Tauri system dependencies for your OS (see Tauri docs)

## Install

```sh
npm install
```

## Run desktop app (recommended)

```sh
npm run tauri:dev
```

## Build desktop app

```sh
npm run tauri:build
```

## Web-only dev mode (no native serial)

```sh
npm run dev:web
```

In web-only mode, serial commands are unavailable because communication now uses Tauri native APIs.
