param([switch]$Test, [switch]$Deploy, [string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $projectRoot
$releaseDirectory = Join-Path $BuildDirectory 'Release'
$qtPath = Join-Path $projectRoot '.deps\Qt\6.8.3\msvc2022_64'
if (!(Test-Path -LiteralPath "$qtPath\bin\Qt6Core.dll")) { throw 'Qt SDK missing. See README.md setup instructions.' }
cmake -S . -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$qtPath"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
cmake --build $BuildDirectory --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
$env:PATH = "$qtPath\bin;$env:PATH"
$mpvDll = Get-ChildItem -LiteralPath "$projectRoot\.deps\mpv" -Recurse -Filter '*mpv*.dll' | Select-Object -First 1
if (!$mpvDll) { throw 'libmpv DLL missing. Run scripts/dependencies.py first.' }
Copy-Item -LiteralPath $mpvDll.FullName -Destination (Join-Path $releaseDirectory 'libmpv-2.dll')
$dependencyLock = Get-Content -Raw -LiteralPath 'dependencies.lock.json' | ConvertFrom-Json
foreach ($toolName in @('ffmpeg.exe', 'ffprobe.exe')) {
    $toolPath = Join-Path $projectRoot ".deps\ffmpeg\$toolName"
    if (Test-Path -LiteralPath $toolPath) {
        $expectedHash = $dependencyLock.ffmpeg.files.$toolName
        if ((Get-FileHash -LiteralPath $toolPath -Algorithm SHA256).Hash -ne $expectedHash) {
            throw "Pinned $toolName checksum mismatch. See dependencies.lock.json."
        }
        Copy-Item -LiteralPath $toolPath -Destination (Join-Path $releaseDirectory $toolName)
    } elseif ($Deploy) {
        throw "Pinned $toolName missing. Run scripts/prepare_ffmpeg.py with the validated FFmpeg bin directory."
    }
}
if ($Test) {
    ctest --test-dir $BuildDirectory -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
}
if ($Deploy) {
    & "$qtPath\bin\windeployqt.exe" --release --no-translations --no-opengl-sw (Join-Path $releaseDirectory 'Kaleidowall.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed' }
}
