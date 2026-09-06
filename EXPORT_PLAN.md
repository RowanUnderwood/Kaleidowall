# Kaleidowall export proposal

Date: 2026-09-06. Approved by the user and implemented. The proposal below records the original design;
`README.md`, `CLAUDE.md`, and `PERFORMANCE.md` describe the delivered behavior and measured results.

Implementation adjustment: export uses the pinned FFmpeg executables with bounded Windows pipes,
Direct3D decoding, shared OpenGL composition, GPU NV12 conversion, and NVENC encoding. This avoids
adding linked FFmpeg development libraries or a CUDA toolkit. NV12 crosses bounded CPU staging buffers;
the direct GPU interoperability bridge remains a possible optimization, rather than a delivery dependency.

## Confirmed behavior

- Export starts a fresh randomized sequence using the current applied settings.
- Clip audio follows Play mode's changing single audio source.
- Add Export immediately beside Settings. Export includes all current layouts, motion,
  transitions, masks, fit/crop behavior, background color, clip bounds, and selection rules.
- Export controls include duration, output resolution, 30/60 fps, quality, destination,
  and audio selection with MP3/WAV import. Importing MP3 snaps duration to the audio length.

## Recommended interface

Use a native dialog styled like the existing controls. Defaults: 60 seconds, 1080p,
60 fps, High quality, MP4. Remember export preferences separately from playback settings.

| Control | Behavior |
|---|---|
| Duration | Hours/minutes/seconds, with fractional precision and actual rounded output duration shown |
| Canvas | 4K UHD (3840 x 2160), 1080p (1920 x 1080), 720p (1280 x 720); landscape 16:9 initially |
| Frame rate | 30 or 60 fps, independent of Play mode's display target |
| Quality | Low, Medium, High; High selected initially |
| Destination | Native Save dialog for folder and filename; confirm replacement of an existing file |
| Audio | None, Follow clip audio, Imported MP3/WAV; initialize from current mute setting |
| Audio volume | Export level initialized from playback volume, independent of preview monitoring |
| Match audio duration | Enabled on import for both MP3 and WAV; uncheck to edit duration |

Probe imported audio asynchronously before starting. With matching enabled, derive length
from the audible sample timeline, accounting for supported encoder delay/padding metadata.
With matching disabled, trim longer audio and fill the remainder of shorter audio with
silence. Do not loop automatically. Imported audio replaces clip audio. Audio-less active
clips produce silence when no eligible audio source exists, matching playback behavior.

Pause ordinary playback during export and restore its previous running/paused/stopped state
after completion or cancellation. Export uses its own selection/shuffle state and seed, so
it does not consume the saved playback shuffle. Freeze the settings and eligible-media
snapshot for the job. Disable controls that would mutate that job while it runs.

Show a letterboxed preview of exported frames in the existing play area, at approximately
3 updates per wall-clock second and at most 720p. Use a latest-frame mailbox so preview
never backlogs or throttles the renderer. Also show progress, frames completed/total,
elapsed time, estimated remaining time, and rendering speed. Distinguish Preparing,
Rendering, Finalizing, Completed, Canceled, and Failed. Hide ETA until it is meaningful.
Progress reaches 100% only after encoding, audio, and container finalization succeed.
Provide Cancel, then Open file and Show in folder after success. Minimized windows can
skip previews while rendering continues.

## Codec and quality

Default to MP4, H.264 High profile via RTX 5090 NVENC, SDR BT.709, 8-bit 4:2:0,
constant frame rate, and AAC stereo at 48 kHz / 192 kb/s when audio is enabled.
Use variable bitrate with a quality target, automatic compatible codec level, a two-second
keyframe interval, and MP4 fast-start finalization. Explicitly convert and tag colors/range;
metadata tagging alone is insufficient. Preserve the current SDR appearance.

Initial H.264 NVENC presets to calibrate on representative mosaic footage:

