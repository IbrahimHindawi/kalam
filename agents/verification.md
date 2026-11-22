# Verification

From the repository root:

    cargo test --locked -p kalam-runtime -p kalam-ricochet
    cargo check --locked -p ricochet --features editor
    cargo xtask doctor --target windows
    cargo xtask editor --scene ricochet --smoke

The last command needs the native toolchain and graphics runtime. Use Cargo
tests and checks for routine changes without a window. Native Windows and
Android artifacts live under engine/build. Never commit downloaded shader
tools, build caches, environment files, or API keys.
