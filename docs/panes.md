# Dockable editor panes

Move the 3D viewport into its own Dear ImGui dockable pane. Put the current editor controls and content in a separate workspace pane. Start with both panes docked side by side. Users can move, resize, or tab them together, and the editor can host additional main panes alongside them.

## Implementation

- [ ] Make the viewport and workspace separate dockable windows with stable IDs. Save and restore their layout and provide a reset to the default layout.
- [ ] Render the scene to a bgfx texture sized to the viewport pane's content area. Draw that texture inside the pane so menus and popups can layer over it. Resize the target with the pane and skip rendering when the pane is hidden.
- [ ] Scope picking, gizmos, framing, and right-mouse camera navigation to the viewport. Use pane-local coordinates for picking and release mouse capture if the pane loses focus or closes.
- [ ] Keep the current controls in the workspace pane and preserve the active document, selection, and pending edits when rearranging the layout. Additional panes should share the same editor session and scene state.

## Verify

- [ ] Dock, resize, tab, close, and reopen the viewport. Confirm the scene keeps its aspect ratio, picking and gizmos still line up, and popups appear over the scene.
- [ ] Open another main pane beside the viewport and workspace. Confirm that focus and shortcuts go to the intended pane, and restart the editor to check layout restoration.

Multiple simultaneous 3D viewports and dragging panes into separate OS windows can follow once docking works within the main editor window.