| Quality | NVENC preset | VBR target quality (CQ) | Intent |
|---|---|---|---|
| Low | P4 | 28 | Small files, quick drafts |
| Medium | P5 | 23 | Balanced quality and size |
| High | P6 | 19 | Detailed final exports |

These are proposed tuning values, not measured quality guarantees. Configure bitrate
policy explicitly so an encoder default does not unintentionally cap quality. Use HQ
tuning; benchmark adaptive quantization, lookahead, and multipass before enabling them.
Output size varies with content; any size estimate must be labeled approximate.

An optional advanced AV1 format is a useful later addition for compression and compatible
destinations. Do not make it a prerequisite for the first export. Detect encoder capabilities
at runtime. If NVENC initialization fails, offer an explicit software H.264 fallback and
show the selected encoder; do not silently change an in-progress job's codec.

## Rendering architecture

The current Canvas scheduler uses elapsed wall time; libmpv owns playback clocks and routes
audio to a device. The current API wrapper does not expose timestamp-addressed decoded video
and PCM audio suitable for an offline exporter. Merely accelerating the UI timer or encoder
will not make the full mosaic export frame-accurate or faster than real time.

1. Extract reusable session scheduling and scene evaluation from Canvas. A shared scene
   model owns selection, clip ranges, layout changes, geometry, masks, and audio-source
   decisions. Playback supplies its real-time clock and readiness events; export supplies
   exact frame timestamps and waits for resources without advancing simulated time.
   Preserve playback's asynchronous readiness and mute-acknowledgment invariants.
2. Extract the compositor into a target-size-aware renderer accepting explicit scene state
   and textures. Reuse existing shader math; remove widget-size/device-pixel-ratio dependencies
   from export. Render to a fixed-resolution offscreen framebuffer. Compute geometry and
   texture sizes for the selected output, not the resized preview window. Avoid inheriting
   the playback texture limit of 1920 for a large tile in a 4K export.
3. Add an export decoder using pinned FFmpeg development libraries (libavformat, libavcodec,
   libavutil, libswresample, and conversion facilities as needed). Keep libmpv for interactive
   playback. Decode sequentially after one accurate seek at each source clip start; retain
   bounded frame queues and select by presentation timestamp for each output timestamp.
   Handle variable frame rates, B-frame reorder, rotation, sample aspect ratio, color metadata,
   nonzero stream timestamps, and early EOF. Do not seek each source for every output frame.
4. Use a worker-owned OpenGL context/offscreen surface with proper Qt thread ownership.
   Run hardware decode on the 5090 where supported, composite on that GPU, convert to NV12,
   and feed NVENC. Prototype CUDA/OpenGL interoperability for the GPU transfer path; use a
   Direct3D bridge if that proves more practical. Windows NVENC does not support an OpenGL
   encode device directly. A bounded asynchronous pixel-buffer readback/upload path is an
   alternative to benchmark, not an assumption of zero-copy performance.
5. Render output frame n at n/fps. For duration D use ceil(D * fps) frames and disclose the
   rounded duration, differing by less than one frame. Never skip output timestamps to
   catch up with wall time. Slow scenes may render below real time while retaining timing.
6. Generate audio from the same timeline: decode and trim the chosen source's PCM intervals,
   resample to 48 kHz, apply export gain, and switch at the same planned clip/layout events.
   Only one source contributes at a time; use silence for gaps. For imported audio, replace
   this track. Pad by less than one video frame where needed to match the rounded duration,
   and account for AAC delay at muxing. Do not capture speakers or unrelated system audio.
7. Encode and mux incrementally with backpressure and bounded memory. Stage output in a
   uniquely named temporary file beside the destination, flush/finalize, validate, and only
   then replace the destination. Cancel stops workers and removes only this job's temporary
   output. Report disk-full, inaccessible/missing source, encoder failure, and muxing failure
   clearly. Fail a job rather than silently changing its planned content on decode errors.

Store a compact job manifest with settings, seed, media identities, planned events, encoder
settings, and timings for repeatable diagnosis. This need not become a user-facing editor.
Reuse deterministic selection logic, but avoid a giant predecoded timeline in memory.

