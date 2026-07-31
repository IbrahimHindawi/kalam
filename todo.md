# Kalam Qt-Owned Context Interaction Model

Status: implemented

This document defines the interaction model for contextual Qt authoring inside
the Qt-hosted Kalam editor. It supersedes the earlier hidden-Neovim-staging
design and the fixed-key invocation details in `docs/spec_1.md`.

## 1. Product model

Kalam is a Qt-hosted Neovim editor with native graphical tools.

- Neovim owns ordinary text editing, commands, source buffers, undo, plugins,
  terminal buffers, and the canonical authored JSON.
- Qt owns graphical tools, including contextual controls, forms, graphs,
  visual editors, and the game viewport.
- The engine consumes validated authored state and supplies previews and
  runtime rendering.

A separate Kalam TUI frontend, terminal ports of Qt tools, and GUI/TUI feature
parity are out of scope for this phase.

## 2. Interaction principles

1. Every meaningful Kalam operation has a named Neovim `:Kalam` command.
2. Every authored workflow is keyboard-complete; mouse access is optional.
3. Keyboard input belongs to exactly one focused surface at a time.
4. Opening a contextual GUI transfers keyboard ownership from Neovim to that
   Qt popup until the popup commits, closes, or is cancelled.
5. The popup owns its temporary values and modal state. It never edits a
   hidden or visible Neovim staging buffer.
6. The source Neovim buffer remains byte-for-byte unchanged during preview.
   Only an explicit commit may make one surgical Neovim buffer edit.
7. Qt styling follows the active Neovim colorscheme and frontend font.

## 3. Command surface

The canonical commands are:

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

Kalam claims no F-key or leader mapping by default. Public actions expose
`<Plug>` mappings so users can choose their own bindings. Compatibility
aliases may remain temporarily.

## 4. Context popup focus and modes

Invoking `:Kalam edit context` captures the source buffer, changedtick, JSON
path, cursor position, and rendered cursor rectangle. It then opens a Qt popup
and gives that popup keyboard focus.

While the popup is open:

- popup keystrokes are consumed by Qt and are not forwarded to Neovim;
- the source Neovim buffer is not used as a staging surface;
- the selected graphical control is always visibly identifiable;
- closing or cancelling returns focus to Neovim.

The popup provides a deliberately small Vim-like modal layer. It is not a
second Neovim instance and does not claim complete Neovim emulation.

### 4.1 Normal mode

The popup opens in Normal mode.

- `j` / `k` move between fields;
- `h` / `l` move between components of a compound field;
- arrow keys provide equivalent movement;
- `i` or `Enter` enters Insert mode on the selected Qt control;
- `:` opens the popup-owned command line;
- unhandled keys remain in the popup and never alter the Neovim buffer.

### 4.2 Insert mode

Insert mode gives focus to the selected real Qt editor: line edit, spin box,
checkbox, or another typed control.

- Typed input is handled by that Qt control.
- Pressing `i` again is ordinary field input; it cannot leak to Neovim.
- `Escape` or `Ctrl-[` returns focus to popup Normal mode.
- Value changes update popup-owned staging and may preview through the engine.
- Preview does not edit the source Neovim buffer.

Qt controls retain appropriate native editing behavior. Implementing every
Neovim text motion, register, mapping, macro, and operator inside individual
Qt fields is a non-goal.

### 4.3 Command mode and bottom statusline

The popup owns its command line and displays it in a statusline that is always
the bottom-most row of the popup.

- `:w` validates and commits the staged values, then keeps the popup open;
- `:wq` commits and closes;
- `:x` commits and closes;
- `:q` closes only when the popup is clean;
- `:q!` discards staged values and closes;
- `Escape` leaves Command or Insert mode without cancelling the popup.

The statusline reports Normal, Insert, Command, dirty, validation, and
write-pending state without relying on a Neovim window beneath the popup.

## 5. Placement and presentation

A contextual popup is a compact child surface inside the Kalam window.

- It is anchored beside the captured Neovim cursor when space permits.
- If the cursor is low, the entire popup is placed above the cursor instead of
  being clamped against the absolute bottom of the editor.
- It stays within the usable editor area.
- Its default bounds do not exceed 520 by 420 logical pixels.
- At least two Neovim text rows are reserved below it so the editor status and
  command area are not obscured.
- Its own statusline remains pinned to the popup bottom.
- It uses Neovim-derived float, border, selection, status, diagnostic, and font
  styling.

Generated forms, graphs, and visual editors may use larger surfaces, but they
follow the same explicit focus, transaction, theme, and dismissal rules.

## 6. Authoring and undo contract

1. Neovim validates the source document and resolves cursor context.
2. Qt creates typed controls initialized from the validated runtime document.
3. Keyboard and mouse edits change only Qt-owned staged values.
4. Valid staged values preview through the runtime without changing source
   text or changedtick.
5. `:w` or `:wq` requests one surgical patch through the Neovim adapter.
6. A commit is rejected if the captured buffer, changedtick, JSON path, or
   byte range is stale.
7. One accepted commit creates one logical Neovim source-buffer undo step.
8. Cancel restores the original runtime values and performs no source edit.
9. A failed write keeps the popup open and preserves staged values when the
   context can be safely refreshed.

Qt does not save the file or independently rewrite JSON. Neovim remains the
canonical source and undo authority even though Qt owns popup interaction.

## 7. Theme bridge

Qt maps editor-adjacent styling from these Neovim highlight roles:

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

Colorscheme, background, and UI-font changes update visible Qt surfaces.
Native widget styling must not override the Kalam palette.

## 8. Non-goals

This phase does not require:

- a separate Kalam TUI frontend or terminal fallbacks for Qt tools;
- a hidden Neovim context staging buffer;
- forwarding the same key to both Neovim and Qt;
- embedding a full Neovim engine inside every Qt field;
- complete Vim operators, registers, macros, mappings, or text objects inside
  native Qt controls;
- an external Qt window positioned over a separate terminal;
- permanent inspector, asset-browser, or console docks;
- Qt directly saving or independently rewriting authored JSON.

## 9. Acceptance criteria

- [x] `:Kalam edit context` opens a compact popup near the source cursor.
- [x] A low source cursor places the popup above it rather than at the screen
      bottom.
- [x] The popup statusline is its bottom-most row and editor-bottom space stays
      visible.
- [x] The popup opens focused in its own Normal mode.
- [x] `j/k` navigate fields and `h/l` navigate compound components.
- [x] `i` enters Insert mode on the selected Qt field.
- [x] Repeated Insert-mode input stays in Qt and cannot edit Neovim.
- [x] `Escape` returns from the field to popup Normal mode.
- [x] `:` opens a Qt-owned command line with `:w`, `:wq`, `:x`, `:q`,
      and `:q!` behavior.
- [x] Preview changes runtime state without changing the source document.
- [x] Commit creates one Neovim patch and cancel creates none.
- [x] Closing the popup returns keyboard focus to Neovim.
- [x] Qt chrome follows live Neovim theme and font changes.
