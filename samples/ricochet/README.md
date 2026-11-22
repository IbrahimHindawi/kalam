# Ricochet arena

Pilot a repair bot through three waves of scrapyard drones. Bank shots off blue metal walls to reach drones behind brown cover. Chasers pursue you and turrets fire at you. Between waves, choose an extra bounce or faster projectiles.

From the main Kalam checkout on Windows, run:

```powershell
nu run.nu --game --scene ricochet
nu run.nu --scene ricochet
```

The second command opens the game with Kalam’s docked editor. Inside the viewport, use **WASD** to move, aim with the mouse, and hold **left-click** to fire. Press **1** or **2** to choose an upgrade after clearing a wave. **X** resets the current mode.

The Windows launcher builds a project-owned `ricochet.exe`. The window crate
registers the game through `kalam_host::game::Application` and links Kalam
statically. The simulation crate also builds on its own for headless play.

The player uses `repair-bot.glb`, a Tripo P2 repair robot with in-place idle
and walk clips. Chasers use a distinct Tripo H3.1 model and a walk clip in
`chaser-bot.glb`. Both have 65-joint Mixamo-compatible rigs. The player
switches clips based on actual movement. In normal play, chasers share a
walk pose while keeping independent positions and headings. Kalam evaluates
the joint hierarchy on the CPU and uploads the joint palette. The Slang
vertex shaders use that palette for deformation in both the bgfx lighting
and shadow passes. Independently posed instances can share the same mesh.
Ricochet uses CPU deformation when vertex palette textures are unsupported.
Set `KALAM_CPU_SKINNING=1` to force the CPU path for comparison.

[res.toml](../../engine/assets/samples/ricochet/res.toml) maps stable asset IDs
to model paths, authored forward axes, world heights, and animation aliases.
The runtime resolves these once at startup. Clip aliases can name a clip
embedded in the mesh or reference a companion file, for example
`idle = { path = "characters/robot/idle.gltf", animation = "Idle" }`. An
unnamed companion with one animation needs only `path`.

Press **T** for the test range. Stand at spawn and aim at the gold marker on the far wall. The preview line turns green when the shot will hit. The turret stays alive and the HUD counts hits. Open **weapon.json** in the left dock to tune speed, damage, rate of fire, or bounce count. New shots adopt valid changes. Return to the viewport and replay the shot without closing the game. **Save All** writes drafts to disk. External JSON edits are watched by the editor. In standalone mode, use **R** to reload files from disk.

The data files are under `engine/assets/samples/ricochet/`. The optional sample crate owns game rules and runs at 120 ticks per second. Layout edits reset the room so actors cannot remain inside moved walls.

Each numeric field in `weapon.json` carries its own editor rules. The Form view edits `value`. Switch to JSON to edit the range or default. If you omit `value`, the loader uses `default`. Invalid edits retain the last valid weapon. The game still enforces its runtime capacity limits.

```json
{
  "max_bounces": {
    "value": 2,
    "type": "integer",
    "default": 2,
    "min": 0,
    "max": 8
  }
}
```

From the Rust engine checkout, replay the reference bank shot and profile 10,000 ticks using the busiest authored wave:

```powershell
cargo run --release -p kalam-ricochet
cargo test -p kalam-ricochet
cargo xtask editor --scene ricochet --smoke
```

The headless report includes the bounce and hit ticks, median and p95 simulation time, and allocation requests during the measured ticks.

To sample windowed frame time under the same busy wave, run a standalone smoke build with profiling enabled. This measures host wall time through frame submission and excludes the first 30 frames.

```powershell
$env:KALAM_RICOCHET_PROFILE = "1"
cargo xtask run --target windows --scene ricochet --smoke
Remove-Item Env:KALAM_RICOCHET_PROFILE
```

Set `KALAM_SKIN_CROWD` to a number from 1 to 64 during a profile run to
include additional chasers with independent animation phases. The report
also measures host time for animation updates, uploads, and draw preparation.
Use the same count and add `--capture` with a different output prefix in
each run to compare the GPU and CPU paths.
