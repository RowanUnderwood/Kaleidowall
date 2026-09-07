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

## Application icon

`assets/kaleidowall.svg` is the editable geometric artwork. Run `python scripts/generate_icon.py`
(Pillow required) after artwork changes and keep the generated PNG and multi-resolution ICO in source
control. Ordinary builds need no image tooling: CMake embeds the ICO through `assets/kaleidowall.rc`
for Windows Explorer/shortcuts and a Qt resource for `QApplication::setWindowIcon` (window/taskbar).
The ICO contains 16, 20, 24, 32, 40, 48, 64, 96, 128 and 256 px variants with transparency.

## Architecture

### Shared player and Windows screensaver

`kaleido_app` compiles Canvas, the mpv backend, Window and its settings/library/preset/export panels once.
Both `Kaleidowall.exe` and `Kaleidowall.scr` link it. The screensaver branch is `codex/screensaver`; keep
features in shared libraries, never copied into the screensaver entry point. `screensaver_main.cpp`
sets the same application/organization names and data path as the player. `screensaver.*` implements
the case-insensitive `/s`, `/c[:HWND]`, `/p HWND` protocol, native preview parenting/lifetime, config owner
modality, input dismissal, per-data-directory session lock and monitor handling.

`Window(data, true)` presents the same settings/library widgets as a configuration-only central panel.
Its hidden Canvas uses no shuffle key, and must be explicitly deleted before the Library. Normal
screensaver Canvas uses `screensaverShuffle`, startup default settings, and no settings writes; preview
uses no shuffle key and forces mute. `screensaverPreferences.muted` is separate from shared Settings.
`Library::startupSettings` implements default-preset / last-used fallback. Empty libraries stay quiet.

Mirrors are QOpenGLWidgets sharing decoder textures via `AA_ShareOpenGLContexts`; only the primary
Canvas schedules/decodes/audio-routes. Both use `Canvas::drawTiles` and the same Compositor. Fit each
mirror to the primary aspect ratio. GL fences order primary texture writes and mirror reads; flush
producer contexts, wait on the GPU, and destroy mirrors before Canvas. No CPU frame readbacks on the
normal mirror path. Screensaver decoders set `stop-screensaver=no` so they do not inhibit screen sleep.

`build.ps1 -Test -Deploy` emits both launchers with a shared deployed runtime. Install/remove scripts
operate on the separate per-user Screensaver directory, preserving the shared application database.
Register `SCRNSAVE.EXE` as the exact unquoted absolute path, activate through SystemParametersInfo,
and open Control Panel separately. Do not pass a quoted path to `desk.cpl,InstallScreenSaver`: it can
persist quotes plus trailing whitespace, making Settings, Preview and idle launch silently fail.
`kaleido_screensaver` tests arguments, persistence isolation, config lifetime and real Win32 child
preview. `scripts/screensaver_test.py` covers real decoding, mirrored screenshots and input dismissal.

### Player components

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
  Folder membership is prefix-only (`covered()`), so a video belongs to every root above it. `folders.enabled`
  drives the library panel's tick boxes: `videos()` derives `Video::folderEnabled` (true while *any*
  covering folder is ticked, and for a video no root covers at all), and `eligibilityReason` turns that
  into "Folder disabled" so playback, export, and the video table all honour it from one place. Two traps:
  `scan()` walks only ticked roots, so its `missing` pass must be restricted to the roots it actually
  walked or every parked video is flagged missing; and the schema has no migration framework beyond
  `CREATE TABLE IF NOT EXISTS`, so `folders.enabled` is added by a `PRAGMA table_info` probe plus
  `ALTER TABLE`. `videos()` ignores its query result, so a missed migration reads as an empty library
  rather than an error — add columns with that probe, and name columns in every `INSERT`.
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
  Preset identity uses combo item data, not its decorated display text. `presetState` in `kv` stores
  `loadedPreset` separately from `defaultPreset`; selection alone never loads a preset. Startup applies
  the saved default, otherwise retains last-used settings and their loaded-preset association.
  Save/Save as apply visible edits after overwrite confirmation. `Library::removePreset` transactionally
  refuses the last deletion and clears matching metadata; Window loads the next/previous preset only
  when deleting the loaded one. Starter presets are seeded only during window initialization when empty.
  `kaleido_presets` tests actual buttons/dialogs, deletion fallback, last-preset protection, and restart behavior.

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
- **Honeycomb** uses equal flat-top hexagons on a staggered lattice, fit and centered as a whole by
  `makeLayout`. Its bounding rectangles intentionally overlap; the hexagon interiors do not. Mask 3
  fills these bounds without the ordinary two-pixel tile inset, using hard shared edges to prevent
  alpha-blended background seams. Playback and export fall back to Hexagons below three slots.
  `Canvas::resizeGL` refits Honeycomb targets so hexagons remain regular after resizing.
- **Inset** resolves once per layout change through `resolveLayoutMode` to Inset Circles/Hexagons/
  Honeycomb (mask codes 4/5/6). Selection is uniform among eligible shapes, independent of weights;
  Honeycomb needs at least four total slots. Slot zero is the full-screen background and counts toward
  the pool limit. `DrawTile::firstSlot` identifies it even when earlier textures are not ready; never
  infer its identity from the submitted draw-list index. Render it first, rectangular, without inset,
  crop-to-fill. Foreground Fit bars become transparent. The compositor blends these properties during
  transitions using the old/new mask codes. Both layers use ordinary clip/shuffle/audio scheduling.

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
- **Never hardcode a GPU index.** FFmpeg addresses adapters through three orderings that disagree on a
  multi-GPU machine: DXGI adapter order (`-hwaccel_device`), the NVENC/CUDA ordinal (`-gpu`), and NVML/PCI
  order (`nvidia-smi -i`). All three were once `0`, which put decode and composition on the RTX 5090 while
  NVENC silently ran on the 4090 and the benchmark sampled the wrong card. `gpu.*` enumerates DXGI natively
  and NVENC by parsing an out-of-range `-gpu` probe, then matches both against the compositing GPU's
  `GL_RENDERER`; `startProcess` pins `CUDA_DEVICE_ORDER=PCI_BUS_ID` on every FFmpeg child so an ordinal
  means the same card everywhere. `export.json` records `gpuDecode`/`gpuEncode` and `export_test.py`
  asserts both equal the compositor.
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
