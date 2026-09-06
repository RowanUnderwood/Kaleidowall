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
                # Sample every GPU, not index 0. The encoder, the decoder and the compositor each
                # address adapters through a different index space, so pinning the query to one
                # index reports zeros whenever the work lands elsewhere. export.json names the card
                # that actually encoded; the per-GPU series below is what makes a split visible.
                if shutil.which("nvidia-smi"):
                    value = subprocess.run(["nvidia-smi", "--query-gpu=index,name,utilization.gpu,utilization.encoder,utilization.decoder,memory.used",
                        "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=5)
                    gpus = {}
                    for line in value.stdout.strip().splitlines():
                        fields = [f.strip() for f in line.split(",")]
                        if len(fields) != 6:
                            continue
                        try:
                            gpus[fields[1]] = dict(index=int(fields[0]), gpuPercent=float(fields[2]),
                                encoderPercent=float(fields[3]), decoderPercent=float(fields[4]), vramMiB=float(fields[5]))
                        except ValueError:
                            pass
                    if gpus:
                        sample["gpus"] = gpus
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
                       decodeUploadMs=report["decodeUploadMs"], encoderWriteMs=report["encoderWriteMs"],
                       gpuEncode=report.get("gpuEncode", ""), gpuDecode=report.get("gpuDecode", ""))
        for key in ("rssMiB", "systemCpuPercent"):
            values = [s[key] for s in samples if key in s]
            if values: summary["peak_" + key] = max(values)
        # Peaks for the card export said it used, so the figures describe the same GPU as the run.
        encoding = report.get("gpuEncode") or report.get("gpu", "").split("/")[0].strip()
        for key in ("vramMiB", "gpuPercent", "encoderPercent", "decoderPercent"):
            values = [s["gpus"][encoding][key] for s in samples if encoding in s.get("gpus", {})]
            if values: summary["peak_" + key] = max(values)
        every = sorted({name for s in samples for name in s.get("gpus", {})})
        if every:
            summary["encoderPercentByGpu"] = {name: max(
                (s["gpus"][name]["encoderPercent"] for s in samples if name in s.get("gpus", {})), default=0.0)
                for name in every}
        summaries.append(summary)
        print(json.dumps(summary), flush=True)
summary_name = "summary.json" if args.tiles == [2, 4, 8] and args.sizes == [1080, 2160] else (
    "summary-" + "-".join(map(str, args.tiles)) + "tiles-" + "-".join(map(str, args.sizes)) + "p.json")
(out / summary_name).write_text(json.dumps(summaries, indent=2))
