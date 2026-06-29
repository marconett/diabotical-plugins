Diabotical community plugins
============================
Version: __VERSION__

------------------------------------------------------------
INSTALL
------------------------------------------------------------
1. Copy these files into your **Diabotical game folder** (the one that contains `diabotical.exe`).
2. Windows: double-click **`install.bat`**.
   Linux / Steam Deck (Proton): run **`./install.sh`** from a terminal in that folder.
3. Start the game normally. Done!

------------------------------------------------------------
UPDATING
------------------------------------------------------------

Move the new `dbt_plugins.dll` into the game folder and run the installer again
(install.bat on Windows, ./install.sh on Linux).

------------------------------------------------------------
UNINSTALL
------------------------------------------------------------
In the game folder:
  1. Delete  GFSDK_SSAO_D3D11.win64.dll   (this is our plugin DLL)
  2. Rename  gfsdk_ssao_orig.dll  ->  GFSDK_SSAO_D3D11.win64.dll