# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Prism Player (`PrismPlayer`): a native Windows video mosaic player. C++20 + Qt 6.8 Widgets, one OpenGL 3.3
compositor surface, libmpv for decode, FFprobe for metadata, SQLite for the library. No browser runtime, no
transcoding step. `README.md` documents user-facing behaviour and current scope limits; `PERFORMANCE.md`
records the transition-stall investigation and the measured before/after numbers.

## Commands

All commands run from the repo root in PowerShell.

```powershell
./scripts/setup.ps1                 # one-time: .venv, Qt 6.8.3 via aqtinstall, libmpv, then a test build
./scripts/build.ps1                 # configure + build Release (VS 2022 x64), copy libmpv-2.dll
./scripts/build.ps1 -Test -Deploy   # also run ctest and windeployqt
./Start-Prism.ps1                   # run build/Release/PrismPlayer.exe with Qt on PATH
```

Tests and validation:

```powershell
ctest --test-dir build -C Release --output-on-failure       # both suites
ctest --test-dir build -C Release -R prism_backend          # one suite (prism_core | prism_backend)
build\Release\prism_tests.exe clipNeverCrossesMargins       # one QtTest function
build\Release\prism_tests.exe -functions                    # list test functions
python scripts/smoke_test.py                               # real UI, generated multi-codec fixtures
python scripts/orientation_test.py                         # vertical-flip regression through the real player
python scripts/benchmark.py --label local60 --fps 60        # transition/frame-pacing profile
```

The Python scripts need `ffmpeg`/`ffprobe` on PATH and a built `build/Release/PrismPlayer.exe`; they prepend
`.deps/Qt/6.8.3/msvc2022_64/bin` to PATH themselves. `prism_tests.exe` run directly needs that Qt `bin` on
PATH (ctest and `build.ps1` handle it). Output, fixtures, and JSON/PNG telemetry land under `test-output/`
(gitignored). Formatting is `.clang-format` (LLVM base, 4 spaces, 110 columns).

Dependencies live in `.deps/` — nothing is installed system-wide. Qt is pinned to 6.8.3 and the libmpv
archive URL/SHA-256 to `dependencies.lock.json`; `scripts/dependencies.py` verifies the hash before
extracting. Changing either means editing the lock file and re-running `setup.ps1`.

## Architecture

`main.cpp` → `Window` (Qt Widgets chrome, dock panels, shortcuts) → `Canvas` (the single `QOpenGLWidget`
that owns all playback) → `Decoder`/`MpvApi` (libmpv) and `Library` (SQLite + FFprobe).

- **`src/core.*`** — pure, Qt-Core-only logic: `Settings` (JSON round-trip + `normalize()` clamping),
  `chooseClip`/`eligibilityReason` (skip margins, per-video overrides, percentage mode), `ShuffleBag`,
  `makeLayout`/`pickMode`. Deliberately free of GUI/GL/mpv dependencies so `prism_core` links into tests.
- **`src/library.*`** — one SQLite file (`folders`, `videos`, `kv`) in WAL mode. `kv` stores settings,
  shuffle state, and `preset:<name>` rows. Scanning runs on a `QThread::create` worker that shells out to
  FFprobe per file and marshals each result back with `QMetaObject::invokeMethod(..., QueuedConnection)`;
  incremental rescans skip files whose (size, mtime) match. Video ids are lowercased canonical paths, which
  is how overlapping folders deduplicate. The recognised extension list is in `Library::scan`.
- **`src/mpv_backend.*`** — libmpv is loaded dynamically through `QLibrary`; every entry point is a member
  function pointer in `MpvApi` (the `API(name)` macro). Tests exploit this by assigning fake function
  pointers, so `Decoder` logic is unit-testable with no real mpv. `Decoder` keeps an event-driven property
  cache, sends writes via `set_property_async`, suppresses unchanged writes, and tracks `muteConfirmed` /
  `stopPending` acknowledgments. `Decoder::render` passes `FLIP_Y = 0` on purpose — it renders to an
  offscreen FBO, and the default flip would invert it (`orientation_test.py` guards this).
