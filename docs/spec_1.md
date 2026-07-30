# spec_1.md

# Neovim-Qt + Qt Widgets + Native DX11 Engine Editor
## Implementation Specification for Codex Sol

Revision date: 2026-07-30

This specification supersedes `spec_0.md` and all prior requirements involving Zonvie, Zig, Dear ImGui, permanent Properties/Console docks, C++ source annotations such as `@live`, or a generic live-property registry.

The new stack is:

```text
Qt Widgets editor shell
+ Neovim-Qt frontend/client
+ native Win32/D3D11 viewport
+ text-authoritative JSON
+ generated and contextual Qt controls
+ hot-reloaded C/C++ game module
```

The engine remains written in the existing style:

> C with templates.

Qt is an editor-shell dependency only. It must not enter the engine, hot-reloaded game module, asset formats, or shipping executable.

The implementation reference examined for this revision is Gin commit
`c6090099b01b3610cddbb14a6dc7c61d593f6dc9`.

Gin is a read-only design and source reference for Kallam. It is not a runtime
dependency, Git parent, subtree, or source-layout mandate. Kallam starts as a
clean engine repository and extracts only the proven low-level patterns named
in this specification.

---

# 0. Mission

Implement a fast, text-first game-engine development environment with two persistent regions:

```text
+--------------------------------+--------------------------------+
| Neovim editor                  | Native D3D11 engine viewport   |
|                                |                                |
| C/C++ source                   | Live runtime                   |
| JSON scene/config data         | Selection and gizmos later     |
|                                |                                |
+--------------------------------+--------------------------------+
```

Neovim owns text editing.

Qt owns desktop GUI behavior and transient generated controls.

The engine owns rendering, runtime memory, simulation, and runtime validation.

JSON is the canonical authored representation.

Graphical controls are generated on demand from the current Neovim buffer and are never saved as a second editor format.

Normal code iteration must support:

```text
edit C/C++ in Neovim
-> F6
-> Ninja compiles only changed translation units
-> links a new game_module.dll
-> editor validates and hot-reloads it
-> current last-known-valid JSON documents rehydrate runtime state
-> viewport resumes
```

Normal data iteration must support:

```text
edit JSON manually
or
open a contextual/full generated Qt GUI
-> preview changes directly in runtime memory
-> viewport updates immediately
-> accept commits one surgical Neovim text edit
-> normal Neovim undo restores the old text
```

---

# 1. Product Principles

## 1.1 Text is authoritative

The canonical project data is plain text:

- JSON scenes;
- JSON post-processing configurations;
- JSON materials and animation settings where appropriate;
- C/C++ source;
- future text formats only when a real use case demands them.

Generated GUI state is ephemeral.

Do not save `.ui` files, generated property-panel files, editor databases, or duplicate serialized GUI state for authored engine values.

## 1.2 GUI is a projection of text semantics

Qt controls exist because some values are better manipulated graphically:

- floats with ranges;
- colors;
- transforms;
- enum choices;
- curves;
- animation values;
- asset references;
- viewport gizmos.

The GUI is not a second source of truth.

It is generated from:

```text
current Neovim buffer
+ current authoring schema
+ current runtime state
```

## 1.3 Runtime preview is direct

During a drag or gizmo interaction:

```text
Qt control / viewport gizmo
-> plain engine API
-> live C/C++ runtime state
-> next rendered frame
```

Do not rewrite, reparse, or reload JSON on every interaction tick.

Text is patched only when the user accepts or releases the interaction.

## 1.4 Shipping runtime is independent

The shipping game must not depend on:

- Qt;
- Neovim;
- Neovim-Qt;
- editor RPC;
- generated GUI code;
- editor widgets;
- editor-only reflection consumers.

The shipping runtime may load cooked binary assets derived from JSON.

---

# 2. Persistent Editor Layout

Revision 1 has exactly two persistent visual regions.

Use a horizontal `QSplitter`:

```text
Editor_Main_Window
└── QSplitter
    ├── Neovim_Editor_Region
    └── Engine_Viewport_Widget
```

No permanent Properties dock.

No permanent Console dock.

No permanent asset browser.

No permanent inspector.

Build diagnostics should initially use Neovim's quickfix list.

Transient UI is allowed:

- F4 contextual popup;
- Shift+F4 generated full-document GUI;
- temporary build-status notification;
- file dialog;
- modal error dialog for unrecoverable editor failures.

## 2.1 Left region

The left region contains the Neovim-Qt editing widget.

It must support ordinary Neovim behavior:

- normal, insert, visual, and command modes;
- splits inside Neovim;
- keyboard and mouse input;
- clipboard;
- syntax highlighting;
- LSP;
- Tree-sitter;
- plugins;
- undo and redo;
- user configuration.

## 2.2 Right region

The right region contains only the engine viewport.

The viewport must not contain:

- a permanent property panel;
- a permanent console;
- source text;
- tutorial text;
- editor instructions;
- persistent Qt widgets over the rendered game.

Temporary engine-rendered selection outlines and gizmos are allowed.

---

# 3. Top-Level Build Targets

The repository must produce separate targets.

```text
nveditor.exe
    Qt + Neovim-Qt + editor host + DX11 viewport host

game_module.dll
    hot-reloaded C/C++ engine/game code

game.exe
    plain Win32/D3D11 shipping or standalone development host
    no Qt

reflection_tool.exe
    Kallam-owned compile-time reflection/code-generation tool

asset_cooker.exe
    future JSON-to-binary cooker
```

## 3.1 Normal iteration target

The editor's F6 command must build only:

```text
game_module.dll
```

It must not rebuild `nveditor.exe` unless editor code changed.

## 3.2 Qt modules

Initially permit only:

```text
Qt Core
Qt Gui
Qt Widgets
```

Do not add:

- Qt Quick;
- QML;
- Qt WebEngine;
- Qt Multimedia;
- Qt Network;
- Qt Designer-generated forms;
- a plugin framework;
- Python/PySide integration.

Those may be evaluated later, separately.

## 3.3 Revision 1 toolchain baseline

Use:

```text
CMake
-> Ninja
-> clang-cl or MSVC-compatible cl
-> Windows SDK / D3D11
```

The primary development configuration is Ninja with `clang-cl`.

MSVC remains a supported compiler when it produces the same C ABI and target
boundaries.

Engine-owned template translation units compile as C++20. Third-party C
implementation files may remain C when required.

For engine, game, bridge, and reflection targets:

- disable exceptions;
- disable RTTI;
- export compile commands;
- use one explicit MSVC-compatible runtime-library policy per configuration;
- do not allocate in one DLL and free through another target's CRT;
- keep warnings and compiler options target-local.

The upstream Neovim-Qt and Qt build may retain the flags they require. Do not
force engine-specific compiler restrictions onto vendored upstream code.

CMake is the source of truth for target dependencies. Helper scripts may
configure or invoke CMake, but must not contain a second hidden build graph.

Do not:

- hard-code developer-local dependency or asset paths;
- keep obsolete build entry points beside the supported path;
- download an unpinned branch during a normal configure;
- require the current working directory to infer important build inputs;
- generate source files without declaring their exact inputs and outputs.

Pin all source dependencies by commit or immutable release version.

---

# 4. Coding-Style Firewall

There are two coding domains.

```text
editor/qt/
    Qt-adapter C++

bridge/
    plain C-compatible ABI

engine/
game/
reflection/
    C with templates
```

They must remain visibly separate.

## 4.1 Engine and game code: hard requirements

In `bridge/**`, `engine/**`, `game/**`, `reflection/**`, and the hot-reloaded
module, do not introduce:

- Qt headers or Qt types;
- STL containers;
- `std::string`;
- `std::vector`;
- `std::map`;
- `std::unordered_map`;
- `std::optional`;
- `std::variant`;
- `std::function`;
- `std::span`;
- smart pointers;
- exceptions;
- RTTI;
- `dynamic_cast`;
- virtual class hierarchies;
- abstract interfaces;
- ownership-obscuring RAII wrappers;
- ranges;
- coroutines;
- service locators;
- dependency injection frameworks;
- event-bus architecture;
- visitor patterns;
- modern C++ refactors for their own sake.

Prefer:

- plain structs;
- plain enums;
- free functions;
- explicit initialization and destruction;
- arenas;
- caller-owned buffers;
- pointer-and-count views;
- fixed arrays;
- handles;
- templates for containers and low-level reusable primitives;
- direct Win32;
- direct D3D11;
- explicit COM lifetime handling;
- explicit error codes;
- explicit ownership comments.

## 4.2 Qt editor code

Qt-specific code may use the Qt mechanisms required to operate Qt correctly:

- `QObject`;
- `QWidget` inheritance;
- signals and slots;
- `QString`;
- Qt parent-child ownership;
- Qt models where genuinely needed;
- `QProcess`;
- `QTimer`;
- `QSplitter`;
- `QDialog`;
- `QColorDialog`;
- `QDoubleSpinBox`;
- `QSlider`.

Even in editor code:

- do not introduce exceptions;
- do not introduce STL or smart pointers in new project code;
- do not create deep application class hierarchies;
- do not wrap every engine object in a `QObject`;
- do not copy Qt types into engine-facing structures;
- keep adapters thin and explicit.

Do not perform a broad "modernization" of Neovim-Qt upstream code.

Modify only what is necessary.

## 4.3 Include firewall

Enforce:

```text
engine/** must not include any Qt header.
game/** must not include any Qt header.
editor/qt/** may include Qt.
bridge/** exposes plain procedural types only.
```

## 4.4 Dependency direction

Enforce an acyclic dependency direction.

```text
engine_core

render_api       -> engine_core
render_d3d11     -> render_api + engine_core
engine_runtime   -> render_api + engine_core
game_logic       -> engine_runtime + engine_core
module_bridge    -> ABI-only core headers
game_module      -> game_logic + render_d3d11 + module_bridge

platform_win32   -> engine_core
editor_host      -> module_bridge + platform_win32 + render_d3d11
editor_qt        -> editor_host + editor_neovim + Qt
standalone_host  -> module_bridge + platform_win32 + render_d3d11

reflection_tool  -> small shared core headers only
```

More precisely:

- `engine/**` must not include `game/**`;
- simulation code must not include editor headers;
- simulation code must not read editor or platform input globals;
- simulation must not query presentation systems for gameplay truth such as
  animation duration or UI state;
- resource metadata must not depend on gameplay implementation types;
- editor and standalone hosts talk to game state only through `bridge/**`;
- rendering and audio may consume simulation output, but simulation must remain
  bootable without creating a window, swap chain, audio device, or Qt object.

Do not recreate Gin's dependency cycles between gameplay, presentation,
resources, input, and audio.

## 4.5 Core data structures

Carry forward the useful Gin style:

```cpp
array<Entity>
vec<float>
array_reserve<Entity>(...)
vec_append<float>(...)
```

The engine owns a small lowercase core layer for:

- fixed-width types;
- strings and string views;
- arrays and vectors;
- handles;
- hashing;
- arenas and temporary-memory scopes;
- narrow algorithms that genuinely benefit from templates.

These templates must remain plain-data helpers. They must not grow into an STL
replacement with hidden allocation, constructors, ownership, or exceptions.

Haikal may be consulted as historical reference, but it is not an active
Kallam dependency. Generic containers come from the engine-owned template
layer. Reflection comes from the focused reflection tool.

## 4.6 Memory requirements

Preserve Gin's useful lifetime categories, not its allocator implementation.

Initial lifetime classes are:

```text
permanent
frame/transient
scene or level
tool scratch
```

The Windows arena implementation must:

- use explicit reserve and commit sizes;
- commit on demand;
- detect overflow before pointer arithmetic;
- support temporary marks or scopes;
- support explicit release;
- decommit or release large discarded ranges where appropriate;
- report allocation failure rather than silently returning overlapping memory;
- be testable without D3D11 or Qt.

Do not copy Gin's hard-coded one-terabyte-per-arena reservation, no-op destroy,
or unsafe shrink/reallocation behavior.

No allocation ownership crosses the hot-DLL boundary. Cross-boundary memory is
one of:

- caller-owned input valid for the duration of the call;
- module-owned borrowed output copied immediately by the host;
- caller-provided output storage with an explicit capacity.

## 4.7 Module state and globals

Subsystem state must be explicit and owned by a host, game instance, or
document instance.

Avoid mutable process-wide registries such as unrestricted global shader,
texture, mesh, entity, or input arrays.

Use:

- explicit state parameters;
- typed handles;
- centralized handle validation;
- explicit registry ownership;
- generated typed resource IDs where the asset manifest is authoritative.

Small immutable lookup tables and generated descriptor tables may remain
global.

Split orchestration, simulation, presentation, camera, menus, settings, debug
rendering, and authoring adapters before they accumulate into one translation
unit. File size alone is not the rule; mixed ownership and mixed reasons to
change are.

---

# 5. Neovim-Qt Integration

## 5.1 Base strategy

Fork or vendor Neovim-Qt at a pinned revision.

Reuse its existing:

- Neovim process management;
- MessagePack-RPC connection;
- UI attachment;
- grid rendering;
- cursor tracking;
- keyboard translation;
- mouse translation;
- clipboard support;
- highlight handling;
- popup-menu and command-line behavior where already supported.

Do not rewrite Neovim's UI protocol for Revision 1.

## 5.2 Editor shell integration

Create a new editor main window or minimally adapt the existing Neovim-Qt main window so that the Neovim widget occupies the left side of a `QSplitter`.

Keep the Neovim widget reusable as one Qt child widget.

Avoid spreading engine concerns throughout Neovim-Qt internals.

Add a small integration boundary:

```cpp
struct Editor_Neovim_Context
{
    // Qt/Neovim adapter-owned state.
};

bool editor_nvim_get_active_buffer(...);
bool editor_nvim_get_cursor(...);
bool editor_nvim_get_changedtick(...);
bool editor_nvim_get_buffer_text(...);
bool editor_nvim_set_text(...);
bool editor_nvim_open_file_location(...);
bool editor_nvim_set_quickfix(...);
```

The exact implementation may use Neovim-Qt's RPC facilities, but the rest of the editor should call a narrow adapter.

## 5.3 Cursor pixel position

The Qt editor must expose:

```text
active Neovim grid
cursor row and column
cell width and height
grid origin inside the widget
cursor rectangle in widget coordinates
```

This is required to anchor contextual Qt popups next to the text cursor.

Add one explicit signal or callback when the rendered cursor rectangle changes.

Do not infer it through fragile polling if Neovim-Qt already has the data.

## 5.4 F4 command

F4 opens a contextual editor for the supported JSON field at or near the cursor.

F4 may be implemented either as:

- a Qt shortcut that queries Neovim state; or
- a bundled Neovim command that notifies the GUI.

The implementation must remain remappable.

Provide a Neovim command:

```vim
:NvEditContext
```

Default mapping:

```vim
F4 -> :NvEditContext
```

## 5.5 Full generated GUI command

Provide:

```vim
:NvEditForm
```

Default mapping:

```vim
Shift-F4 -> :NvEditForm
```

This generates a temporary Qt form for the active valid JSON buffer.

The generated form is never saved to disk.

---

# 6. Native D3D11 Viewport in Qt

## 6.1 Widget type

Implement a custom native Qt viewport widget.

Conceptually:

```cpp
class Engine_Viewport_Widget : public QWidget
{
    Q_OBJECT

public:
    Engine_Viewport_Widget(Editor_Host* host, QWidget* parent);

protected:
    QPaintEngine* paintEngine() const override;
    void resizeEvent(QResizeEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
};
```

Use the Qt mechanisms necessary to force creation of a native child window.

The viewport host obtains the underlying `HWND`.

## 6.2 D3D11 ownership

The stable editor host owns:

- `ID3D11Device`;
- `ID3D11DeviceContext`;
- DXGI factory state;
- the viewport swap chain;
- viewport back-buffer RTV;
- viewport depth buffer;
- viewport dimensions.

The hot-reloaded game module borrows the device/context and renders into a host-provided render target.

The module must not `AddRef`, release, resize, or present borrowed
editor-owned COM objects.

Gameplay and simulation public headers must not include D3D11 or DXGI headers.
Concrete backend types remain inside the D3D11 renderer and stable host.

