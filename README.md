# Kalam

Kalam is a Rust game engine and editor. SDL3 handles the platform, bgfx handles
rendering, Slang supplies shaders, and the Windows editor uses Dear ImGui.
Games can keep their code and assets in separate repositories and link the
engine at build time. Ricochet is the in-tree sample.

## Run on Windows

For a fresh checkout, install the pinned shader tools once.

    python engine/tools/shaders.py --target windows --bootstrap --doctor

Use a developer terminal with Rust, Python 3.11+, CMake, Ninja, Clang, and the
MSVC and Windows SDK libraries available. Nushell is needed for the launcher.

    nu run.nu
    nu run.nu --scene ricochet
    nu run.nu --game --scene ricochet

Without Nushell, use cargo xtask editor --scene ricochet. Run
cargo xtask doctor --target windows to check the toolchain.

See [engine/README.md](engine/README.md) for native builds, Android, and how to
link a separate game. For agent handoff and verification, start with
[agents/README.md](agents/README.md).
