"""Verify the rendered video's vertical orientation, including the libmpv/FBO path."""
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'test-output' / 'orientation'
MEDIA = OUT / 'media'
MEDIA.mkdir(parents=True, exist_ok=True)
ffmpeg = shutil.which('ffmpeg') or r'C:\ProgramData\chocolatey\bin\ffmpeg.exe'
font = OUT / 'arial.ttf'
if not font.exists():
    shutil.copyfile(Path(os.environ['WINDIR']) / 'Fonts' / 'arial.ttf', font)
font_filter = font.relative_to(ROOT).as_posix()
fixture = MEDIA / 'Orientation.mp4'
subprocess.run([
    ffmpeg, '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
    'color=c=0xe83c43:s=640x360:r=30:d=16', '-vf',
    'drawbox=x=0:y=180:w=640:h=180:color=0x285ac4:t=fill,'
    f'drawtext=fontfile={font_filter}:text=TOP:fontsize=48:fontcolor=white:x=(w-tw)/2:y=40,'
    f'drawtext=fontfile={font_filter}:text=BOTTOM:fontsize=48:fontcolor=white:x=(w-tw)/2:y=260',
    '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-y', str(fixture)
], cwd=ROOT, check=True, timeout=60)

run = OUT / time.strftime('%Y%m%d-%H%M%S')
run.mkdir()
env = os.environ.copy()
env['PATH'] = str(ROOT / '.deps/Qt/6.8.3/msvc2022_64/bin') + os.pathsep + env['PATH']
with (run / 'app.log').open('w') as log:
    subprocess.run([
        str(ROOT / 'build/Release/Kaleidowall.exe'), '--data-dir', str(run),
        '--library', str(MEDIA), '--smoke', '12'
    ], env=env, stdout=log, stderr=log, check=True, timeout=45)
pixels = subprocess.check_output([
    ffmpeg, '-hide_banner', '-loglevel', 'error', '-i', str(run / 'smoke-frame.png'),
    '-vf', 'scale=1:4:flags=area', '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'
])
top, bottom = tuple(pixels[:3]), tuple(pixels[-3:])
print('Top RGB:', top, 'Bottom RGB:', bottom, flush=True)
assert top[0] > top[2] + 70, f'Top of video is not red: {run}'
assert bottom[2] > bottom[0] + 70, f'Bottom of video is not blue: {run}'
print('PASS: video is upright. Screenshot:', run / 'smoke-frame.png')
