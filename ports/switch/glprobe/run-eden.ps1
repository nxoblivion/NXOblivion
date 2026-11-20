# Runs glprobe.nro in Eden, waits for PROBE_DONE, screenshots the Eden window, saves results, kills Eden.
# Usage (from repo root): powershell -File ports/switch/glprobe/run-eden.ps1
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$eden = "$root\tools\eden\app"
$sd = "$eden\user\sdmc\glprobe.txt"
Remove-Item $sd -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W2 { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f); [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; } }
"@
$p = Start-Process "$eden\eden-cli.exe" -ArgumentList '-g',"$root/ports/switch/glprobe/glprobe.nro" -PassThru
$ok = $false
for ($i = 0; $i -lt 120; $i++) {
  Start-Sleep 1
  if ((Test-Path $sd) -and (Select-String -Path $sd -Pattern 'rendered 120 frames' -Quiet)) { $ok = $true; break }
}
Start-Sleep 3
$shot = "$PSScriptRoot\screenshot-eden.png"
$proc = Get-Process eden-cli -ErrorAction SilentlyContinue | Select-Object -First 1
if ($proc) {
  [W2]::ShowWindow($proc.MainWindowHandle, 9) | Out-Null
  [W2]::SetWindowPos($proc.MainWindowHandle, [IntPtr](-1), 50, 50, 1296, 759, 0x0040) | Out-Null  # HWND_TOPMOST so nothing covers it
  [W2]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
  Start-Sleep -Milliseconds 2500
  $r = New-Object W2+RECT; [W2]::GetWindowRect($proc.MainWindowHandle, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $h = $r.B - $r.T
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
  $bmp.Save($shot); $g.Dispose(); $bmp.Dispose()
  Write-Host "screenshot $shot window ${w}x${h} title=$($proc.MainWindowTitle)"
}
Get-Process eden* -ErrorAction SilentlyContinue | Stop-Process -Force
Copy-Item $sd "$PSScriptRoot\results-eden.txt" -ErrorAction SilentlyContinue
"`n`n=== Eden log excerpt (GPU/nvdrv/EGL related) ===" | Add-Content "$PSScriptRoot\results-eden.txt"
Select-String -Path "$eden\user\log\eden_log.txt" -Pattern '<(Error|Warning|Critical)>' | Where-Object { $_.Line -notmatch 'STUBBED|Filesystem|Network|RomFS' } |
  Select-Object -First 60 | ForEach-Object { $_.Line } | Add-Content "$PSScriptRoot\results-eden.txt"
Write-Host "finished=$ok"
