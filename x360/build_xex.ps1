# Links the game XEX (x360\build\default.xex) from the compiled runtime (build_runtime.ps1) and the compiled game
# code (build_gen.ps1). The XEX holds code recompiled from your own default.xbe: never share it.
#   powershell -ExecutionPolicy Bypass -File x360\build_xex.ps1 [-Debug]
param([switch]$Debug)
$ErrorActionPreference = 'Stop'
$X = $env:XEDK
if (-not $X) { throw 'XEDK is not set: install the Xbox 360 SDK' }
$env:PATH = "$X\bin\win32;$env:PATH"
$env:LIB = "$X\lib\xbox"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $here 'build'
$objs = @(Get-ChildItem "$out\rt" -Filter *.obj | ForEach-Object { $_.FullName }) +
        @(Get-ChildItem "$out\gen" -Filter *.obj | ForEach-Object { $_.FullName })
$libs = if ($Debug) { 'xapilibd.lib','d3d9d.lib','d3dx9d.lib','xgraphicsd.lib','xaudiod2.lib','xmcored.lib','xinput2d.lib','xboxkrnl.lib' }
        else { 'xapilib.lib','d3d9.lib','d3dx9.lib','xgraphics.lib','xaudio2.lib','xmcore.lib','xinput2.lib','xboxkrnl.lib' }
& link /nologo /LTCG /DEBUG /OPT:REF /OPT:ICF /OUT:"$out\bre360.exe" /PDB:"$out\bre360.pdb" @objs @libs 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
& imagexex /nologo /IN:"$out\bre360.exe" /OUT:"$out\default.xex"
if ($LASTEXITCODE -ne 0) { throw 'imagexex failed' }
Get-Item "$out\default.xex" | Select-Object FullName, Length
