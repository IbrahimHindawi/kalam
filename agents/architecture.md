# Engine map

- engine/runtime owns scene parsing, materials, animation, the game-visible
  ECS data, and headless simulation. Keep it independent of bgfx and SDL3.
- engine/host owns game registration, native resource handles, and the Rust
  editor. The editor validates drafts and retains the last accepted preview.
- engine/native owns SDL3, bgfx, and the narrow ImGui bridge. Cross-platform
  rendering changes belong here and in engine/shaders.
- engine/tools/task.py drives builds from Cargo xtask. An external game
  supplies a separate manifest and its own content directory. Windows still
  links the resulting game crate and engine into one executable.
- Samples and their fixtures live under samples and engine/assets. The
  supported editor is the Rust and ImGui editor. The historical Qt adapter
  under engine/editor is outside the normal build.

## World FX

`engine/runtime/src/fx.rs` reads version 1 layered FX JSON. The game supplies an asset, seed, world-space anchors, and elapsed simulation time. Emitters support particles, ribbons, meshes, beams and decals. Release an emitter to let its existing particles expire. Stop it to remove them immediately.

`engine/host/src/world_fx.rs` resolves texture and mesh IDs through the game's catalog. Pass the resulting draws to `render_view_at_fx` with the opaque scene. The transparent pass composites into HDR before tonemapping. It tests scene depth and preserves the HDR alpha channel that the editor uses for selection.

A project can expose an ImGui pane through `Application::editor_tool`. The host owns docking and input. The project owns its documents, previews and save behavior. `KALAM_EDITOR_TOOL=1` opens the project pane on launch.
