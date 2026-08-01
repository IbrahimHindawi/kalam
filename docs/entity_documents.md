# Entity Documents and Selection Sync

## Implementation Specification

Status: proposed

Authored state becomes a directory of small JSON files — one per entity —
with scenes as thin instance lists. Selecting an object in the viewport
opens its file at the exact key; moving the cursor in an entity buffer
highlights that object in the viewport. The ECS backing the game module is
an implementation detail behind the unchanged `bridge/` contract.

This specification builds on `spec_2.md` (implemented) and extends the
document model of `spec_1.md` section 10 from "open buffers" to a project
document registry. It is independent of `docs/buffer_hosted_viewport.md`;
either may land first, and only the integrated smoke touches both.

# 0. Mission

The editing loop this enables, keyboard-complete end to end:

```text
pick in viewport -> (document_id, object_id) -> (file, json path)
    -> :edit file, cursor on the key -> :kalam edit context -> commit
cursor on a key -> (document_id, object_id) -> highlighted in viewport
```

Three of the four hops exist today: `viewport_interaction` returns
`(document_id, object_id)`, `source_path_for_object` maps an object to its
JSON path, and `Editor_Neovim_Adapter::open_file_location` positions the
cursor. This specification scales them from one hardcoded document to a
project of entity files and adds the reverse hop.

# 1. Scope and non-goals

In scope:

- The entity-file and scene-manifest layout and its identity rules.
- A project document registry in `Editor_Runtime_Host`: every authored
  file is a live document, whether or not a buffer has it open.
- Selection-to-source: a viewport pick opens the owning file at the
  object's key.
- Source-to-selection: the cursor's JSON path highlights the object in
  the viewport (one additive, versioned bridge extension).
- ECS-backed module state: the game module may store authored state in an
  archetype ECS (Flecs is the candidate), registered from the same
  `//@kalam` annotations, with zero bridge or editor changes.

Non-goals (deliberately deferred):

- Prefab overrides, inheritance, or per-instance property deltas. A scene
  entry instantiates an entity file as-is plus a transform. Overrides are
  the classic scope trap; they get their own specification or they eat
  this one.
- File watching. Documents reload on explicit `:kalam project sync`, on
  buffer write, and on build reload; a filesystem watcher can come later.
- Entity renaming/moving with reference fixup.
- Multiple simultaneous scenes.

# 2. Authored file layout

```text
project/
  scenes/main.scene.json        one instance list
  entities/crate.entity.json    one entity each
  entities/enemies/grunt.entity.json
```

- An entity file holds one object tree, exactly like today's
  `cube.scene.json` body: a `$schema` id and component groups whose keys
  and ranges come from the generated author schema.
- A scene file's schema is `nveditor.scene_manifest/v1`: an ordered list
  of instances, each `{ "entity": "<project-relative path>",
  "name": "<unique instance name>", "transform": { ... } }`.
- Paths in manifests are project-relative, forward-slash, case-exact.
  The registry rejects (with a diagnostic, not a crash) entries whose
  on-disk casing differs, so Windows' case-insensitive filesystem cannot
  mint two identities for one file.

# 3. Identity

The identity chain, every link deterministic and documented:

- `document_id = hash64(normalized project-relative path)` — the same
  64-bit FNV-style hash already used for field ids. Renaming a file is
  identity loss by design (rename fixup is deferred).
- Instance identity inside a scene is `manifest json path` (`$.instances[3]`)
  plus the instance `name`; the name is the stable half, and duplicate
  names in one manifest are a validation error.
- `object_id` and per-field `json_path` inside an entity document work
  exactly as today (`Author_Object_Source`).
- ECS entity naming, when the module uses one, mirrors the file path:
  `entities/enemies/grunt.entity.json` becomes scope path
  `enemies.grunt`, instanced under the scene instance name. This mapping
  lives in the module and is never visible across the bridge.

# 4. Project document registry

`Editor_Runtime_Host` grows from per-buffer documents to a registry:

- On project open (and on `:kalam project sync`) the host scans the
  project root for `*.entity.json` and `*.scene.json`, validates each,
  and applies them through the existing `apply_document`; scene manifests
  are applied last so instances resolve.
- **A buffer shadows the disk.** While a file is open in Neovim, its
  buffer state (delivered through the existing `nveditor.document_valid`
  flow) is authoritative and disk content for that path is ignored; when
  the buffer closes without writing, the registry re-reads the file.
- Validation diagnostics for unopened files land in the quickfix list
  through the existing `set_quickfix` path, titled by file.
- The registry answers `file_path_for_document(document_id)` — the
  missing inverse the selection loop needs — and enumerates documents for
  tests and future tooling.

# 5. Selection to source

On a `viewport_interaction` result with `Selection_Changed` and
`Has_Selection`:

- The main window resolves `document_id` through the registry to a file
  path and `object_id` through `source_path_for_object` to a JSON path,
  then asks the Lua runtime (existing `context_for_path` machinery) for
  the key's exact position and calls `open_file_location`.
- Picking an instance in a scene opens the **scene manifest** at that
  instance entry; a follow-up `:kalam edit entity` command jumps from an
  instance entry to its entity file. Picking in a single-entity preview
  opens the entity file directly.