The game module owns and releases every game-specific GPU object it creates,
including shaders, buffers, textures, samplers, pipeline state, and
post-processing intermediates. These objects belong to the module's renderer
state and must be released through the old module API before that DLL unloads.

The borrowed device and immediate context are valid for the lifetime of the
game instance. A per-frame render target is valid only during the `render`
call that receives it.

## 6.3 Render target boundary

Use plain engine-native structures:

```c
typedef struct Host_Render_Target
{
    void* d3d11_render_target_view; // Borrowed ID3D11RenderTargetView*
    void* d3d11_depth_stencil_view; // Borrowed ID3D11DepthStencilView*
    uint32_t width;
    uint32_t height;
    uint64_t generation;
} Host_Render_Target;
```

No Qt type crosses this boundary.

`generation` changes whenever resize or target recreation invalidates
target-sized module resources.

Skip module rendering when the target is zero-sized, minimized, occluded, or
otherwise unavailable.

## 6.4 Resize

On viewport resize:

```text
Qt resize event
-> host marks resize pending
-> resize is coalesced/debounced
-> host recreates swap-chain resources
-> next frame receives new Host_Render_Target
```

Do not recreate D3D11 resources for every one-pixel intermediate resize event if it causes visible stalls.

## 6.5 Frame ownership

There is no independent engine `while` loop inside the module.

The stable host owns the monotonic clock, fixed-step accumulator, and
presentation schedule. Put this scheduler in shared host code used by both
`nveditor.exe` and `game.exe`.

Revision 1 uses:

```text
fixed simulation step: 1 / 60 second
maximum measured frame delta: 0.25 second
maximum catch-up updates per presented frame: 5
```

If the host remains behind after the catch-up cap, discard whole excess fixed
ticks while retaining the fractional accumulator. Never run an unbounded
catch-up loop on the Qt thread.

Render once after zero or more fixed updates. Pass
`interpolation_alpha = accumulator / fixed_delta_time` to render. A module may
ignore the value when it has no interpolated presentation state, but the ABI
includes it so interpolation does not require a later ABI break.

Rendering must not advance authoritative simulation.

Reset timing backlog after:

- a successful module reload;
- pause or explicit suspension;
- minimize, occlusion, or a hidden viewport;
- a debugger stall;
- device recreation.

The Qt event loop drives presentation using a precise timer or explicit queued
frame scheduling.

Each editor presentation frame performs:

```text
process pending build/reload result
collect and latch viewport input
measure and clamp elapsed time
run zero or more game_fixed_update calls, bounded by the catch-up cap
game_render once with interpolation alpha
Present
schedule next frame
```

The Neovim editor must remain responsive even if the game module fails to load.

## 6.6 Input ownership and tick consumption

The viewport host owns input collection. The module receives an immutable,
borrowed `Game_Input` snapshot for one fixed update.

Each digital button stores:

```text
is_down
pressed_count
released_count
```

Conceptually:

```c
enum
{
    Game_Key_Count = 256,
    Game_Mouse_Button_Count = 8
};

typedef struct Game_Button
{
    uint32_t is_down;
    uint32_t pressed_count;
    uint32_t released_count;
} Game_Button;

typedef struct Game_Input
{
    uint32_t struct_size;
    uint32_t flags;
    uint64_t fixed_tick_index;

    Game_Button keys[Game_Key_Count];
    Game_Button mouse_buttons[Game_Mouse_Button_Count];

    int32_t pointer_x_pixels;
    int32_t pointer_y_pixels;
    int32_t pointer_delta_x_pixels;
    int32_t pointer_delta_y_pixels;
    float wheel_delta;
} Game_Input;
```

Use an engine-owned key enumeration and an explicit Qt/Win32 translation
table. Do not expose Qt key values or rely on platform virtual-key ordinals as
the persistent engine key ABI.

Both edge counts may be nonzero when a press and release occur between fixed
updates.

The host accumulates button edges, pointer delta, and wheel delta until a fixed
update consumes them. A presentation frame with zero fixed updates must not
drop input.

When one presentation frame runs several catch-up updates:

1. The first fixed update receives all pending edges and accumulated deltas.
2. Later fixed updates receive held state with edge and delta fields cleared.

This prevents one physical press from being replayed on every catch-up tick.

Further requirements:

- focus loss synthesizes releases and clears mouse capture and relative deltas;
- Qt shortcuts and Neovim keystrokes do not enter `Game_Input` unless the
  native viewport owns focus or active mouse capture;
- pointer positions and deltas are expressed in render-target physical pixels;
- the Qt adapter performs explicit logical-to-physical DPI conversion;
- no equivalent of Gin's process-global `g_input` is allowed.

---

# 7. Stable Editor Host and Hot-Reloaded Module

## 7.1 Process model

Revision 1 uses one process:

```text
nveditor.exe
    stable Qt/Neovim/DX11 host
    loads game_module.dll
```

The editor stays alive while the module is rebuilt and replaced.

## 7.2 Module API

Export one C ABI negotiation entry point.

Do not return the function table by value.

Conceptually:

```c
typedef uint32_t Game_Result;
typedef struct Game_API Game_API;
typedef struct Game_Error Game_Error;

#if defined(__cplusplus)
extern "C" {
#endif

__declspec(dllexport)
Game_Result game_get_api(
    uint32_t host_api_version,
    uint32_t host_game_api_struct_size,
    Game_API* out_api,
    Game_Error* out_error
);

#if defined(__cplusplus)
}
#endif
```

Use fixed-width, C-compatible types:

```c
typedef struct String_View
{
    const char* data;
    uint64_t size;
} String_View;

enum
{
    Game_Result_Ok = 0,
    Game_Result_Invalid_Argument,
    Game_Result_Unsupported_Schema,
    Game_Result_Invalid_Document,
    Game_Result_Create_Failed,
    Game_Result_Device_Failed,
    Game_Result_Internal_Error
};

typedef struct Game_Error
{
    String_View message;
    String_View json_path; // Empty when unrelated to authored JSON.
} Game_Error;

typedef uint64_t Author_Document_Id;

typedef struct Game_Create_Desc
{
    uint32_t struct_size;
    uint32_t host_api_version;

    void* d3d11_device;            // Borrowed ID3D11Device*.
    void* d3d11_immediate_context; // Borrowed ID3D11DeviceContext*.

    void* host_user;
    void (*host_log)(void* host_user, uint32_t level, String_View message);
} Game_Create_Desc;
```

`String_View` is UTF-8 and need not be NUL-terminated.

The device/context pointers and host logging callback in a successful
`Game_Create_Desc` may be retained until `destroy` returns. All other input
pointers and views are borrowed only for the duration of their call unless a
type explicitly states otherwise.

Use a plain function table:

```c
typedef struct Game_API
{
    uint32_t api_version;
    uint32_t struct_size;

    Game_Result (*create)(
        const Game_Create_Desc* desc,
        void** out_game,
        Game_Error* out_error
    );

    void (*destroy)(
        void* game
    );

    void (*fixed_update)(
        void* game,
        const Game_Input* input,
        float fixed_delta_time
    );

    Game_Result (*render)(
        void* game,
        const Host_Render_Target* target,
        float interpolation_alpha,
        Game_Error* out_error
    );

    Game_Result (*apply_document)(
        void* game,
        const Author_Document_State* state,
        Game_Error* out_error
    );

    Game_Result (*remove_document)(
        void* game,
        Author_Document_Id document_id,
        Game_Error* out_error
    );

    uint32_t (*get_author_schema_count)(void);

    Game_Result (*get_author_schema)(
        uint32_t schema_index,
        Author_Schema_View* out_schema,
        Game_Error* out_error
    );

    Game_Result (*read_author_field)(
        void* game,
        Author_Document_Id document_id,
        Author_Object_Id object_id,
        Author_Field_Id field_id,
        Author_Value* out_value,
        Game_Error* out_error
    );

    Game_Result (*preview_author_field)(
        void* game,
        Author_Document_Id document_id,
        Author_Object_Id object_id,
        Author_Field_Id field_id,
        const Author_Value* value,
        Game_Error* out_error
    );
} Game_API;
```

The exact spelling may change, but the following ABI rules are mandatory:

