param([string]$BuildDirectory = 'build', [switch]$NoOpenSettings)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$sourceDirectory = Join-Path (Join-Path $projectRoot $BuildDirectory) 'Release'
$installDirectory = Join-Path $env:LOCALAPPDATA 'Kaleidowall\Screensaver'
$installedSaver = Join-Path $installDirectory 'Kaleidowall.scr'
foreach ($file in @('Kaleidowall.scr', 'libmpv-2.dll', 'Qt6Core.dll', 'platforms\qwindows.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $sourceDirectory $file))) {
        throw "Missing $file. Run scripts/build.ps1 -Test -Deploy first."
    }
}
New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
$backupFile = Join-Path $installDirectory 'previous-screensaver.json'
$desktopKey = 'HKCU:\Control Panel\Desktop'
if (!(Test-Path -LiteralPath $backupFile)) {
    $previous = Get-ItemProperty -LiteralPath $desktopKey
    @{ saver = $previous.'SCRNSAVE.EXE'; active = $previous.ScreenSaveActive } |
        ConvertTo-Json | Set-Content -LiteralPath $backupFile -Encoding UTF8
}
foreach ($file in Get-ChildItem -LiteralPath $sourceDirectory -File) {
    if ($file.Extension -eq '.dll' -or $file.Name -in @('Kaleidowall.scr', 'Kaleidowall.exe', 'ffmpeg.exe', 'ffprobe.exe')) {
        Copy-Item -LiteralPath $file.FullName -Destination $installDirectory -Force
    }
}
foreach ($folder in @('platforms', 'styles', 'imageformats', 'iconengines', 'sqldrivers', 'tls', 'networkinformation', 'generic')) {
    $sourceFolder = Join-Path $sourceDirectory $folder
    if (Test-Path -LiteralPath $sourceFolder) {
        Copy-Item -LiteralPath $sourceFolder -Destination $installDirectory -Recurse -Force
    }
}
# InstallScreenSaver treats its whole raw argument as a filename and can persist literal quotes
# and a trailing space. SCRNSAVE.EXE must contain the actual path, not a quoted command line.
Set-ItemProperty -LiteralPath $desktopKey -Name 'SCRNSAVE.EXE' -Value $installedSaver -Type String
if ((Get-ItemProperty -LiteralPath $desktopKey).'SCRNSAVE.EXE' -cne $installedSaver) {
    throw 'Windows did not retain the exact screensaver path.'
}
if (!('KaleidowallSaverInstaller' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class KaleidowallSaverInstaller {
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool SystemParametersInfo(uint action, uint parameter, IntPtr value, uint flags);
}
'@
}
if (![KaleidowallSaverInstaller]::SystemParametersInfo(0x11, 1, [IntPtr]::Zero, 3)) {
    throw 'Windows could not activate the screensaver. The files were copied, but registration is incomplete.'
}
# This is the interactive settings window promised by the installer, not a background helper.
if (!$NoOpenSettings) {
    Start-Process -FilePath "$env:WINDIR\System32\control.exe" -WindowStyle Normal `
        -ArgumentList 'desk.cpl,screensaver,@screensaver'
}
Write-Host 'Kaleidowall installed for this user. Set the wait time and sign-in preference in Windows Screen Saver Settings.'
