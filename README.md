# Kaleidowall

A native Windows video mosaic player built with C++20, Qt 6 Widgets, an OpenGL 3.3 compositor, libmpv, FFprobe, and SQLite. The first version focuses on 2D playback. There is no browser runtime or video transcoding step.

## Run on this machine

Double-click **`build/Release/Kaleidowall.exe`**, or run `./Start-Kaleidowall.ps1` in PowerShell.

1. Open **Library → Add folder**. Scanning is recursive and runs in the background.
2. Choose a preset or adjust settings, then select **Apply settings**.
3. Press **Play**. Use **F11** or double-click the player for fullscreen.

The source files stay where they are. Removing a library folder only removes its index entries. The app stores its database, presets, settings, and shuffle position in `%LOCALAPPDATA%/Kaleidowall/Kaleidowall`. A `--data-dir PATH` argument uses a separate database for testing.

## Included

- One to 32 concurrent playback slots, capped by eligible library size unless duplicates are enabled.
- Independent random clip durations and layout-change intervals. Existing slots keep playing while layouts animate; surplus slots retire after the transition.
- Single video, strips, grids, a half-screen hero with smaller tiles, randomized masonry, circular masks, and hexagonal masks.
- Weighted layout selection; a weight of zero disables a mode.
- Crop-to-fill or aspect-preserving fit, reduced motion, and saved presets.
- A background color picker under **Settings → Layout & Motion**. Choose a color and select **Apply settings**; it fills gaps around shaped tiles, aspect-fit borders, and the idle canvas. The color is saved with settings and presets.
- Global skip-beginning/end values in seconds or percentages, plus per-video overrides in seconds. The entire clip must fit in the usable range.
- Persistent global shuffle with reservations. Canceled selections are not consumed. Active videos are avoided across shuffle cycles. If all remaining candidates are active, a slot holds its last frame until a candidate becomes available.
- Muted playback or one random audio source, volume, play/pause, stop, next clips, next layout, and audio-source selection.
- Fullscreen controls that appear on mouse movement and hide after three seconds. Settings/library remain available in fullscreen.
- Folder indexing, incremental rescans, overlapping-folder path deduplication, search, per-video enable/exclude controls, and explanations for ineligible files.
- A performance panel with the actual OpenGL GPU, compositor frame rate, CPU paint time, frame-interval p95/maximum, scheduler stalls, active/idle players, new texture updates, hardware-decoding method, decoder drop counts, and memory ceilings.

**Pause** freezes both the clip and layout clocks. **Stop** releases the decoders. A new clip for an existing slot opens on a hidden player and is swapped in as a clean cut once its first frame is decoded, so the outgoing clip keeps playing instead of freezing. Exclusion changes apply to new selections; currently eligible clips finish their existing ranges. A video that becomes entirely ineligible is replaced.

## Controls

| Action | Shortcut |
|---|---|
| Play / pause | Space |
| Fullscreen | F11 or double-click video |
| Leave fullscreen / close panel | Escape |
| Next clips | Ctrl+Right |
| Next layout | Ctrl+L |
| Settings | Ctrl+, |

## Build from source

Prerequisites: Windows x64, Visual Studio 2022 with Desktop development with C++, Windows SDK, CMake 3.24+, Python 3.10+, and FFprobe on PATH (or beside the executable). Qt and libmpv install into this project's `.deps` directory; no system-wide package installation is needed.

```powershell
./scripts/setup.ps1
```

For subsequent builds:

```powershell
./scripts/build.ps1 -Test -Deploy
```

Qt is pinned to 6.8.3. The libmpv archive URL and SHA-256 are recorded in `dependencies.lock.json`; setup verifies the archive before extracting it. Qt Widgets is used for the initial native interface, with all video composition in one OpenGL surface. This avoids requiring Qt Quick 3D for the 2D release.