- the ABI header compiles independently as C and C++;
- ABI integers use `<stdint.h>` fixed-width types;
- do not expose C++ `bool`, `long`, `size_t`, STL, Qt, templates, references,
  exceptions, or compiler-owned containers;
- the host zeroes `out_api` and validates API version, `struct_size`, and every
  required function pointer;
- structure layout has compile-time assertions in both a C and C++ smoke test;
- all module calls and immediate-context use are serialized on the host/UI
  thread in Revision 1;
- a module-owned error or descriptor view is copied by the host before another
  call into that module or `FreeLibrary`;
- no module function pointer, descriptor pointer, COM object, worker thread,
  audio callback, or runtime pointer survives `destroy` and DLL unload.

On `create` failure, `*out_game` remains null and the candidate unwinds every
partial CPU and GPU allocation.

Fallible module calls must report failure. Recoverable resource, document,
shader, and device errors must not call `exit`, `abort`, `DebugBreak`, or
terminate the editor process.

## 7.3 Reload strategy

Use copied DLL filenames so the linker can overwrite the canonical output.

Example:

```text
build/game_module.dll
-> runtime/game_module_hot_0042.dll
-> LoadLibraryW(runtime copy)
```

Reload sequence:

```text
1. Ninja reports successful build.
2. Copy canonical DLL to a unique hot-load filename.
3. Load the candidate DLL without unloading the active DLL.
4. Resolve game_get_api and negotiate into a zeroed Game_API table.
5. Validate API version, struct size, and required function pointers.
6. Create candidate runtime state while the old state remains intact.
7. Enumerate, validate, and deep-copy every candidate authoring schema.
8. Apply every current last-known-valid authored document to the candidate.
9. If any candidate step fails, copy its error, clear context bindings,
   destroy any candidate state, unload the candidate, and resume the old module.
10. At a frame boundary, cancel active GUI/gizmo transactions and stop all
    calls into the old API.
11. Clear immediate-context bindings so it holds no old module resources.
12. Destroy old runtime state through the old API.
13. Unload the old DLL.
14. Publish the candidate API, state, and editor-owned schema copies.
15. Reset the frame accumulator and resume rendering.
```

Candidate loading, API negotiation, creation, schema validation, or document
application are all part of one transactional reload.

If any candidate phase fails:

- keep the old module running;
- show the error in Neovim quickfix or a temporary notification;
- do not destroy or mutate the old runtime;
- when there is no old module, keep Neovim and a placeholder viewport usable.

The module renderer must bind every D3D11 state it relies on. It may not assume
immediate-context state survives across frames, candidate validation, resize,
or reload.

Hot DLL and PDB copies may be deleted only after the corresponding DLL has
unloaded. Keep cleanup bounded: retain the active copy and a small configurable
number of recent diagnostic copies, and prune older unloaded copies on startup
or after a successful reload.

## 7.4 State preservation

Revision 1 does not attempt arbitrary binary state migration.

After a successful reload:

```text
all current last-known-valid authored document snapshots
-> new runtime state
```

This is intentional.

Text-authoritative reconstruction is the robust default.

A later revision may preserve compatible transient state, but it must not block Revision 1.

---

# 8. Incremental Build System

The term is:

> incremental compilation / incremental build.

Use Ninja as the normal build executor.

## 8.1 F6 behavior

Provide:

```vim
:NvBuildReload
```

Default mapping:

```vim
F6 -> :NvBuildReload
```

The editor uses `QProcess` to run:

```text
ninja game_module
```

or the repository-equivalent target.

## 8.2 Build isolation

Changing game or engine module code must not rebuild:

- Qt;
- Neovim-Qt;
- `nveditor.exe`;
- unchanged engine libraries;
- unrelated reflection outputs.

Use target-scoped include directories, compile definitions, compile options,
and link libraries. Do not use directory-wide umbrella include paths to hide
dependency direction.

Public project headers must compile standalone with only their declared
includes. The bridge ABI headers must have both C and C++ compile-smoke targets.

## 8.3 Translation-unit hygiene

Preserve fast incremental builds:

- keep volatile implementation in `.cpp`;
- avoid giant umbrella headers;
- keep the PCH stable and minimal;
- do not include generated reflection implementation globally;
- generate per-domain reflection files where practical;
- avoid unity/jumbo builds for the frequently edited module unless measurements prove they improve incremental latency;
- declare generated-file dependencies correctly.

Additionally:

- generated reflection and manifest outputs live in the build tree;
- generators write only when content changes;
- do not generate or modify `reflect.h` inside the source tree;
- keep miniaudio, stb implementation bodies, Qt, Windows SDK umbrella headers,
  and other very large third-party headers out of the hot-module PCH;
- isolate third-party C sources from project C++ warning policy;
- a no-op Ninja build must perform no compilation, linking, generation, or
  asset copying;
- editing one implementation file must compile only its declared dependency
  closure and relink the targets that consume it;
- build scripts, caches intended for source control, and generated files must
  contain no absolute path to the Gin checkout.

## 8.4 Build output

Capture stdout and stderr asynchronously through `QProcess`.

Allow only one module build process at a time. If F6 is pressed during a build,
mark one newer build request pending rather than launching parallel linkers.

Assign a monotonically increasing build generation. Only the newest successful
generation may become a reload candidate; ignore stale process completion and
stale copied-DLL paths.

Parse MSVC and clang-style diagnostics into Neovim's quickfix list.

On failure:

```text
old module keeps running
quickfix receives errors
Neovim opens or focuses the first error on request
```

On success:

```text
quickfix is cleared
new module is hot-reloaded
```

No permanent Qt console is required.

---

# 9. Startup and Responsiveness Requirements

Qt must not become an excuse for Unreal-style startup.

## 9.1 Startup order

The editor should become visible progressively:

```text
1. Create QApplication.
2. Show the main splitter window.
3. Start or attach Neovim.
4. Create the native viewport/D3D11 host.
5. Attempt to load the last successful game module.
6. Open the requested file.
7. Parse only the active relevant document.
```

Do not block startup on:

- scanning the entire project;
- parsing all JSON files;
- generating every GUI;
- compiling the game;
- building thumbnails;
- indexing every asset;
- loading every scene;
- restoring a huge editor database.

## 9.2 Broken-project behavior

The editor must remain usable when:

- the module does not compile;
- the module DLL is missing;
- the active JSON buffer is invalid;
- module creation reports failure;
- the reflection schema is unavailable.

Neovim text editing is never conditional on a healthy runtime.

Revision 1 recovers from reported module errors. It cannot guarantee recovery
from an access violation, stack corruption, infinite loop, `abort`, or arbitrary
memory corruption inside an in-process DLL. True fault and hang isolation
requires a future multi-process architecture and is outside this revision.

## 9.3 Lazy work

Only generate a full GUI when the user invokes `:NvEditForm`.

Only parse the active JSON buffer and any directly required source-link index.

Do not maintain a global retained GUI representation for unopened documents.

---

# 10. JSON Document Model

## 10.1 Canonical text

JSON in the Neovim buffer is the canonical authored state.

The literal JSON member `"$schema"` selects the exact authoring schema ID.

The initial fixed scene remains:

```json
{
  "$schema": "nveditor.scene/v1",
  "cube": {
    "color": [0.120, 0.580, 0.950, 1.000],
    "transform": {
      "translation": [0.000, 0.000, 0.000],
      "rotation_degrees": [25.000, 35.000, 0.000],
      "scale": [1.000, 1.000, 1.000]
    }
  }
}
```

A post-processing document may be:

```json
{
  "$schema": "nveditor.post_process/v1",
  "exposure": 1.000,
  "bloom_threshold": 0.800,
  "vignette": 0.150,
  "tint": [1.000, 1.000, 1.000, 1.000]
}
```

## 10.2 Validation

Before applying a manual buffer change or committing a generated GUI edit:

```text
validate complete buffer through vim.json.decode
```

Do not apply partially invalid JSON to runtime state.

Syntax validity is the first gate, not the only gate. Reject duplicate object
keys because they make JSON-path identity and surgical edits ambiguous. After
syntax validation, the module performs schema and semantic validation before
transactionally replacing runtime state.

