# Standalone host validation

The `game` executable has an executable-level smoke mode. It creates a hidden
Win32 childless window, uses the D3D11 WARP adapter, loads `game_module.dll`
through a unique copied filename, applies any supplied scene/post-process JSON
snapshots, advances the shared fixed-step scheduler, renders and presents, then
destroys the module before the stable D3D11 host.

The intended CTest command is:

```text
game --smoke --frames 4
     --module <absolute path to game_module.dll>
     --hot-dir <build>/runtime/hot
     --scene <source>/examples/cube.scene.json
     --post <source>/examples/post_process.json
```

`standalone_host_tests.cpp` separately proves the explicit Win32 virtual-key,
scan-code, and extended-bit conversion into the engine-owned `Game_Key` ABI.
