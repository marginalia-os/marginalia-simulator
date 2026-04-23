# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

A macOS desktop simulator for CrossPoint firmware (an ESP32-C3 e-reader). PlatformIO compiles the firmware as a native binary; SDL2 renders the e-ink display in a window. The simulator lives in this repo as a library that is added to the firmware's `platformio.ini`.

## Build & Run

```bash
# Build
pio run -e simulator

# Run compiled binary
.pio/build/simulator/program

# Build and run (custom PlatformIO target)
pio run -e simulator -t run_simulator
```

The PlatformIO environment config lives in `sample-platformio.ini` — copy the `[env:simulator]` section into the firmware's `platformio.ini`.

## Architecture

All simulator mock sources are in `src/`. The entry point is `src/simulator_main.cpp`, which calls `setup()` / `loop()` from the firmware and drives the display.

**Display pipeline** — `HalDisplay` manages an SDL2 window at half-scale (240×400 logical for the physical 800×480 framebuffer). `refreshDisplay()` converts the 1-bit framebuffer to RGBA32 pixels and sets an atomic `pendingPresent` flag. The main thread calls `presentIfNeeded()` each loop iteration (SDL must run on the main thread on macOS). SDL rotation undoes the firmware's coordinate transform:

| Orientation | SDL angle |
|---|---|
| Portrait | +90° |
| PortraitInverted | −90° |
| Landscape | 0° |

**Input** — `HalGPIO` polls SDL events and maps keyboard keys to device buttons:

| Key | Button |
|---|---|
| ↑ / ↓ | Page back / forward (side buttons) |
| ← / → | Left / right front buttons |
| Return | Confirm |
| Escape | Back |
| P | Power |

**Storage** — `HalStorage` wraps POSIX file descriptors (`::open`, `::read`, `::write`, `lseek`). Do **not** rewrite this layer using `std::fstream` — it has EOF/seek state bugs that are fully documented in `.claude/CONTEXT-sim-notes.md`. The virtual filesystem root is `./fs_/` relative to the binary's working directory. Place books at `./fs_/books/`.

**FreeRTOS mocks** (`src/freertos/`) — `xTaskCreate` launches a `std::thread`; `ulTaskNotifyTake` / `xTaskNotify` use a `std::condition_variable`. `SemaphoreHandle_t` wraps `std::recursive_mutex`.

## Key Implementation Notes

- `FsApiConstants.h` passes native POSIX flag values — do **not** add SdFat→POSIX flag translation in `HalFile::Impl::open()`.
- `BookMetadataCache::lutOffset` must be `uint32_t`, not `size_t` — the 8-byte/4-byte mismatch only manifests on the 64-bit macOS simulator.
- Graceful exit: the main loop checks `display.shouldQuit()` (atomic bool set by SDL quit event) rather than calling `exit()`.
- LOG output goes to `stderr` via `std::cerr` in `HardwareSerial.h`.

## Stale Cache

After any storage-layer changes, delete `./fs_/.crosspoint/` to clear section caches built with old code before re-testing ebook rendering.

## Detailed Development History

`.claude/CONTEXT-sim-notes.md` contains the full record of every build error fixed, every runtime bug fixed, and the rationale for each architectural decision. Read it before making changes to `HalDisplay`, `HalStorage`, or the FreeRTOS mocks.
