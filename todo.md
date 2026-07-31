# Kalam Neovim-Native Qt Interaction Model

Status: implemented

This document defines the next interaction model for the Qt-hosted Kalam
editor. It supersedes the fixed-key invocation details in `docs/spec_1.md`,
including the default F4, Shift-F4, and F6 mappings. The existing validation,
preview, commit, cancellation, and Neovim undo guarantees remain in force.

## 1. Product model

Kalam is a Qt-hosted Neovim editor with native graphical tools.

Neovim is not a text widget embedded inside a conventional GUI editor. It is
the editing and command authority. Qt is the presentation and graphical-tool
layer around it.

The primary product is the integrated Qt application:

- Neovim supplies modes, mappings, commands, buffers, plugins, terminal
  buffers, registers, macros, undo, and authored text.
- Qt supplies the Neovim frontend, application layout, contextual controls,
  generated forms, graphs, visual editors, and game viewport.
- The engine consumes only validated authored state and supplies previews and
  runtime rendering.

Developing a separate Kalam TUI frontend, porting Qt tools to terminal
presentations, and maintaining GUI/TUI feature parity are out of scope for
this phase.

## 2. Interaction principles

1. **Command first.** Every meaningful Kalam operation has a named Neovim
   command. A fixed keyboard shortcut is never the only way to invoke it.
2. **One action, multiple affordances.** A Neovim command, user mapping, Qt
   button, menu item, and mouse gesture must dispatch the same underlying
   action rather than maintaining separate behavior.
3. **Keyboard complete.** Every authored workflow must be usable without a
   mouse. The mouse remains supported but is never required.
4. **Neovim owns authored state.** Qt never saves or independently rewrites a
   scene document. Accepted GUI changes are applied through the Neovim buffer
   and participate in normal Neovim undo.
5. **Qt follows Neovim visually.** The active Neovim colorscheme and frontend
   font drive the appearance of editor-adjacent Qt surfaces.
6. **Focus is explicit.** Keyboard input belongs to exactly one surface at a
   time, the focused surface is visibly identifiable, and returning to
   Neovim is always predictable.
7. **Transient by default.** Contextual tools appear when requested and
   disappear after accept or cancel. Permanent GUI chrome is added only when
   a workflow genuinely requires it.

## 3. Canonical Neovim command surface

Kalam exposes one discoverable command namespace:

```vim
:Kalam edit context
:Kalam edit form
:Kalam build reload
:Kalam focus editor
:Kalam focus viewport
:Kalam toggle viewport
:Kalam play
:Kalam stop
```

The exact set may grow, but it must obey these rules:

- `:Kalam` with no arguments lists available operations and short
  descriptions.
- Arguments have Neovim command-line completion.
- Invalid or unavailable operations produce a concise Neovim notification.
- Commands are safe to call from user mappings, Lua configuration, scripts,
  and Qt actions.
- Commands dispatch stable internal action identifiers so invocation source
  does not alter behavior.

The existing commands `:NvEditContext`, `:NvEditForm`, and `:NvBuildReload`
may remain temporarily as compatibility aliases. New documentation and tests
use the `:Kalam` namespace.

### 3.1 Mappings

Kalam does not claim F4, Shift-F4, F6, a leader sequence, or another global
user key by default.

The plugin exposes documented `<Plug>` mappings for its public actions. Users
may bind those mappings to F-keys, leader sequences, chords, or no keys at
all. Any optional convenience mappings must be explicitly enabled by
configuration and must remain remappable.

Removing the default mapping does not remove keyboard access: command-line
invocation and user-configured mappings are first-class interaction paths.

## 4. Focus and keyboard contract

### 4.1 Neovim focus

Neovim owns keyboard input during ordinary editing. Qt surfaces must not
consume editing keys, application-wide shortcuts, or mouse-wheel events when
they are not focused.

Invoking a Kalam command captures the current Neovim window, buffer, cursor
position, and rendered cursor rectangle before opening a contextual surface.

