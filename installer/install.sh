#!/usr/bin/env bash
# Diabotical community plugins - installer (Linux / Steam Deck / Proton)
# Run this from your Diabotical game folder, e.g.:
#   ./install.sh
set -euo pipefail

# work in the folder this script lives in
cd "$(dirname "$(readlink -f "$0")")"

echo "=============================================="
echo "  Diabotical community plugins - installer"
echo "=============================================="
echo

if [ ! -f "dbt_plugins.dll" ]; then
  echo "ERROR: dbt_plugins.dll was not found in this folder."
  echo
  echo "Copy dbt_plugins.dll, install.sh and readme.txt into your"
  echo "Diabotical game folder (the one with diabotical.exe), then"
  echo "run ./install.sh again."
  exit 1
fi

if [ ! -f "diabotical.exe" ]; then
  echo "WARNING: diabotical.exe is not in this folder."
  echo "This installer should be run from your Diabotical game folder."
  echo
fi

# --- already installed?  just swap in the new plugin DLL ---
if [ -f "gfsdk_ssao_orig.dll" ]; then
  echo "Plugins already installed - updating to this version..."
  rm -f "GFSDK_SSAO_D3D11.win64.dll"
  mv "dbt_plugins.dll" "GFSDK_SSAO_D3D11.win64.dll"
  echo
  echo "Done. Updated. Launch the game as usual."
  exit 0
fi

if [ ! -f "GFSDK_SSAO_D3D11.win64.dll" ]; then
  echo "ERROR: GFSDK_SSAO_D3D11.win64.dll was not found."
  echo "That file ships with Diabotical - this does not look like the"
  echo "game folder. Nothing was changed."
  exit 1
fi

echo "Backing up original:  GFSDK_SSAO_D3D11.win64.dll -> gfsdk_ssao_orig.dll"
mv "GFSDK_SSAO_D3D11.win64.dll" "gfsdk_ssao_orig.dll"

echo "Installing plugins:   dbt_plugins.dll -> GFSDK_SSAO_D3D11.win64.dll"
mv "dbt_plugins.dll" "GFSDK_SSAO_D3D11.win64.dll"

echo
echo "Done! Plugins installed. Launch the game normally (no launch options needed)."
echo
echo "To uninstall: delete GFSDK_SSAO_D3D11.win64.dll and rename"
echo "gfsdk_ssao_orig.dll back to GFSDK_SSAO_D3D11.win64.dll."
