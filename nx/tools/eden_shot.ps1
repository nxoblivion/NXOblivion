# Captures the window of one process (the Eden that tools/run_eden.sh started: its pid.txt) to a PNG, without
# bringing it to the front (PrintWindow), so a window the user is looking at is never covered.
# Usage: powershell -File nx/tools/eden_shot.ps1 -ProcessId <pid> -Out <file.png>
param([int]$ProcessId, [string]$Out)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class EdenShot {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$p = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
if (-not $p -or $p.MainWindowHandle -eq [IntPtr]::Zero) { Write-Host "no window for $ProcessId"; exit 1 }
$r = New-Object EdenShot+RECT
[EdenShot]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
# 2 = PW_RENDERFULLCONTENT (OpenGL / DirectX surfaces)
[EdenShot]::PrintWindow($p.MainWindowHandle, $dc, 2) | Out-Null
$g.ReleaseHdc($dc)
$bmp.Save($Out)
Write-Host "window ${w}x${h} title=$($p.MainWindowTitle) -> $Out"
