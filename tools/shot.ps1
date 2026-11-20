param([string]$out="tools/shot.png")
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$p = Get-Process oblivion -ErrorAction SilentlyContinue | Select-Object -First 1
if(-not $p){ Write-Host "not running"; exit 1 }
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; } }
"@
[W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 500
$r = New-Object W+RECT; [W]::GetWindowRect($p.MainWindowHandle,[ref]$r) | Out-Null
$w=$r.R-$r.L; $h=$r.B-$r.T
Write-Host "window $($r.L),$($r.T) ${w}x${h} title=$($p.MainWindowTitle)"
$bmp = New-Object System.Drawing.Bitmap $w,$h
$g=[System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L,$r.T,0,0,$bmp.Size)
$bmp.Save($out); 
