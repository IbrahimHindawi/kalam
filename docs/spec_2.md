# spec_2.md

# Kalam-Owned Neovim UI Client

## Implementation Specification

Status: implemented

This specification replaces the embedded Neovim-Qt dependency with a
Kalam-owned Neovim UI client. It supersedes `spec_1.md` section 5
("Neovim-Qt Integration") and the Neovim-Qt portions of sections 2 and 3.2.
Everything else in `spec_1.md` — the product principles, the JSON document
model, contextual edit transactions, the viewport, the host/module boundary,
and the coding-style firewall — remains normative and unchanged.

`docs/architecture.md`, `docs/dependencies.md`, `README.md`, and
`PORTABLE_README.md` must be updated when the milestones below land.

# 0. Mission

Remove the pinned, patched Neovim-Qt v0.2.19 checkout and replace it with a
small client owned by this repository that speaks Neovim's versioned,
documented remote UI protocol directly.

After this work:

- Neovim itself remains the only external editor dependency, spawned
  unmodified in `--embed` mode.
- The editor's rendering, input routing, focus ownership, cursor geometry,
  and theming are implemented by Kalam code against the msgpack-RPC UI
  protocol, not by adapting a third-party GUI application.
- The build no longer fetches, verifies, or patches Neovim-Qt or its
  msgpack-c dependency.

The stack changes from:

```text
editor_qt -> patched Neovim-Qt (Shell, NeovimConnector, NeovimApi0) -> nvim --embed
```

to:

```text
editor_qt -> Kalam grid widget + Kalam RPC client -> nvim --embed
```

# 1. Scope and non-goals

In scope for revision 1 of this client:

- msgpack codec, msgpack-RPC transport, process lifecycle.
- Single-grid `ext_linegrid` rendering with correct text, highlights,
  cursor shapes, and scrolling.
- Full keyboard translation, mouse translation, and IME commit strings.
- Cursor pixel geometry as a first-class output (popup anchoring).
- Highlight-table and default-colors delivery to the theme bridge as
  structured events.
- Behavioural parity with the current editor: every existing `:Kalam`
  workflow, the context popup, the generated form, quickfix, and the
  acceptance smoke pass unchanged.

Explicit non-goals for revision 1 (deferred, see section 12):

- `ext_multigrid`, `ext_cmdline`, `ext_popupmenu`, `ext_messages`,
  `ext_tabline`.
- Ligature shaping, smooth scrolling, cursor blink animation, and IME
  preedit overlay rendering.
- Any TUI frontend or non-Windows platform work.

# 2. Component layout

All new code lives in `editor/qt/` and follows the Qt-side style rules of
`spec_1.md` section 4.2. The grid model is deliberately widget-free so it can
be tested headlessly.

```text
editor/qt/neovim_msgpack.h/.cpp      msgpack encode/decode, no Qt widgets, QtCore only
editor/qt/neovim_rpc_client.h/.cpp   process spawn, framing, requests, notifications
editor/qt/neovim_grid_model.h/.cpp   redraw-event state machine, cell storage, damage
editor/qt/neovim_grid_widget.h/.cpp  QWidget painter over the grid model, input capture
editor/qt/editor_neovim_shell.*      reimplemented over the grid widget (same public API)
editor/qt/editor_neovim_adapter.*    reimplemented over the RPC client (same public API)
editor/qt/neovim_theme_bridge.*      consumes structured highlight events (same public API)
```

Dependency direction within the editor:

```text
neovim_msgpack      -> QtCore
neovim_rpc_client   -> neovim_msgpack + QtCore
neovim_grid_model   -> QtCore + QtGui (colors, no widgets)
neovim_grid_widget  -> neovim_grid_model + neovim_rpc_client + QtWidgets
editor_neovim_shell -> neovim_grid_widget
editor_neovim_adapter -> neovim_rpc_client
neovim_theme_bridge -> neovim_rpc_client events
```

## 2.1 Public seams are frozen

The following surfaces keep their current names, signatures, and signal
contracts so that `Editor_Main_Window`, `Author_Transaction_Controller`,
`Context_Popup`, `Generated_Form`, and the build controller compile without
modification:

