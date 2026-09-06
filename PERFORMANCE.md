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

## Offline export measurements — 2026-09-06

Twelve-second exports, High H.264 NVENC quality, 60 fps, synthetic 1920×1080/30 H.264 sources, 8–10 second clips, 3–4 second layout intervals, and 1.2 second transitions. Preview is enabled. Timings include export preparation/rendering/finalization but exclude library scanning and application startup. These are local workload measurements, not guarantees for other codecs or files.

| Tiles | Output | Export time | Speed | Peak process-tree RAM |
|---|---|---|---|---|
| 2 | 1080p60 | 5.13 s | 2.34× | 1.12 GiB |
| 4 | 1080p60 | 7.79 s | 1.54× | 1.52 GiB |
| 8 | 1080p60 | 14.00 s | 0.86× | 2.31 GiB |
| 2 | 2160p60 | 10.87 s | 1.10× | 2.11 GiB |
| 4 | 2160p60 | 11.63 s | 1.03× | 2.50 GiB |
| 8 | 2160p60 | 16.47 s | 0.73× | 2.99 GiB |

The initial QProcess stdout implementation buffered decoded frames on Windows even without an event loop. At eight sources it reached about 17.5 GiB RAM at 1080p60 and took 29.41 seconds for a 12-second export. Explicitly bounded native pipes reduced this to about 2.31 GiB and 14.00 seconds. The older summary is retained in `test-output/export-benchmark/unbounded-summary.json`.

The renderer uses GPU composition and RGB-to-NV12 conversion, two asynchronous pixel-buffer objects, and NVENC encoding. Source and output NV12 bytes still cross CPU memory; direct GPU interoperability could improve throughput further. Source decode/transfer dominates the eight-tile cases. CPU decoding threads are bounded by the slot count. Memory is allocated on demand instead of preallocating the machine’s 128 GiB.

A separate 640×360 mixed-codec validation workload exported 12 seconds of 1080p60 output in about 3.6 seconds. Source resolution matters: that result should not be confused with the 1080p-source measurements above. Very short jobs also spend a larger fraction of their time starting decoders and finalizing the MP4.

All measured GPU composition ran on the RTX 5090. Earlier runs reported zero NVENC utilization despite successful `h264_nvenc` encoding, which was recorded here as an unreliable driver counter. It was not: the encode was running on a different card. FFmpeg addresses GPUs through three index spaces that disagree on a mixed machine — DXGI adapter order for `d3d11va`, the NVENC/CUDA ordinal for `h264_nvenc`, and NVML/PCI order for `nvidia-smi` — and all three were hardcoded to index 0. On this machine DXGI 0 and NVML 0 are the 5090, but the NVENC ordinal follows CUDA’s “fastest first” heuristic and index 0 was the 4090, so the benchmark sampled the 5090 while the encoder ran elsewhere. Export now resolves the decode adapter and NVENC device by name from the compositing GPU’s `GL_RENDERER` and pins `CUDA_DEVICE_ORDER=PCI_BUS_ID` for every FFmpeg child; `export.json` records `gpuDecode`/`gpuEncode`, and `export_benchmark.py` samples every GPU and summarizes the one that encoded. A 20-second 1080p60 four-tile export after the change measured 21% peak encoder and 4% peak decoder utilization on the 5090, and zero on both the 4090 and the 3090. GPU/CPU telemetry may include other work on the machine.

A short capacity stress check also exported 32 simultaneous 1080p sources (duplicates enabled), producing
two seconds of 1080p60 output in 9.74 seconds. Peak process-tree RAM was 6.44 GiB and reported GPU memory
was 5.64 GiB. This checks capacity/cleanup, not sustained throughput or 32 simultaneous 4K/8K sources.
Its report is `test-output/export-benchmark/summary-32tiles-1080p.json`.

Reproduce with `python scripts/export_benchmark.py --seconds 12`. JSON timings, source/audio manifests, preview screenshots, and sampled memory/GPU telemetry are under `test-output/export-benchmark`. `KALEIDOWALL_EXE` selects an alternate executable. Offline exports advance by exact frame timestamps regardless of wall-clock speed; no real-time capture fallback was needed.

### Playback regression after sharing the compositor

The final 30-second run at a 60 fps target recorded a 16.71 ms mean frame interval, 18.08 ms p95,
38.42 ms maximum, and zero intervals above 50 ms after startup. Maximum layout and audio-control
passes were 3.77 ms and 0.021 ms respectively. One prepared-cut delay reached 218 ms, while rendering
continued; source readiness can still defer cuts. Raw results are in
`test-output/benchmark/export-regression-20260906-134907`.

All three CTest suites and 20 export integration scenarios passed. The original orientation check passed,
and the mixed-codec playback smoke test passed at `test-output/run-20260906-135334`. The latter exposed
a delayed mute-property cache value during an acknowledged audio handoff; successful latest mute replies
now refresh that cache, with a backend regression test preserving stale/failed-reply handling.
