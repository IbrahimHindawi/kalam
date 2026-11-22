# Kalam Rust renderer

The scene and assets live in Rust. A private C99 bridge owns SDL's window, native surface and bgfx command submission. The preview shows a grid of metallic/roughness materials, an asymmetric textured sphere, a shadow receiver and an alpha-blended sprite. Drag with the mouse or one finger to orbit. Escape closes the Windows window. Press R to reload the deployed scene and material together. Invalid edits leave the current scene and camera intact.

## Windows build

Use a Windows x64 development host with Rust's MSVC target, Visual Studio C++ build tools and Windows SDK, Clang/Clang++, CMake, Ninja, Git, and Python 3. The first build downloads pinned native and Rust dependencies. Run from the branch root:

```powershell
python engine/tools/shaders.py --bootstrap --doctor
cargo xtask doctor --target windows
cargo xtask run --target windows
```

The standalone executable and its assets land under `engine/build/windows/`. The bgfx runtime uses the same pinned package as shaderc. CMake fetches the pinned SDL3 revision or accepts an installed matching package in `SDL3_DIR`. The shader tools run on the host only. Build inputs and release hashes for Slang and bgfx tools are recorded in `engine/tools/shaders.py` and native source revisions in `engine/native/CMakeLists.txt`.

## Scene authoring

Authored JSON supports fields with inline editing rules. The registered runtime
loaders resolve each field's `value`, falling back to `default` when it is
absent. The native Form view edits the value directly. Use JSON to edit a
default or change a range. An invalid reload reports the failing field and
keeps the last valid data. Plain JSON values also load.

```json
{
  "radiance": {
    "value": [3, 3, 3],
    "type": "vec3",
    "default": [3, 3, 3],
    "min": 0,
    "max": 64
  }
}
```

Supported types are `integer`, `number`, `vec2`, `vec3`, `vec4`, `string`,
and `boolean`. Numeric bounds are optional and inclusive. Vector bounds
apply to each component. Scene and asset IDs, format versions, and command
names are plain JSON. Runtime invariants such as entity capacity, asset
references, and valid collision layouts are checked after field resolution.

Edit `engine/assets/scenes/reference.json` for the next build. For live edits, change
the copy under `engine/build/windows/assets/scenes/` and press R. The scene has
stable instance IDs, an orbit camera (yaw and pitch in radians), a directional
sun, and exposure. Instances select a built-in `sphere` or `floor` mesh and supply
a position, optional uniform scale and optional `yaw_degrees`. A material can
be inline or reference the bundled `"reference"` material in
`assets/materials/reference.json`. Both files are validated as one update.
The current shadow map covers a fixed region around the origin.

## Walkthrough

Run `cargo xtask run --target windows --scene room` from the Rust worktree.
Use **WASD** to walk, **left-drag** to look, and **R** to reload the scene.
The player moves at a fixed 120 Hz simulation step. The scene's optional `walk`
object sets the spawn, floor level, movement speed, and rectangular collision
bounds. Reloads preserve the player pose, clamping position when bounds change.

A scene can map mesh IDs to filenames through `"meshes": {"room-shell": "room.glb"}`.
Files live under `engine/assets/meshes/` and contain one static indexed or
unindexed triangle mesh with positions, normals, and optional UVs. Export in
mesh-local coordinates and assign materials and placement in the Kalam scene.
External buffers, skinning, animations, and glTF material import are outside
this first importer. GLBs are limited to 8 MiB and 65,536 vertices per mesh.
The renderer uploads replacement assets before swapping the active scene.
CPU mesh data stays available to rebuild GPU buffers after a device reset.

## Ephemeral scene controls

In a Windows scene, press **F1** to build the panel from
`assets/lenses/inspect.json`. The default panel binds sliders to player speed
and renderer exposure. Controls that do not apply to the scene are omitted.
The panel captures input while open. Press F1 again to release the panel and
resume movement. Edits stay in the live scene until R reloads the authored files.
Reopening reads the descriptor again.

The descriptor sets the title, labels, order, and bindings. The bindings own
their valid ranges and use the same edits as the headless script. To open the
panel immediately, run `cargo xtask run --target windows --scene room --lens`.

## Headless simulation

```powershell
cargo xtask sim --scene room --script engine/examples/walk-script.json
```

A version 1 script sets held buttons with `input`, adjusts aim with `look`,
advances exact ticks with `step`, sets movement speed with `set_speed` or exposure with `set_exposure`, and
records a snapshot with `inspect`. The JSON report includes the tick rate,
stable scene instance IDs, every requested observation, and a final player
snapshot. Each invocation starts at the authored spawn. Edits live in memory
for that replay. Invalid commands exit with an error that identifies the command
index. Pass `--asset-root` to check a different asset tree.

