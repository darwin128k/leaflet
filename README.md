# leaflet

GoldSrc GameUI sidecar. It is not a `GameUI.dll` replacement: the stock
`valve/cl_dlls/GameUI.dll` stays on disk. The launcher loads this DLL via
`-dll leaflet.dll` (or `rev.ini` `[Loader] Dlls=`). Export `Launcher_Init`.

Hooks the main-menu layout (left-anchored item list) and picks widescreen
vs 4:3 background tiles. A watcher re-applies the hook when GoldSrc unloads
and reloads GameUI after a video-mode change.

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
(`WS_EX_LAYERED | TRANSPARENT | NOACTIVATE`) draws a matte top strip
with LVGL 9 while prefetch runs. GameUI stays native. The overlay does
not hook `SwapBuffers` or take focus.

## Prefetch

Optional FastDL pull while the main menu is up. Progress is the LVGL
top strip (filename + blue bar `#3d8bfd`), not GameUI `LoadingDialog`.

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