- `Editor_Neovim_Adapter`: constructor arguments change (it receives the new
  `Neovim_Rpc_Client` instead of `NeovimQt::NeovimConnector`), but every
  public method and every signal listed in `editor_neovim_adapter.h` is
  preserved, including `request_context_for_path`, `patch_text`,
  `set_quickfix`, and the ready/disconnected/context/form/build signals.
- `Editor_Neovim_Shell`: remains the QWidget the main window embeds. It keeps
  `cursor_geometry()` and `cursorGeometryChanged(Cursor_Geometry)`. It stops
  inheriting `NeovimQt::Shell` and becomes a thin subclass or owner of
  `Neovim_Grid_Widget`.
- `Neovim_Theme_Bridge`: keeps its application-facing API and fallback rules;
  only its input changes from scraped Neovim-Qt state to structured
  `hl_attr_define` / `default_colors_set` / `hl_group_set` events.
- `editor/neovim/runtime/lua/nveditor.lua` and every `:Kalam` command are
  untouched. Neovim cannot observe which client renders it.

# 3. msgpack codec

Kalam owns its msgpack implementation. The subset Neovim's RPC actually uses
is small, closed, and testable; carrying pinned third-party msgpack-c for it
is not justified once Neovim-Qt is gone.

Requirements:

- Encode and decode: nil, boolean, all integer widths (positive and negative,
  fixint through int64/uint64), float32/float64, str 8/16/32 and fixstr,
  bin 8/16/32, array and map (fix and 16/32), and ext 8/16/32 plus fixext.
- Ext types are preserved as an opaque `(type, payload)` pair. Neovim encodes
  `Buffer`, `Window`, and `Tabpage` handles as ext; the client treats them as
  opaque tokens and never interprets the payload beyond equality.
- Decoding is incremental: the decoder consumes a byte buffer and either
  yields one complete value plus the number of bytes consumed, or reports
  "need more data" without consuming. Malformed input yields a typed error,
  never an abort or an exception.
- Recursion depth and container sizes are bounded with explicit limits;
  exceeding a limit is a decode error.
- Values map to `QVariant` (`QVariantList`, `QVariantMap` with string keys,
  `QByteArray` for str/bin) so downstream code stays idiomatic Qt.
- Round-trip property: for every supported value, decode(encode(v)) == v.

# 4. RPC client and process lifecycle

`Neovim_Rpc_Client` owns one `QProcess` running `nvim --embed` (executable
name from the existing `--neovim` command-line option) and speaks
msgpack-RPC over its stdio.

- Framing follows the msgpack-RPC spec: `[0, msgid, method, params]` request,
  `[1, msgid, error, result]` response, `[2, method, params]` notification.
  Anything else on the wire is a protocol error and triggers disconnect
  handling.
- Requests return a lightweight handle exposing finished/error signals with
  the decoded result; msgids are monotonically allocated by the client.
  Responses for unknown msgids are logged and dropped.
- Notifications are surfaced as `(name, arguments)` signals; `redraw` is
  routed to the grid model, `nveditor` runtime notifications to the adapter,
  exactly as `handle_notification` consumes them today.
- Startup handshake, in order:
  1. spawn `nvim --embed` with a clean argument list;
  2. `nvim_get_api_info`; require `api_level >= 11` and record the channel id;
  3. `nvim_set_client_info` with name `nveditor`, the application version,
     and type `ui`;
  4. adapter runtime initialization (unchanged);
  5. `nvim_ui_attach(columns, rows, {ext_linegrid = true})` once the shell
     widget has a size.
- Shutdown: `request_neovim_close` maps to `nvim_cmd` `qa` (or `qa!` when
  forced) exactly as today; the client then waits for process exit with the
  existing 2-second escalation to kill in `main.cpp`, which keeps working
  because the client exposes a `processExited(int)` signal in place of
  `NeovimConnector::processExited`.
- Process crash, stdout close, or a protocol error emits
  `disconnected(reason)` once; the adapter forwards it unchanged. The client
  never auto-restarts Neovim; restart policy stays with the main window.

