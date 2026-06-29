# Diabotical community plugins

A small set of quality-of-life fixes and features for **Diabotical**, packaged as a single drop-in .dll file.

Works on **Windows** and on **Linux (Proton)**, no launch options needed.

## Plugins

| Plugin | What it does |
|---|---|
| **demo_spec_switch** | Allow cycling through other players pov in client side recorded demos (just press **SPACE**) |
| **demo_gzip_tail** | Fixed a bug where client side recorded demos always ended prematurely, preventing the end of the game to be recorded. |
| **replay_reserve** | Fixed a bug that could lead to the client lagging for a short time when client demo recording is enabled. |
| **weeball_throw_demo** | Fixes the first-person weeball throw animation and cooldown not showing during demo playback. |

## Install

1. Go to the [**Releases**](../../releases/latest) page and download the latest release.
2. Unzip it. You'll get `dbt_plugins.dll`, `install.bat`, `install.sh`, and `readme.txt`.
3. Copy them into your **Diabotical game folder** (the one that contains `diabotical.exe`).
4. Run the installer — **Windows:** double-click `install.bat`. **Linux / Steam Deck (Proton):** run `./install.sh` from a terminal in that folder.
5. Start the game normally. Done!

### Updating

Move the new `dbt_plugins.dll` into the game folder and run `install.bat` again.

## Uninstall

In the game folder:

1. Delete `GFSDK_SSAO_D3D11.win64.dll` (that's the plugin file).
2. Rename `gfsdk_ssao_orig.dll` back to `GFSDK_SSAO_D3D11.win64.dll`.

## Notes

- This is an **unofficial community modification**, not affiliated with the GD Studios. Use at your own risk.
- Found a bug or have an idea? Open an issue.