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
- The Switch correctly **extracts the APK** — unpacking the game's native libraries and assets from the `.apk` file onto the SD card. This works reliably.
- The Switch loads and parses the ELF binary (`libgame.so`) and attempts to link it against our shim table
- The on-screen progress display shows each launch stage in real time
- **Docked mode is detected** — the footer warns you when launched in docked mode, since games require touch screen input (handheld only)
- The launcher **no longer crashes the Switch** when `svcSetMemoryPermission` fails — instead it shows a clear diagnostic screen explaining the blocker
- Full diagnostic output is written to `sdmc:/BareDroidNX/compat_log.txt`

The game does not yet run — there are outstanding blockers (see [Current Blockers](#current-blockers)) — but the groundwork is there.

---

## Controls

> **Handheld mode only.** Android games use touch screen input. The Switch touchscreen only works in handheld mode — in docked mode there is no touch input, so games will be uncontrollable. BareDroidNX now detects docked mode and shows a warning in the footer.

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

### 1. ~~Code pages not executable~~ — **RESOLVED**

The ELF loader now uses libnx's `jitCreate` / `jitTransitionToExecutable` API instead of `memalign` + `svcSetMemoryPermission`. This creates a dual-view mapping (writable side for loading, executable side for running) that the Switch kernel allows. The old `0xD801` error no longer occurs.

### 2. ~~Many unresolved symbols~~ — **RESOLVED (120+ symbols shimmed)**

All symbols that appeared in the Hill Climb Racing compat log are now shimmed:

- `setjmp` / `longjmp` — forwarded to newlib
- `sem_init`, `sem_wait`, `sem_post`, `sem_destroy` — single-threaded stubs
- `clock_gettime`, `nanosleep`, `gettimeofday`, `gmtime`, `localtime`, `mktime`, `strftime` — newlib passthrough
- All wide char/locale functions (`wcslen`, `wmemcpy`, `iswupper`, `setlocale`, `newlocale`, etc.)
- Bionic fortified wrappers (`__strlen_chk`, `__memcpy_chk`, `__strcat_chk`, etc.)
- Networking: `socket`, `connect`, `recv`, `send`, etc. — all stub returning `ENOTSUP`
- `pthread_mutexattr_init/destroy/settype`, `sched_yield`, `syscall`, `getcwd`
- `android_set_abort_message`, `dl_iterate_phdr`, `sincosf`, `__sF`, `__stack_chk_guard`
- `strtoll_l`, `strtoull_l`, `strtold_l`, `vasprintf`, `stpcpy`, `strerror`, `vsscanf`

### 3. Background threads not supported

`pthread_create` is stubbed to return a fake handle and never actually spawn a thread. Games that rely on a separate render or physics thread will appear frozen or crash.

### 4. Game may crash at runtime

Even with all symbols resolved and code executable, the game could crash during initialisation (NULL deref, bad GLES call, unimplemented JNI method, etc.). The next step is to get a crash address from the compat log and identify which code path is failing.

---

## Performance Expectations (Hill Climb Racing 1.67.0)

We're testing the `.apk` release of **Hill Climb Racing 1.67.0** specifically — the current Play Store release ships as a `.xapk` (a zip-of-zips wrapper some third-party distributors use for split/expansion APKs). BareDroidNX's APK parser only understands plain `.apk` (zip) files right now, so `.xapk` support is out of scope until a later phase. Pinning to 1.67.0 keeps testing on a format we can actually ingest.

There's no measured frame rate yet — the game hasn't booted far enough to render a single frame. Here's the theoretical ceiling based on the hardware alone:

- Hill Climb Racing is a simple 2D vector-style physics game, originally tuned to hit 60 FPS on 2012-era phones with GPUs far weaker than the Switch's (Adreno 200/203, Mali-400MP class hardware).
- The Switch's Tegra X1 (4× Cortex-A57 @ ~1020 MHz in handheld mode, Maxwell-based GPU) has roughly an order of magnitude more compute than HCR's original minimum-spec target. Raw rendering throughput should not be the bottleneck.
- **Theoretical ceiling: a locked 60 FPS** — the same cap the game's own engine uses on Android — assuming it boots and renders at all.
- The real risk to frame rate isn't the silicon, it's BareDroidNX's compat layer: `pthread_create` is currently stubbed (no real background thread), so any physics/render thread split the game relies on will serialize onto one thread; GLES calls are translated through switch-mesa rather than a native Android GPU driver, adding some per-draw-call overhead.
- Docked vs handheld GPU clocks differ a lot on Switch, but it's moot here — the game requires touchscreen input, which only works in handheld mode, so handheld is the only mode worth benchmarking once it runs.

This section gets replaced with real measured numbers once the game boots far enough to render a frame.

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
- [x] Docked mode detection — footer warns when not in handheld mode
- [x] Early abort when code pages are not executable — shows diagnostic screen instead of crashing Switch
- [x] **Fix code-page permissions** — ELF loader now uses libnx `jitCreate` / `jitTransitionToExecutable` for dual-view RW+Rx mapping
- [x] All 120+ unresolved symbols shimmed — `sem_*`, `clock_gettime`, `nanosleep`, `gettimeofday`, wide-char, locale, Bionic fortified wrappers, networking stubs, `android_set_abort_message`, `sincosf`, `setjmp`/`longjmp`, `__sF`, `__stack_chk_guard`
- [x] **Per-constructor logging** — each of the 417 `DT_INIT_ARRAY` constructors is logged (address + index) with an immediate flush before it's called, so the crash site shows in `compat_log.txt` when the Switch dies mid-constructor
- [x] **Load all .so files** — all three libs loaded smallest-first so cross-library symbols are available before any constructors run
- [x] **40+ new shims** — signal handling (`sigaction`, `sigemptyset/fill/add/del`, `pthread_sigmask`), thread naming (`pthread_setname_np`, `prctl`), process info (`gettid`, `getpid`, `getauxval`), memory (`mprotect`, `mmap/munmap`), barriers, `sleep`/`usleep`, `strtod_l`/`strtof_l`, `access`, `lstat`, `chmod`, `ioctl`, `pipe`, `dup/dup2`, `raise`, `kill`, `pthread_kill`, `__register_atfork`, `clock_nanosleep`
- [x] **Heap staging buffer for ELF loading** — segment copy, relocation, and dynamic parsing now happen on a plain heap buffer; only a single bulk `memcpy` touches JIT-writable memory, right before `jitTransitionToExecutable`. This fixed a hard crash on the very first write to JIT memory.
- [x] **Per-relocation-entry logging + bounds checks in `applyRela`** — every RELA/JMPREL entry now logs its index, type, symbol index, and resolved name before being applied; `sym.st_name` is now bounds-checked against `DT_STRSZ` before being dereferenced as a string (the gap-derived `sym_count` heuristic can overrun into `.gnu.version` data sitting between `.dynsym` and `.dynstr`, producing garbage string pointers — this is the leading suspect for the current crash). `R_AARCH64_COPY` sizes are now capped at 64KB to guard against a bogus `st_size`. PT_LOAD segment copies and the `PT_DYNAMIC` pointer are now bounds-checked against the staging buffer too.
- [ ] **Identify relocation crash** — need a new compat_log run; the last logged `RELA[n/3060]`/`JMPREL[n/...]` entry before the crash is the culprit
- [ ] Real touch input delivery via `AInputQueue` / `ALooper`

### Phase 1 — Touch input

- [ ] Map Switch touchscreen events to `AInputQueue` touch events delivered to the game
- [x] Docked-mode detection — footer shows warning when not in handheld mode
- [ ] Block launch in docked mode with a hard block + clear message ("Controls only work in handheld mode — the game requires touch input")

### Phase 2 — Stability

- [ ] Real `pthread` support using libnx `Thread` (for games with background render/physics threads)
- [x] Load all `.so` files in dependency order (smallest-first) — was previously only loading the largest
- [ ] Implement `dl_iterate_phdr` so stack unwinders work
- [ ] Save/load state via proper `internalDataPath` on the SD card
- [ ] Catch and display fatal signal info on crash instead of hard-locking

### Phase 3 — Polish

- [ ] Add NRO icon (author: aaronworld.uk)
- [ ] Version bump system — each build increments `APP_VERSION`
- [ ] Per-APK settings overlay (resolution, framerate cap)
- [ ] APK delete / manage from the UI
- [x] WebP icon decoding (`IMG_INIT_WEBP`) + WebP fallback candidates — many modern app icons ship as WebP, not PNG
- [x] Linear texture scaling (`SDL_HINT_RENDER_SCALE_QUALITY`) for smoother icon downscaling
- [x] Colored monogram placeholder (Android-style initial + hashed color) replaces the flat gray box when no icon is found
- [x] Larger APK list icons (72px → 84px) and result-screen icon (96px → 112px)
- [x] APK file size shown in the list (e.g. "42.1 MB")

### Not Planned (for now)

- Online / multiplayer games (Roblox, Fortnite, etc.)
- Google Play Services (GMS) stub
- ARM32 (`armeabi-v7a`) game support — Switch is 64-bit only

---

## Changelog / Change Checklist

> This section is updated with every significant change. Most recent first.

### [Unreleased / Current Build]

- [x] **Performance Expectations section** — theoretical 60 FPS ceiling for Hill Climb Racing 1.67.0 based on Tegra X1 vs. the game's original minimum-spec hardware, plus the `.apk` vs `.xapk` note explaining why we test 1.67.0 instead of the latest release
- [x] **APK chooser QoL**: WebP icon decoding, linear icon scaling, colored monogram placeholders for missing icons, larger icons, file size shown per APK
- [x] **Heap staging buffer for ELF loading** — fixed a hard crash on the first write to JIT-writable memory by doing all segment copy / relocation / dynamic parsing on a heap buffer, then a single bulk `memcpy` into JIT memory right before `jitTransitionToExecutable`
- [x] **Per-relocation-entry logging + bounds checks** in `applyRela()` — logs every RELA/JMPREL entry's index/type/symbol before applying it, bounds-checks `sym.st_name` against `DT_STRSZ` (guards against the gap-derived `sym_count` overrunning into `.gnu.version` data), and caps `R_AARCH64_COPY` size at 64KB
- [x] **Per-constructor logging** — `elfRunCtors()` logs each constructor address + index and flushes to `compat_log.txt` before calling it; the last logged entry before a Switch crash pinpoints the culprit
- [x] **Load all .so files** — replaced `findMainSo` with `findAllSos`; all three libs are loaded smallest-first so cross-library symbol resolution works before any constructors run
- [x] **40+ new shims** — `sigaction`, `sigemptyset`, `sigfillset`, `sigaddset`, `sigdelset`, `sigismember`, `pthread_sigmask`, `sigprocmask`, `prctl`, `gettid`, `getpid`, `getuid`, `getgid`, `getauxval`, `mprotect`, `mmap`, `munmap`, `pipe`, `dup`, `dup2`, `ioctl`, `access`, `chmod`, `fchmod`, `lstat`, `pthread_setname_np`, `pthread_getname_np`, `pthread_attr_setstack`, `pthread_barrier_*`, `kill`, `raise`, `pthread_kill`, `sleep`, `usleep`, `clock_nanosleep`, `strtod_l`, `strtof_l`, `__register_atfork`
- [x] **ELF loader: replace `memalign` + `svcSetMemoryPermission` with libnx JIT API** — `jitCreate` / `jitTransitionToWritable` / `jitTransitionToExecutable` give a dual-view RW+Rx mapping; the 0xD801 blocker is resolved
- [x] **JIT dual-mapping**: relocations are written to the writable (`rw_addr`) side; GOT entries store exec (`rx_addr`) addresses; symtab/strtab are copied to heap before the JIT transition unmaps the write side
- [x] **120+ new shims**: `setjmp`/`longjmp`, `sem_*`, all time functions, all wide-char and locale functions, all Bionic fortified string wrappers, networking stubs (return `ENOTSUP`), `sched_yield`, `syscall`, `sysconf`, `getcwd`, `dl_iterate_phdr`, `android_set_abort_message`, `sincosf`, `stpcpy`, `vasprintf`, `vsscanf`, `strerror`, `strtold`, `puts`/`putchar`, `rename`/`remove`, `__sF` data stub, `__stack_chk_guard` address, `pthread_mutexattr_*`, `__cxa_finalize`
- [x] Capture `svcSetMemoryPermission` result code from ELF loader and surface it on the diagnostic screen
- [x] Abort launch early when code pages are not executable — shows diagnostic screen instead of hard-crashing the Switch
- [x] Add "Checking code permissions" progress step so the user sees where the launch stopped
- [x] Docked mode detection — footer turns amber and warns that games need handheld (touch screen) mode
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