# 5. Grid model

`Neovim_Grid_Model` is a pure state machine: redraw events in, cell state and
damage rectangles out. It holds no widget and does no painting.

- State: one grid (id 1) of cells `{ text: up to one grapheme, hl_id }`,
  grid dimensions, cursor `(grid, row, column)`, the highlight attribute
  table, default colors, current mode list and active mode index, and the
  busy flag.
- Handled events: `grid_resize`, `grid_clear`, `grid_line`, `grid_scroll`,
  `grid_cursor_goto`, `grid_destroy`, `default_colors_set`, `hl_attr_define`,
  `hl_group_set`, `mode_info_set`, `mode_change`, `busy_start`, `busy_stop`,
  `flush`, `option_set`, `set_title`, `set_icon`, `bell`, `visual_bell`,
  `mouse_on`, `mouse_off`.
- Unknown redraw events are ignored and counted, never fatal; the protocol
  explicitly allows new events at existing api levels.
- `grid_line` semantics are implemented exactly: each cell is
  `[text, hl_id?, repeat?]`; a missing `hl_id` inherits the previous cell's;
  a `repeat` count expands runs; an empty `text` marks the right half of a
  double-width cell; the optional `wrap` flag is accepted and ignored in
  revision 1.
- `grid_scroll` moves the cell region in-place; revealed rows arrive as
  subsequent `grid_line` events and are not synthesized.
- Damage: every mutating event accumulates dirty cell rectangles. `flush`
  publishes the accumulated damage and clears it. Nothing repaints before
  `flush`; this is the frame boundary the widget relies on.
- Highlight attributes store the documented rgb attribute keys (foreground,
  background, special, reverse, bold, italic, underline and variants,
  strikethrough, blend). The theme bridge subscribes to the same table; no
  second copy of highlight state exists.

# 6. Grid widget and rendering

`Neovim_Grid_Widget` paints the grid model and owns all user input while the
editor surface has focus.

- Cell metrics: the widget owns a fixed-pitch `QFont` (the frontend font the
  theme bridge already applies). Cell width is the maximum horizontal advance
  over the printable ASCII range; cell height is ascent + descent + leading,
  rounded up to whole device-independent pixels. A non-monospace advance
  spread is reported once as a warning and the maximum is used.
- Resize: widget resizes compute `columns = width / cell_width`,
  `rows = height / cell_height` and issue `nvim_ui_try_resize`. Neovim
  answers with `grid_resize`; the model, not the widget, is authoritative for
  grid dimensions.
- Painting: `paintEvent` draws only the damage published by the last `flush`,
  cell run by cell run — background rectangle first, then text with the
  resolved highlight attributes, honoring reverse video, bold, italic,
  underline variants, and special color. Double-width glyphs draw once across
  two cells.
- Cursor: rendered from `mode_info_set` shape data — block, vertical bar with
  `cell_percentage`, horizontal bar with `cell_percentage` — using the
  cursor-attribute highlight when defined, reverse video otherwise. The busy
  flag hides the cursor. Blink timing values are accepted and ignored in
  revision 1 (cursor is steady).
- DPI: all metrics are computed in device-independent pixels; the widget
  recomputes cell metrics and re-issues `nvim_ui_try_resize` on
  `QEvent::DevicePixelRatioChange` and on font changes.
- `option_set guifont`, when non-empty, is parsed (name and `:h<size>`) and
  applied through the theme bridge's existing font path so `:set guifont`
  keeps working; an unparsable value is reported via `notify_user` and
  ignored.

## 6.1 Cursor pixel geometry

Cursor geometry becomes exact instead of adapted: on `grid_cursor_goto` and
on every metric change, the shell publishes
`Cursor_Geometry{ cell rectangle in widget coordinates, mapped to global }`
through the existing `cursorGeometryChanged` signal. This is the anchor for
`Context_Popup` placement per `spec_1.md` section 11.2 and must be correct
under widget moves, resizes, DPI changes, and font changes — the existing
`moveEvent`/`resizeEvent`/`showEvent` republish behaviour is preserved.

