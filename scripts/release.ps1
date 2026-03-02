#Requires -Version 5.1
<#
.SYNOPSIS
    Build Release and package a FOMOD-ready ZIP.
.PARAMETER Version
    Semantic version string. Defaults to the value in CMakeLists.txt.
#>
param(
    [string]$Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RootDir  = Split-Path $PSScriptRoot -Parent
$BuildDir = Join-Path $RootDir 'build'

# --- Resolve version ---------------------------------------------------------

if (-not $Version) {
    $cml = Get-Content (Join-Path $RootDir 'CMakeLists.txt') -Raw
    if ($cml -match 'set\(PROJECT_VERSION\s+"(\d+\.\d+\.\d+)"\)') {
        $Version = $Matches[1]
    } else {
        Write-Error 'Could not parse version from CMakeLists.txt'
    }
}

Write-Host "=== Huginn Release v$Version ===" -ForegroundColor Cyan

# --- Build Release ------------------------------------------------------------

Write-Host '>> Configuring...'
cmake --preset vs2022-windows "-DPROJECT_VERSION=$Version" -DCOPY_OUTPUT=OFF
if ($LASTEXITCODE -ne 0) { Write-Error 'CMake configure failed' }

Write-Host '>> Building Release...'
cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { Write-Error 'CMake build failed' }

# --- Locate artifacts ---------------------------------------------------------

$Dll = Join-Path $BuildDir 'src\Release\Huginn.dll'
$Pdb = Join-Path $BuildDir 'src\Release\Huginn.pdb'

if (-not (Test-Path $Dll)) { Write-Error "DLL not found at $Dll" }

# --- Assemble staging directory -----------------------------------------------

$Staging = Join-Path $BuildDir 'release-staging'
if (Test-Path $Staging) { Remove-Item $Staging -Recurse -Force }

# Core files (always installed)
$coreSkse = New-Item (Join-Path $Staging 'Core\SKSE\Plugins')     -ItemType Directory -Force
$coreUi   = New-Item (Join-Path $Staging 'Core\Interface\Huginn') -ItemType Directory -Force

Copy-Item $Dll                                                          $coreSkse
Copy-Item (Join-Path $RootDir 'configs\Huginn.ini')                     $coreSkse
Copy-Item (Join-Path $RootDir 'configs\Huginn_keybindings.ini')         $coreSkse
Copy-Item (Join-Path $RootDir 'configs\Huginn_Overrides.ini')           $coreSkse
Copy-Item (Join-Path $RootDir 'src\swf\Intuition.swf')                  $coreUi

# Template presets
foreach ($ini in Get-ChildItem (Join-Path $RootDir 'configs\templates') -Filter '*.ini') {
    $dest = New-Item (Join-Path $Staging "Templates\$($ini.BaseName)") -ItemType Directory -Force
    Copy-Item $ini.FullName (Join-Path $dest 'Huginn.ini')
}

# dMenu integration (optional)
$dmenuSrc = Join-Path $RootDir 'Data\SKSE\Plugins\dmenu\customSettings\Huginn.json'
if (Test-Path $dmenuSrc) {
    $dmenuDest = New-Item (Join-Path $Staging 'dMenu\SKSE\Plugins\dmenu\customSettings') -ItemType Directory -Force
    Copy-Item $dmenuSrc $dmenuDest
}

# FOMOD metadata — stamp version into info.xml
$fomodDest = New-Item (Join-Path $Staging 'fomod') -ItemType Directory -Force
$infoXml = [xml](Get-Content (Join-Path $RootDir 'fomod\info.xml'))
$infoXml.fomod.Version = $Version
$infoXml.Save((Join-Path $fomodDest 'info.xml'))
Copy-Item (Join-Path $RootDir 'fomod\ModuleConfig.xml') $fomodDest

# --- Helper: create ZIP -------------------------------------------------------

function New-ReleaseZip {
    param(
        [string]$SourceDir,
        [string]$ZipPath,
        [string[]]$Exclude = @()
    )
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }

    $sevenZip = Get-Command 7z -ErrorAction SilentlyContinue
    if ($sevenZip) {
        $excludeArgs = $Exclude | ForEach-Object { "-xr!$_" }
        Push-Location $SourceDir
        & 7z a -tzip $ZipPath . @excludeArgs | Out-Null
        Pop-Location
    } else {
        Compress-Archive -Path "$SourceDir\*" -DestinationPath $ZipPath -Force
    }
}

# --- Create ZIPs --------------------------------------------------------------

$ZipPath = Join-Path $BuildDir "Huginn-$Version.zip"
New-ReleaseZip -SourceDir $Staging -ZipPath $ZipPath -Exclude '*.pdb'

Write-Host ''
Write-Host "=== Package ready: $ZipPath ===" -ForegroundColor Green

# Debug ZIP (includes PDB)
if (Test-Path $Pdb) {
    Copy-Item $Pdb $Staging
    $DebugZip = Join-Path $BuildDir "Huginn-$Version-debug.zip"
    New-ReleaseZip -SourceDir $Staging -ZipPath $DebugZip
    Write-Host "    Debug package: $DebugZip" -ForegroundColor DarkGray
}