- `Clear_Selection` changes nothing in the editor; buffers never close or
  jump behind the user.
- The jump is a cursor move only — never a mode change, never an
  automatic popup. `:kalam edit context` remains the explicit next step.

# 6. Source to selection

The reverse hop makes the JSON buffer the scene outliner.

- Lua: in `kalam.entity`/`kalam.scene` buffers, a debounced
  `CursorMoved`/`CursorHold` autocmd computes the JSON path at the cursor
  (the same resolver `context_for_path` uses) and, when the enclosing
  top-level object changes, notifies
  `nveditor.cursor_context { path, json_path }`.
- Editor: maps to `(document_id, object_id)` and forwards a highlight
  interaction to the module.
- Bridge: one additive extension, the only ABI change in this
  specification. `Game_API_Version` bumps to 2;
  `Game_Viewport_Interaction` gains `Game_Viewport_Interaction_Highlight`
  with explicit `document_id`/`object_id` fields appended
  (`struct_size`-guarded, so a version-1 module simply reports
  unsupported and the editor degrades to no highlight).
- The module renders the highlight however it likes (tint, outline);
  highlight state is per-frame editor input, never authored state, and
  never survives a reload.

# 7. ECS-backed module state

The game module may replace `authored_state.cpp`'s bespoke storage with an
archetype ECS. The contract, so the editor never knows or cares:

- `Game_API` is unchanged (version 2 only adds the highlight
  interaction). `apply_document`, `read_author_field`,
  `preview_author_field`, and reload semantics keep their exact behavior;
  the existing bridge, host, and reload test suites are the conformance
  tests for the swap.
- The reflection tool gains a second emitter: alongside the author schema
  tables it generates ECS component registration (member names, types,
  and ranges) from the same `//@kalam field` annotations. One annotation
  source, two consumers; the emitters are independent build targets so an
  ECS-free module still builds.
- Flecs, if adopted, is vendored as its amalgamated pinned source with a
  custom build configuration (meta and pipeline addons; no REST, no HTTP,
  no stats in shipping; the REST/Explorer addon may be enabled in debug
  builds only, as a diagnostic). It lives entirely inside `game/`;
  `engine/`, `bridge/`, and `editor/` never include it, and the source
  firewall enforces that.
- Hot-reload state preservation may use ECS-to-JSON serialization once
  proven equivalent; until then the existing preservation path stands.

# 8. Testing

- Identity: hash determinism across runs and machines; case-mismatch
  rejection; duplicate instance names rejected.
- Registry: scan/validate/apply a fixture project (three entities, one
  scene); buffer-shadows-disk with an edited unwritten buffer; close
  without write re-reads disk; quickfix population for a broken file.
- Selection: a fixture pick resolves to (file, json path) and the
  adapter receives the open-location call — unit level with a fake
  module; end-to-end in the integrated smoke against the fixture project.
- Cursor context: runtime tests assert `nveditor.cursor_context` fires
  once per enclosing-object change, debounced, and not in non-kalam
  buffers.
- Bridge v2: a version-1 module loaded by a version-2 host degrades
  cleanly (no highlight, everything else works) — an explicit firewall
  test alongside the existing ABI layout checks.
- ECS swap: the entire existing bridge/game/reload suite passing over the
  ECS-backed module *is* the acceptance test; no new editor tests exist
  for it, by construction.

# 9. Milestones

- **E1 — registry**: file layout, identity, project scan, multi-document
  apply, buffer shadowing, quickfix for unopened files. Editor behavior
  otherwise unchanged.
- **E2 — pick to source**: registry inverse lookup, open-at-key wiring,
  `:kalam edit entity`, smoke extended to a fixture project. The demo:
  click a crate, land on its JSON key.
- **E3 — cursor to highlight**: Lua cursor context, bridge v2 highlight
  interaction, module tint rendering. The demo: `/grunt` in the manifest
  and the enemy lights up.
- **E4 — ECS module**: reflection emitter, ECS storage behind the
  unchanged bridge, full suite green, reload parity. Independent of
  E2/E3 and may proceed in parallel after E1.

# 10. Risks

- Prefab-override pressure arrives the first day two instances need
  different colors; the answer is a new specification, not a quiet field
  on the manifest. Scenes hold transforms only, on purpose.
- The bridge bump is the first `Game_API_Version` change; it proves (or
  breaks) the `struct_size` compatibility story — worth doing for that
  reason alone, with the degradation test written first.
- Registry apply order and document interdependency (manifest referencing
  a broken entity) needs deterministic, diagnosable failure — never a
  half-applied scene.
- An ECS brings its own iteration/identity semantics; behavior drift
  under reload is the risk the existing suite must be trusted to police.

# 11. Acceptance criteria

1. A fixture project of entity files plus one scene manifest loads at
   startup with no buffers open; the viewport shows the instanced scene.
2. Picking an instance opens the manifest at that entry with the cursor
   on its key; `:kalam edit entity` jumps to the entity file; the context
   popup commits a field edit through the existing transaction path.
3. Cursor movement across top-level keys in an entity buffer highlights
   the corresponding object in the viewport within one debounce interval.
4. A version-1 game module still loads and edits under the version-2
   host, minus highlighting.
5. With the ECS-backed module enabled, the complete pre-existing test
   suite passes unmodified.