# 7. Input

The widget receives every key first; keyboard ownership rules from `todo.md`
section 2 are unchanged — while a context popup is open, the popup owns the
keyboard and the grid widget receives nothing.

## 7.1 Keyboard

- Keys are translated to `nvim_input` notation and sent as the
  `nvim_input` API call (never `nvim_feedkeys`).
- Special keys map to their `<...>` names: Esc, CR, Tab, BS, Del, arrows,
  Home/End, PageUp/PageDown, Insert, F1–F35, and keypad variants (`<kPlus>`,
  `<kEnter>`, `<k0>`–`<k9>`, ...).
- Modifiers compose as `<C-...>`, `<A-...>`, `<S-...>`, `<D-...>` with the
  documented rules: Shift is included only for special keys or when it does
  not already determine the character; a printable character produced by
  Shift alone is sent bare.
- `<` is escaped as `<lt>`. Bare modifier presses send nothing.
- Dead keys are left to the platform: only composed results reach
  `nvim_input`, via the key event's text or an input-method commit.
- The translation is a single pure function
  `QString input_notation(const QKeyEvent&)` so it is table-testable without
  a widget or a Neovim process.

## 7.2 Mouse

Mouse press, release, drag, and wheel translate to `nvim_input_mouse` with
button, action, modifier string, grid 0, and cell coordinates derived from
the metrics. `mouse_on`/`mouse_off` gate delivery. Focus-click behaviour of
the main window (clicking the editor region focuses Neovim) is unchanged.

## 7.3 IME

`inputMethodEvent` commit strings are sent as literal `nvim_input` text with
`<` escaped. Preedit strings are accepted and, in revision 1, not rendered as
an overlay; `inputMethodQuery` reports the cursor rectangle so the
composition window is positioned at the caret. Preedit overlay rendering is
deferred (section 12).

# 8. Theme bridge

`Neovim_Theme_Bridge` keeps its public API, fallback ordering, and palette
application rules from `todo.md` section 8. Its data source changes:

- Highlight definitions arrive as decoded `hl_attr_define` and
  `hl_group_set` events with the `rgb_attr` maps, plus `default_colors_set`
  for Normal foreground/background/special.
- The bridge no longer issues its own highlight queries during colorscheme
  changes; a `ColorScheme` autocmd in the runtime remains only if a needed
  group is not delivered through `hl_group_set` (to be verified in M4 and
  removed if redundant).
- The existing regression tests for fallback order and palette application
  are preserved; only the fixture format changes from adapter fakes to
  synthetic redraw events.

# 9. Removal plan

The following are deleted once milestone M5 completes, in one change:

- the `kalam_add_neovim_qt` CMake module and the `Kalam::NeovimQt` target;
- the Neovim-Qt v0.2.19 fetch, hash verification, patch application, and
  `editor/neovim/upstream_patch/`;
- the pinned msgpack-c fetch that existed only for Neovim-Qt;
- every `#include` of Neovim-Qt headers (`shell.h`, `neovimconnector.h`,
  `msgpackrequest.h`, ...) and the `NeovimQt` namespace from Kalam sources;
- the Neovim-Qt entries in `docs/dependencies.md` and
  `THIRD_PARTY_NOTICES.md`, replaced by a note that the UI client is
  first-party and the protocol contract is Neovim api_level >= 11.

A firewall test is added: no file under `editor/` may include a Neovim-Qt
header or name the `NeovimQt` namespace. Staging of the Neovim runtime
directory beside `nveditor.exe` is unchanged.

# 10. Testing

All tests run under the existing ctest presets. Real-Neovim tests locate
`nvim` exactly as the current headless document tests do.

- Codec: unit tests for every type family, boundary widths (fixint edges,
  8/16/32 length promotions), ext round-trip, truncated-input resumption,
  malformed-input errors, and the decode(encode(v)) == v property over a
  generated corpus.
- RPC client: against a scripted fake stdio peer — request/response
  correlation, out-of-order responses, unknown msgids, notification
  dispatch, garbage-on-wire disconnect. Against real `nvim --embed` —
  handshake, api_level check, `nvim_ui_attach`, clean `qa` shutdown, kill
  escalation.
