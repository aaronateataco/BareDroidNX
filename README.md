<div align="center">

<img src="https://aaronworld.uk/avatar.png" width="88" height="88" style="border-radius:50%" alt="Aaron's avatar" />

# BareDroidNX

**Run Android games natively on Nintendo Switch Horizon OS — without Android**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![GitHub Stars](https://img.shields.io/github/stars/FascinatingPistachio/BareDroidNX?style=social)](https://github.com/FascinatingPistachio/BareDroidNX/stargazers)
[![Status](https://img.shields.io/badge/status-pre--alpha-red.svg)](#status)
[![Built with Claude AI](https://img.shields.io/badge/built%20with-Claude%20AI-orange.svg)](https://anthropic.com)
[![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-red.svg)](#)

*Made by [aaronworld.uk](https://aaronworld.uk) · [Give it a star ⭐](https://github.com/FascinatingPistachio/BareDroidNX/stargazers) if you find it interesting!*

</div>

---

> **Disclaimer — Please Read**
>
> Approximately **99.8% or more of the code in this project was written by Anthropic's Claude AI** (claude-sonnet-4-6). I (the author) am not a developer and am not capable of writing this myself. Claude and I are working on this together as an experiment. I handle testing on real hardware, describe problems, and decide direction; Claude writes and debugs the code.
>
> This project is in extremely early stages. It may not run anything at all reliably yet. Use entirely at your own risk.

---

## What Is This?

BareDroidNX is a **compatibility / translation layer** that lets Android native (NDK) games run directly on the Nintendo Switch's **Horizon OS** — the Switch's real operating system.

**This is NOT Android running on the Switch.** There is no Android OS, no emulator, no virtual machine. The game's ARM64 machine code runs directly on the Switch's Tegra X1 processor, the same chip Android phones use. A thin shim layer fakes just enough of the Android runtime (libc, OpenGL ES, JNI, asset management) that the game's native `.so` library doesn't notice it isn't on Android.

Think of it like Wine on Linux — not emulation, but translation.

### Current Focus

We are currently only attempting **simple, old, 2D Android games** — specifically games that:

- Ship an `arm64-v8a` native library (`.so` file)
- Use OpenGL ES 2.0 or 3.0 for rendering
- Have **no** Google Play Services dependency
- Save locally (no cloud saves required)
- **Do not require network/online connectivity**

We are **not** planning support for online-only games (such as Roblox, Fortnite, etc.) anytime soon — if ever. The project is focused on proving that even the simplest games can run, which is still a significant technical challenge.

Our test target is **Hill Climb Racing 1.x** by Fingersoft — a simple 2D physics game with no online requirement, widely used in compatibility testing.

### What We've Achieved So Far

- The NRO launches from hbmenu and shows a working APK browser UI
- The Switch correctly **decompiles and extracts the APK** — unpacking the game's native libraries and assets from the `.apk` file onto the SD card. This works!
- The Switch loads and parses the ELF binary (`libgame.so`) and attempts to link it against our shim table
- The on-screen progress display now shows each launch stage in real time
- Full diagnostic output is written to `sdmc:/BareDroidNX/compat_log.txt`

The game does not yet run — there are outstanding blockers (see [Current Blockers](#current-blockers)) — but the groundwork is there.

---

## Controls

> **Handheld mode only.** Android games use touch screen input. The Switch touchscreen only works in handheld mode — in docked mode there is no touch input, so games will be uncontrollable. BareDroidNX will warn you about this in a future update.

**BareDroidNX launcher controls (in the APK browser):**

| Button | Action |
|--------|--------|
| D-pad / Left stick | Navigate APK list |
| **A** | Launch selected APK |
| **Y** | Rescan APK folder |
| **+** | Quit |
| **B** | Back (on result screen) |

---

## Setup

1. Copy `BareDroidNX.nro` to `sdmc:/switch/`
2. Place `.apk` files in `sdmc:/BareDroidNX/apks/`
3. Launch from hbmenu (Atmosphere CFW required)
4. Navigate with D-pad or left stick, press **A** to launch
5. If a launch fails, check `sdmc:/BareDroidNX/compat_log.txt` for the full error log

---

## Building

Requires [devkitPro](https://devkitpro.org/) with `devkitA64` and `libnx` installed.

```sh
export DEVKITPRO=/opt/devkitpro
make
```

Output: `BareDroidNX.nro`

### Dependencies (via pacman/devkitPro)

```
switch-sdl2 switch-sdl2_image switch-sdl2_ttf
switch-libpng switch-libjpeg-turbo switch-minizip
switch-mesa switch-glad
```

---

## Current Blockers

These are the known issues preventing Hill Climb Racing from running:

### 1. Code pages not executable (`svcSetMemoryPermission` → `0xD801`)

When we load the game's `.so` with our custom ELF loader, we `memalign()` the memory and then call `svcSetMemoryPermission` to make the code segment executable (Rx). The kernel returns `0xD801` (module 1 / kernel, description 108 = `InvalidMemoryPermissions`).

**Why:** On Switch, heap-allocated memory cannot be made executable via `svcSetMemoryPermission`. You must use JIT-capable memory APIs (`svcMapCodeMemory`, `svcSetMemoryPermission` on mapped-code regions, or libnx's `jit` helpers). This requires the NRO to have JIT permission in its NACP (or CFW-level permissions). **This is the primary blocker.**

### 2. Many unresolved symbols

Hill Climb Racing's `libgame.so` requires approximately 120+ libc/POSIX symbols that we haven't shimmed yet, including:

- `longjmp` / `setjmp` (C exception handling)
- `sem_init`, `sem_wait`, `sem_post` (POSIX semaphores)
- `clock_gettime`, `nanosleep`, `gettimeofday` (time)
- Wide character functions: `wcslen`, `wcscpy`, `wmemcpy`, `iswupper`, etc.
- Locale functions: `setlocale`, `newlocale`, `freelocale`, `uselocale`
- Networking: `socket`, `connect`, `send`, `recv`, `bind`, `listen`, `select`
- DNS: `gethostbyname`
- Bionic-specific fortified wrappers: `__strlen_chk`, `__memcpy_chk`, `__strcat_chk`, etc.
- `strftime`, `gmtime`, `localtime`, `mktime`
- `pthread_mutexattr_init/destroy/settype`
- `syscall`, `sched_yield`, `getcwd`
- `android_set_abort_message`, `dl_iterate_phdr`

These need to be added to `shim_table.cpp` one by one. Many will need real implementations (e.g. time functions), not just stubs.

### 3. Background threads not supported

`pthread_create` is stubbed to return a fake handle and never actually spawn a thread. Games that rely on a separate render or physics thread will appear frozen or crash.

---

## TODO / Roadmap

> Items are roughly ordered by priority. "Phase 0" is the current work.

### Phase 0 — Make any game do *something* (in progress)

- [x] APK browser UI with icon extraction
- [x] APK extraction (libs + assets) onto SD card
- [x] Custom ARM64 ELF loader with RELA relocation
- [x] JNI environment shim (fake JavaVM / JNIEnv)
- [x] EGL setup (GLES 2 + 3 shim table passthrough)
- [x] On-screen progress display during launch stages
- [x] Full diagnostic result screen with error details
- [ ] **Fix code-page permissions** — use JIT-capable memory allocation (libnx `jit` API or `svcMapCodeMemory`)
- [ ] Add missing libc shims (see [Current Blockers](#current-blockers) above)
- [ ] Stub `sem_init`/`sem_wait`/`sem_post`/`sem_destroy`
- [ ] Stub `clock_gettime`, `nanosleep`, `gettimeofday`
- [ ] Stub wide-char and locale functions
- [ ] Stub Bionic fortified string wrappers (`__strlen_chk`, etc.)
- [ ] Add `android_set_abort_message` shim (logs message then calls `abort`)
- [ ] Stub networking (return errno `ENETDOWN` / `ECONNREFUSED` — we won't implement real networking for now)

### Phase 1 — Touch input

- [ ] Map Switch touchscreen events to `AInputQueue` touch events delivered to the game
- [ ] Add handheld-mode detection — show warning banner if launched while docked
- [ ] Block launch in docked mode with a clear message ("Controls only work in handheld mode — the game requires touch input")

### Phase 2 — Stability

- [ ] Real `pthread` support using libnx `Thread` (for games with background render/physics threads)
- [ ] Handle multiple `.so` files — load all libs in dependency order, not just the largest
- [ ] Implement `dl_iterate_phdr` so stack unwinders work
- [ ] Save/load state via proper `internalDataPath` on the SD card
- [ ] Catch and display fatal signal info on crash instead of hard-locking

### Phase 3 — Polish

- [ ] Add NRO icon (author: aaronworld.uk)
- [ ] Version bump system — each build increments `APP_VERSION`
- [ ] Per-APK settings overlay (resolution, framerate cap)
- [ ] APK delete / manage from the UI

### Not Planned (for now)

- Online / multiplayer games (Roblox, Fortnite, etc.)
- Google Play Services (GMS) stub
- ARM32 (`armeabi-v7a`) game support — Switch is 64-bit only

---

## Changelog / Change Checklist

> This section is updated with every significant change. Most recent first.

### [Unreleased / Current Build]

- [x] Add `LaunchResult` struct — `launchApk()` now returns structured error info instead of a bare bool
- [x] Add `ProgressCb` callback — UI shows each launch stage on-screen in real time
- [x] Replace "(loader not yet implemented)" stub screen with a proper diagnostic result screen showing failure stage, unresolved symbol count, and svc error codes
- [x] Track and display unresolved ELF symbol count after each load
- [x] Change NRO author to `aaronworld.uk`
- [x] Improve README with badges, profile link, AI disclaimer, in-depth TODO

### 0.1.0 — Initial release

- [x] APK browser UI (SDL2, 1280×720)
- [x] APK icon + metadata extraction (AndroidManifest.xml + resources.arsc parser)
- [x] Full APK extraction to SD card (libs + assets)
- [x] Custom ARM64 ELF loader with RELA relocation processing
- [x] JNI / JavaVM fake environment
- [x] GLES 2 + GLES 3 shim table (400+ functions)
- [x] EGL setup via switch-mesa
- [x] SD card logging (`log.txt` + `compat_log.txt`)

---

## License

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

This project is licensed under the **MIT License** — see [LICENSE](LICENSE) for the full text.

**In plain English:** You can use, copy, modify, share, and even sell this code freely, as long as you keep the copyright notice. There is no warranty — if it breaks your Switch, that's on you (please use CFW responsibly). You do not need to open-source any modifications you make, but you must include the original copyright line.

The MIT license does **not** cover the Android games themselves — those belong to their respective developers. BareDroidNX only provides the compatibility layer.

---

## About

Made by [Aaron](https://aaronworld.uk) — a non-developer who wanted to see Android games on their modded Switch, and is figuring it out one step at a time with the help of Claude AI.

> **~99.8% of all code in this repo was written by [Claude](https://anthropic.com) (claude-sonnet-4-6 model).** I describe the problem, test on real hardware, and point out what's broken. Claude writes the fixes. This is an honest experiment in AI-assisted hardware hacking.

If this interests you, star the repo and check back. Progress will be slow but real.
