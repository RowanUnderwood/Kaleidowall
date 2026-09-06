"""End-to-end export validation using generated media, the real UI, GPU compositor and encoder.

Run after scripts/build.ps1 -Test. Outputs remain under test-output/export-validation.
"""
import array
import ctypes
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "test-output/export-validation"
MEDIA = OUT / "media"
ORIENTATION = OUT / "orientation"
for folder in (OUT, MEDIA, ORIENTATION):
    folder.mkdir(parents=True, exist_ok=True)
FFMPEG = shutil.which("ffmpeg")
FFPROBE = shutil.which("ffprobe")
assert FFMPEG and FFPROBE
ENV = os.environ.copy()
ENV["PATH"] = str(ROOT / ".deps/Qt/6.8.3/msvc2022_64/bin") + os.pathsep + ENV["PATH"]


def ffmpeg(*args):
    return subprocess.check_output([FFMPEG, "-hide_banner", "-loglevel", "error", *map(str, args)], timeout=90)


for name, rate, frequency, codec in [
    ("one", 24, 440, "libx264"), ("two", 30, 660, "libx265"),
    ("three", 60, 880, "libvpx-vp9"), ("four", 30, 1100, "wmv2")
]:
    fixture = MEDIA / f"{name}.mkv"
    if not fixture.exists():
        ffmpeg("-f", "lavfi", "-i", f"testsrc2=size=640x360:rate={rate}:duration=12",
               "-f", "lavfi", "-i", f"sine=frequency={frequency}:sample_rate=48000:duration=12",
               "-c:v", codec, "-threads", "2", "-pix_fmt", "yuv420p", "-c:a", "pcm_s16le", "-y", fixture)
upright = ORIENTATION / "upright.mp4"
if not upright.exists():
    ffmpeg("-f", "lavfi", "-i", "color=c=0xe83c43:s=640x360:r=30:d=12",
           "-vf", "drawbox=x=0:y=180:w=640:h=180:color=0x285ac4:t=fill",
           "-c:v", "libx264", "-pix_fmt", "yuv420p", "-y", upright)
mp3 = OUT / "soundtrack.mp3"
wav = OUT / "soundtrack.wav"
for track, codec in [(mp3, "libmp3lame"), (wav, "pcm_s16le")]:
    ffmpeg("-f", "lavfi", "-i", "sine=frequency=523:sample_rate=48000:duration=1.123",
           "-c:a", codec, "-y", track)

base = dict(minSlots=2, maxSlots=2, clipMin=.7, clipMax=1.3, layoutMin=1, layoutMax=2,
            transition=.6, volume=80, modes=["Grid", "Circles", "Hexagons"], backgroundColor="#182c40")
results = []


