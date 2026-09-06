# Transition performance investigation — 2026-09-06

The observed transition stalls were primarily in the application's UI-thread control path, not in steady video painting. The old performance panel only displayed the duration of the latest paint call, so it did not expose long waits elsewhere in the UI event loop.

## Changes

- Warm and reuse idle libmpv players/render contexts up to the session's configured capacity. Retiring a tile sends an asynchronous stop rather than destroying a player on the UI thread.
- Observe playback properties into an event-driven cache; avoid synchronous queries in the scheduler and performance panel.
- Send property changes asynchronously and suppress unchanged writes. Audio handoffs wait for the outgoing mute command's acknowledgment before enabling another source.
- Update a video texture only when libmpv reports new content, or a texture allocation needs filling. Layout animation continues using the cached textures.
- Schedule fractional frame intervals against monotonic deadlines, avoiding repeated rounding of a 60 fps interval to 17 ms.
- Show p95/maximum frame intervals, maximum scheduler duration, active/idle players, texture allocation, and texture update rate in the performance panel.

## Local measurements

RTX 5090, windowed 1440×900 application, performance panel open. Eight synthetic 1920×1080/30 H.264 files; 2–4 simultaneous slots; 3–5 second clips; layout changes every 3–4 seconds with 1.2 second transitions. The 60 fps runs last 40 seconds, and the 30 fps run lasts 30 seconds. Summaries omit the first five seconds, including initial player warmup. Frame intervals measure the time between actual paint calls, not just the time spent inside paint.

| Measurement | Before, target 60 | Improved, target 60 | Improved, target 30 |
|---|---:|---:|---:|
| Average achieved paint rate | 56.69 fps | 59.96 fps | 30.00 fps |
| Longest frame interval | 475.15 ms | 39.65 ms | 45.11 ms |
| Intervals longer than 50 ms | 19 | 0 | 0 |
| Longest layout change | 428.34 ms | 3.24 ms | 4.31 ms |
| Longest tile retirement | 458.99 ms | 0.83 ms | 0.40 ms |
| Longest audio-control pass | 340.65 ms | 0.02 ms | 0.02 ms |

These are comparable local workloads, not a replay of every file in the user's library or a guarantee of perfect frame pacing. Clip seeking, new codec initialization, GPU driver work, display timing, and higher stream counts can still cause occasional gaps. Initial Play or raising the pool capacity incurs warmup; retained idle contexts/textures use additional memory until Stop.

Raw reports are under `test-output/benchmark/baseline60-20260906-042521`, `final60-20260906-043532`, and `final30-20260906-043700`. Run `python scripts/benchmark.py --label NAME --fps 60` to reproduce the workload. The benchmark intentionally uses generated media and an isolated library database.

The core and backend test suites cover shuffle/clip behavior, asynchronous mute acknowledgments (including stale/failed replies), event-based property caching, redundant write suppression, render invalidation, and player-stop acknowledgment. The mixed-codec smoke test additionally checks active/idle pool bounds, exclusive audio, clip survival through layouts, pause/resume, and retirement to single-video mode.
