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

## License

MIT. See [LICENSE](LICENSE).
