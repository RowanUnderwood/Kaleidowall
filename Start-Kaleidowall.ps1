$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$candidates = @(
    (Join-Path $projectRoot 'build\Release\Kaleidowall.exe'),
    (Join-Path $projectRoot 'build\export\Release\Kaleidowall.exe')
)
$latestBuild = Get-Item -LiteralPath $candidates -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$latestBuild) { throw 'Build the player with scripts/build.ps1 first.' }
$exePath = $latestBuild.FullName
$env:PATH = "$projectRoot\.deps\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
Start-Process -FilePath $exePath -WorkingDirectory $projectRoot