The command uses the same content loader and fixed-step Rust movement code
as the windowed engine. SDL3, bgfx and the graphics device are only needed
for the windowed build.

## Windows editor

To try a playable Kalam project, run `nu run.nu --scene ricochet` from
the main checkout. See the [ricochet arena guide](../samples/ricochet/README.md)
for standalone play, live tuning, and headless verification.

Games can build their own executable and register an `Application` from
`kalam_host::game` before the native host starts. Registration supplies the
scene, editable document paths, validation, and the game loop. A window crate
under `samples/<name>/window` can be built by the launcher when
`--scene <name>` matches its package name. Ricochet uses this entry point.
The default Kalam executable does not depend on the sample crates.

### Build a game from a separate repository

Keep game source, assets, and `Cargo.lock` in the game repository. For a private
repository, pin Kalam as a Git submodule at `vendor/kalam` using an engine commit
containing the Rust workspace. Update that pin when you want a newer engine.

Use the following in the game repository's root `Cargo.toml`. The launcher uses
Python 3.11 or newer to read an external game manifest.

```toml
[package]
name = "my-game"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["staticlib"]

[features]
editor = ["kalam-host/editor"]

[dependencies]
kalam-host = { path = "vendor/kalam/engine/host" }
kalam-runtime = { path = "vendor/kalam/engine/runtime" }
```