### 4.2 Neovim-owned context sessions

Opening a contextual editor creates a real temporary Neovim buffer and window.
That staging buffer, not Qt, owns keyboard focus and editing state:

- Normal, Insert, command-line, undo, redo, registers, mappings, and motions
  are native Neovim behavior;
- `j` and `k` move between one-field-per-line staging rows, while `h` and `l`
  move within the staged JSON text;
- `i`, `a`, and the user's normal mappings enter or manipulate Insert mode;
- `Escape` and `Ctrl-[` return to Normal mode and do not cancel the session;
- the context window has a real Neovim statusline and command line;
- `:w` validates and applies the staged values, then keeps the session open;
- `:wq` applies and closes; `:q` closes only when clean; `:q!` discards;
- closing returns to the originating Neovim window and buffer.

The Qt surface is a keyboard-passive graphical projection of this buffer. It
shows typed controls, accepts mouse manipulation, and mirrors Neovim's mode,
cursor, and active field. Mouse changes write through to the same staging
buffer so Neovim undo and modified-buffer rules remain authoritative. No
keystroke is independently interpreted by both Neovim and Qt.

### 4.3 Persistent viewport and game focus

The viewport is visible independently of keyboard ownership.

- While Neovim is focused, ordinary keys continue to operate Neovim.
- `:Kalam focus viewport` transfers input to viewport navigation.
- `Escape` leaves viewport navigation and returns focus to Neovim.
- The viewport captures camera or game input only while explicitly focused or
  in an explicit play-capture state.
- Play capture must always have a visible, configurable escape path.
- `:Kalam focus editor` restores Neovim focus from any non-modal Kalam
  surface.

The editor must never require clicking the viewport merely to make a
document-level command available.

## 5. Contextual and generated Qt surfaces

Contextual editors occupy the text-grid area of their temporary Neovim window
inside the Qt-hosted application. The Qt body is projected over that grid,
while the real Neovim statusline and command-line area remain visible.

They must:

- preserve enough text-grid space for the Neovim statusline and command line;
- remain within the usable Kalam window or screen area;
- use a Neovim-style border, title, and focus treatment;
- track their staging window geometry, cursor, mode, and active field;
- leave keyboard operation to the underlying Neovim context buffer;
- preserve the existing preview, accept, cancel, and changedtick checks.

Generated full-document forms, graphs, and visual editors may use a larger
overlay, temporary tab, or temporary split. They follow the same command,
focus, theme, transaction, and dismissal rules as small contextual editors.

Native operating-system dialogs are exceptions only where Qt cannot
reasonably provide an integrated surface.

## 6. Theme bridge

Neovim is the source of truth for editor-adjacent Qt styling. Qt resolves and
maps at least the following highlight roles:

| Qt role | Preferred Neovim highlight |
| --- | --- |
| Base text and background | `Normal` |
| Transient surface | `NormalFloat` |
| Border and separator | `FloatBorder`, then `WinSeparator` |
| Active selection | `PmenuSel`, then `Visual` |
| Inactive row or hover | `Pmenu`, then `CursorLine` |
| Active header or status | `StatusLine` |
| Inactive header or status | `StatusLineNC` |
| Error, warning, information, hint | matching `Diagnostic*` groups |

Requirements:

- linked highlight groups are resolved before applying the Qt palette;
- unset or transparent colors use documented fallbacks;
- a `:colorscheme` change updates visible Qt surfaces without restarting
  Kalam;
- changes to Neovim background and UI font settings trigger the same refresh;
- textual Qt chrome uses the Neovim frontend font and compatible metrics where
  practical;
- native Windows widget styling must not visibly override the Kalam palette;
- active and inactive panes remain distinguishable in low-contrast themes.

The game framebuffer itself is not recolored to match Neovim. The viewport
chrome, empty background, overlays, graphs, diagnostics, and editor gizmos may
use the shared palette.

