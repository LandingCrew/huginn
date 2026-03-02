@echo off
setlocal enabledelayedexpansion

:: release.bat — Build Release and package a FOMOD-ready ZIP
:: Usage: release.bat [version]
::   version defaults to the value in CMakeLists.txt

set "ROOT_DIR=%~dp0.."
set "BUILD_DIR=%ROOT_DIR%\build"

:: Read version from arg or CMakeLists.txt
if not "%~1"=="" (
    set "VERSION=%~1"
) else (
    for /f "delims=" %%A in ('powershell -NoProfile -Command "if((Get-Content '%ROOT_DIR%\CMakeLists.txt') -match 'set.PROJECT_VERSION\s+\"(\d+\.\d+\.\d+)\"'){$Matches[1]}"') do set "VERSION=%%A"
)

if not defined VERSION (
    echo ERROR: Could not parse version from CMakeLists.txt
    exit /b 1
)

echo === Huginn Release v%VERSION% ===

:: --- Build Release -----------------------------------------------------------
echo ^>^> Configuring...
cmake --preset vs2022-windows -DPROJECT_VERSION="%VERSION%" -DCOPY_OUTPUT=OFF
if errorlevel 1 goto :fail

echo ^>^> Building Release...
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 goto :fail

:: --- Locate DLL and PDB -----------------------------------------------------
set "DLL=%BUILD_DIR%\src\Release\Huginn.dll"
set "PDB=%BUILD_DIR%\src\Release\Huginn.pdb"

if not exist "%DLL%" (
    echo ERROR: DLL not found at %DLL%
    exit /b 1
)

:: --- Assemble staging directory ----------------------------------------------
set "STAGING=%BUILD_DIR%\release-staging"
if exist "%STAGING%" rmdir /s /q "%STAGING%"

:: Core files (always installed)
mkdir "%STAGING%\Core\SKSE\Plugins"
mkdir "%STAGING%\Core\Interface\Huginn"

copy /y "%DLL%"                                             "%STAGING%\Core\SKSE\Plugins\" >nul
copy /y "%ROOT_DIR%\configs\Huginn.ini"                     "%STAGING%\Core\SKSE\Plugins\" >nul
copy /y "%ROOT_DIR%\configs\Huginn_keybindings.ini"         "%STAGING%\Core\SKSE\Plugins\" >nul
copy /y "%ROOT_DIR%\configs\Huginn_Overrides.ini"           "%STAGING%\Core\SKSE\Plugins\" >nul
copy /y "%ROOT_DIR%\src\swf\Intuition.swf"                  "%STAGING%\Core\Interface\Huginn\" >nul

:: Template presets (each in its own folder, overrides Huginn.ini)
for %%F in ("%ROOT_DIR%\configs\templates\*.ini") do (
    set "TNAME=%%~nF"
    mkdir "%STAGING%\Templates\!TNAME!"
    copy /y "%%F" "%STAGING%\Templates\!TNAME!\Huginn.ini" >nul
)

:: dMenu integration (optional)
mkdir "%STAGING%\dMenu\SKSE\Plugins\dmenu\customSettings"
copy /y "%ROOT_DIR%\Data\SKSE\Plugins\dmenu\customSettings\Huginn.json" ^
        "%STAGING%\dMenu\SKSE\Plugins\dmenu\customSettings\" >nul

:: FOMOD metadata
mkdir "%STAGING%\fomod"
copy /y "%ROOT_DIR%\fomod\info.xml"         "%STAGING%\fomod\" >nul
copy /y "%ROOT_DIR%\fomod\ModuleConfig.xml" "%STAGING%\fomod\" >nul

:: --- Create ZIP --------------------------------------------------------------
set "ZIP_NAME=Huginn-%VERSION%.zip"
set "ZIP_PATH=%BUILD_DIR%\%ZIP_NAME%"
if exist "%ZIP_PATH%" del /q "%ZIP_PATH%"

:: Try 7z first, fall back to PowerShell
where 7z >nul 2>&1
if not errorlevel 1 (
    pushd "%STAGING%"
    7z a -tzip "%ZIP_PATH%" . -xr!*.pdb >nul
    popd
) else (
    powershell -NoProfile -Command ^
        "Compress-Archive -Path '%STAGING%\*' -DestinationPath '%ZIP_PATH%' -Force"
)

echo.
echo === Package ready: %ZIP_PATH% ===

:: Debug ZIP (includes PDB)
if exist "%PDB%" (
    copy /y "%PDB%" "%STAGING%\" >nul
    set "ZIP_DEBUG=%BUILD_DIR%\Huginn-%VERSION%-debug.zip"
    if exist "!ZIP_DEBUG!" del /q "!ZIP_DEBUG!"
    where 7z >nul 2>&1
    if not errorlevel 1 (
        pushd "%STAGING%"
        7z a -tzip "!ZIP_DEBUG!" . >nul
        popd
    ) else (
        powershell -NoProfile -Command ^
            "Compress-Archive -Path '%STAGING%\*' -DestinationPath '!ZIP_DEBUG!' -Force"
    )
    echo     Debug package: !ZIP_DEBUG!
)

exit /b 0

:fail
echo.
echo === BUILD FAILED ===
exit /b 1
