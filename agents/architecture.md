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
