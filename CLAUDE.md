# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Embedded firmware for a music knob device, targeting the ESP32-C6-Mini (WeAct Studio). Built on Zephyr RTOS v4.3.0 using C.

## Build Commands

All commands use `just` (task runner). West (Zephyr's build tool) runs inside a Python venv managed by the justfile.

```bash
just init              # One-time setup: creates venv, installs west, fetches deps
just build             # Build firmware (Debug by default)
just flash             # Flash to device
just debug             # Start OpenOCD debug session
just rtt               # Segger RTT terminal for logging
just menuconfig        # Interactive Kconfig configuration
just clean             # Remove build artifacts
just ram-report        # Memory usage report
```

Set `BUILD_TYPE=Release` for release builds. Configuration is via env vars or a `.env` file (`APP`, `BOARD`, `BUILD_TYPE`, `SYSBUILD`).

## Project Structure

- `project/app/` — Application source code (`src/main.c`)
- `project/app/prj.conf` — Default Kconfig settings
- `project/app/debug.conf` — Debug overlay (logging, shell, stack protection)
- `project/boards/esp32c6_supermini/` — Custom board definition (device tree, pinctrl, Kconfig)
- `project/west.yml` — West manifest (Zephyr version, module imports)
- `project/zephyr/module.yml` — Registers `project/` as a Zephyr module
- `deps/` — Fetched dependencies (Zephyr, modules, tools) — not checked in

## Code Style

- C with Zephyr conventions, formatted via `.clang-format` (LLVM-based)
- 8-space tabs, 100-column limit, Linux brace style
- Zephyr logging API: `LOG_MODULE_REGISTER(name)` + `LOG_INF()`, `LOG_ERR()`, etc.

## Architecture Notes

- The `project/` directory is a Zephyr module, not just an app directory. It provides both the application (`app/`) and custom board definitions (`boards/`).
- Hardware configuration uses Zephyr's Device Tree system (`.dts`/`.dtsi` files in the board directory).
- Build configuration uses Kconfig (`.conf` files, `Kconfig` files). Use `just menuconfig` to explore options.
- Version is auto-generated from git tags via `git describe`.