Use the Tree-sitter source tree or another range-aware editor helper to detect
duplicate keys; `vim.json.decode` alone is not sufficient for that check.

Manual text editing may temporarily make the document invalid; the editor must wait until it becomes valid again.

While text is invalid or rejected semantically, keep the last-known-valid
runtime document active and report diagnostics without blocking Neovim.

Use a small debounce before revalidation after text changes.

## 10.3 Exact source ranges

Semantic JSON decoding is not sufficient for surgical edits.

The editor also needs exact Neovim row/column ranges for fields.

Preferred approach:

```text
Neovim Tree-sitter JSON tree
-> JSON path
-> exact node range
```

Implement a bundled Lua helper that can return:

```text
buffer number
changedtick
JSON path
node type
start row/column
end row/column
visible source text
```

Neovim and Tree-sitter row/column ranges use zero-based UTF-8 byte columns.
Qt logical character indices must never be substituted for these byte columns.

If the JSON Tree-sitter parser is unavailable, Revision 1 may use a constrained fallback for the fixed one-line numeric arrays, but it must report the limitation clearly.

Do not use fragile global string replacement.

## 10.4 Buffer change tracking

Every edit transaction stores:

```text
buffer number
changedtick
schema ID
document ID
object ID
JSON path
source range
original runtime value
original textual value
```

If the buffer changes unexpectedly while a transaction is active:

- cancel the transaction;
- close the popup;
- do not patch a stale range;
- revalidate the newest buffer snapshot;
- if it is valid, apply that newest snapshot;
- otherwise, reapply the cached last-known-valid document.

Do not blindly restore one original field after a newer document has already
been applied; that can overwrite the user's newer text-authoritative value.

If the RPC text patch fails or returns an unexpected changedtick, use the same
cancel-and-rehydrate path.

## 10.5 Document snapshot ABI

`Author_Document_State` is a complete borrowed UTF-8 snapshot, not a retained
generic JSON DOM.

Conceptually:

```c
typedef struct Author_Object_Source
{
    Author_Object_Id object_id;
    String_View source_id; // JSON `id`; empty for the document root.
    String_View json_path; // `$` for the document root.
} Author_Object_Source;

typedef struct Author_Document_State
{
    uint32_t struct_size;
    uint32_t flags;

    Author_Document_Id document_id;
    Author_Object_Id root_object_id;
    uint64_t revision;

    String_View schema_id;
    String_View project_relative_path;
    String_View json_utf8;

    const Author_Object_Source* objects;
    uint32_t object_count;
    uint32_t reserved;
} Author_Document_State;
```

Rules:

- `json_utf8` contains one complete syntactically valid document;
- every pointer and view is caller-owned and valid only during
  `apply_document`;
- the module performs schema and semantic validation while converting the
  snapshot to ordinary runtime structs;
- the module copies or converts everything it retains before returning;
- `apply_document` is synchronous and transactional;
- on failure, runtime state for that document remains at its previous
  last-known-valid version;
- `revision` is a host-monotonic snapshot revision, not a pointer or descriptor
  index;
- parsing on accepted document changes and reload is allowed;
- parsing JSON every frame or every preview tick is not allowed;
- `remove_document` releases runtime state associated with one document ID.

The host allocates nonzero opaque `Author_Document_Id` and `Author_Object_Id`
values from an editor-session namespace and preserves them across module
reloads.

The root object is always present in `objects` with JSON path `$`. Revision 1's
fixed scene and post-processing documents may contain only this root object.
Nested groups such as `cube` and `transform` are addressed by field JSON path;
they do not automatically become authored objects.

Later selectable entities receive object IDs only when they have unique,
explicit JSON `id` strings. The host reuses an object's session ID while that
source ID survives edits. Duplicate source IDs reject the snapshot.

Document and object IDs are never:

- pointers;
- descriptor indices;
- JSON array positions;
- runtime entity handles;
- serialized C enum ordinals.

---

# 11. Contextual F4 Transactions

## 11.1 Supported controls

For the fixed cube schema:

- `color` -> Qt color editor;
- `translation` -> three-axis numeric/drag control;
- `rotation_degrees` -> three-axis degree control;
- `scale` -> three-axis numeric/drag control;
- `transform` -> combined translation/rotation/scale control;
- `cube` -> color plus full transform control.

For schema-generated fields, the control type comes from authoring metadata.

## 11.2 Popup placement

The popup is a child or tool window anchored to the Neovim cursor rectangle.

It must:

- appear beside the text cursor;
- remain inside a usable screen area;
- close or reposition when the editor scrolls;
- return focus to Neovim after accept/cancel;
- support Escape to cancel;
- support Enter to accept where appropriate.

## 11.3 Transaction flow

```text
OPEN
    validate JSON
    resolve JSON path and source range
    resolve author field descriptor
    extract visible text value
    hydrate runtime from current document
    read engine-validated runtime value
    snapshot original value

PREVIEW
    Qt control changes
    -> preview_author_field
    -> live runtime memory changes
    -> next viewport frame shows result
    -> no text edit yet

ACCEPT
    read final validated runtime value
    format according to author schema
    verify buffer changedtick is still valid
    patch only the selected JSON node through nvim_buf_set_text
    preserve normal Neovim undo
    close popup

CANCEL
    if changedtick is unchanged, restore original runtime value
    otherwise use the cancel-and-rehydrate path from Section 10.4
    close popup
    do not edit text
```

## 11.4 Undo

A single accepted interaction must create one logical Neovim undo step.

For a combined transform control:

- either patch one enclosing transform object in one `nvim_buf_set_text` call; or
- join multiple array replacements into one undo block.

Pressing `u` must restore the prior text.

After undo produces a valid document, the runtime must rehydrate from the new text.

---

# 12. Full JSON-to-GUI Generation

## 12.1 Purpose

The active JSON buffer may be projected into a complete temporary Qt form.

Example:

```text
post_process.json
-> generated Qt form
    Exposure
    Bloom Threshold
    Vignette
    Tint
```

The generated form and Neovim buffer are two views of the same document.

The form is not saved.

## 12.2 Invocation

```vim
:NvEditForm
```

Default:

```vim
Shift-F4
```

## 12.3 Presentation

Revision 1 may use one of these, in priority order:

1. A temporary modeless `QDialog` or tool window.
2. A temporary second tab or stacked page inside the left editor region.
3. A transient splitter inserted beside the Neovim widget.

Do not add a permanent third editor region.

Choose the least invasive implementation compatible with Neovim-Qt.

## 12.4 Generation rules

The generator consumes:

```text
valid active JSON document
+ matching Author_Schema
```

It may use:

- `QFormLayout`;
- `QScrollArea`;
- collapsible object groups;
- `QDoubleSpinBox`;
- `QSlider`;
- `QCheckBox`;
- `QComboBox`;
- `QColorDialog` or custom color widget;
- custom Vec2/Vec3/Vec4 controls;
- custom transform control.

Do not infer important semantics from array length alone.

Examples:

```text
[1, 2, 3] could be position, Euler rotation, scale, or arbitrary data.
```

The authoring schema decides:

- type;
- semantic;
- units;
- range;
- step;
- widget;
- grouping;
- runtime binding.

## 12.5 Synchronization

Text to GUI:

```text
Neovim buffer changes
-> debounce
-> validate
-> refresh document snapshot
-> update or rebuild generated controls
-> apply current valid document to runtime
```

GUI to text:

```text
control preview
-> runtime only

control accept
-> one surgical Neovim buffer edit
-> normal buffer-change path refreshes the form
```

Prevent feedback loops with explicit transaction flags.

## 12.6 Scale limits

Do not generate thousands of controls for a huge scene file.

Each authoring schema may declare:

```text
full_form_supported
context_edit_supported
viewport_gizmo_supported
```

Large scene documents should primarily use:

- text navigation;
- search;
- viewport selection;
- contextual controls;
- focused generated forms for one selected object.

---

# 13. Compile-Time Reflection and Authoring Schema

## 13.1 Gin reflection seed

Extract and extend the focused reflection/code-generation pattern proven in
Gin.

Gin's current engine-owned reflection tool scans explicit markers and emits
plain enum metadata. It is a useful seed, but it does not yet provide authored
struct fields, JSON paths, widget semantics, ranges, stable field IDs, or
schema tables.