- **`src/canvas.*`** — the scheduler and compositor, and where nearly all subtlety lives (see below).
- **`src/window.*`** — toolbars, the right-hand dock with three stacked panels (0 settings, 1 library,
  2 performance — `showPanel(int)` uses those indices), presets, per-video editing, fullscreen
  auto-hiding controls.

### Canvas invariants

- **Everything is UI-thread and non-blocking.** The performance work in `PERFORMANCE.md` was about the
  control path, not paint throughput. Never introduce a synchronous mpv query, a player construction, or a
  destruction on the tick/paint path. Use `command_async`/`set_property_async` plus the cached properties.
- **Two pools.** `players` are on-screen segments; `spares` are warm, hidden decoders. `warmPool(cap * 2)`
  (one visible + one preparing decoder per maximum segment) is called from `nextLayout` only. `recycle()`
  returns a slot to reuse — async stop, context and FBO retained. `Stop` releases the pool.
- **Clips cut, they don't reload.** `prepareClips()` starts the next clip on a hidden spare; once it has a
  decoded frame, `cutPreparedClips()` (called from `paintGL`) swaps decoder/FBO/video/clip between the
  visible slot and the spare. Segment identity, geometry, and the layout animation are unaffected by a cut.
  The outgoing clip keeps playing until the swap, so a cut never shows a frozen frame.
- **Shuffle reservations are two-phase.** `reserve()` → `commit()` only once the frame is actually visible,
  `cancel()` otherwise, and persisted state stores reservations as *remaining*. A crash or a cancelled load
  therefore never silently consumes a clip. Persist through `persistShuffle()` after any commit.
- **Audio is exclusive.** `routeAudio()` mutes every other player and waits for all outgoing
  `muteConfirmed` acknowledgments before unmuting the chosen source. The smoke test asserts at most one
  unmuted player across `slots` + `idleSlots` in every sample.
- **Frame scheduling.** A single-shot `QChronoTimer` re-armed by `scheduleTick()` against monotonic
  deadlines (`nextTickAt`), so fractional intervals don't round-drift. Textures update only when
  `render_context_update` reports a new frame or the FBO was reallocated; layout animation keeps using
  cached textures.
- **Masks and fit happen in the fragment shader** in `initializeGL` (rect/circle/hexagon SDFs blended by
  `maskProgress`, crop-vs-fit scaling, background colour outside the video rect). Layout geometry comes
  from `makeLayout`; the shader never picks positions.

### Diagnostics and headless modes

`Canvas::diagnostics()` returns the JSON the performance panel and all three Python scripts consume — the
smoke and benchmark assertions read its field names directly, so renaming a field breaks those tests.
`main.cpp` supports `--data-dir PATH` (isolated database), `--library PATH` (index a folder at startup),
`--smoke SECONDS`, and `--benchmark SECONDS --benchmark-fps N`; the latter two drive a scripted session,
save screenshots plus `smoke.json`/`benchmark.json`, and quit. `Canvas::beginProfile()` seeds the RNG (42)
so benchmark runs are comparable, and `recordTiming` stages (`tick`, `layout`, `audio`, `preload`,
`frameGap`, `cut`, `cutFirstUpdate`) are what `benchmark.py` summarises.

### Test layout

`tests/core_tests.cpp` (`prism_core`) covers clip bounds, shuffle/reservation semantics, layout coverage,
settings clamping, and database persistence via `prism_core` + `QTemporaryDir`. `tests/backend_tests.cpp`
(`prism_backend`) compiles `mpv_backend.cpp` directly and drives it with fake mpv function pointers —
mute-acknowledgment ordering, stale/failed replies, redundant-write suppression, texture reuse, stop
acknowledgment. New backend behaviour should be reachable this way rather than only through the GUI.
