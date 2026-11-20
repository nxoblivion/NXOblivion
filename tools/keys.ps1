# Usage: keys.ps1 <process> <out.png> <keys...>  -- focus window, send keys (SendKeys syntax, "wait:N" sleeps N ms), then screenshot
param([string]$proc, [string]$out, [Parameter(ValueFromRemainingArguments=$true)][string[]]$keys)
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W2 { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; } }
"@
$p = Get-Process $proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if(-not $p){ Write-Host "not running"; exit 1 }
# Tap Alt first: Windows only lets a process that just received input steal focus.
[W2]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [W2]::keybd_event(0x12,0,2,[UIntPtr]::Zero)
[W2]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 400
foreach($k in $keys){
  if($k -like "wait:*"){ Start-Sleep -Milliseconds ([int]$k.Substring(5)); continue }
  # vk:<hex> = hold a virtual key 150ms (games poll state, SendKeys taps are too short)
  if($k -like "vk:*"){ [W2]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [W2]::keybd_event(0x12,0,2,[UIntPtr]::Zero); [W2]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 200; Write-Host ("key {0} fg={1} target={2}" -f $k, [W2]::GetForegroundWindow(), $p.MainWindowHandle); $vk=[Convert]::ToByte($k.Substring(3),16); $sc=[byte]@{0x0D=0x1C;0x20=0x39;0x26=0x48;0x28=0x50;0x1B=0x01;0x41=0x1E;0x4D=0x32;0x53=0x1F;0x57=0x11}[[int]$vk]; [W2]::keybd_event($vk,$sc,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 150; [W2]::keybd_event($vk,$sc,2,[UIntPtr]::Zero); Start-Sleep -Milliseconds 300; continue }
  [System.Windows.Forms.SendKeys]::SendWait($k); Start-Sleep -Milliseconds 300
}
$r = New-Object W2+RECT; [W2]::GetWindowRect($p.MainWindowHandle,[ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R-$r.L),($r.B-$r.T)
[System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.L,$r.T,0,0,$bmp.Size); $bmp.Save($out)
Write-Host "saved $out"