Implement `kalam_host::game::Application` and export `kh_register_game` as shown
in [Ricochet's window crate](../samples/ricochet/window/src/lib.rs). Set up the
game's scenes and editable documents under `assets/`. Generate the lockfile once,
commit it, and ignore `target/` in the game repository. From the game root, run:

```powershell
cargo generate-lockfile
python vendor/kalam/engine/tools/task.py run --game-manifest Cargo.toml
python vendor/kalam/engine/tools/task.py editor --game-manifest Cargo.toml
```

Use `--asset-root` if the game's content lives elsewhere. The editor reads and
edits the game's content directory directly. The game executable and staged
engine shaders go under `target/kalam/` in the game repository. For distribution,
place the game's content alongside the staged shaders in the executable's `assets/`
directory. Kalam and the game link statically into that executable.

Games can query both mouse buttons and the Tab and F key edges through
`kalam_host::game::input`. Editor builds can present a transient choice panel
with `present_choices`. A game draw can supply a linear tangent-space normal
texture and a scalar emissive strength. Existing materials use the geometric
normal and zero emission by default.

### Skinned assets

A project's `res.toml` can register skinned models by stable asset ID. The
Ricochet catalog resolves model paths, forward axes, scale, and clip aliases
at startup. Kalam accepts a GLB or a glTF with one local companion BIN. Clips
can be embedded or loaded from a separate file with matching node names,
order, and hierarchy.

Pose evaluation preserves authored local matrices for unkeyed nodes. A
companion clip can animate a matrix-authored bone when its rest matrix can be
represented as translation, rotation, and scale. Missing channels retain
their rest components. Clip schedules update animated locals and affected
descendants in parent order. Per-channel key cursors optimize forward
playback, with a search on seeks and wraps. Repeated samples reuse the pose
and uploaded vertices. Ricochet advances animation with simulation ticks and
feeds the same posed mesh to the lighting and shadow passes.

```powershell
cargo xtask editor --scene entities
```

From the main checkout, use `nu run.nu`. The editor uses one SDL3 window.
bgfx renders the scene and the Dear ImGui panels into the same backbuffer.
The editor workspace, document forms, undo history, and gizmos live in Rust.
The native bridge supplies Dear ImGui and SDL3 functions and submits ImGui
draw data to bgfx.

- The **Scene** and **Document** panels dock on either side of the viewport.
  Drag their tabs to re-dock, stack, or float them within the editor. Drag the
  divider to resize them. Kalam remembers the layout between launches.
- The top-center controls start, pause, resume, and stop play. **Alt+P**
  toggles play and pause, **Alt+Q** returns to editing, and **Alt+R** restarts
  the session using the latest valid scene and game data. Authored drafts
  stay open when you stop or restart.
- Left-click an entity in the viewport to select it, or use the **Scene** panel.
  Click empty space or press Escape in the viewport to clear selection.
  Press **F** to center and fit the selection in the edit viewport.
- In the focused viewport, **Q** selects, **W** moves along a world axis
  or plane, **E** rotates with axis rings, and **R** scales along a local
  axis or uniformly from the center handle. Hold **Ctrl** while dragging
  to snap in 0.25-unit, 15-degree, or 10% steps. Ricochet walls move and
  scale in XZ and rotate around Y. Dragging previews the JSON draft,
  **Escape** cancels the drag, and **Save All** persists it.
- Hold right-click in the viewport to look around with the mouse and fly with
  **WASD**. **Q/E** descend and ascend along the camera's local up axis.
  Release right-click to restore the pointer.
- In **Document**, switch between the generated **Form** and the read-only,
  syntax-colored **JSON** view. **Copy JSON** copies the current draft.
  Valid edits reach the scene before saving. Invalid drafts keep the last valid
  scene on-screen. **Ctrl+Z** and **Ctrl+Shift+Z** undo and redo form edits and
  gizmo drags.
  On Windows, the JSON view prefers Iosevka Term Nerd Font Mono. Set
  `KALAM_NVIM_FONT` to an absolute font file path to choose another face.
- Use **Save All** or **Ctrl+S**. **Discard Draft** reloads the selected document
  from disk and is disabled when it has no unsaved changes. The editor watches
  the manifest, its entity files, and the reference material for external edits.
- Press `:` outside text input or Ctrl+Shift+P for Kalam's command line. It accepts
  `scene`, `entity <id>`, `form`, `text`, `write`, `reload`, and `viewport`.

Each scene manifest lists entity IDs in order. For example, the `centerpiece`
entry in `scenes/entities.json` loads `scenes/entities/centerpiece.json`. The
entity file owns its local components. If it names an archetype, omitted
components inherit from the template, objects override components, and
`null` removes a component.

The editor smoke test submits ImGui draw batches from a hidden window and
verifies that an unsaved exposure edit reaches the renderer.
Run `cargo xtask editor --smoke`.

## Android

Use the same Windows host to compile the Vulkan stages and Android arm64 native library. The APK carries SDL3, the renderer, material data and shader blobs. See [Android setup](android/README.md) for SDK, NDK and device prerequisites.

```powershell
cargo xtask doctor --target android
cargo xtask build --target android
cargo xtask run --target android --smoke
```

## Reproduce the checks

```powershell
cargo test --workspace --locked
cargo xtask shaders --target all
cargo xtask run --target windows --capture engine/build/acceptance/windows
cargo xtask run --target windows --backend vulkan --capture engine/build/acceptance/vulkan
cargo xtask run --target windows --smoke --lifecycle-test
python engine/tools/check_capture.py engine/build/acceptance/windows.tga --reference engine/tests/reference-windows.png
python engine/tools/check_capture.py engine/build/acceptance/vulkan.tga --reference engine/tests/reference-windows.png
```

The image checker uses Pillow. Captures fix the camera and exposure and add an eight-step linear-light ramp. The checker probes the HDR display transform, asymmetric sprite quadrants, alpha blend and an independent CPU evaluation of an illuminated floor sample. At the fixture aspect ratio it also compares a reduced image with the reviewed D3D11 baseline (mean channel error ÃƒÂ¢Ã¢â‚¬Â°Ã‚Â¤3/255 and at most 2.5% of pixels above 24/255). Smoke runs reload the scene, invalidate old handles, and check the native resource count returns to baseline. Device captures can be pulled with `cargo xtask run --target android --capture engine/build/acceptance/android`; use the numeric probes alone for a different aspect ratio.

## Rendering details

Opaque geometry is submitted to a 1024Ãƒâ€šÃ‚Â² directional shadow pass and scene-linear HDR color pass. Material shading uses GGX/Smith/Schlick, linear material channels, sRGB color textures and baked split-sum environment lighting. Shadows use point-sampled light-space depth with a 0.003 depth bias. The postprocess uses fixed exposure 1, Reinhard tone mapping and the sRGB transfer function. The sprite is composited over the output. To regenerate the checked-in linear RGBA8 environment fixtures, run `python engine/tools/bake_environment.py`.

Rust retains scene descriptions and typed handles, explicitly unloads each scene's GPU objects, and resets draw-list scratch once per frame. Input, window and device loss remain behind the native ABI. Filament's material equations are the design reference. See [the renderer plan](../docs/plan.md) for scope and acceptance criteria.

## Hardware qualification

On 2026-11-19 the Windows scene passed on an NVIDIA GeForce RTX 5070 Ti (driver 32.0.16.2124) at 960Ãƒâ€”600. D3D11 and Vulkan captures passed the numerical probes and the same spatial baseline. A three-frame run that also rebuilds resources reported 0.753 ms (D3D11) and 82.381 ms (Vulkan) for the third CPU frame in one sample; these include startup effects and are not steady-state performance results. Windows resize/minimize/restore passed. The Windows build used rustc 1.97.1, Clang 22.1.6, CMake 4.3.3, and Ninja 1.13.2.

The Android arm64 Rust static library and Vulkan shader records built. The connected Rockchip cm16 reports Android 13/API 33, a Mali-G52, Vulkan 1.1, and sampled RGBA16F and D16 support. APK assembly, presentation, and lifecycle testing await installation of the Android SDK components listed above. The Windows Vulkan capture is supplementary coverage.
