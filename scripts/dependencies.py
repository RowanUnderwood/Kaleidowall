"""Download the Windows libmpv runtime and matching public headers locally."""
import hashlib
import json
from pathlib import Path
import urllib.request
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / '.deps' / 'mpv'
DEST.mkdir(parents=True, exist_ok=True)

def fetch(url):
    req = urllib.request.Request(url, headers={'User-Agent': 'PrismPlayer-dev-setup'})
    with urllib.request.urlopen(req, timeout=120) as response:
        return response.read()

asset = json.loads((ROOT / 'dependencies.lock.json').read_text())['mpv']
archive = DEST / asset['asset']
if not archive.exists():
    archive.write_bytes(fetch(asset['url']))
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
if asset['sha256'] != digest:
    raise RuntimeError('Pinned libmpv asset checksum mismatch')
subprocess.run(['tar', '-xf', str(archive), '-C', str(DEST)], check=True)
headers = DEST / 'include' / 'mpv'
headers.mkdir(parents=True, exist_ok=True)
for name in ['client.h', 'render.h', 'render_gl.h']:
    if not (headers / name).exists():
        raise RuntimeError('Archive is missing expected header: ' + name)
(DEST / 'download-manifest.json').write_text(json.dumps(asset, indent=2))
print('libmpv ready:', asset['asset'], digest)