def export(name, seconds=2.1, height=720, fps=30, quality="high", audio=None,
           settings=None, media=MEDIA, extra=(), expected=0, destination=None):
    run = OUT / name
    run.mkdir(exist_ok=True)
    settings_file = run / "settings.json"
    settings_file.write_text(json.dumps(settings or base))
    output = destination or run / "video.mp4"
    args = [os.environ.get("KALEIDOWALL_EXE", str(ROOT / "build/Release/Kaleidowall.exe")), "--data-dir", str(run / "data"),
            "--library", str(media), "--export", str(output), "--export-seconds", str(seconds),
            "--export-size", str(height), "--export-fps", str(fps), "--export-quality", quality,
            "--export-settings", str(settings_file), "--export-overwrite"]
    if audio == "clips":
        args += ["--export-clip-audio"]
    elif audio:
        args += ["--export-audio", str(audio)]
    args += list(extra)
    with (run / "app.log").open("w") as log:
        completed = subprocess.run(args, env=ENV, stdout=log, stderr=log, timeout=180)
    report = json.loads((run / "data/export.json").read_text())
    assert completed.returncode == expected, (name, completed.returncode, report)
    if expected:
        assert not report["success"]
        assert not list(output.parent.glob(".kaleidowall-*")), "Leaked export staging directory"
        return output, report
    metadata = report["metadata"]
    video = next(s for s in metadata["streams"] if s["codec_type"] == "video")
    n = math.ceil(seconds * fps - 1e-8)
    assert int(video["nb_frames"]) == n
    assert video["height"] == height
    assert video["width"] == {720: 1280, 1080: 1920, 2160: 3840}[height]
    assert video["avg_frame_rate"] == f"{fps}/1"
    assert abs(float(video["duration"]) - n / fps) < 1 / fps
    assert video["codec_name"] == "h264" and video["pix_fmt"] == "yuv420p"
    assert video["color_space"] == "bt709" and video["color_range"] == "tv"
    assert bool([s for s in metadata["streams"] if s["codec_type"] == "audio"]) == bool(audio)
    ffmpeg("-i", output, "-f", "null", "-")
    assert not list(output.parent.glob(".kaleidowall-*")), "Leaked export staging directory"
    summary = dict(name=name, duration=n / fps, elapsed=report["elapsedSeconds"],
                   speed=round((n / fps) / report["elapsedSeconds"], 2),
                   decodeMs=report["decodeUploadMs"], encodeMs=report["encoderWriteMs"])
    results.append(summary)
    print("PASS", summary, flush=True)
    return output, report


for height in (720, 1080, 2160):
    for fps in (30, 60):
        export(f"matrix-{height}-{fps}", height=height, fps=fps, quality={720: "low", 1080: "medium", 2160: "high"}[height])

output, report = export("clip-audio", seconds=6.137, fps=60, audio="clips")
pcm = array.array("h", ffmpeg("-i", output, "-map", "0:a:0", "-ac", "1", "-ar", "48000", "-f", "s16le", "-"))
frequencies = dict(one=440, two=660, three=880, four=1100)
assert len(report["audio"]) >= 2
for span in report["audio"]:
    if span["end"] - span["start"] < .15:
        continue
    center = (span["start"] + span["end"]) / 2
    start = int(center * 48000) - 1024
    samples = pcm[start:start + 2048]
    def power(freq):
        re = sum(v * math.cos(2 * math.pi * freq * i / 48000) for i, v in enumerate(samples))
        im = sum(v * math.sin(2 * math.pi * freq * i / 48000) for i, v in enumerate(samples))
        return re * re + im * im
    actual = max(frequencies.values(), key=power)
    assert actual == frequencies[Path(span["source"]).stem], (span, actual)
print("PASS: clip audio follows the selected video source at the planned timestamps", flush=True)

# Prefer the default audio track, as Play does, when a source has multiple tracks.
dual = OUT / "dual-audio-media"
dual.mkdir(exist_ok=True)
ffmpeg("-i", upright, "-f", "lavfi", "-i", "sine=frequency=300:sample_rate=48000:duration=12",
       "-f", "lavfi", "-i", "sine=frequency=900:sample_rate=48000:duration=12",
       "-map", "0:v:0", "-map", "1:a:0", "-map", "2:a:0", "-c:v", "copy", "-c:a", "pcm_s16le",
       "-disposition:a:0", "0", "-disposition:a:1", "default", "-y", dual / "two-tracks.mkv")
output, _ = export("default-audio-track", seconds=1, settings=dict(base, minSlots=1, maxSlots=1), media=dual, audio="clips")
samples = array.array("h", ffmpeg("-ss", "0.3", "-i", output, "-t", "0.1", "-ac", "1", "-ar", "48000", "-f", "s16le", "-"))
def tone_power(frequency):
    re = sum(v * math.cos(2 * math.pi * frequency * i / 48000) for i, v in enumerate(samples))
    im = sum(v * math.sin(2 * math.pi * frequency * i / 48000) for i, v in enumerate(samples))
    return re * re + im * im
assert tone_power(900) > tone_power(300) * 10, "Export ignored the default audio track"

