# Aether Engine - stage a distributable engine install from a build and zip it.
# This is what the release workflow publishes and what the hub's auto-updater
# downloads; run it locally to test the exact layout users get:
#
#   pwsh tools/make_install.ps1 -BuildDir build -Config Release -OutDir dist
#
# Layout (everything at the zip root, no wrapper folder):
#   AetherHub.exe / AetherEditor.exe / AetherRuntime.exe / AetherCore.dll
#   engine.json          rewritten with install-relative SDK paths
#   shaders/  Templates/
#   SDK/include/**.h     engine headers for GameModule compiles
#   SDK/lib/AetherCore.lib
param(
    [string]$BuildDir = "build",
    [string]$Config = "Release",
    [string]$OutDir = "dist"
)
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repoRoot (Join-Path $BuildDir (Join-Path "bin" $Config))

foreach ($exe in "AetherHub.exe", "AetherEditor.exe", "AetherRuntime.exe", "AetherCore.dll",
                 "AetherCore.lib", "engine.json") {
    if (-not (Test-Path (Join-Path $bin $exe))) {
        throw "missing $exe in $bin - build the $Config config first"
    }
}
$manifest = Get-Content (Join-Path $bin "engine.json") -Raw | ConvertFrom-Json
$version = $manifest.version
if (-not $version) { throw "engine.json in $bin has no version" }

$OutDir = Join-Path $repoRoot $OutDir
$stage = Join-Path $OutDir "AetherEngine-$version-win64"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

# Binaries + shaders (already copied next to the exes by the build).
foreach ($f in "AetherHub.exe", "AetherEditor.exe", "AetherRuntime.exe", "AetherCore.dll") {
    Copy-Item (Join-Path $bin $f) $stage
}
Copy-Item -Recurse (Join-Path $bin "shaders") (Join-Path $stage "shaders")

# Project templates ship with the engine so the hub can create projects.
Copy-Item -Recurse (Join-Path $repoRoot "Templates") (Join-Path $stage "Templates")

# SDK: headers (tree-preserving) + import lib, referenced relatively from
# engine.json so the install can live anywhere.
$sdkInclude = Join-Path $stage "SDK\include"
$src = Join-Path $repoRoot "src"
Get-ChildItem -Recurse -Path $src -Include *.h, *.hpp, *.inl | ForEach-Object {
    $rel = $_.FullName.Substring($src.Length + 1)
    $dest = Join-Path $sdkInclude $rel
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    Copy-Item $_.FullName $dest
}
New-Item -ItemType Directory -Force (Join-Path $stage "SDK\lib") | Out-Null
Copy-Item (Join-Path $bin "AetherCore.lib") (Join-Path $stage "SDK\lib")

$toolset = if ($manifest.toolset) { $manifest.toolset } else { "" }
@"
{
  "name": "AetherEngine",
  "version": "$version",
  "toolset": "$toolset",
  "sdkInclude": "SDK/include",
  "sdkLib": "SDK/lib"
}
"@ | Out-File -Encoding utf8 (Join-Path $stage "engine.json")

$zip = Join-Path $OutDir "AetherEngine-$version-win64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Host "staged: $stage"
Write-Host "zip:    $zip"
