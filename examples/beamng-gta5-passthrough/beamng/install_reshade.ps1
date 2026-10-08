<#
.SYNOPSIS
  Installs the compositor's BeamNG half into BeamNG.drive's Bin64: ReShade (as dxgi.dll: BeamNG 0.39 renders with
  DirectX 12 only, -gfx d3d11 finds no adapter), the GTAxBeamExport.addon64 add-on and its effect.
  BeamNG warns about the dxgi.dll at startup (Cancel stops the warning).
  Only adds files; -Remove deletes exactly those. A dxgi.dll that something else put there is left alone unless -Force.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File beamng\install_reshade.ps1
  powershell -ExecutionPolicy Bypass -File beamng\install_reshade.ps1 -Remove
#>
param(
  [string]$BeamDir = $env:BEAMNG_DIR,
  [switch]$Remove,
  [switch]$Force
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$runtime = Join-Path $here '..\gta\third_party\runtime'
$shaders = Join-Path $here '..\gta\third_party\shaders'
$addon = Join-Path $here 'exporter\build\GTAxBeamExport.addon64'
$files = 'dxgi.dll', 'GTAxBeamExport.addon64', 'ReShade.ini', 'ReShadePreset.ini', 'ReShade.log',
  'reshade-shaders\Shaders\GTAxBeamExport.fx', 'reshade-shaders\Shaders\ReShade.fxh', 'reshade-shaders\Shaders\ReShadeUI.fxh'

function Find-SteamApp([string]$appId) {
  $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
  $libs = @()
  if ($steam) {
    $libs += $steam
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
      $libs += [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"') | ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
    }
  }
  foreach ($lib in $libs | Select-Object -Unique) {
    $acf = Join-Path $lib "steamapps\appmanifest_$appId.acf"
    if (Test-Path $acf) {
      $dir = [regex]::Match((Get-Content $acf -Raw), '"installdir"\s+"([^"]+)"').Groups[1].Value
      return Join-Path $lib "steamapps\common\$dir"
    }
  }
}

if (-not $BeamDir) { $BeamDir = Find-SteamApp '284160' }
$bin = Join-Path $BeamDir 'Bin64'
if (-not $BeamDir -or -not (Test-Path (Join-Path $bin 'BeamNG.drive.x64.exe'))) {
  throw "BeamNG.drive (Bin64\BeamNG.drive.x64.exe) not found; pass -BeamDir or set BEAMNG_DIR"
}

if ($Remove) {
  foreach ($f in $files) {
    $p = Join-Path $bin $f
    if (Test-Path $p) { Remove-Item -Force $p; Write-Host "removed $p" }
  }
  foreach ($d in 'reshade-shaders\Shaders', 'reshade-shaders') {
    $p = Join-Path $bin $d
    if ((Test-Path $p) -and -not (Get-ChildItem $p)) { Remove-Item $p }
  }
  return
}

if (-not (Test-Path (Join-Path $runtime 'ReShade64.dll'))) { throw "fetch ReShade first: gta\fetch_deps.ps1" }
if (-not (Test-Path $addon)) { throw "build the add-on first: beamng\exporter\build.bat" }
$d3d = Join-Path $bin 'dxgi.dll'
if ((Test-Path $d3d) -and ((Get-FileHash $d3d).Hash -ne (Get-FileHash (Join-Path $runtime 'ReShade64.dll')).Hash) -and -not $Force) {
  throw "not replacing the dxgi.dll already in $bin (-Force to replace)"
}

Copy-Item (Join-Path $runtime 'ReShade64.dll') $d3d -Force
Copy-Item $addon $bin -Force
$shaderDir = Join-Path $bin 'reshade-shaders\Shaders'
New-Item -ItemType Directory -Force $shaderDir | Out-Null
Copy-Item (Join-Path $here 'exporter\GTAxBeamExport.fx'), (Join-Path $shaders 'ReShade.fxh'), (Join-Path $shaders 'ReShadeUI.fxh') $shaderDir -Force
$rsIni = Join-Path $bin 'ReShade.ini'
if (-not (Test-Path $rsIni)) {
  Set-Content -Path $rsIni -Value @"
[GENERAL]
EffectSearchPaths=.\reshade-shaders\Shaders\
TextureSearchPaths=.\reshade-shaders\Textures\
PresetPath=.\ReShadePreset.ini

[OVERLAY]
TutorialProgress=4
ShowClock=0
ShowFPS=0
"@
}
Set-Content -Path (Join-Path $bin 'ReShadePreset.ini') -Value "Techniques=GTAxBeamExport@GTAxBeamExport.fx`r`nTechniqueSorting=GTAxBeamExport@GTAxBeamExport.fx"
Write-Host "installed into $bin"