for track in (mp3, wav):
    output, _ = export("import-" + track.suffix[1:], seconds=2.137, audio=track)
    pcm = array.array("h", ffmpeg("-i", output, "-map", "0:a:0", "-ac", "1", "-ar", "48000", "-f", "s16le", "-"))
    assert max(abs(n) for n in pcm[24000:26000]) > 100
    assert max(abs(n) for n in pcm[72000:80000]) < 20, "Short soundtrack did not end in silence"

output, _ = export("upright", seconds=1, settings=dict(base, minSlots=1, maxSlots=1, reducedMotion=True), media=ORIENTATION)
pixels = ffmpeg("-ss", "0.5", "-i", output, "-vf", "scale=1:4:flags=area", "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-")
assert pixels[0] > pixels[2] + 70 and pixels[-1] > pixels[-3] + 70, list(pixels)
print("PASS: GPU NV12 conversion preserves orientation and colors", flush=True)

# Rotation metadata and anamorphic sample aspect ratios must survive the separate export decoder.
for label, transform in [("rotation", ["-c", "copy", "-metadata:s:v:0", "rotate=90"]),
                         ("anamorphic", ["-vf", "setsar=2", "-c:v", "libx264"])]:
    folder = OUT / (label + "-media")
    folder.mkdir(exist_ok=True)
    source = folder / "source.mp4"
    ffmpeg("-i", upright, *transform, "-y", source)
    output, _ = export(label, seconds=1, settings=dict(base, minSlots=1, maxSlots=1, reducedMotion=True, crop=False), media=folder)
    actual = ffmpeg("-ss", "0.5", "-i", output, "-vf", "scale=160:90", "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-")
    expected = ffmpeg("-ss", "0.5", "-i", source, "-vf", "scale=w=trunc(iw*sar):h=ih,setsar=1,scale=160:90:force_original_aspect_ratio=decrease,pad=160:90:(ow-iw)/2:(oh-ih)/2:color=0x182c40", "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-")
    # Borders and resampling differ slightly; the interior must preserve geometry, orientation and colors.
    differences = [abs(actual[(y*160+x)*3+c] - expected[(y*160+x)*3+c])
                   for y in range(10,80) for x in range(10,150) for c in range(3)]
    assert sum(differences) / len(differences) < 12, (label, sum(differences)/len(differences))

export("software", seconds=1, extra=("--export-software",))
preserve = OUT / "preserve.mp4"
preserve.write_bytes(b"existing user file")
export("cancel", seconds=120, destination=preserve, extra=("--export-cancel-ms", "1200"), expected=3)
assert preserve.read_bytes() == b"existing user file"
export("protect-source", destination=upright, media=ORIENTATION, expected=1)
ffmpeg("-i", upright, "-f", "null", "-")

if os.name == "nt":
    # A final publication failure must preserve the existing destination, even after a full render.
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                                   ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.CreateFileW(str(preserve), 0x80000000, 0, None, 3, 0x80, None)
    assert handle not in (None, ctypes.c_void_p(-1).value)
    try:
        export("locked-destination", seconds=.2, destination=preserve, expected=1)
    finally:
        kernel.CloseHandle(handle)
    assert preserve.read_bytes() == b"existing user file"

unicode_media = OUT / "unicode-media"
unicode_media.mkdir(exist_ok=True)
shutil.copyfile(upright, unicode_media / "媒体 — café.mp4")
export("unicode", seconds=.2, media=unicode_media, destination=OUT / "wall — café 测试.mp4")
empty = OUT / "empty-library"
empty.mkdir(exist_ok=True)
export("empty", media=empty, expected=1)

output, _ = export("steady-1080p60", seconds=12, height=1080, fps=60,
                   settings=dict(base, clipMin=8, clipMax=10, layoutMin=3, layoutMax=4))
ffmpeg("-ss", "3", "-i", output, "-frames:v", "1", "-y", OUT / "export-frame.png")
(OUT / "results.json").write_text(json.dumps(results, indent=2))
print("All export integration checks passed.", flush=True)