Kallam owns the resulting reflection tool. Extend it narrowly to emit the
authoring metadata required here.

The tool must:

- remain a small external C++ program;
- use the engine's lowercase core types where useful;
- produce deterministic output;
- diagnose malformed annotations, duplicate IDs, and collisions;
- write generated output into the build tree;
- update output files only when content changes;
- write atomically so an interrupted generation cannot leave a partial header;
- declare exact source and generator dependencies to CMake/Ninja.

Do not restore Haikal as an active dependency.

Do not rewrite reflection in Rust, Zig, or Qt.

Do not use Qt's meta-object system as engine reflection.

## 13.2 Generated metadata

Compile-time reflection should generate small plain runtime descriptor tables.

Conceptual types:

```c
typedef uint64_t Author_Field_Id;
typedef uint64_t Author_Object_Id;

typedef uint32_t Author_Field_Kind;

enum
{
    Author_Field_F32,
    Author_Field_I32,
    Author_Field_Bool,
    Author_Field_Vec2,
    Author_Field_Vec3,
    Author_Field_Vec4,
    Author_Field_Color3,
    Author_Field_Color4,
    Author_Field_Euler_Degrees3,
    Author_Field_Transform,
    Author_Field_Enum,
    Author_Field_String,
    Author_Field_Asset_Reference
};

typedef uint32_t Author_Field_Semantic;

enum
{
    Author_Semantic_None,
    Author_Semantic_Color,
    Author_Semantic_Position,
    Author_Semantic_Rotation,
    Author_Semantic_Scale,
    Author_Semantic_Transform,
    Author_Semantic_Angle,
    Author_Semantic_Percentage,
    Author_Semantic_Asset
};

typedef struct Author_Transform_Value
{
    float translation[3];
    float rotation_degrees[3];
    float scale[3];
} Author_Transform_Value;

typedef struct Author_Value
{
    Author_Field_Kind kind;
    uint32_t reserved;

    union
    {
        float f32[4];
        int32_t i32;
        uint32_t boolean;
        int64_t enum_value;
        String_View string;
        Author_Transform_Value transform;
    } data;
} Author_Value;

typedef struct Author_Field_Desc
{
    Author_Field_Id id;

    String_View name;
    String_View json_path;
    String_View group;

    Author_Field_Kind kind;
    Author_Field_Semantic semantic;

    float minimum[4];
    float maximum[4];
    float step[4];

    uint32_t flags;
} Author_Field_Desc;

typedef struct Author_Schema_View
{
    String_View schema_id;
    uint32_t schema_version;

    const Author_Field_Desc* fields;
    uint32_t field_count;

    uint32_t flags;
} Author_Schema_View;
```

The exact generator internals may differ, but the ABI must expose equivalent
semantics through this plain procedural boundary.

`schema_id` is the exact, case-sensitive string accepted in JSON `$schema`, for
example `nveditor.scene/v1`.

Schema rules:

- every schema ID is unique within one module;
- duplicate schema IDs reject a candidate DLL;
- schema enumeration order is not stable and must never be serialized;
- the editor enumerates and deep-copies every schema, field table, and nested
  string before the DLL can unload;
- if the `/vN` suffix and numeric `schema_version` are both retained, they must
  agree;
- a serialized shape or meaning change requires a schema-version bump.

Field identity rules:

- field ID zero is invalid;
- a field ID is unique within one schema ID;
- it remains stable across builds and hot reload for the same authored meaning;
- derive it from a stable reflection key or explicit override;
- never derive it from descriptor order, memory address, current array index,
  or a mutable C enum ordinal;
- collisions are generator errors, not runtime tie-breaks;
- a source/member rename either intentionally changes identity or supplies an
  explicit legacy stable key.

Persistent field lookup is always the pair `(schema_id, field_id)`.

## 13.3 What metadata generates

The authoring schema should eventually drive:

- F4 widget selection;
- full-form generation;
- validation;
- numeric ranges;
- units;
- formatting;
- viewport gizmo selection;
- JSON path lookup;
- runtime getter/setter dispatch;
- future asset cooking;
- future machine-readable schema export for LLM tooling.

## 13.4 Hot reload

Generated descriptors live in `game_module.dll`.

After hot reload:

- old descriptor pointers are invalid;
- the editor must discard or replace them;
- descriptors are copied into editor-owned memory or re-queried;
- active transactions are cancelled;
- stable numeric field IDs are used to re-resolve fields.

Compile-time generation does not prevent hot reload.

The new DLL simply supplies a new descriptor table.

## 13.5 Scope

Reflect only authored fields.

Do not expose:

- GPU handles;
- transient AI state;
- physics contacts;
- allocator internals;
- raw D3D objects;
- derived matrices;
- temporary debug values unless explicitly authored.

Keep runtime-only debug UI separate if it is later needed.

---

# 14. Viewport Selection and Gizmos

This is a later milestone, but the architecture must allow it.

## 14.1 Source identity

Authored entities require stable source IDs:

```json
{
  "id": "tower_archer_17",
  "transform": {
    "translation": [4.000, 2.000, -8.000],
    "rotation_degrees": [0.000, 90.000, 0.000],
    "scale": [1.000, 1.000, 1.000]
  }
}
```

Runtime maintains:

```text
Author_Object_Id
<-> runtime entity handle
```

This is a source link, not a full reflection/object system.

## 14.2 Viewport selection

Clicking an authored runtime entity:

```text
viewport pick
-> runtime entity
-> Author_Object_Id
-> editor resolves matching JSON object
-> Neovim opens/jumps to its range
```

Generated runtime objects without authored identity remain runtime-only and do not need a text location.

## 14.3 Gizmo transaction

Viewport gizmos use the same transaction model as F4 controls:

```text
mouse down on gizmo
-> snapshot runtime transform and source range

drag
-> direct runtime preview
-> viewport updates immediately

release
-> read engine-validated transform
-> patch JSON transform
-> one Neovim undo step

Escape/cancel
-> restore runtime snapshot
-> no text edit
```

Gizmos are rendered by the engine in the D3D11 viewport.

Qt only forwards input and coordinates.

---

# 15. Runtime Data and Cooked Binary Assets

## 15.1 Runtime memory

The engine runs on ordinary binary C/C++ structures.

JSON is not read every frame.

## 15.2 Editor flow

```text
valid JSON buffer
-> reject duplicate keys
-> wrap complete UTF-8 text and source identities in Author_Document_State
-> game_module apply_document
-> schema-specific semantic validation and decode
-> ordinary runtime structs
```

The host does not retain a second generic JSON object graph. The module may use
a small JSON parser during document application, but it must not expose that
parser representation as runtime state.

## 15.3 Shipping flow

Later:

```text
authored JSON
-> asset_cooker.exe
-> explicit versioned binary asset
-> game.exe loads binary
```

Do not use a generic binary-JSON format merely to avoid designing runtime data.

CBOR/MessagePack may be considered for IPC in a future multi-process editor, but they are not required for Revision 1.

Do not `fwrite()` arbitrary C++ structs containing pointers or compiler-dependent layout.

Cooked binary formats must be explicit and versioned.

## 15.4 Asset and GPU realization boundary

Separate:

```text
source/cooked bytes
-> validated CPU asset data
-> renderer-specific GPU realization
```

Mesh, image, animation, material, and scene decoders must be testable without a
D3D11 device. D3D11 upload code consumes validated CPU data and produces
module-owned GPU resources.

Revision 1 loads only the assets required by the current example and active
document. Do not reproduce Gin's eager load-all startup path.

Runtime shader compilation is allowed for early development and diagnostics.
The shipping path must eventually accept offline-compiled, versioned shader
artifacts; runtime compilation must not become the only release strategy.

---

# 16. Qt Removal from Final Release

Qt is completely removable from the game build if target boundaries are obeyed.

```text
nveditor.exe
    links Qt

game_module.dll
    does not link Qt

game.exe
    does not link Qt
```

The same game module API should be usable from:

- the Qt editor host;
- the plain Win32 standalone host;
- the shipping host where appropriate.

CMake or the chosen build system must make accidental Qt linkage into engine/game targets impossible.

Example policy:

```text
target: module_bridge    -> no Qt
target: engine_core      -> no Qt
target: engine_runtime   -> no Qt
target: render_d3d11     -> no Qt
target: game_logic       -> no Qt
target: game_module      -> no Qt
target: standalone_host  -> no Qt
target: game             -> no Qt
target: editor_host      -> no Qt
target: editor_qt        -> Qt allowed
target: nveditor         -> Qt allowed
```

---

# 17. Repository Layout

Recommended layout:

```text
/
├── editor/
│   ├── qt/
│   │   ├── editor_main_window.*
│   │   ├── engine_viewport_widget.*
│   │   ├── context_popup.*
│   │   ├── generated_form.*
│   │   ├── build_controller.*
│   │   └── qt_value_widgets.*
│   │
│   ├── neovim/
│   │   ├── neovim_qt_fork_or_vendor/
│   │   ├── editor_neovim_adapter.*
│   │   └── runtime/
│   │       └── nveditor.lua
│   │
│   ├── host/
│   │   ├── editor_host.*
│   │   ├── frame_scheduler.*
│   │   └── module_loader.*
│   │
│   └── win32/
│       └── d3d11_viewport_host.*
│
├── bridge/
│   ├── game_api.h
│   └── author_schema.h
│
├── engine/
│   ├── core/
│   │   ├── core_types.*
│   │   ├── core_templates.*
│   │   ├── string_view.*
│   │   └── arena.*
│   ├── platform/
│   │   └── win32/
│   ├── render/
│   │   ├── render_api.*
│   │   └── d3d11/
│   ├── runtime/
│   └── assets/
│
├── game/
│   ├── game_module.*
│   └── authored types
│
├── reflection/
│   ├── reflection_tool.*
│   └── author annotations
│
├── cooker/
│   └── future asset cooker
│
├── examples/
│   ├── cube.scene.json
│   └── post_process.json
│
├── tests/
│   ├── core/
│   ├── bridge/
│   ├── document/
│   └── render/
│
├── cmake/
├── docs/
│   ├── spec_1.md
│   └── gin_reuse.md
│
├── CMakeLists.txt
└── CMakePresets.json or one equivalent configure entry point
```

Generated Ninja files, reflection output, test binaries, copied hot DLLs, and
cooked assets belong under ignored build/runtime directories, not in the source
tree.

Kallam has no existing engine source tree to preserve. Do not wholesale-copy
Gin and then rename it. Extract audited primitives into Kallam-owned modules
with the dependency direction and tests required by this specification.

---

# 18. Gin Reference and Extraction Policy

## 18.1 Reference baseline

The Gin baseline for Revision 1 is commit:

```text
c6090099b01b3610cddbb14a6dc7c61d593f6dc9
```

The local checkout may be used for implementation research, but no Kallam
target, generated file, manifest, cache committed to source control, or build
script may depend on its absolute filesystem path.

Record each material extraction in `docs/gin_reuse.md`:

- Gin source file and baseline commit;
- concept or code extracted;
- Kallam destination module;
- game-specific assumptions removed;
- behavior intentionally changed;
- characterization and replacement tests added.

Extract the smallest coherent primitive. Do not import an entire subsystem
merely because one helper is useful.

## 18.2 Candidate concepts to carry forward

The following Gin concepts are approved starting points, subject to the rest of
this specification:

- fixed-width core types and explicit string views;
- lowercase `array<T>` and `vec<T>` arena-backed templates;
- explicit permanent, transient, and scene/level lifetime categories;
- free-function subsystem APIs with plain state structs;
- fixed-step simulation separated from rendering;
- explicit gameplay/simulation phase ordering;
- Win32 and D3D11 creation, resize, teardown, and validation patterns;
- one runtime backend shared by game and authoring projections;
- focused compile-time reflection/code generation;
- manifest-driven resource metadata and typed handles;
- build-time validation for missing assets and duplicate generated IDs;
- incremental asset staging and stale-output pruning.

These are patterns, not blanket source approval.

## 18.3 Explicit non-import list

Do not import or reproduce:

- Gin's `gin.c`, `gops.c`, `fxops.c`, or `fxed.c` orchestration wholesale;
- Gin gameplay, levels, resources, characters, shaders, or VFX content as
  default Kallam engine content;
- the custom immediate-mode `guiops` editor path;
- process-global input, viewport dimensions, world state, or resource arrays;
- cyclic gameplay/render/resource/audio dependencies;
- ordinal resource array indices as persistent authored identity;
- the current Saha arena implementation;
- the combined `dx11ops_state` as one host/module boundary;
- the monolithic single-executable CMake target;
- broad directory-wide include paths;
- a PCH containing large third-party implementations;
- eager load-all resource startup;
- runtime-only shader compilation as the sole shipping shader path;
- source-tree-generated reflection output;
- obsolete build scripts, legacy OpenGL residue, or inactive Haikal build
  integration.

## 18.4 Renderer extraction boundary

Gin's D3D11 code combines two lifetimes that Kallam must separate:

```text
stable host lifetime
    device
    immediate context
    DXGI factory
    HWND swap chain
    back buffer and depth target
    resize and Present

hot module lifetime
    game shaders and pipeline state
    vertex/index/constant buffers
    game textures and material resources
    post-processing intermediates
    debug rendering and gizmo resources
```

Extract helper logic only after assigning it to one of these owners.

Asset decoding/import data must also remain separable from GPU realization so
loaders and schema tests can run headlessly.

## 18.5 Resource identity

Generated typed resource handles are allowed for fast runtime lookup, but their
table position is not a durable authored ID.

Requirements:

- manifests use project-relative paths;
- generated identifiers are deterministic;
- duplicate inferred IDs are build errors;
- runtime registries are instance-owned;
- handles include validation and, where reuse is possible, a generation;
- serialized source identity uses explicit stable strings or IDs, not a C enum
  ordinal;
- decoding, validation, and GPU upload are distinct stages.

---

# 19. Automated Verification

Use CTest through Ninja for first-party automated tests.

Revision 1 requires test coverage for:

## 19.1 Core memory and containers

- arena alignment and commit growth;
- multiplication, alignment, and pointer-arithmetic overflow;
- reserve/commit exhaustion and reported OOM;
- nested temporary-scope LIFO validation;
- frame reset and scene reset;
- vector growth and shrink-copy bounds;
- repeated initialization/destruction and VM release.

## 19.2 Frame scheduler and input

- zero-tick frames retain input edges;
- press and release between ticks are both observed;
- multi-tick catch-up consumes edges only once;
- focus loss synthesizes release state;
- elapsed-time clamp and catch-up cap;
- backlog reset after reload, pause, and debugger-sized stalls;
- interpolation alpha range;
- editor-host and standalone-host scheduler parity.

## 19.3 ABI and reload

- public ABI headers compile as C and C++;
- expected structure sizes, alignments, and offsets;
- API version, table-size, and required-pointer validation;
- successful copied-DLL reload from module A to module B;
- rejection of a missing entry point, incompatible API, create failure, invalid
  schema, and document-apply failure while module A continues running;
- editor-owned schema copies remain valid after the old DLL unloads;
- repeated create/destroy/reload cycles leave no module thread, callback,
  allocation, or COM resource alive.

## 19.4 Documents and reflection

- deterministic reflection output;
- write-if-content-changed behavior;
- malformed annotations and duplicate/colliding IDs fail generation;
- unknown schema, duplicate JSON key, duplicate source ID, invalid JSON, and
  out-of-range values leave last-known-valid runtime state active;
- Unicode preceding an edited field does not corrupt UTF-8 byte ranges;
- changedtick races and RPC patch failure take the cancel-and-rehydrate path;
- one accepted control transaction remains one Neovim undo step.

## 19.5 Rendering and dependency firewalls

- hidden-window D3D11 WARP smoke test for create, render, resize, and destroy;
- resize generation invalidates target-sized module resources;
- repeated reload/resize under the D3D11 debug layer reports no live-object
  leaks attributable to Kallam;
- project public headers compile standalone;
- include/firewall checks reject Qt in engine/game and D3D11 in simulation
  headers;
- binary import inspection proves `game.exe` and `game_module.dll` do not import
  Qt libraries;
- a clean generated build leaves the source tree unchanged;
- a no-op Ninja build performs no work.

