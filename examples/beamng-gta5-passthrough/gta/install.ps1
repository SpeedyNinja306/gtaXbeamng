<#
.SYNOPSIS
  Installs GTAxBeam into GTA V Legacy (story mode): ScriptHookV + its ASI loader, GTAxBeam.asi, GTAxBeam.ini,
  args.txt (-nobattleye, so GTA Online can't start), and ReShade with the GTAxBeam.fx compositor effect. ReShade goes
  in as ReShade64.asi, loaded by the ASI loader (GTA loads the system dxgi.dll, so a ReShade dxgi.dll never runs).
  Only adds files; -Remove deletes exactly those.
  A dinput8.dll, ReShade64.asi or args.txt that some other mod (or you) put there is left alone unless -Force.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File gta\install.ps1
  powershell -ExecutionPolicy Bypass -File gta\install.ps1 -Remove
#>
param(
  [string]$GtaDir = $env:GTA_DIR,
  [switch]$Remove,
  [switch]$Force
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$runtime = Join-Path $here 'third_party\runtime'
$build = Join-Path $here 'build'
$argsText = '-nobattleye -noBE'
$files = 'ScriptHookV.dll', 'dinput8.dll', 'args.txt', 'GTAxBeam.asi', 'GTAxBeam.ini', 'ReShade64.asi', 'ReShade.ini',
  'ReShadePreset.ini', 'ReShade.log', 'reshade-shaders\Shaders\GTAxBeam.fx', 'reshade-shaders\Shaders\ReShade.fxh',
  'reshade-shaders\Shaders\ReShadeUI.fxh'

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

if (-not $GtaDir) { $GtaDir = Find-SteamApp '271590' }
if (-not $GtaDir -or -not (Test-Path (Join-Path $GtaDir 'GTA5.exe'))) {
  throw "GTA V Legacy (GTA5.exe) not found; pass -GtaDir or set GTA_DIR"
}

if ($Remove) {
  foreach ($f in $files) {
    $p = Join-Path $GtaDir $f
    if (Test-Path $p) { Remove-Item -Force $p; Write-Host "removed $p" }
  }
  foreach ($d in 'reshade-shaders\Shaders', 'reshade-shaders') {
    $p = Join-Path $GtaDir $d
    if ((Test-Path $p) -and -not (Get-ChildItem $p)) { Remove-Item $p }
  }
  return
}

if (-not (Test-Path (Join-Path $runtime 'ScriptHookV.dll'))) { throw "fetch the runtime first: gta\fetch_deps.ps1" }
if (-not (Test-Path (Join-Path $build 'GTAxBeam.asi'))) { throw "build it first: gta\build.bat" }

$clash = @()
$d8 = Join-Path $GtaDir 'dinput8.dll'
if ((Test-Path $d8) -and ((Get-FileHash $d8).Hash -ne (Get-FileHash (Join-Path $runtime 'dinput8.dll')).Hash)) { $clash += 'dinput8.dll' }
$at = Join-Path $GtaDir 'args.txt'
if ((Test-Path $at) -and ((Get-Content $at -Raw).Trim() -ne $argsText)) { $clash += 'args.txt' }
$rs = Join-Path $GtaDir 'ReShade64.asi'
if ((Test-Path $rs) -and ((Get-FileHash $rs).Hash -ne (Get-FileHash (Join-Path $runtime 'ReShade64.dll')).Hash)) { $clash += 'ReShade64.asi' }
if ($clash -and -not $Force) { throw "not replacing what is already in ${GtaDir}: $($clash -join ', ') (-Force to replace)" }

Copy-Item (Join-Path $runtime 'ScriptHookV.dll'), (Join-Path $runtime 'dinput8.dll'), (Join-Path $build 'GTAxBeam.asi') $GtaDir -Force
Set-Content -NoNewline -Path $at -Value $argsText
$ini = Join-Path $GtaDir 'GTAxBeam.ini'
if (-not (Test-Path $ini)) { Copy-Item (Join-Path $here 'GTAxBeam.ini') $ini }

Copy-Item (Join-Path $runtime 'ReShade64.dll') $rs -Force
$shaderDir = Join-Path $GtaDir 'reshade-shaders\Shaders'
New-Item -ItemType Directory -Force $shaderDir | Out-Null
Copy-Item (Join-Path $here 'shaders\GTAxBeam.fx'), (Join-Path $here 'third_party\shaders\ReShade.fxh'),
  (Join-Path $here 'third_party\shaders\ReShadeUI.fxh') $shaderDir -Force
$rsIni = Join-Path $GtaDir 'ReShade.ini'
if (-not (Test-Path $rsIni)) {
  # GTA's depth: reversed Z. The tutorial is marked done so ReShade's first-run overlay doesn't cover the game.
  Set-Content -Path $rsIni -Value @"
[GENERAL]
EffectSearchPaths=.\reshade-shaders\Shaders\
TextureSearchPaths=.\reshade-shaders\Textures\
PresetPath=.\ReShadePreset.ini
PreprocessorDefinitions=RESHADE_DEPTH_INPUT_IS_REVERSED=1,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0,RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0,RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000

[OVERLAY]
TutorialProgress=4
ShowClock=0
ShowFPS=0
"@
}
# the technique stays on; the add-on's BngActive uniform keeps it a pure passthrough until BeamNG frames arrive
Set-Content -Path (Join-Path $GtaDir 'ReShadePreset.ini') -Value "Techniques=GTAxBeam@GTAxBeam.fx`r`nTechniqueSorting=GTAxBeam@GTAxBeam.fx"
Write-Host "installed into $GtaDir"
