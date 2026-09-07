"""Real .scr validation with an isolated database and generated media; does not register the saver."""
import ctypes
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import time

from PIL import Image, ImageChops, ImageStat

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "test-output" / ("screensaver-" + time.strftime("%Y%m%d-%H%M%S"))
OUT.mkdir(parents=True)
MEDIA = OUT / "media"
MEDIA.mkdir()
EXE = Path(os.environ.get("KALEIDOWALL_SCR", ROOT / "build/Release/Kaleidowall.scr"))
FFMPEG = ROOT / ".deps/ffmpeg/ffmpeg.exe"
ENV = os.environ.copy()
ENV["PATH"] = str(ROOT / ".deps/Qt/6.8.3/msvc2022_64/bin") + os.pathsep + ENV["PATH"]


def run(name, *args, seconds=4):
    report = OUT / (name + ".json")
    with (OUT / (name + ".log")).open("w") as log:
        p = subprocess.run([str(EXE), *args, "--data-dir", str(OUT), "--test-seconds", str(seconds),
                            "--test-report", str(report)], env=ENV, stdout=log, stderr=log, timeout=30)
    assert p.returncode == 0, (name, p.returncode)
    return json.loads(report.read_text()) if report.exists() else None


run("configuration", "/c", seconds=1)
assert (OUT / "configuration.json.png").exists()
for i, color in enumerate(["red", "green", "blue", "yellow"]):
    subprocess.run([str(FFMPEG), "-v", "error", "-f", "lavfi", "-i", f"color={color}:s=640x360:r=30:d=8",
                    "-f", "lavfi", "-i", f"sine=frequency={440+i*110}:duration=8",
                    "-vf", "drawbox=x=0:y=180:w=640:h=180:color=white:t=fill", "-c:v", "libx264",
                    "-c:a", "aac", "-y", str(MEDIA / f"{i}.mp4")], check=True, timeout=30)

settings = dict(minSlots=2, maxSlots=2, clipMin=1, clipMax=2, layoutMin=1, layoutMax=2,
                transition=.3, muted=False, modes=["Inset"], crop=True)
default = dict(settings, minSlots=3, maxSlots=3)
with sqlite3.connect(OUT / "library.sqlite") as db:
    for key, value in {"settings": settings, "preset:Saver test": default,
                       "presetState": {"defaultPreset": "Saver test"}, "shuffle": {"marker": 123}}.items():
        db.execute("INSERT OR REPLACE INTO kv VALUES(?,?)", (key, json.dumps(value)))
    for path in MEDIA.glob("*.mp4"):
        db.execute("INSERT INTO videos(id,path,title,duration,width,height,codec,audio) VALUES(?,?,?,?,?,?,?,?)",
                   (str(path).lower().replace("\\", "/"), str(path), path.stem, 8, 640, 360, "h264", 1))

report = run("mirrored", "/s", "--test-windowed", "--test-mirrors", "2", seconds=6)
assert report["mirrors"] == 2 and report["muted"] and report["layout"] == "Inset"
assert len(report["slots"]) == 3 and all(s["ready"] for s in report["slots"])
assert len(report["slots"]) + len(report["idleSlots"]) <= 6, "Mirrors created extra decoders"
assert all(s["muted"] == "yes" for s in report["slots"] + report["idleSlots"])
source = Image.open(OUT / "mirrored.json.png").convert("RGB")
for i in range(2):
    mirror = Image.open(OUT / f"mirrored.json.mirror{i}.png").convert("RGB")
    expected = source.resize(mirror.size, Image.Resampling.BILINEAR)
    difference = ImageStat.Stat(ImageChops.difference(mirror, expected))
    assert max(difference.mean) < 8, ("Mirror differs or is inverted", i, difference.mean)
with sqlite3.connect(OUT / "library.sqlite") as db:
    stored = {k: json.loads(v) for k, v in db.execute("SELECT key,value FROM kv")}
    assert stored["settings"] == settings and stored["shuffle"] == {"marker": 123}
    assert "screensaverShuffle" in stored
    db.execute("INSERT OR REPLACE INTO kv VALUES(?,?)", ("screensaverPreferences", '{"muted":false}'))

report = run("audio", "/s", "--test-windowed")
assert not report["muted"]
assert sum(s["muted"] == "no" for s in report["slots"] + report["idleSlots"]) == 1

# Send input to this test process's own window, without injecting global keyboard input.
user32 = ctypes.windll.user32
user32.FindWindowW.restype = ctypes.c_void_p
user32.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
with (OUT / "dismiss.log").open("w") as log:
    p = subprocess.Popen([str(EXE), "/s", "--test-windowed", "--data-dir", str(OUT), "--test-seconds", "20"],
                         env=ENV, stdout=log, stderr=log)
    try:
        deadline = time.monotonic() + 8
        hwnd = None
        while time.monotonic() < deadline and p.poll() is None:
            hwnd = user32.FindWindowW(None, "Kaleidowall Screensaver")
            if hwnd:
                break
            time.sleep(.1)
        assert hwnd, "Screensaver did not open"
        time.sleep(1.5)
        user32.PostMessageW(hwnd, 0x100, 0x1B, 0)  # WM_KEYDOWN / Escape
        assert p.wait(timeout=6) == 0
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()
print("PASS: configuration, shared default/library, mirrored decoding/orientation, audio isolation, input dismissal")
print("Results:", OUT)
