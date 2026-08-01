# Buffer-Hosted Viewport

## Implementation Specification

Status: proposed

The D3D11 viewport becomes a Neovim window. A placeholder buffer named
`kalam://viewport` participates in ordinary Neovim layout — splits, tabs,
`<C-w>` navigation, sessions, `:q` — and the Qt shell positions the native
viewport widget exactly over whichever window shows that buffer. Neovim owns
the layout; Qt owns the pixels.

This specification extends `spec_2.md` (it promotes the deferred
`ext_multigrid` work from section 12 into scope) and supersedes the
fixed two-region splitter layout of `spec_1.md` section 2. The keyboard
ownership rules of `todo.md` section 2 and the interaction principles of
`spec_1.md` section 1 remain normative and unchanged.

# 0. Mission

After this work:

- The editor's central widget is the Neovim grid surface. There is no
  persistent QSplitter layout; every visible region is a Neovim window.
- `:kalam toggle viewport` opens or closes a split showing
  `kalam://viewport`; the engine viewport renders inside that window's
  rectangle and follows every resize, drag, and layout change.
- The grid model understands `ext_multigrid`: one grid per window, exact
  window geometry from `win_pos`/`win_float_pos`, and floating windows
  composited correctly.
- Keyboard ownership is unchanged: Neovim keys travel through the viewport
  window normally; an explicit enter action hands the keyboard to the
  viewport for camera and gizmo control; leave returns it.

# 1. Scope and non-goals

In scope:

- `ext_multigrid` attach and the multigrid event vocabulary in the grid
  model: per-grid cell stores, `win_pos`, `win_float_pos`, `win_hide`,
  `win_close`, `grid_destroy`, `msg_set_pos`, `win_viewport` (accepted,
  ignored).
- Compositing rendering and grid-targeted mouse input in the grid widget.
- Grid-offset-aware cursor geometry (context popup anchoring keeps working
  in any window).
- The `kalam://viewport` buffer contract in `nveditor.lua`.
- The Qt overlay manager that binds the existing `Engine_Viewport_Widget`
  to the viewport window's rectangle.
- Removal of the two-region splitter; smoke updates.

Non-goals (deferred exactly as in `spec_2.md` section 12):

- Rendering floats as native Qt popup widgets (floats are composited into
  the grid surface in this revision).
- `ext_cmdline`, `ext_popupmenu`, `ext_messages`, `ext_tabline`.
- More than one live engine viewport at a time.
- IME preedit overlay, ligatures, cursor blink.

# 2. Multigrid grid model

`Neovim_Grid_Model` generalizes from one implicit grid to a grid table plus
a window registry. Attaching without `ext_multigrid` must keep today's
behavior byte-for-byte; the existing model tests keep passing unmodified.

- Grid store: each grid id owns cells, dimensions, and pending damage.
  `grid_resize`, `grid_line`, `grid_clear`, `grid_scroll`, and
  `grid_cursor_goto` apply to their grid id instead of being filtered to
  grid 1. `grid_destroy` drops the grid.
- Window registry: `win_pos [grid, win, start_row, start_col, width,
  height]` records the grid's placement on the global layout grid (grid 1)
  and the owning window handle. `win_float_pos` records anchor-relative
  placement, resolved against the anchor grid's placement. `win_hide` and
  `win_close` mark the grid unplaced. `msg_set_pos [grid, row, scrolled,
  sep_char]` places the message grid full-width at the given row.
- Window handles arrive as msgpack ext values. The model exposes the
  numeric handle through a codec helper (`msgpack_ext_handle_value`) that
  decodes the integer payload Neovim documents for Buffer/Window/Tabpage
  ext types; this is the only place ext payloads are interpreted, and only
  for equality with window ids reported by the Lua runtime.
- Screen composition: the model answers "which grid, and which grid-local
  cell, is at global cell (row, column)" — topmost float first, then
  placed windows, then grid 1 — and translates per-grid damage into global
  cell rectangles. The widget stays a dumb painter.
- Cursor geometry: `grid_cursor_goto` names a grid; the published cursor
  rectangle is the grid's placement offset plus the grid-local cell. The
  `Cursor_Geometry.grid` field carries the real grid id, and popup
  anchoring works in any window.
- The update summary gains `window_layout_changed`, set by any
  `win_pos`/`win_float_pos`/`win_hide`/`win_close`/`msg_set_pos`/
  `grid_destroy`, published to consumers on flush like the cursor.

## 2.1 Grid widget changes

- Attach passes `ext_multigrid = true` in addition to `ext_linegrid`.
- Painting walks the model's composition order per damage rectangle.
- Mouse translation becomes grid-targeted: the widget asks the model which
  grid is under the pixel and sends `nvim_input_mouse` with that grid id
  and grid-local row/column. Wheel and drag follow the pressed grid until
  release.
- `nvim_ui_try_resize` keeps addressing the outer grid; per-window sizes
  are Neovim's business.

# 3. The kalam://viewport buffer contract

Owned by `editor/neovim/runtime/lua/nveditor.lua`:

- `require('nveditor').viewport_bufnr()` returns (creating on first use) a
  scratch buffer: name `kalam://viewport`, `buftype=nofile`,
  `bufhidden=hide`, `swapfile=off`, `modifiable=off`,
  `filetype=kalam.viewport`, containing one placeholder line
  (`[ Kalam viewport — <CR> to enter ]`).
- `:kalam toggle viewport` closes the current tabpage's viewport window if
  one exists, otherwise opens `botright vsplit` sized to roughly two fifths
  of the columns and shows the buffer. `:kalam focus viewport` ensures the
  window exists, moves the cursor into it, and sends the existing
  `nveditor.focus_viewport` notification (the Qt-side keyboard handoff is
  unchanged).
