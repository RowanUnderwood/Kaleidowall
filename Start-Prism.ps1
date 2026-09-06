$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$exePath = Join-Path $projectRoot 'build\Release\PrismPlayer.exe'
if (!(Test-Path -LiteralPath $exePath)) { throw 'Build the player with scripts/build.ps1 first.' }
$env:PATH = "$projectRoot\.deps\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
Start-Process -FilePath $exePath -WorkingDirectory $projectRoot
