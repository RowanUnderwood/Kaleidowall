"""Profile exports with real 1080p sources and GPU engine telemetry. Generated media only."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--seconds", type=float, default=12)
parser.add_argument("--tiles", type=int, nargs="+", default=[2, 4, 8])
parser.add_argument("--sizes", type=int, nargs="+", default=[1080, 2160])
args = parser.parse_args()
out = ROOT / "test-output/export-benchmark"
media = ROOT / "test-output/benchmark/media"
media.mkdir(parents=True, exist_ok=True)
ffmpeg = shutil.which("ffmpeg")
first = media / "1080p-01.mp4"
if not first.exists():
    subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
        "testsrc2=s=1920x1080:r=30:d=20", "-c:v", "libx264", "-preset", "ultrafast", "-threads", "8", "-y", str(first)], check=True)
for i in range(2, 9):
    target = media / f"1080p-{i:02}.mp4"
    if not target.exists(): shutil.copyfile(first, target)
env = os.environ.copy()
env["PATH"] = str(ROOT / ".deps/Qt/6.8.3/msvc2022_64/bin") + os.pathsep + env["PATH"]
try:
    import psutil
except ImportError:
    psutil = None
summaries = []
for size in args.sizes:
    for tiles in args.tiles:
        run = out / f"{tiles}tiles-{size}p60"
        run.mkdir(parents=True, exist_ok=True)
        settings = dict(minSlots=tiles, maxSlots=tiles, duplicates=tiles > 8, clipMin=8, clipMax=10,
                        layoutMin=3, layoutMax=4, transition=1.2)
        (run / "settings.json").write_text(json.dumps(settings))
        samples = []
        with (run / "app.log").open("w") as log:
            process = subprocess.Popen([os.environ.get("KALEIDOWALL_EXE", str(ROOT / "build/Release/Kaleidowall.exe")), "--data-dir", str(run / "data"),
                "--library", str(media), "--export", str(run / "video.mp4"), "--export-seconds", str(args.seconds),
                "--export-size", str(size), "--export-fps", "60", "--export-settings", str(run / "settings.json"),
                "--export-overwrite"], env=env, stdout=log, stderr=log)
            started = time.monotonic()
            while process.poll() is None:
                sample = dict(seconds=time.monotonic()-started)
                if psutil:
                    try:
                        parent = psutil.Process(process.pid)
                        sample["rssMiB"] = sum(p.memory_info().rss for p in [parent, *parent.children(recursive=True)] if p.is_running()) / 1048576
                        sample["systemCpuPercent"] = psutil.cpu_percent()
                    except (psutil.NoSuchProcess, psutil.AccessDenied):
                        pass
                if shutil.which("nvidia-smi"):
                    value = subprocess.run(["nvidia-smi", "-i", "0", "--query-gpu=utilization.gpu,utilization.encoder,utilization.decoder,memory.used",
                        "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=5)
                    try:
                        for key, number in zip(["gpuPercent", "encoderPercent", "decoderPercent", "vramMiB"], value.stdout.strip().split(",")):
                            sample[key] = float(number)
                    except ValueError:
                        pass
                samples.append(sample)
                if time.monotonic() - started > 600:
                    process.kill()
                    raise RuntimeError("Export benchmark timed out")
                time.sleep(.25)
            assert process.returncode == 0, (run, process.returncode, (run / "data/export.json").read_text())
        report = json.loads((run / "data/export.json").read_text())
        (run / "telemetry.json").write_text(json.dumps(samples, indent=2))
        summary = dict(tiles=tiles, height=size, duration=report["duration"], elapsed=report["elapsedSeconds"],
                       speed=report["duration"] / report["elapsedSeconds"],
                       decodeUploadMs=report["decodeUploadMs"], encoderWriteMs=report["encoderWriteMs"])
        for key in ("rssMiB", "vramMiB", "gpuPercent", "encoderPercent", "decoderPercent", "systemCpuPercent"):
            values = [s[key] for s in samples if key in s]
            if values: summary["peak_" + key] = max(values)
        summaries.append(summary)
        print(json.dumps(summary), flush=True)
summary_name = "summary.json" if args.tiles == [2, 4, 8] and args.sizes == [1080, 2160] else (
    "summary-" + "-".join(map(str, args.tiles)) + "tiles-" + "-".join(map(str, args.sizes)) + "p.json")
(out / summary_name).write_text(json.dumps(summaries, indent=2))
