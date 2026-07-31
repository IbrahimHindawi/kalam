# Kalam architecture

The normative requirements live in [`spec_1.md`](spec_1.md). This note records
the source and target boundaries enforced by the build.

## Dependency direction

```text
engine_core

render_api       -> engine_core
render_d3d11     -> render_api + engine_core
engine_runtime   -> render_api + engine_core
game_logic       -> engine_runtime + engine_core
module_bridge    -> ABI-only bridge headers
game_module      -> game_logic + render_d3d11 + module_bridge

platform_win32   -> engine_core
editor_host      -> module_bridge + platform_win32 + render_d3d11
editor_qt        -> editor_host + Qt + nvim --embed (msgpack-RPC)
standalone_host  -> module_bridge + platform_win32 + render_d3d11

reflection_tool  -> selected engine_core headers only
```

Dependencies point downward only. CMake targets receive narrow, target-local
include directories and link dependencies; there are no directory-wide include
paths or compile definitions.

## Source firewalls

- `bridge/` is standalone C-compatible ABI: fixed-width integers, pointers,
  counts, procedural tables, and no platform or Qt headers.
- `engine/`, `game/`, and `reflection/` use plain structs, free functions,
  explicit arenas, handles, and small lowercase C++20 templates.
- `editor/host/` and `editor/win32/` remain Qt-free.
- Only `editor/qt/` may include Qt.
- Simulation-facing public headers contain neither D3D11 nor editor types.
- Editor and standalone hosts reach game state only through `bridge/`.

Automated source scans and standalone-header compilation tests guard these
boundaries as targets are added.

## Ownership boundaries

The stable host owns the clock, input latch, D3D11 device/context, swap chain,
viewport targets, resize, and presentation. The module borrows host interfaces
only for documented call lifetimes and owns every game-specific CPU/GPU
resource it creates. Allocations never cross the DLL boundary.
