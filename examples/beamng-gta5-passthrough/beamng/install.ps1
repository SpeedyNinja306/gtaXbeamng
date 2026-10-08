<#
.SYNOPSIS
  Installs (or removes) the gtaxbeam BeamNG.drive mod as an unpacked mod in the user folder.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File beamng\install.ps1
  powershell -ExecutionPolicy Bypass -File beamng\install.ps1 -Remove
#>
param(
  [switch]$Remove,
  [string]$UserDir = $env:BEAMNG_USER_DIR
)
$ErrorActionPreference = 'Stop'

if (-not $UserDir) {
  # 0.38+ keeps the user folder at %LOCALAPPDATA%\BeamNG\BeamNG.drive\current; older builds use BeamNG.drive\<major.minor>
  $current = Join-Path $env:LOCALAPPDATA 'BeamNG\BeamNG.drive\current'
  if (Test-Path $current) { $UserDir = $current }
}
if (-not $UserDir) {
  $root = Join-Path $env:LOCALAPPDATA 'BeamNG.drive'
  $versionFile = Join-Path $root 'version.txt'
  if (-not (Test-Path $versionFile)) { throw "BeamNG user folder not found under $root; set BEAMNG_USER_DIR" }
  $parts = (Get-Content $versionFile -TotalCount 1).Trim().Split('.')
  $UserDir = Join-Path $root ($parts[0] + '.' + $parts[1])
}

$src = Join-Path $PSScriptRoot 'gtaxbeam'
$dst = Join-Path $UserDir 'mods\unpacked\gtaxbeam'

if (Test-Path $dst) {
  Remove-Item -Recurse -Force $dst
  Write-Host "removed $dst"
}
if ($Remove) { return }

New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
Copy-Item -Recurse $src $dst
Write-Host "installed $dst"
