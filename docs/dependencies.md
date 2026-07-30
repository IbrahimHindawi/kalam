# Editor dependencies

The engine, game module, standalone host, reflection tool, and their tests do
not depend on Qt or Neovim-Qt. The `KALAM_BUILD_EDITOR` branch adds the editor's
third-party dependencies through `cmake/KalamNeovimQt.cmake`.

## Pinned upstreams

| Dependency | Immutable input | Integrity check | License |
| --- | --- | --- | --- |
| [Qt](https://download.qt.io/archive/qt/6.8/6.8.3/) | 6.8.3 | exact version required by CMake; use an official Qt 6.8.3 package | LGPL-3.0-only, GPL-2.0-only, GPL-3.0-only, or commercial, by module/package terms |
| [equalsraf/neovim-qt](https://github.com/equalsraf/neovim-qt) | v0.2.19 commit `6c506094f5d0e9663a3a907622d5aef8e19de85f` | exact clean Git `HEAD`, followed by a checked patch application | ISC |
| [msgpack/msgpack-c](https://github.com/msgpack/msgpack-c) | `cpp-3.2.0` commit `29b9505cb76710802e130d5b8403ec1b40de1c8d` | archive SHA-256 `ff865a36bad5c72b8e7ebc4b7cf5f27a820fce4faff9c571c1791e3728355a39`, or exact clean Git `HEAD` | Boost Software License 1.0 |

The normal online path fetches the official Neovim-Qt repository at the full
commit ID and the official msgpack-c archive at the recorded SHA-256. Neither
dependency tracks a branch or an unverified release URL.

For an offline or developer-managed cache, set both source overrides:

```powershell
$env:KALAM_QT_PREFIX = "C:/Qt/6.8.3/msvc2022_64"
cmake --preset windows-clang-editor-debug `
  -DKALAM_NEOVIM_QT_SOURCE_DIR=C:/deps/neovim-qt `
  -DKALAM_MSGPACK_SOURCE_DIR=C:/deps/msgpack-c
```

The Neovim-Qt override must be a clean Git checkout at the exact commit. A
msgpack override may be the exact clean Git checkout or the unmodified
`cpp-3.2.0` archive contents; the latter is checked with hashes for the public
version header and every compiled C source. Standard CMake
`FETCHCONTENT_BASE_DIR` and `FETCHCONTENT_FULLY_DISCONNECTED` also work when a
FetchContent cache has already been populated.

Qt itself is supplied by the developer or CI image. Point `CMAKE_PREFIX_PATH`
or `Qt6_DIR` at an official Qt 6.8.3 installation. CMake rejects any other
Qt version. Kalam requests exactly `Qt6::Core`,
`Qt6::Gui`, and `Qt6::Widgets`; Qt Network, SVG, Test, QML, and WebEngine are
not requested or linked.

## Kalam's narrow Neovim-Qt profile

Kalam copies the pinned source into the build tree and applies
`editor/neovim/upstream_patch/process_only.patch`. The pristine checkout is
never modified. The patch has five deliberately small responsibilities:

- use `QIODevice::readyRead` for message-pack I/O and remove local/TCP/stdin
  transports, eliminating Qt Network and keeping only spawned `QProcess` RPC;
- use the current `nvim_get_api_info` metadata method so the pinned client can
  negotiate with supported modern Neovim releases;
- stop each request timeout before dispatching its success or error handlers,
  preventing a completed request from reporting a later false fatal timeout;
- replace upstream `App::openNewWindow` coupling with a host-facing Shell
  signal; and
- expose cursor rectangle updates needed to anchor native editor overlays.

The target compiles only generated Neovim RPC bindings, the `QProcess`
connector, message-pack transport, keyboard/popup support, `Shell`, and
`ShellWidget`. It does not compile upstream `App`, `MainWindow`, error widget,
context menu, tree view, tab line, or scrollbar sources, nor any SVG resource.
CMake scans the selected source list for those forbidden files and Qt
Network/SVG tokens after applying the patch.

The msgpack project is not added as a subdirectory. Kalam builds its five
pinned C sources directly, avoiding its legacy global CMake flags, examples,
tests, install rules, and source-tree generation.

## Root wiring and consumer API

The root editor branch is enabled only by `KALAM_BUILD_EDITOR`:

```cmake
if(KALAM_BUILD_EDITOR)
    include(KalamNeovimQt)
    add_subdirectory(editor/neovim)
    add_subdirectory(editor/qt)
endif()
```

`editor/neovim/CMakeLists.txt` creates `kalam_neovim_qt` and the consumer alias
`Kalam::NeovimQt`. Editor code includes the upstream-compatible headers
`neovimconnector.h` and `shell.h` and links only the alias. The integration
also publishes `kalam_neovim_qt_provenance.h`, containing the exact upstream
version/commit and `KALAM_NEOVIM_RUNTIME_RELATIVE_PATH`.

The reusable process API is:

```cpp
NeovimQt::NeovimConnector* connector =
    NeovimQt::NeovimConnector::spawn(arguments, neovim_executable);
auto* shell = new NeovimQt::Shell(connector, parent);
```

`spawn` inserts `--embed` before Neovim's filename separator. The editor must
not add `-u NONE`, `--clean`, or another option that disables the user's normal
Neovim configuration. It may add an early `--cmd` that prepends the staged
runtime directory to `runtimepath`.

The patch adds these host-facing Shell members:

- `QRect neovimCursorRectForHost() const noexcept`;
- `neovimCursorRectChanged(const QRect&)`; and
- `neovimNewWindowRequested(const QVariantList&)`.

The `nveditor` executable stages that merged runtime beside itself with:

```cmake
kalam_stage_neovim_runtime(nveditor)
```

That merges the pinned upstream GUI shim and Kalam's `nveditor` Lua runtime
into `nvim-runtime` beside the executable. Project runtime files are copied
second and therefore intentionally win if a path is ever shared.

## Patch-only verification

Patch/revision/firewall validation does not require Qt or msgpack:

```powershell
cmake `
  -DKALAM_NEOVIM_QT_SOURCE_DIR=C:/deps/neovim-qt `
  -DKALAM_NEOVIM_QT_VERIFY_OUTPUT_DIR=build/neovim-qt-verified `
  -P editor/neovim/upstream_patch/verify.cmake
```

This is useful on bootstrap machines. A full editor configure still requires a
Qt 6 Core/Gui/Widgets development installation and deliberately fails before
downloading other dependencies when Qt is absent.