Every milestone adds or updates its automated tests. The final manual acceptance
test remains required; automation does not replace the integrated editor check.

---

# 20. Required Milestones

## Milestone 0 - Repository and engine foundation

Deliver:

- clean Kallam repository and ignored out-of-source build/runtime directories;
- one documented CMake/Ninja configure and build path using `clang-cl`;
- target DAG and include firewalls from Section 4;
- engine-owned core types, string view, lowercase array/vector templates, and
  safe Windows arena foundation;
- C-compatible bridge ABI headers with C and C++ layout smoke tests;
- CTest integration and passing core/ABI tests;
- `docs/gin_reuse.md` provenance record for any extracted implementation;
- no source or build dependency on the local Gin checkout.

## Milestone 1 - Build Neovim-Qt unchanged

Deliver:

- pinned upstream revision;
- successful Windows build;
- Neovim launches;
- normal keyboard/mouse editing works;
- current user configuration can load.

## Milestone 2 - Two-region Qt shell

Deliver:

- `QSplitter`;
- Neovim on the left;
- empty native viewport widget on the right;
- no permanent extra panels;
- correct focus switching.

## Milestone 3 - Native D3D11 viewport

Deliver:

- editor-owned D3D11 device/context;
- child-HWND swap chain;
- resize handling;
- render-target generation tracking;
- clear color or triangle rendered;
- hidden-window WARP smoke test;
- Neovim remains responsive.

## Milestone 4 - Hot-loaded C/C++ module

Deliver:

- `game_module.dll`;
- plain `Game_API`;
- reported create/fixed-update/render/destroy contract;
- shared bounded fixed-step scheduler and latched input semantics;
- module renders a cube;
- module loads through a copied hot filename;
- candidate API/create/schema/apply failure leaves the old module running;
- module-created GPU resources are released before unload.

## Milestone 5 - Incremental build and F6 reload

Deliver:

- `QProcess` runs Ninja;
- only changed translation units compile;
- module relinks;
- quickfix receives compiler errors;
- successful build reloads the module;
- rapid F6 requests coalesce and stale build generations cannot reload;
- editor and Neovim stay alive.

## Milestone 6 - Fixed JSON scene application

Deliver:

- open `examples/cube.scene.json`;
- validate through `vim.json.decode`;
- reject duplicate keys and preserve last-known-valid state;
- send a complete borrowed `Author_Document_State` snapshot;
- apply to module;
- cube color/TRS reflect JSON.

## Milestone 7 - Contextual F4 Qt controls

Deliver:

- cursor-to-JSON-path resolution;
- cursor-anchored popup;
- color and TRS controls;
- direct runtime preview;
- accept patches only selected JSON node;
- cancel restores runtime;
- `u` restores prior text and runtime follows.

## Milestone 8 - Compile-time author schema

Deliver:

- Gin's enum-reflection seed is extended to emit author descriptors;
- generated output is deterministic and remains in the build tree;
- descriptors for scene and post-process schemas compile into the module;
- editor enumerates and deep-copies every schema;
- F4 dispatch uses descriptors rather than hard-coded cube switches where practical;
- descriptors refresh after hot reload.

## Milestone 9 - Full generated post-process form

Deliver:

- open `examples/post_process.json`;
- Shift-F4 opens temporary generated form;
- fields generated from schema;
- GUI and text stay synchronized;
- direct runtime preview;
- no generated GUI file saved.

## Milestone 10 - Viewport selection and transform gizmo

Deliver:

- authored object ID;
- viewport selection jumps Neovim to JSON object;
- transform gizmo previews runtime directly;
- release patches text once;
- undo restores previous transform.

---

# 21. Acceptance Test

The upgraded prototype is accepted when:

1. Launch `nveditor.exe examples/cube.scene.json`.
2. The window appears quickly without scanning the whole project.
3. Neovim accepts normal keyboard and mouse editing in the left region.
4. The right region contains only the live D3D11 cube viewport.
5. Editing valid cube JSON updates runtime after debounce.
6. F4 on `color`, `translation`, `rotation_degrees`, `scale`, `transform`, and `cube` opens the correct Qt control.
7. Dragging controls updates runtime memory and the viewport immediately without rewriting JSON every frame.
8. Accepting patches only the intended JSON node through Neovim RPC.
9. Escape cancels and restores the original runtime value.
10. `u` in Neovim restores the prior text and the viewport follows the restored document.
11. Editing one C/C++ translation unit and pressing F6 compiles only the required object files, relinks the module, and hot-reloads it.
12. A failed compile leaves the previous module running and populates Neovim quickfix.
13. Shift-F4 on `post_process.json` creates a complete temporary generated Qt form.
14. The generated form is derived entirely from the current buffer and schema and is not saved to disk.
15. Closing the editor explicitly releases the game module, D3D11 resources, Neovim connection, and Qt-owned widgets.
16. `game.exe` and `game_module.dll` have no Qt dependency.
17. The full CTest suite passes from a clean out-of-source Ninja build.
18. A no-op Ninja build performs no work and leaves the source tree unchanged.
19. Injected candidate API, create, schema, and document-apply failures each
    leave the previous cube module rendering.
20. Rapid repeated F6 requests never allow an older build generation to replace
    a newer successful module.
21. `game.exe` loads the same module ABI and fixed-step scheduler without Qt or
    Neovim.
22. Repeated module reload and viewport resize cycles complete under the D3D11
    debug layer without Kallam-owned live-object leaks.
23. Binary import inspection and header-firewall tests confirm that Qt is absent
    from engine/game targets and D3D11 is absent from simulation headers.

---

# 22. Explicit Non-Goals

Do not implement in this revision:

- a wholesale Gin fork, rename, or source-tree transplant;
- migration of Gin gameplay, levels, assets, or immediate-mode editor UI;
- bug-compatible preservation of Gin allocator, global-state, or build behavior;
- a Unity-style permanent inspector;
- a generalized retained object editor;
- a Qt object model for engine entities;
- automatic serialization of arbitrary runtime memory;
- a full runtime object graph;
- a plugin marketplace;
- QML;
- Qt Quick;
- WebEngine;
- Python/PySide;
- a node graph;
- an animation curve editor;
- a global asset database;
- project-wide eager indexing;
- multiple game processes;
- isolation from arbitrary in-process module crashes or hangs;
- shared-texture IPC;
- runtime state migration across arbitrary layout changes;
- collaborative editing;
- remote debugging;
- cross-platform viewport support;
- generic unknown-JSON form generation with guessed semantics;
- modern C++ refactoring of the engine.

---

# 23. Final Architecture

```text
+------------------------------------------------------------------+
| nveditor.exe                                                     |
|                                                                  |
| Qt Widgets                                                       |
| +--------------------------------------------------------------+ |
| | QSplitter                                                    | |
| | +--------------------------+ +-------------------------------+ | |
| | | Neovim-Qt               | | Native DX11 Viewport          | | |
| | |                          | |                               | | |
| | | C/C++ source             | | game_module.dll runtime       | | |
| | | JSON authored data       | | selection/gizmos              | | |
| | | contextual Qt popup      | |                               | | |
| | +--------------------------+ +-------------------------------+ | |
| +--------------------------------------------------------------+ |
|                                                                  |
| Build controller -> Ninja -> game_module.dll -> hot reload       |
| Author schema <- compile-time C++ reflection                     |
|                                                                  |
+-------------------------------+----------------------------------+
                                |
                                | plain procedural API
                                v
+------------------------------------------------------------------+
| C-with-templates engine/game module                              |
|                                                                  |
| no Qt                                                            |
| no Neovim                                                        |
| no STL/smart-pointer/exception architecture                      |
| direct D3D11 runtime                                             |
| authored state + reflection descriptors                          |
|                                                                  |
+------------------------------------------------------------------+
```

The core philosophy is:

> Neovim owns text.  
> Qt supplies desktop GUI superpowers.  
> JSON remains the authored truth.  
> Generated GUI is temporary.  
> Runtime preview writes directly to engine memory.  
> Compile-time reflection removes repetitive semantic glue.  
> Ninja and a hot-reloaded DLL keep iteration fast.  
> The shipping engine never knows Qt exists.
