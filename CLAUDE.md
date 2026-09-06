# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Kaleidowall: a native Windows video mosaic player (`Kaleidowall.exe`, C++ namespace `kaleido`). C++20 + Qt
6.8 Widgets, one OpenGL 3.3 compositor surface, libmpv for decode, FFprobe for metadata, SQLite for the
library. No browser runtime; direct-file playback plus offline MP4 export. `README.md` documents user-facing behaviour and current scope limits; `PERFORMANCE.md`
records the transition-stall investigation and the measured before/after numbers.

## Commands

All commands run from the repo root in PowerShell.

```powershell
./scripts/setup.ps1                 # one-time: .venv, Qt 6.8.3 via aqtinstall, libmpv, then a test build
./scripts/build.ps1                 # configure + build Release (VS 2022 x64), copy libmpv-2.dll
./scripts/build.ps1 -Test -Deploy   # also run ctest and windeployqt
./Start-Kaleidowall.ps1                   # run build/Release/Kaleidowall.exe with Qt on PATH
```

Tests and validation:

```powershell
$env:PATH = "$PWD\.deps\Qt\6.8.3\msvc2022_64\bin;$env:PATH"  # required by the ctest lines below
ctest --test-dir build -C Release --output-on-failure        # both suites
ctest --test-dir build -C Release -R kaleido_backend         # one suite (kaleido_core | kaleido_backend)
build\Release\kaleido_tests.exe clipNeverCrossesMargins      # one QtTest function
build\Release\kaleido_tests.exe -functions                   # list test functions
python scripts/smoke_test.py                                 # real UI, generated multi-codec fixtures
python scripts/orientation_test.py                           # vertical-flip regression, real player
python scripts/benchmark.py --label local60 --fps 60         # transition/frame-pacing profile
python scripts/export_test.py                              # real export, formats, audio, cancel, orientation
python scripts/export_benchmark.py --seconds 12             # 1080p sources, 2/4/8 tiles, GPU telemetry
```

The Python scripts need `ffmpeg`/`ffprobe` on PATH and a built `build/Release/Kaleidowall.exe`; they
prepend `.deps/Qt/6.8.3/msvc2022_64/bin` to PATH themselves. `ctest` and the test executables do not —
only `build.ps1 -Test` sets PATH before invoking ctest, so a standalone `ctest` run fails every test with
`0xc0000135` (DLL not found) unless that Qt `bin` is on PATH first. Output, fixtures, and JSON/PNG
telemetry land under `test-output/` (gitignored). Formatting is `.clang-format` (LLVM base, 4 spaces,
110 columns).

Dependencies live in `.deps/` — nothing is installed system-wide. Qt is pinned to 6.8.3 and the libmpv
archive URL/SHA-256 to `dependencies.lock.json`; `scripts/dependencies.py` verifies the hash before
extracting. Changing either means editing the lock file and re-running `setup.ps1`.

## Architecture

`main.cpp` → `Window` (Qt Widgets chrome, dock panels, shortcuts) → `Canvas` (the single `QOpenGLWidget`
that owns all playback) → `Decoder`/`MpvApi` (libmpv) and `Library` (SQLite + FFprobe).

