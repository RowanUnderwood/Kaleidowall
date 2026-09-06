"""Measure actual UI stalls with 2–4 concurrent 1080p sources. No user media needed."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--label', default='run')
parser.add_argument('--fps', default=60, type=int)
parser.add_argument('--seconds', default=40, type=int)
args = parser.parse_args()
out = ROOT / 'test-output' / 'benchmark'
media = out / 'media'
media.mkdir(parents=True, exist_ok=True)
ffmpeg = shutil.which('ffmpeg') or r'C:\ProgramData\chocolatey\bin\ffmpeg.exe'
fixture = media / '1080p-01.mp4'
if not fixture.exists():
    subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
                    'testsrc2=s=1920x1080:r=30:d=20', '-c:v', 'libx264', '-preset', 'ultrafast',
                    '-crf', '23', '-g', '60', '-threads', '8', '-y', str(fixture)], check=True)
for i in range(2,9):
    target = media / f'1080p-{i:02}.mp4'
    if not target.exists(): shutil.copyfile(fixture, target)
run = out / (args.label + '-' + time.strftime('%Y%m%d-%H%M%S'))
run.mkdir()
env = os.environ.copy()
env['PATH'] = str(ROOT / '.deps/Qt/6.8.3/msvc2022_64/bin') + os.pathsep + env['PATH']
with (run / 'app.log').open('w') as log:
    subprocess.run([os.environ.get("KALEIDOWALL_EXE", str(ROOT / "build/Release/Kaleidowall.exe")), '--data-dir', str(run),
                    '--library', str(media), '--benchmark', str(args.seconds), '--benchmark-fps', str(args.fps)],
                    env=env, stdout=log, stderr=log, check=True, timeout=args.seconds+60)
report = json.loads((run/'benchmark.json').read_text())
assert not report['final']['error'], report['final']['error']
summary = {}
for name in sorted({e['stage'] for e in report['events']}):
    values = sorted(e['ms'] for e in report['events'] if e['stage']==name and e['at']>=5)
    if not values: continue
    summary[name] = {'count':len(values), 'mean':sum(values)/len(values),
                     'p95':values[min(len(values)-1,int(len(values)*.95))], 'max':max(values),
                     'over50ms':sum(v>50 for v in values)}
print(json.dumps(summary,indent=2))
(run/'summary.json').write_text(json.dumps(summary,indent=2))
print('Result:',run,flush=True)