## Performance strategy and feasibility gate

Target faster-than-real-time offline rendering; actual speed depends on the entire pipeline,
especially source codecs, resolution, simultaneous tiles, seeking, and storage throughput.
Do not promise a fixed speed multiplier or 32 simultaneous 4K sources at 60 fps.

Use the 9950X3D for demux, audio, software decode fallback, and bounded parallel jobs; prevent
each of many decoders from independently claiming every CPU thread. Use the 5090's decoder,
graphics/compute, and encoder engines concurrently. Bound host caches initially around
8-16 GiB, then tune from measured benefits, and separately cap VRAM according to available
capacity. Allocate on demand. Do not fill 128 GiB merely because it exists.

Measure stage timings, queue starvation, decode/encode rates, GPU-engine utilization,
CPU load, RAM/VRAM, and disk throughput. Bottleneck removal is the objective; 100% utilization
of every resource simultaneously is neither required nor necessarily possible. This machine
also reports a 4090 and 3090, but multi-GPU scheduling is outside the initial design: transfers
can offset the benefit and the existing renderer already uses the 5090.

First implementation milestone: a short fixed-scene export using mixed frame-rate sources,
one animated layout transition, one clip cut, and synchronized audio. Measure 1080p60 and
4K60 with encoding and preview enabled. Verify correctness before comparing export speed.
If offline rendering works but is slower than real time, keep it: it still preserves every
output frame. If timestamp-controlled decoding/interop cannot be made practical within the
agreed scope, report the measured blocker and use real-time offscreen capture as the fallback,
with explicit dropped/duplicated-frame diagnostics and synchronized app-only audio. Do not
silently degrade an offline export into screen recording. Real-time capture is also unable
to guarantee smooth 4K60 for every workload.

## Delivery and validation sequence

1. Prove the timestamp-controlled decode/composite/encode/audio pipeline and benchmark it.
2. Extract shared scene/render code and run existing core, backend, smoke, orientation, and
   playback-performance checks to protect Play mode.
3. Build the export controller, pinned dependency packaging, output handling, and audio modes.
4. Add dialog, duration matching, preview, progress, cancellation, and completion actions.
5. Validate all resolution/fps combinations and quality presets. Check frame count, timestamps,
   duration, stream metadata, decoding, and A/V sync using synthetic numbered frames and tones.
   Compare representative transition/mask/crop/background frames against the shared renderer;
   check orientation, colors, and output at different preview window sizes.
6. Exercise missing/corrupt sources, silent clips, MP3 padding, WAV, Unicode paths, filename
   replacement, disk-full/finalization errors, cancel at every stage, NVENC unavailability,
   minimized preview, repeat exports, memory stability, and restoration of playback state.
7. Benchmark representative 2/4/8-tile workloads at 1080p60 and 4K60; add higher source counts
   as stress cases. Record measurements in PERFORMANCE.md, update README/CLAUDE.md and
   dependency notices, and hand off changed files and validation results to Claude.

## Evidence gathered during planning

- Inspected core, Canvas, window, mpv backend, existing tests/documentation and dependency pins.
- Local FFmpeg is 7.0 essentials; h264_nvenc exposes the proposed presets and CQ control.
- A 60-frame synthetic 1080p60 H.264 NVENC encode on GPU 0 (RTX 5090) produced no errors.
  This establishes basic encoder availability only, not full export performance.
- NVIDIA driver reported 610.88 and approximately 32 GiB of VRAM on the 5090.
- NVIDIA describes CUDA/DirectX Windows encoding, Linux-only direct OpenGL encoding,
  preset tradeoffs, capability queries, and pipeline concurrency in its
  [NVENC programming guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html).
- See [FFmpeg documentation](https://ffmpeg.org/ffmpeg.html) for encoding/muxing controls.

No further user answers are required to proceed with the recommended scope once implementation
is requested. Proposed defaults include landscape 16:9, WAV duration matching, no automatic
audio looping, paused ordinary playback during export, and H.264 MP4 as the initial format.
