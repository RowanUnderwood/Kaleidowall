"""Create synthetic codec fixtures, exercise the real UI, and check playback telemetry."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'test-output'
MEDIA = OUT / 'media'
MEDIA.mkdir(parents=True, exist_ok=True)
ffmpeg = shutil.which('ffmpeg') or r'C:\ProgramData\chocolatey\bin\ffmpeg.exe'
formats = [('H264', 'libx264', 'aac', 'mp4'), ('HEVC', 'libx265', 'aac', 'mkv'),
           ('MPEG4', 'mpeg4', 'mp3', 'avi'), ('MPEG2', 'mpeg2video', 'mp2', 'mpg'),
           ('VP9', 'libvpx-vp9', 'libopus', 'webm'), ('WMV2', 'wmv2', 'wmav2', 'wmv')]
for i, (name, video, audio, extension) in enumerate(formats):
    target = MEDIA / f'{name} - synthetic.{extension}'
    if target.exists():
        continue
    command = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
               'testsrc2=size=640x360:rate=30', '-f', 'lavfi', '-i',
               f'sine=frequency={220+i*110}:sample_rate=48000', '-t', '16',
               '-vf', f'hue=h={i*47}', '-c:v', video, '-threads', '4', '-c:a', audio,
               '-y', str(target)]
    subprocess.run(command, check=True, timeout=120)
    print('Generated', name, flush=True)

(MEDIA / 'Unreadable fixture.mp4').write_text('Deliberately invalid media for failure handling.')
short = MEDIA / 'Too short.mp4'
if not short.exists():
    subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
                    'color=c=blue:s=320x180:r=30:d=1', '-c:v', 'libx264', '-y', str(short)], check=True)

run_dir = OUT / ('run-' + time.strftime('%Y%m%d-%H%M%S'))
run_dir.mkdir()
env = os.environ.copy()
env['PATH'] = str(ROOT / '.deps/Qt/6.8.3/msvc2022_64/bin') + os.pathsep + env['PATH']
with (run_dir / 'app.log').open('w') as log:
    result = subprocess.run([str(ROOT / 'build/Release/PrismPlayer.exe'), '--data-dir',
                             str(run_dir), '--library', str(MEDIA), '--smoke', '25'],
                            env=env, stdout=log, stderr=log, timeout=70)
if result.returncode:
    raise RuntimeError(f'Player exited with {result.returncode}. See {run_dir}/app.log')
report = json.loads((run_dir / 'smoke.json').read_text())
samples = report['samples']
ready = [s for s in samples if any(p['ready'] for p in s['slots'])]
assert ready, f'No decoded frames: {run_dir}'
assert max(sum(p['ready'] for p in s['slots']) for s in ready) >= 2
assert len({s['layout'] for s in ready}) >= 2, 'Layouts did not change'
assert any(s['paused'] for s in samples), 'Pause was not exercised'
assert any(not s['paused'] for s in ready[8:]), 'Resume was not exercised'
for sample in samples:
    all_players = sample['slots'] + sample.get('idleSlots', [])
    assert sum(p['muted'] == 'no' for p in all_players) <= 1, 'Multiple audible sources'
    assert len(all_players) <= 8, 'Player pool grew beyond two decoders per maximum segment'
    assert all(p['paused'] == 'yes' and p['muted'] == 'yes'
               for p in sample['idleSlots'] if p['prepared']), 'Prepared clip is playing or audible'
    titles = [p['title'] for p in sample['slots'] if p['ready'] and not p['retiring']]
    assert len(titles) == len(set(titles)), 'Duplicate active video'
    for p in sample['slots']:
        if p['ready']:
            assert p['start'] >= 1 and p['start'] + p['length'] <= 15.2, 'Clip crossed exclusion'
assert not report['final']['error'], report['final']['error']
assert report['final']['eligible'] == 6, 'Invalid and short files should be excluded'
assert len(report['final']['slots']) == 1, 'Single-video mode did not retire other slots'
assert report['final']['idleSlots'], 'Retired players were not retained for reuse'
assert report['final']['cleanCuts'] >= 3, 'Prepared clips were not swapped into visible segments'
assert any(p['muted'] == 'no' for s in ready for p in s['slots']), 'Audio was never enabled'
paused_samples = [s for s in samples if s['paused']]
assert len(paused_samples) >= 2
assert max(s['sessionTime'] for s in paused_samples) - min(s['sessionTime'] for s in paused_samples) < 0.05
assert any(a['layout'] != b['layout'] and any(
    pa['slot'] == pb['slot'] and pa['title'] == pb['title'] and pa['start'] == pb['start']
    for pa in a['slots'] for pb in b['slots'] if pa['ready'] and pb['ready'])
    for a, b in zip(samples, samples[1:])), 'No clip survived a layout change'
print('PASS: concurrent playback, independent layouts, pause/resume, safe exclusions, unique videos, exclusive audio, invalid files, single-video mode.')
print('Results:', run_dir)
print('Renderer:', report['final']['renderer'])
print('FPS:', report['final']['fps'])
