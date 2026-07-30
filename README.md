# Kalam

Kalam is a text-first Windows game-engine development environment built around
Neovim, a Qt Widgets shell, a native D3D11 viewport, JSON-authored data, and a
hot-reloaded C/C++ game module.

The authoritative implementation specification is
[`docs/spec_1.md`](docs/spec_1.md). The default preset builds the dependency-free
engine, game module, standalone host, tools, and tests. A separate preset adds
the pinned Qt/Neovim editor without introducing Qt into shipping targets.

## Prerequisites

- Windows 10 or newer
- CMake 3.28 or newer
- Ninja
- `clang-cl` and a Windows SDK (the primary configuration)
- Neovim on `PATH` for the editor and headless document tests
- the official Qt 6.8.3 MSVC 2022 package for an editor build

Visual Studio's MSVC-compatible `cl` is also supported. Engine-owned C++
translation units use C++20, with exceptions and RTTI disabled.

## Configure, build, and test

Run from a terminal where the Windows SDK and MSVC libraries are visible:

```powershell
cmake --preset windows-clang-debug
cmake --build --preset windows-clang-debug
ctest --preset windows-clang-debug
```

All build, generated, test, hot-reload, and runtime output stays under
`build/` or `runtime/`; a clean build does not modify the source tree.

## Build and launch the editor

Point `KALAM_QT_PREFIX` at the official Qt 6.8.3 kit, then use the editor
preset:

```powershell
$env:KALAM_QT_PREFIX = "C:/Qt/6.8.3/msvc2022_64"
$env:PATH = "$env:KALAM_QT_PREFIX/bin;$env:PATH"
cmake --preset windows-clang-editor-debug
cmake --build --preset windows-clang-editor-debug
ctest --preset windows-clang-editor-debug
.\build\windows-clang-editor-debug\nveditor.exe .\examples\cube.scene.json
```

The build verifies and patches an immutable Neovim-Qt v0.2.19 checkout, stages
the editor runtime beside `nveditor.exe`, and links only Qt Core, Gui, and
Widgets. It may fetch the pinned Neovim-Qt and msgpack sources on first
configure. For hashes, offline source overrides, licensing, and the exact
dependency boundary, see [`docs/dependencies.md`](docs/dependencies.md).
