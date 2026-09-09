# leaflet

GoldSrc GameUI sidecar. It is not a `GameUI.dll` replacement: the stock
`valve/cl_dlls/GameUI.dll` stays on disk. The launcher loads this DLL via
`-dll leaflet.dll` (or `rev.ini` `[Loader] Dlls=`). Export `Launcher_Init`.

Hooks the main-menu layout (left-anchored item list) and picks widescreen
vs 4:3 background tiles. A watcher re-applies the hook when GoldSrc unloads
and reloads GameUI after a video-mode change.

## Architecture (UI layers)

Leaflet is growing into a small in-game UI kit on top of stock VGUI — not a
full LVGL replacement for Options (Options stay hooked VGUI). Layers:

| Layer | Files | Role |
|-------|--------|------|
| **vgui_bridge** | `vgui_bridge.*` | One place for FindChild / SetPos / SetSize / Park |
| **ui_api** | `ui_api.*`, `include/leaflet.h` | Internal helpers plus exported Label/Button/Image/Dialog constructors |
| **ui_layout** | `ui_layout.*` | Percentage grid columns with min/max constraints and declarative row tables |
| **ui_caps** | `ui_caps.*` | Soft flags from cvars (`metaAudio` / `metaVoice`) — hide mixer rows if Meta\* unloaded |
| **scheme / theme** | `scheme.*` | Color tokens (`OverlayTheme` / `UiTheme_Current`); TrackerScheme today, RapidJSON themes next |
| **Fit\*** | `layout.cpp` | Page-specific data and exceptional geometry; Audio, Voice, Multiplayer, Video, and Mouse use `ui_layout` |
| **roundframe** | `roundframe.*` | Painted chrome (pills, tracks) using theme tokens |
| **overlay / LVGL** | `overlay.*` | Prefetch sheet only (progress bar), not Options |

**Creating a control:** stock Options controls can still live in
`cstrike/resource/OptionsSub*.res`. Code-created controls use the public
`include/leaflet.h` API:

```cpp
void *dialog = leaflet::Dialog(parent, "ExampleDialog", "Example",
                               120, 80, 360, 220);
leaflet::Label(dialog, "Message", "Leaflet UI");
leaflet::Image(dialog, "Logo", "resource/leaflet/logo", true);
leaflet::Button(dialog, "Accept", "OK", "OK");
```

The same four constructors are exported with a C ABI as
`Leaflet_CreateLabel`, `Leaflet_CreateButton`, `Leaflet_CreateImage`,
and `Leaflet_CreateDialog`. Creation is idempotent by parent/name. A
button sends its command to its VGUI parent; that parent must handle the
command. `Dialog` creates a stock non-modal `vgui2::Frame` and remains
experimental until its close/reopen lifecycle has been exercised in-game.

**Laying out controls:** define `UiGridColumn` tracks in basis points
(`5000` = 50%), with optional minimum and maximum widths. Describe rows as
`UiTableRow`/`UiTableCell`; `UiTable_Apply` resolves names and places them.
This is intentionally a small CSS Grid-like model, not a flex engine.

`Ensure` means “return the named control if GameUI loaded it; otherwise
construct a supported fallback.” It does not describe layout.

VGUI parents own created controls. Returned pointers must not be retained
after `GameUI.dll` unloads or reloads (for example, after a video-mode
change). Dynamic constructors are enabled only when their machine-code
signatures match the supported Nov-2020 GameUI build; otherwise they return
`NULL`.

**MetaAudio / MetaVoice unload:** leaflet does not unload those DLLs; if their cvars disappear, `UiCaps` clears and Voice mixer extras (Noise gate / Voice monitor) are parked off-screen. Stock transmit/receive remain.

RapidJSON from MetaHook (`thirdparty/rapidjson`) is on the include path for future `themes/*.json` — no custom JSON parser.

Prefetch progress stays a single continuous bar (no dual cube/strip styles).

## Build

```bat
build.bat
```

Needs Visual Studio 2022 (vcvars32), CMake, and Ninja. The script copies
`leaflet.dll` next to `hw.dll` and restores stock `GameUI.dll` from
`valve/cl_dlls/GameUI_orig.dll` when that backup is present.

Does not patch `steam_api.dll`.

CMake `-DLEAFLET_LVGL=OFF` skips FetchContent for LVGL and compiles an
empty overlay stub. Default is ON: a click-through layered HWND
(`WS_EX_LAYERED | TRANSPARENT | NOACTIVATE`) dims the game client and
draws a macOS-dark sheet (window + inner progress plate) with LVGL 9 while prefetch runs. Clicks pass
through to GameUI. The overlay does not hook `SwapBuffers` or take focus.

## Prefetch

Optional FastDL pull while the main menu is up. Progress is a centered
matte window (`#1C1C1E`) with a lighter inner plate (`#3A3A3C`) filling
`#0A84FF`, over a dimmed screen — not GameUI `LoadingDialog`.

```
[Prefetch]
FastDL=http://example.com/fastdl/
Host=203.0.113.10
Port=27015
```

Empty `FastDL` / `Host` disables it. Missing map `.bsp`, `.res`, and files
listed in the `.res` are fetched with libcurl into `cstrike_downloads/`
(stock copies in `cstrike/` are left alone). Missing `maps`, `overviews`,
`sound` and the rest of the tree are created as needed.

Connect does not wait for prefetch. Files already in `cstrike_downloads`
are used as-is. The rest GoldSrc downloads itself (server / `sv_downloadurl`).
If Connect is pressed while curl is still running, prefetch aborts the
current `.part` so the engine is the only writer.

The LVGL strip is shown only while a file is actually transferring. A
quiet A2S check plus an already-complete cache does not put a banner on
screen.

## License

MIT. See [LICENSE](LICENSE).
