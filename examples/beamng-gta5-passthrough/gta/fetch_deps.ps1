<#
.SYNOPSIS
  Fetches what the GTA side needs into third_party\ (none of it may be redistributed, so it isn't in the repo):
    shv\      ScriptHookV SDK: main.h, nativeCaller.h, types.h, ScriptHookV.lib (dev-c.com; needs browser headers)
    runtime\  ScriptHookV.dll and its ASI loader dinput8.dll, for install.ps1; ReShade64.dll (ReShade with add-on
              support), which both games get: GTA as ReShade64.asi, BeamNG as Bin64\d3d11.dll
    reshade\  ReShade's add-on API headers (crosire/reshade at the same version), for the compositor and the exporter
    shaders\  ReShade.fxh and ReShadeUI.fxh, which the effects include (crosire/reshade-shaders, slim branch)
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File gta\fetch_deps.ps1
#>
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$here = $PSScriptRoot
$shv = Join-Path $here 'third_party\shv'
$runtime = Join-Path $here 'third_party\runtime'
New-Item -ItemType Directory -Force $shv, $runtime | Out-Null
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('gtaxbeam_' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $tmp | Out-Null

$ua = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36'
$base = 'https://www.dev-c.com/gtav/scripthookv/'
try {
  $page = Invoke-WebRequest -UseBasicParsing -UserAgent $ua -Headers @{ 'Accept' = 'text/html'; 'Accept-Language' = 'en-US' } $base
  $links = [regex]::Matches($page.Content, '/files/ScriptHookV_[^"]*\.zip') | ForEach-Object { $_.Value } | Sort-Object -Unique
  if (-not $links) { throw "no ScriptHookV downloads found: get the SDK and ScriptHookV from $base" }
  foreach ($l in $links) {
    $out = Join-Path $tmp (Split-Path $l -Leaf)
    Invoke-WebRequest -UseBasicParsing -UserAgent $ua -Headers @{ 'Referer' = $base } ("https://www.dev-c.com" + $l) -OutFile $out
    Write-Host "downloaded $(Split-Path $l -Leaf)"
  }
  $sdk = Get-ChildItem $tmp -Filter 'ScriptHookV_SDK_*.zip' | Select-Object -First 1
  $rt = Get-ChildItem $tmp -Filter 'ScriptHookV_*.zip' | Where-Object { $_.Name -notlike '*SDK*' } | Select-Object -First 1
  if (-not $sdk -or -not $rt) { throw "expected both ScriptHookV and its SDK on $base" }
  Expand-Archive -Force $sdk.FullName (Join-Path $tmp 'sdk')
  Expand-Archive -Force $rt.FullName (Join-Path $tmp 'rt')
  foreach ($f in 'main.h', 'nativeCaller.h', 'types.h') { Copy-Item (Join-Path $tmp "sdk\inc\$f") $shv -Force }
  Copy-Item (Join-Path $tmp 'sdk\lib\ScriptHookV.lib') $shv -Force
  Copy-Item (Join-Path $tmp 'rt\bin\ScriptHookV.dll'), (Join-Path $tmp 'rt\bin\dinput8.dll') $runtime -Force
  Write-Host "ScriptHookV: $($rt.Name)"

  $reshadeVersion = '6.8.0'
  $reshadeInc = Join-Path $here 'third_party\reshade'
  $shaders = Join-Path $here 'third_party\shaders'
  New-Item -ItemType Directory -Force $reshadeInc, $shaders | Out-Null
  $setup = Join-Path $tmp 'reshade.exe'
  Invoke-WebRequest -UseBasicParsing -UserAgent $ua -Headers @{ 'Referer' = 'https://reshade.me/' } `
    "https://reshade.me/downloads/ReShade_Setup_${reshadeVersion}_Addon.exe" -OutFile $setup
  # the setup exe carries its DLLs as an appended zip; Windows' bsdtar reads past the exe in front of it
  tar -xf $setup -C $tmp ReShade64.dll
  if (-not (Test-Path (Join-Path $tmp 'ReShade64.dll'))) { throw "could not extract ReShade64.dll from the ReShade setup" }
  Copy-Item (Join-Path $tmp 'ReShade64.dll') $runtime -Force
  foreach ($f in 'reshade.hpp', 'reshade_api.hpp', 'reshade_api_device.hpp', 'reshade_api_pipeline.hpp', 'reshade_api_resource.hpp',
    'reshade_api_format.hpp', 'reshade_events.hpp', 'reshade_overlay.hpp') {
    Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/crosire/reshade/v$reshadeVersion/include/$f" -OutFile (Join-Path $reshadeInc $f)
  }
  foreach ($f in 'ReShade.fxh', 'ReShadeUI.fxh') {
    Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/crosire/reshade-shaders/slim/Shaders/$f" -OutFile (Join-Path $shaders $f)
  }
  Write-Host "ReShade $reshadeVersion (add-on support)"
  Get-ChildItem $shv, $runtime, $reshadeInc, $shaders | Select-Object FullName, Length | Format-Table -AutoSize
}
finally {
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