The goal is for Qt surfaces to look like graphical extensions of the active
Neovim theme, not to force every graphical control into a character-cell
imitation.

## 7. Authoring and undo contract

The existing transaction model remains authoritative:

1. Neovim validates the active document and resolves cursor context.
2. Neovim creates a temporary staging buffer with one typed field per line.
3. Keyboard edits act directly on that buffer; Qt mouse changes write through
   to it and therefore participate in its native undo history.
4. Valid staged values preview through the runtime without editing or saving
   the source document.
5. `:w` or `:wq` applies one surgical source-buffer change through Neovim.
6. One accepted write creates one logical source-buffer undo step.
7. `:q!` or another cancel path restores runtime state and makes no source
   text edit.
8. A changedtick or context mismatch blocks a stale commit and reports the
   conflict without overwriting newer text.

Mouse manipulation, keyboard manipulation, viewport gizmos, contextual
controls, and generated forms all use this same transaction contract.

## 8. Non-goals

This interaction revision does not require:

- pixel-identical reproduction of every Neovim or terminal cell;
- a separate Kalam TUI frontend;
- terminal versions or fallbacks for Qt graphical tools;
- GUI/TUI feature parity;
- an external Qt window positioned over a separate terminal;
- permanent inspector, asset-browser, or console docks;
- Qt becoming an independent JSON editor;
- hardcoded cube-specific shortcuts;
- removing mouse support.

## 9. Acceptance criteria

- [x] Starting Kalam places keyboard focus in Neovim.
- [x] Ordinary Neovim modes, mappings, plugins, splits, and terminal buffers
      continue to work without Qt shortcut interference.
- [x] `:Kalam` exposes and completes the supported action namespace.
- [x] Context edit, generated form, build/reload, and focus operations work
      without default F-key mappings.
- [x] F4, Shift-F4, and F6 are unclaimed unless the user explicitly maps or
      enables them.
- [x] Every shipped Qt authoring surface can be opened, operated, accepted,
      cancelled, and dismissed using only the keyboard.
- [x] `Escape` returns a context buffer from Insert to Normal mode; explicit
      viewport capture still returns to Neovim without modifying the buffer.
- [x] Context editing uses a real Neovim staging buffer with native motions,
      modes, command line, modified-buffer protection, and undo/redo.
- [x] Context `:w`, `:wq`, `:q`, and `:q!` have native Neovim-style semantics.
- [x] The Qt context body is keyboard-passive and leaves the real Neovim
      statusline and command-line area visible.
- [x] The currently focused Neovim or Qt surface is visually unambiguous.
- [x] Mouse and keyboard invocation of the same operation produce identical
      authored transactions.
- [x] Accept creates one logical Neovim undo step; cancel creates none.
- [x] Qt never writes an authored JSON document directly.
- [x] Changing the Neovim colorscheme updates open Qt surfaces immediately.
- [x] Qt chrome remains legible with both light and dark Neovim themes.
- [x] The viewport receives navigation or game input only while explicitly
      focused or captured.
- [x] Closing a transient surface restores the originating Neovim window,
      buffer, and cursor context.

## 10. Implementation order

- [x] Introduce the shared Kalam action registry and `:Kalam` dispatcher.
- [x] Expose `<Plug>` mappings and remove fixed default F-key mappings.
- [x] Define and test editor, transient-surface, viewport, and play-capture
      focus transitions.
- [x] Make existing contextual and generated controls keyboard complete.
- [x] Add the Neovim-highlight-to-Qt theme bridge with live updates.
- [x] Apply Neovim-style chrome and focus indicators to transient surfaces.
- [x] Replace Qt-emulated context editing with a real Neovim staging buffer
      and a keyboard-passive Qt projection.
- [x] Route viewport and remaining Qt actions through the shared action model.
- [x] Update `docs/spec_1.md`, runtime tests, Qt tests, and user documentation
      after this proposal is accepted.
