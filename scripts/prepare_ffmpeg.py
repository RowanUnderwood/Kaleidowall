"""Copy the validated FFmpeg executables into .deps/ffmpeg without changing system installations.

python scripts/prepare_ffmpeg.py [--source-dir PATH_TO_FFMPEG_BIN]
The default checks PATH and the standard Chocolatey FFmpeg directory. Only pinned hashes are accepted.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
lock = json.loads((ROOT / "dependencies.lock.json").read_text())["ffmpeg"]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source-dir", type=Path)
args = parser.parse_args()
dest = ROOT / ".deps/ffmpeg"
dest.mkdir(parents=True, exist_ok=True)
for name, expected in lock["files"].items():
    candidates = [dest / name]
    if args.source_dir:
        candidates.append(args.source_dir / name)
    found = shutil.which(name)
    if found:
        candidates.append(Path(found))
    candidates.append(Path(os.environ.get("ProgramData", "C:/ProgramData")) / "chocolatey/lib/ffmpeg/tools/ffmpeg/bin" / name)
    source = next((p for p in candidates if p.is_file() and hashlib.sha256(p.read_bytes()).hexdigest() == expected), None)
    if source is None:
        raise SystemExit(f"Validated {name} ({lock['version']}) not found. Pass --source-dir with its bin directory. "
                         "A different version requires updating dependencies.lock.json and rerunning export validation.")
    if source.resolve() != (dest / name).resolve():
        shutil.copyfile(source, dest / name)
    print(f"Verified {name}: {expected}")