- Buffer-local mappings: `<CR>` and `i` invoke `:kalam focus viewport`;
  `q` closes the window. No global mappings are claimed, per the command
  surface rules.
- The runtime tracks which windows display the buffer with `BufWinEnter`,
  `BufWinLeave`, and `WinClosed` autocmds and notifies
  `nveditor.viewport_windows` with `{ windows = { <win_id>... },
  active = <win_id or 0> }` on every change. The client matches these ids
  against `win_pos` window handles.

# 4. Qt overlay manager

The binding between the viewport window's cell rectangle and the native
widget lives in the main window (or a small `Viewport_Overlay` helper it
owns).

- **The `Engine_Viewport_Widget` is never reparented.** It sets
  `WA_NativeWindow` and hands its `winId()` to the D3D11 viewport host;
  reparenting would recreate the HWND under the swap chain. The widget is
  created once as a child of the central container and repositioned only
  with `setGeometry`, `raise`, `show`, and `hide`.
- The central widget becomes a plain container: the
  `Editor_Neovim_Shell` fills it entirely; the viewport widget floats
  above it, positioned by mapping the viewport window's global cell
  rectangle through the shell's cell metrics. The QSplitter and its
  smoke assertions are removed.
- Placement updates on: flush with `window_layout_changed`, shell resize,
  font or DPI change, and `nveditor.viewport_windows` notifications.
- When several windows show the buffer, the overlay binds to the `active`
  window from the runtime notification; other windows keep showing the
  placeholder text.
- The overlay hides (placeholder text shows through) when the bound window
  is absent, hidden, floating, or smaller than 8x3 cells.
- Focus rules are unchanged: entering the viewport transfers the keyboard
  to `Engine_Viewport_Widget` (existing `focus_viewport` path); leaving
  returns through the existing `focus_editor` path; clicking the overlay
  focuses it directly, exactly as the splitter-hosted viewport did.

# 5. Startup layout

On adapter readiness the runtime opens the default session layout: the
scene document in the primary window and `:kalam toggle viewport` applied
once, replacing the splitter's former 60/40 arrangement. A user who closes
the viewport window simply has a full-width editor; nothing recreates the
window behind their back.

# 6. Testing

- Model unit tests (fixture-driven, headless): grid placement and
  composition order, float anchoring, `msg_set_pos`, per-grid damage
  translation, cursor geometry with offsets, hit-testing for mouse
  targeting, `grid_destroy` and `win_hide` cleanup, single-grid attach
  regression (existing suites unmodified).
- Codec: `msgpack_ext_handle_value` round-trips against ext payloads
  produced by real Neovim.
- Live golden run (extends the existing real-nvim model test): attach with
  multigrid, `:vsplit`, assert two placed grids whose rectangles tile the
  screen, type in each window, close one.
- Runtime tests (`tests/neovim/runtime_tests.lua` style, headless):
  viewport buffer options, toggle open/close idempotence, mapping
  presence, `nveditor.viewport_windows` notification payloads.
- Widget render fixture: two placed grids plus one float composite into
  the expected cells.
- Integrated smoke: replaces the splitter-layout assertion with —
  viewport window opened via the runtime, overlay geometry equal to the
  window's mapped cell rectangle (within one cell), `<C-w>` navigation
  into and out of the window, toggle close hides the overlay, and the
  existing selection/preview/context steps run against the overlay-hosted
  viewport.

# 7. Milestones

- **V1 — multigrid model**: grid table, window registry, composition,
  damage translation, cursor offsets, mouse hit-testing; fixtures and the
  live `:vsplit` golden. Editor still attaches single-grid; no visible
  change.
- **V2 — multigrid rendering**: widget attaches with multigrid, composites
  grids and floats, grid-targeted mouse. Editor is fully usable in this
  mode with the splitter still in place.
- **V3 — viewport buffer**: Lua contract, runtime tests, overlay manager,
  splitter removal, startup layout, smoke update. This is the demo
  milestone: viewport in a `:vsplit`, dragged window borders resize the
  engine view live.
- **V4 (deferred)**: floats as native Qt widgets, `ext_cmdline` /
  `ext_popupmenu` as Qt surfaces, multiple viewport buffers.

# 8. Risks

- Multigrid changes mouse coordinate semantics globally; V2 must land with
  the full input test table re-run against a multigrid session, not only
  the viewport window.
- The message grid (`msg_set_pos`) and cmdheight interactions are the
  classic multigrid correctness trap; the render fixtures include a
  scrolled message grid case.
- Overlay geometry during live splitter drags arrives as a stream of
  `win_pos` events between flushes; placement updates only on flush to
  avoid tearing, accepting one-frame lag on drag.
- If HWND recreation is ever observed despite the no-reparent rule (theme
  or window-state changes), the D3D11 viewport host must be re-informed of
  the new handle; a debug assertion guards `winId()` stability across
  overlay placement.

# 9. Acceptance criteria

1. `nvim --embed` attached with `ext_linegrid + ext_multigrid`; all
   spec_2.md tests plus the new multigrid suites pass.
2. `:kalam toggle viewport` opens a split whose window hosts the live
   engine viewport; dragging the separator resizes the D3D11 surface with
   no HWND recreation.
3. `<C-w>` navigation passes through the viewport window; `<CR>` inside
   it enters capture; the existing leave action returns to Neovim; the
   context popup anchors correctly in every window.
4. Closing the viewport window leaves a full-width editor; reopening
   restores the overlay; `:q` on the last text window still follows the
   confirm-quit close handshake.
5. The updated acceptance smoke passes headlessly under WARP.
