# Captures the client area of the running wg3d.exe window (or -Process <name>, e.g. ares) to build\<Name>.png.
# Uses PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT), which reads the window's own (DWM-composed)
# content, so it works while other windows cover it and never steals focus.
# Use alongside tools\run_wg3d.ps1 (started in the background), e.g.
#   tools\run_wg3d.ps1 -Seconds 60 -Name run5     (background)
#   tools\shot_wg3d.ps1 -Name title                (whenever a capture is wanted)
# -Count N -IntervalMs M takes a series: build\<Name>_<i>.png, one every M ms.
param([string]$Name = "shot", [string]$Process = "wg3d", [int]$Count = 1, [int]$IntervalMs = 1000)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class WinCap {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[WinCap]::SetProcessDPIAware() | Out-Null
$root = Split-Path -Parent $PSScriptRoot
$p = Get-Process $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
if (-not $p) { "[shot] no $Process window"; exit 1 }
$h = $p.MainWindowHandle
$t0 = Get-Date
for ($k = 0; $k -lt $Count; $k++) {
    $due = $t0.AddMilliseconds($k * $IntervalMs)
    $wait = ($due - (Get-Date)).TotalMilliseconds
    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]$wait) }
    $r = New-Object WinCap+RECT; [WinCap]::GetClientRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hgt = $r.B - $r.T
    if ($w -le 0 -or $hgt -le 0) { "[shot] empty client rect"; continue }
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [WinCap]::PrintWindow($h, $hdc, 3) | Out-Null  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc)
    $file = if ($Count -eq 1) { Join-Path $root "build\$Name.png" } else { Join-Path $root ("build\{0}_{1:D3}.png" -f $Name, $k) }
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    "[shot] $file (${w}x$hgt)"
}