`build/Release` includes the deployed Qt libraries and libmpv runtime. It is a development build folder, not an installer; another machine also needs the Microsoft Visual C++ 2015–2022 x64 runtime and FFprobe. Do not copy only the EXE.

## Validation

```powershell
./scripts/build.ps1 -Test
python scripts/smoke_test.py
python scripts/orientation_test.py
python scripts/benchmark.py --label local60 --fps 60
python scripts/benchmark.py --label local30 --fps 30
```

The core suite tests safe clip bounds, exact-fit/short files, percentage and per-video overrides, shuffle exhaustion, reservation races/cancellation, restoration after restart, active-source deferral, changing libraries, layout coverage, settings normalization, and database persistence.

The real playback test generates local H.264, HEVC, MPEG-4, MPEG-2, VP9, and WMV2 fixtures, plus a short file and a deliberately invalid file. It exercises multiple decoders, clip changes, independent layouts, pause/resume, fullscreen, library/performance panels, and single-video mode. It saves screenshots and JSON telemetry under `test-output`. It briefly plays a synthetic tone to verify exclusive audio routing.

The orientation regression test renders a labeled red-top/blue-bottom clip through the actual player and checks the resulting framebuffer, catching accidental vertical flips in the libmpv/texture integration.

The benchmark generates eight local 1080p/30 H.264 files and profiles 2–4 concurrent videos, random clip changes, and animated layout transitions with the performance panel open. JSON reports under `test-output/benchmark` include frame gaps, paint, scheduler, layout, retirement, load, and audio-control timings. Summary statistics exclude the first five seconds of startup. The backend regression suite also checks asynchronous mute acknowledgments, property caching, redundant-write suppression, idle-player readiness, and texture reuse.

## Scope and current limits

- One display GPU handles composition and decoding. The verified renderer on the development machine is the RTX 5090. Multi-GPU scheduling across the 4090/3090 is deferred; hardware decoding falls back to software for unsupported formats.
- This is an SDR compositor. HDR passthrough, multi-monitor sessions, 3D cubes, and a Windows `.scr` screensaver are deferred.
- Clip changes open asynchronously on hidden players. mpv players and render contexts are warmed up to two per maximum segment, one on screen and one preparing the next clip, and are reused across layouts, avoiding synchronous player destruction/initialization during ordinary transitions. A prepared clip stays paused and muted until it has a decoded frame and is swapped into the visible segment. Audio handoffs wait for mute acknowledgment before unmuting the next source. There is no audio crossfade yet.
- Pool warmup can delay initial Play or expansion to a higher capacity. The pool retains up to two players per maximum segment for the highest capacity used during the session, including reusable texture allocations; Stop releases it. The performance panel distinguishes active and idle players. Opening/seeking and driver work can still produce occasional frame gaps.
- File support depends on the bundled libmpv/FFmpeg build. The extension candidate list is in `src/library.cpp`; corrupt, unsupported, durationless, or too-short files are excluded. Failed playback sources are skipped for the current app session.
- Folder changes require **Rescan**. Deduplication is by canonical path, not file-content hashing. Folder removal is disabled during a scan.
- The cache setting controls demuxer cache per decoder, not total RAM. Textures and decoder surfaces use additional memory. Texture downscaling does not reduce source decode resolution.
- Settings permit up to 32 slots, but that is not a performance guarantee for 32 high-resolution sources. The codec smoke test uses 640×360 fixtures and the transition benchmark uses 2–4 1080p sources; neither is a 32-stream 4K/8K stress benchmark.

## Source map

- `src/core.*`: settings, eligibility, clip selection, shuffle, and layout geometry.
- `src/library.*`: SQLite persistence and the background FFprobe scanner.
- `src/mpv_backend.*`: dynamically loaded libmpv API and decoder/render integration.
- `src/canvas.*`: decoder slots, playback/layout scheduling, GPU composition, and audio routing.
- `src/window.*`: native controls, presets, library editor, and diagnostics.

See `THIRD_PARTY.md` for dependency sources and licensing notes.
