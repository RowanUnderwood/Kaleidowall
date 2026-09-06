$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $projectRoot
if (!(Test-Path -LiteralPath '.venv\Scripts\python.exe')) {
    python -m venv .venv
    if ($LASTEXITCODE -ne 0) { throw 'Python environment creation failed' }
}
& .\.venv\Scripts\python.exe -m pip install aqtinstall==3.3.0
if ($LASTEXITCODE -ne 0) { throw 'Dependency installer setup failed' }
if (!(Test-Path -LiteralPath '.deps\Qt\6.8.3\msvc2022_64\bin\Qt6Core.dll')) {
    & .\.venv\Scripts\python.exe -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O .deps/Qt --archives qtbase qttools
    if ($LASTEXITCODE -ne 0) { throw 'Qt installation failed' }
}
& .\.venv\Scripts\python.exe scripts/dependencies.py
if ($LASTEXITCODE -ne 0) { throw 'libmpv setup failed' }
& .\.venv\Scripts\python.exe scripts/prepare_ffmpeg.py
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg preparation failed. See README.md export dependencies.' }
& .\scripts\build.ps1 -Test -Deploy
