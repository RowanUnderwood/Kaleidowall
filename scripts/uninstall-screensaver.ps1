$ErrorActionPreference = 'Stop'
$installDirectory = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Kaleidowall\Screensaver'))
$expectedDirectory = [IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Kaleidowall\Screensaver'))
if ($installDirectory -ne $expectedDirectory) { throw 'Unexpected screensaver installation directory.' }
$installedSaver = Join-Path $installDirectory 'Kaleidowall.scr'
$desktopKey = 'HKCU:\Control Panel\Desktop'
$current = (Get-ItemProperty -LiteralPath $desktopKey).'SCRNSAVE.EXE'
if ($current -and $current.Trim().Trim('"').Trim() -eq $installedSaver) {
    $backupFile = Join-Path $installDirectory 'previous-screensaver.json'
    $previous = if (Test-Path -LiteralPath $backupFile) { Get-Content -Raw -LiteralPath $backupFile | ConvertFrom-Json } else { $null }
    if ($previous -and $previous.saver) {
        Set-ItemProperty -LiteralPath $desktopKey -Name 'SCRNSAVE.EXE' -Value $previous.saver
        if ($null -ne $previous.active) {
            Set-ItemProperty -LiteralPath $desktopKey -Name 'ScreenSaveActive' -Value $previous.active
        }
    } else {
        Remove-ItemProperty -LiteralPath $desktopKey -Name 'SCRNSAVE.EXE' -ErrorAction SilentlyContinue
        Set-ItemProperty -LiteralPath $desktopKey -Name 'ScreenSaveActive' -Value '0'
    }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class KaleidowallSaverSettings {
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool SystemParametersInfo(uint action, uint parameter, IntPtr value, uint flags);
}
'@
    $active = (Get-ItemProperty -LiteralPath $desktopKey).ScreenSaveActive -eq '1'
    if (![KaleidowallSaverSettings]::SystemParametersInfo(0x11, [uint32]$active, [IntPtr]::Zero, 3)) {
        throw 'Windows could not refresh the screensaver setting. Installation files have been retained.'
    }
}
if (Test-Path -LiteralPath $installDirectory) {
    $item = Get-Item -LiteralPath $installDirectory
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing to remove a redirected directory.' }
    Remove-Item -LiteralPath $installDirectory -Recurse -Force
}
# Reload the control panel to show the restored selection; the shared library data is elsewhere.
Start-Process -FilePath "$env:WINDIR\System32\rundll32.exe" -WindowStyle Hidden -ArgumentList 'desk.cpl,,1'
Write-Host 'Screensaver removed. Shared libraries, presets and player settings have been preserved.'
