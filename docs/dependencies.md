# Editor dependencies

The engine, game module, standalone host, reflection tool, and their tests do
not depend on Qt or Neovim. The `KALAM_BUILD_EDITOR` branch adds the editor's
only third-party dependencies: an official Qt 6.8.3 kit at build time and a
Neovim executable at run time.

The editor's Neovim integration is first-party. Kalam speaks Neovim's
versioned msgpack-RPC UI protocol directly through its own codec, RPC client,
grid model, and grid widget in `editor/qt/` (see `docs/spec_2.md`). The
protocol contract is Neovim `api_level >= 11`, checked during the channel
handshake. No Neovim-Qt, msgpack-c, or any other third-party source is
fetched, vendored, or patched.

## Pinned upstreams

| Dependency | Immutable input | Integrity check | License |
| --- | --- | --- | --- |
| [Qt](https://download.qt.io/archive/qt/6.8/6.8.3/) | 6.8.3 | exact version required by CMake; use an official Qt 6.8.3 package | LGPL-3.0-only, GPL-2.0-only, GPL-3.0-only, or commercial, by module/package terms |
| [Neovim](https://github.com/neovim/neovim) | any release with `api_level >= 11` | handshake-time `nvim_get_api_info` floor check | Apache-2.0 |

Qt itself is supplied by the developer or CI image. Point `CMAKE_PREFIX_PATH`
or `Qt6_DIR` at an official Qt 6.8.3 installation. CMake rejects any other
Qt version. Kalam requests exactly `Qt6::Core`, `Qt6::Gui`, and
`Qt6::Widgets`; Qt Network, SVG, Test, QML, and WebEngine are not requested
or linked.

Neovim is found on `PATH` (or through the editor's `--neovim` option) and is
spawned unmodified as `nvim --embed`. The editor must not add `-u NONE`,
`--clean`, or another option that disables the user's normal Neovim
configuration. Kalam ships no Neovim sources and applies no patches; any
supported Neovim release satisfying the api_level floor works.

## Root wiring and consumer API

The root editor branch is enabled only by `KALAM_BUILD_EDITOR`:

```cmake
if(KALAM_BUILD_EDITOR)
    include(KalamEditorRuntime)
    add_subdirectory(editor/qt)
endif()
```

`editor/qt/CMakeLists.txt` builds the `Kalam::EditorQt` library — including
the first-party protocol client (`neovim_msgpack`, `neovim_rpc_client`,
`neovim_grid_model`, `neovim_grid_widget`, `neovim_input_translation`) — and
the `nveditor` executable.

The reusable process API is:

```cpp
auto* client = new Kalam::Editor::Neovim_Rpc_Client(parent);
client->spawn(neovim_executable);
auto* shell = new Kalam::Editor::Editor_Neovim_Shell(client, parent);
```

`spawn` passes `--embed` and starts the handshake:
`nvim_get_api_info` (with the api_level floor), `nvim_set_client_info`, and
then `ready()`. The shell attaches the UI with `ext_linegrid` once ready.

The `nveditor` executable stages the Kalam-owned Neovim runtime beside
itself with:

```cmake
kalam_stage_editor_runtime(
    nveditor
    "${CMAKE_SOURCE_DIR}/editor/neovim/runtime"
)
```

That copies `editor/neovim/runtime/` (the `nveditor` Lua plugin and modules)
into `nvim-runtime` beside the executable. There is no third-party GUI shim;
the staged runtime is entirely first-party.

## Dependency boundary tests

- `editor.qt_source_firewall` scans every first-party editor source and
  fails on any reference to the removed Neovim-Qt dependency, on forbidden
  Qt modules, and on the C++ standard library in `editor/qt/`.
- `editor.qt_runtime_deployment` verifies the staged Qt runtime and the
  staged Kalam Neovim runtime beside the executable.
- `editor.neovim_rpc_client_real_nvim` and
  `editor.neovim_grid_model_real_nvim` run the protocol client against the
  installed Neovim, covering the handshake floor and the live redraw stream.
