# Run a ROM in BigPEmuDev and photograph its WINDOW (PrintWindow, never the screen) at the
# given seconds; then close it.  From JagLBA tools/emu_run.ps1.
#   powershell -File jaguar\emu_shot.ps1 -Rom build\jag\megadriv_sonic\megadriv_sonic.j64 -Shots "3,10" -Tag sonic
param(
    [Parameter(Mandatory = $true)][string]$Rom,
    [string]$Shots = "5",
    [string]$Tag = "shot",
    [string]$Emu = "D:\Homebrew6\Jaguar-KeenDreams\BigPEmuDEV\BigPEmuDev.exe"
)
$ErrorActionPreference = "Stop"
$romPath = (Resolve-Path $Rom).Path
$outDir = Split-Path -Parent $romPath
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PW {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    public struct R { public int L, T, Rt, B; }
}
"@
Get-Process BigPEmuDev -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600
$p = Start-Process -FilePath $Emu -ArgumentList "`"$romPath`"" -PassThru
$start = Get-Date
try {
    foreach ($s in ($Shots.Split(",") | ForEach-Object { [double]$_.Trim() })) {
        while (((Get-Date) - $start).TotalSeconds -lt $s) { Start-Sleep -Milliseconds 100 }
        $p.Refresh()
        $h = $p.MainWindowHandle
        if ($h -eq [IntPtr]::Zero) { throw "no BigPEmu window" }
        $r = New-Object PW+R
        $tries = 0
        do {
            [void][PW]::GetClientRect($h, [ref]$r)
            if ($r.Rt -gt 0 -and $r.B -gt 0) { break }
            Start-Sleep -Milliseconds 250; $p.Refresh(); $h = $p.MainWindowHandle
        } while (++$tries -lt 40)
        $bmp = New-Object System.Drawing.Bitmap($r.Rt, $r.B)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        $ok = [PW]::PrintWindow($h, $hdc, 3)
        $g.ReleaseHdc($hdc)
        $out = Join-Path $outDir ("{0}_{1}s.png" -f $Tag, $s)
        if ($ok) { $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png) }
        $g.Dispose(); $bmp.Dispose()
        Write-Host "shot $s s -> $out"
    }
} finally {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