- **`src/core.*`** — pure, Qt-Core-only logic: `Settings` (JSON round-trip + `normalize()` clamping),
  `chooseClip`/`eligibilityReason` (skip margins, per-video overrides, percentage mode), `ShuffleBag`,
  `makeLayout`/`pickMode`. Deliberately free of GUI/GL/mpv dependencies so `kaleido_core` links into tests.
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
- **Masks and fit happen in the shared fragment shader** in `compositor.cpp` (rect/circle/hexagon SDFs blended by
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

`tests/core_tests.cpp` (`kaleido_core`) covers clip bounds, shuffle/reservation semantics, layout coverage,
settings clamping, and database persistence via `kaleido_core` + `QTemporaryDir`. `tests/backend_tests.cpp`
(`kaleido_backend`) compiles `mpv_backend.cpp` directly and drives it with fake mpv function pointers —
mute-acknowledgment ordering, stale/failed replies, redundant-write suppression, texture reuse, stop
acknowledgment. New backend behaviour should be reachable this way rather than only through the GUI.

## Shutdown crash: fixed 2026-09-06

`Kaleidowall.exe` used to segfault (`0xC0000005`) on every exit, after the scripted session had already
flushed its screenshots and JSON. All three Python validators failed on the exit code alone. Fixed in
`Decoder::~Decoder`; kept here because the failure mode is easy to reintroduce.

Cause: libmpv calls `CoUninitialize()` one time more than `CoInitialize()` on whichever thread calls
`mpv_terminate_destroy`, whenever that player opened a real audio output (`ao=null` never reproduced it).
Destroying the pool on the GUI thread therefore drained the reference count Qt holds on the main STA — the
apartment was already gone after the second of eight decoders, confirmed with `CoGetApartmentType`, so the
crash was not even shutdown-specific: `Canvas::stop()` did the same damage mid-session. `QGuiApplication`
then faulted inside the `qwindows` plugin while releasing its own COM/WinRT objects, which is why the stack
showed no Kaleidowall frame below `main`.

Fix: `~Decoder` runs `terminate_destroy` on a scratch `std::thread` and joins it, so the stray
`CoUninitialize` lands on a thread whose apartment nobody owns. `render_context_free` still runs on the GUI
thread — it needs the GL context current. `handleIsDestroyedOffTheCallingThread` in `tests/backend_tests.cpp`
guards the invariant.

Do not "simplify" that destructor back to a direct `api.terminate_destroy(handle)` call.

Note for future crash work: `Start-Process -PassThru -Wait` reports `ExitCode 0` for runs that actually
crashed. Use `subprocess.run`, bash `$?`, or `System.Diagnostics.Process` to measure an exit code here.

## Offline export — 2026-09-06

Codex and Claude both work here; see `AGENTS.md`. The user approved `EXPORT_PLAN.md` and requested
implementation. Export creates a fresh randomized session with the currently applied settings. Clip
audio follows Play's changing exclusive source, or an imported MP3/WAV replaces it. Export owns its
shuffle and never writes the library from a worker thread.

- `compositor.*` now owns the shared shader/geometry helpers formerly embedded in Canvas. Play still
  uses libmpv RGBA textures. Export uploads NV12 luma/chroma textures, uses the same compositor, and
  converts the resulting framebuffer to top-down limited-range BT.709 NV12 in a second GPU pass.
- `export_timeline.*` advances at exact output timestamps, sharing core selection/layout helpers and
  compositor easing. Source clips keep their identity/geometry through cuts. Source reads continue to
  the usable end if shuffle defers a cut, then hold the final frame. Audio spans stop at the usable end.
- `exporter.*` runs on a QThread with its own OpenGL context. The QOffscreenSurface is created and
  destroyed on the GUI thread. Decoder processes request D3D11VA and retry in software; NVENC is the
  default encoder, with an explicitly selected x264 fallback. Process arguments are structured QStringLists.
- Raw decoder stdout uses **MediaPipe**, an explicitly bounded Windows pipe. Do not replace it with
  QProcess reads: Qt's Windows pipe reader drains on a background thread even without an event loop,
  which buffered whole clips and caused roughly 18 GiB RSS at eight 1080p sources in the initial prototype.
  Encoder input is bounded; two pixel-buffer objects overlap GPU readback, and preview uses one latest
  downscaled QImage protected by a mutex. Source frame buffers are sized for the selected output, not
  the playback texture limit or preview widget size.
- `export_dialog.*` decodes imported audio to a null sink asynchronously to measure audible length.
  Matching snaps duration; manual duration trims/pads. Video frame count is ceil(duration * fps), so
  matching may add less than one frame of silence. AAC uses 48 kHz stereo at 192 kb/s.
- `window_export.cpp` pauses/locks ordinary playback, shows export frames in the central stack, then
  restores the prior state. Completion controls use QWidgetAction visibility, not child widget hide(),
  because QToolBar can show its widgets again during layout. Closing an active export requests cancel;
  shutdown joins the worker before destroying its offscreen surface.
- Output is staged beside the destination, muxed, checked with FFprobe, and replaced with MoveFileExW
  only after success. Errors/cancel leave existing output intact. Never delete a destination to publish.

The implementation uses pinned FFmpeg **processes**, with bounded CPU-memory NV12 transfers rather
than linked codec libraries or a CUDA zero-copy bridge. Direct3D decode was materially faster to start
than CUDA for the tested short clips; the GPU still handles composition/color conversion/NVENC.
Both FFmpeg executable hashes are in `dependencies.lock.json`; `prepare_ffmpeg.py` prepares `.deps/ffmpeg`,
and `build.ps1 -Deploy` verifies/copies them. No CUDA toolkit or additional system installation is needed.

If the normal executable is open, `./scripts/build.ps1 -Test -Deploy -BuildDirectory build/export` makes
an independent runnable build. Validation scripts honor `KALEIDOWALL_EXE` to test that executable. Keep
the user's existing session intact. CLI export returns 0 on success, 1 on export failure, 2 on conflicting
CLI options or unreadable settings JSON, and 3 on cancellation. `export.json` records source/audio spans, seed, stream metadata,
and pipeline timings; `export_benchmark.py` records process-tree RAM and GPU telemetry separately.

Final validation: all three CTest suites and 20 export integration scenarios passed, plus the original
mixed-codec playback smoke test and orientation check. The 60 fps playback benchmark recorded a 16.71 ms
mean frame interval, 18.08 ms p95, and no intervals above 50 ms after startup. See `PERFORMANCE.md` for
export throughput and the 32-tile capacity check. One playback diagnostic issue surfaced during validation:
the latest successful mute acknowledgment must also refresh the cached mute property, or a delayed
observation can falsely report two unmuted players during a safe handoff. The backend regression test
`acknowledgedMuteRefreshesDelayedObservation` covers this without adding synchronous mpv calls.
