@echo off
setlocal
cd /d "%~dp0"

echo ==============================================
echo   Diabotical community plugins - installer
echo ==============================================
echo.

if not exist "dbt_plugins.dll" (
  echo ERROR: dbt_plugins.dll was not found in this folder.
  echo.
  echo Copy dbt_plugins.dll, install.bat and readme.txt into your
  echo Diabotical game folder ^(the one with diabotical.exe^), then
  echo double-click install.bat again.
  echo.
  pause
  exit /b 1
)

if not exist "diabotical.exe" (
  echo WARNING: diabotical.exe is not in this folder.
  echo This installer should be run from your Diabotical game folder.
  echo.
  pause
)

rem --- already installed?  just swap in the new plugin DLL ---
if exist "gfsdk_ssao_orig.dll" (
  echo Plugins already installed - updating to this version...
  if exist "GFSDK_SSAO_D3D11.win64.dll" del "GFSDK_SSAO_D3D11.win64.dll"
  ren "dbt_plugins.dll" "GFSDK_SSAO_D3D11.win64.dll"
  echo.
  echo Done. Updated. Launch the game as usual.
  echo.
  pause
  exit /b 0
)

if not exist "GFSDK_SSAO_D3D11.win64.dll" (
  echo ERROR: GFSDK_SSAO_D3D11.win64.dll was not found.
  echo That file ships with Diabotical - this does not look like the
  echo game folder. Nothing was changed.
  echo.
  pause
  exit /b 1
)

echo Backing up original:  GFSDK_SSAO_D3D11.win64.dll -^> gfsdk_ssao_orig.dll
ren "GFSDK_SSAO_D3D11.win64.dll" "gfsdk_ssao_orig.dll"

echo Installing plugins:   dbt_plugins.dll -^> GFSDK_SSAO_D3D11.win64.dll
ren "dbt_plugins.dll" "GFSDK_SSAO_D3D11.win64.dll"

echo.
echo Done! Plugins installed. Launch the game normally.
echo.
echo To uninstall: delete GFSDK_SSAO_D3D11.win64.dll and rename
echo gfsdk_ssao_orig.dll back to GFSDK_SSAO_D3D11.win64.dll.
echo.
pause