- Grid model: golden tests driven by recorded redraw streams captured from
  real Neovim sessions (stored as msgpack fixtures under `tests/qt/`),
  asserting final cell state, damage sets, and cursor position; targeted
  unit tests for `grid_line` hl inheritance and repeat expansion,
  double-width cells, `grid_scroll`, and flush batching.
- Input: table tests over `input_notation` covering the special-key map,
  modifier composition, Shift rules, `<lt>` escaping, and keypad keys;
  mouse translation tests over synthetic events.
- Integration: the existing `--acceptance-smoke` hidden-WARP run must pass
  unmodified — same commands, same screenshot surface. The existing
  adapter, transaction regression, and theme bridge test suites are ported
  to the new fixtures without weakening any assertion.
- Rendering: a headless render of a known redraw fixture into a QImage,
  compared against cell-level expectations (colors and non-space glyph
  positions), tolerant of font rasterization differences.

# 11. Milestones

Each milestone builds green and lands with its tests; Neovim-Qt keeps
working until M5.

- M1 — codec and transport: `neovim_msgpack`, `Neovim_Rpc_Client`,
  fake-peer and real-`nvim` handshake tests. No UI changes.
- M2 — grid model: `Neovim_Grid_Model` with redraw fixtures and goldens.
  Still no UI changes.
- M3 — widget: `Neovim_Grid_Widget` rendering and input, exercised by a
  temporary standalone harness window and the render fixture test.
- M4 — swap: `Editor_Neovim_Shell`, `Editor_Neovim_Adapter`, and
  `Neovim_Theme_Bridge` reimplemented on the new client behind their frozen
  seams; `main.cpp` spawns through `Neovim_Rpc_Client`; full editor test
  suite and acceptance smoke pass on the new stack.
- M5 — removal: delete Neovim-Qt per section 9; add the firewall test;
  update documentation.
- M6 (post-revision-1, optional): `ext_multigrid` + externalized floats,
  `ext_cmdline`/`ext_popupmenu` as native Qt surfaces, preedit overlay,
  cursor blink, ligatures.

# 12. Deferred work

Deliberately out of revision 1, tracked for M6: multigrid and float
externalization, cmdline/popupmenu/messages externalization, IME preedit
overlay, cursor blink and smooth scrolling, ligature shaping, and any
`'guifont'` fallback-list handling beyond a single family.

# 13. Risks

- Text rendering fidelity (double-width, emoji, combining marks) is the
  largest correctness surface. Mitigation: grapheme-per-cell model as
  Neovim already segments cells, render fixtures from real sessions, and
  Neovide/goneovim as reference implementations for edge-case decisions.
- Keyboard translation regressions are user-visible and long-tail.
  Mitigation: the pure translation function with an exhaustive table test,
  grown whenever a report arrives.
- IME behaviour on Windows differs across input methods. Mitigation:
  commit-string-only scope in revision 1, cursor-rect reporting, manual test
  checklist with at least one CJK IME before M5.
- Protocol drift across Neovim releases. Mitigation: api_level floor checked
  at handshake, unknown-event tolerance in the grid model, and one CI run
  against the minimum supported Neovim version.

# 14. Acceptance criteria

Revision 1 is complete when, on a clean checkout with no Neovim-Qt sources
fetched:

1. `cmake --preset windows-clang-editor-debug`,
   `cmake --build --preset windows-clang-editor-debug`, and
   `ctest --preset windows-clang-editor-debug` succeed.
2. `nveditor.exe --acceptance-smoke` passes with the same commands and
   screenshot surface as before the migration.
3. Every `:Kalam` command, the context popup (open, preview, commit,
   cancel), the generated form, quickfix population, and colorscheme-driven
   theming behave identically under manual verification with the user's own
   Neovim configuration.
4. The firewall test proves no Kalam source references Neovim-Qt.
5. `docs/dependencies.md` and `THIRD_PARTY_NOTICES.md` no longer list
   Neovim-Qt or msgpack-c.
