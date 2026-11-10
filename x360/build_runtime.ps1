# Compiles the Xbox 360 runtime (x360\runtime) into x360\build\rt\*.obj, one file at a time so each file's
# errors are listed apart.   powershell -ExecutionPolicy Bypass -File x360\build_runtime.ps1 [-Only name] [-Errors n]
param([string]$Only, [int]$Errors = 12, [switch]$Debug)
$X = $env:XEDK
if (-not $X) { throw 'XEDK is not set: install the Xbox 360 SDK' }
$env:PATH = "$X\bin\win32;$env:PATH"
$env:INCLUDE = "$X\include\xbox"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$gen = Resolve-Path (Join-Path $here '..\pc-recomp\generated\game')
$obj = Join-Path $here 'build\rt'
New-Item -ItemType Directory -Force $obj | Out-Null
$opt = if ($Debug) { '/Od','/Zi','/D_DEBUG' } else { '/O2','/Ou','/Zi','/DNDEBUG' }
$flags = @('/nologo','/c','/EHs','/GF','/Gy','/W3','/wd4800','/wd4996','/wd4482','/wd4351','/D_XBOX','/TP','/MT',
           "/I$here\shim", "/I$here\runtime", "/I$gen", "/Fd$obj\vc.pdb") + $opt
$failed = 0
# a header newer than an object means that object was made from another layout (v22: State grew, texture/shaders/vertex
# kept the old offsets): with -Only, those are compiled too
$newest = (Get-ChildItem "$here\runtime", "$here\shim" -Recurse -Filter *.h | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime
function Stale($f) { $o = Join-Path $obj "$($f.BaseName).obj"; -not (Test-Path $o) -or (Get-Item $o).LastWriteTime -lt $newest }
Get-ChildItem "$here\runtime" -Recurse -Filter *.cpp | Where-Object { -not $Only -or $_.BaseName -eq $Only -or (Stale $_) } | ForEach-Object {
    $out = & cl @flags /Fo"$obj\$($_.BaseName).obj" $_.FullName 2>&1 | Out-String
    $bad = $out -split "`n" | Where-Object { $_ -match ': (fatal )?error ' }
    if ($LASTEXITCODE -ne 0) {
        $failed++
        Write-Host "== $($_.Name): $($bad.Count) errors"
        $bad | Select-Object -First $Errors | ForEach-Object { Write-Host ('   ' + (($_.Trim()) -replace '^.*\\runtime\\','')) }
    } else { Write-Host "ok $($_.Name)" }
}
if ($failed) { exit 1 }
