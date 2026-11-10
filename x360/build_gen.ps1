# Compiles the recompiled game code (pc-recomp/generated/game) for the Xbox 360 into x360\build\gen\*.obj.
# The code comes from your own default.xbe: never share the objects or anything linked from them.
#   powershell -ExecutionPolicy Bypass -File x360\build_gen.ps1 [-Debug]
param([switch]$Debug)
$ErrorActionPreference = 'Stop'
$X = $env:XEDK
if (-not $X) { throw 'XEDK is not set: install the Xbox 360 SDK' }
$env:PATH = "$X\bin\win32;$env:PATH"
$env:INCLUDE = "$X\include\xbox"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$gen = Resolve-Path (Join-Path $here '..\pc-recomp\generated\game')
$obj = Join-Path $here 'build\gen'
New-Item -ItemType Directory -Force $obj | Out-Null

$opt = if ($Debug) { '/Od','/Zi','/D_DEBUG' } else { '/O2','/Ou','/fp:fast','/GL','/DNDEBUG' }
# generated code: unused labels and variables, narrowing and unary minus on unsigned are expected there
$flags = @('/nologo','/c','/MP','/EHs','/bigobj','/GF','/Gy','/D_XBOX','/TP',
           '/wd4102','/wd4244','/wd4101','/wd4146','/wd4800',
           "/I$here\runtime", "/I$gen", "/Fo$obj\\") + $opt
$files = Get-ChildItem $gen -Filter *.cpp | ForEach-Object { $_.FullName }
# JSRF: arquivos em que /GL da erro interno na geracao de codigo do link (tools/ltcg_loop.py)
$skipFile = Join-Path $here 'tools\ltcg_skip.txt'
$noGl = @()
if (Test-Path $skipFile) { $noGl = Get-Content $skipFile | ForEach-Object { "x86_recomp.$($_.Trim()).cpp" } }
$plain = @($files | Where-Object { $noGl -contains (Split-Path $_ -Leaf) })
$whole = @($files | Where-Object { $noGl -notcontains (Split-Path $_ -Leaf) })
$flagsPlain = @($flags | Where-Object { $_ -ne '/GL' })
$t = Measure-Command {
    $out = & cl @flags $whole 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -and $plain.Count) { $out += & cl @flagsPlain $plain 2>&1 | Out-String }
}
$bad = $out -split "`n" | Where-Object { $_ -match ': (fatal )?error |: warning ' }
if ($bad) { $bad | Select-Object -First 40 | ForEach-Object { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { throw 'compile failed' }
"compiled $($files.Count) files in $([int]$t.TotalSeconds) s"
