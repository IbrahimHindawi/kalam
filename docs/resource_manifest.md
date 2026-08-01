# Resource Manifest

## Implementation Specification

Status: proposed

Kalam adopts gin's resource pipeline shape — one declarative manifest, ids
inferred from paths, generated compile-time tables, hash-gated staging —
rebuilt through kalam's own machinery: the manifest is a JSON document the
editor already knows how to validate and edit, the tables come from the
reflection tool, and the staging tool is first-party C++ with no Python in
the build.

This is an audited design extraction from gin's `pipeline/res.toml`,
`res_meta_gen.py`, and `resources_inject.py` (record it in
`docs/gin_reuse.md` when implemented). It builds on `spec_2.md`
(implemented) and composes with `docs/entity_documents.md`: entity files
reference resources by manifest name, and the project registry validates
those references.

# 0. Mission

After this work:

- Every asset the game module uses — shader sources, textures, later
  meshes — is declared in one manifest file, `res/res.manifest.json`.
- The reflection tool generates compile-time resource tables from the
  manifest: enum handles, stable 64-bit ids, path and parameter tables.
  A typo in a resource reference is a compile error, and no engine or
  module code ever looks a resource up by string at runtime.
- A first-party staging tool copies assets from an optional out-of-repo
  vault into `res/`, content-hash-gated, and stages `res/` beside the
  editor and standalone hosts.
- The embedded HLSL string in `module_renderer.cpp` moves to a file
  declared in the manifest; shader iteration stops requiring a C++
  rebuild of the renderer translation unit.
- The manifest is an ordinary kalam document: validated by the runtime,
  diagnostics in quickfix, editable with the same loop as every other
  authored file.

# 1. Scope and non-goals

In scope:

- The `nveditor.res_manifest/v1` JSON manifest format.
- Path-derived identity with authored ids rejected, gin-style.
- The reflection-tool emitter for `res_manifest.generated.h`.
- The `res_stage` staging tool (vault sync plus host staging).
- Module-side loading of manifest resources at create time: HLSL shader
  files compiled through the existing `D3DCompile` path, and textures
  from DDS files through a first-party parser.
- Project-root delivery to the module (one additive, `struct_size`-guarded
  extension of `Game_Create_Desc`).
- Manifest validation in the editor with quickfix diagnostics, and
  resource-reference validation from entity documents.

Non-goals (deferred, each with its named future decision):

- PNG/JPEG decoding and glTF meshes. Both mean vendoring pinned
  single-file dependencies (stb_image, cgltf) inside the module's
  firewall; that is a `docs/dependencies.md` decision to make explicitly,
  not a default. Revision 1 is dependency-free: DDS textures (the format
  is a documented header over GPU-native data) and HLSL text.
- Image conversion, atlas generation, mesh baking, audio. The staging
  tool copies; it does not transform. Gin's converters stay in gin until
  a kalam equivalent earns its keep.
- Per-resource hot swap. Resources load at module create; `:kalam build
  reload` (which already rebuilds and reloads the module) is the resource
  reload path in revision 1.
- Packaging/archives for shipping; the standalone host reads the staged
  `res/` directory.

# 2. The manifest

`res/res.manifest.json`, schema id `nveditor.res_manifest/v1`. JSON, not
TOML: kalam's canonical authored text is JSON (`spec_1.md` section 10),
and using it buys validation, tree-sitter ranges, quickfix, the context
popup, and cursor sync with zero new editor machinery.

```json
{
  "$schema": "nveditor.res_manifest/v1",
  "vault": "C:/devel/kalam_assets",
  "shaders": [
    { "program": "module_basic", "source": "shaders/module_basic.hlsl" }
  ],
  "textures": [
    { "path": "textures/checker.dds", "srgb": true }
  ]
}
```

- `vault` is optional and machine-local; when present, staging syncs
  vault-relative paths into `res/` before hashing. Paths in `shaders` and
  `textures` are always `res/`-relative, forward-slash, case-exact
  (the same Windows-casing rule as `docs/entity_documents.md`).
- No entry carries an authored id. The generator infers identity from
  the path and **rejects any `id` key outright**, exactly as gin's
  generator does — identity cannot drift from location, and two entries
  cannot claim one name.
- Arrays are ordered; generated tables preserve manifest order so output
  is deterministic and diffs are readable.

# 3. Identity

- Handle names are inferred from paths, gin-style:
  `textures/checker.dds` becomes `res_tex_checker`;
  `shaders/module_basic.hlsl` becomes `res_shader_module_basic`.
  Collisions after normalization (case folding, separators to
  underscores) are generation errors naming both entries.
- Every resource also gets `resource_id = hash64(kind ":" normalized
  path)` in the same FNV-family hash as field and document ids. The enum
  handle is for compile-time use inside the module; the 64-bit id is for
  serialized references from entity documents, stable across manifest
  reordering.
- Renaming a file is identity loss by design, consistent with document
  identity in `docs/entity_documents.md`.

# 4. Generated tables

The reflection tool gains a manifest emitter (a new mode of the existing
`reflection/` executable, driven by a root custom target alongside
`author_schema_generation`):

- Input: `res/res.manifest.json`. Output:
  `generated/res_manifest.generated.h` in the build tree only — the
  source tree stays clean, enforced by the existing
  `build.source_tree_unchanged` test.
- Output shape follows gin's X-macro tables, in kalam engine style:

```c
#define res_shader_xmacro(_) \
    _(res_shader_module_basic, 0x4F1A..., "res/shaders/module_basic.hlsl")

#define res_tex_xmacro(_) \
    _(res_tex_checker, 0x9C2B..., "res/textures/checker.dds", 1)
```

  plus expanded enums (`res_shader_module_basic`, ..., `res_shader_count`)
  and constant tables, so consumers may use either the enum or the
  X-macro directly.
- The emitter shares the reflection tool's rules: deterministic ordering,
  atomic write-if-content-changed, explicit malformed-input and collision
  failures with file/line diagnostics, exit codes over aborts.
- The header is C-compatible plain data with no allocation and no
  platform includes; a standalone-compile smoke test guards it like the
  other generated headers.

# 5. Staging

`res_stage`, a first-party tool (C++ on `engine/core`, same policy as the
reflection tool), replaces gin's Python injector:

- Mode one, vault sync: for each manifest entry, when `vault` is set and
  the vault copy is newer or content-differs (SHA-256, sizes first),
  copy vault → `res/`. Prints one line per action; `--check` reports
  without copying and exits nonzero on drift, for CI.
- Mode two, host staging: mirror `res/` beside a target executable
  (`build/<preset>/res/`), hash-gated. Wired as a POST_BUILD step for
  `nveditor` and the standalone host, exactly like the Neovim runtime
  staging in `KalamEditorRuntime.cmake`.
- Missing manifest paths are staging errors listing every absent file,
  never a partial silent success.
- `:kalam res sync` runs vault sync + regeneration + the existing build
  path, surfacing failures through the build controller's quickfix flow.

# 6. Module loading and ownership

- `Game_Create_Desc` gains a `String_View project_root` appended under
  `struct_size` guard (additive; a module built against the old struct
  still loads). Hosts pass the project root they already know.
- At create, the module resolves `project_root + table path` for each
  manifest resource: shader files feed the existing `D3DCompile` path
  (the embedded `module_renderer_shader_source` string is deleted);
  DDS textures load through a first-party parser (header validation,
  BC1–BC7 and RGBA8 formats, explicit failure with the offending path in
  `Game_Error`).
- Ownership rules are unchanged and load-bearing: the module owns every
  GPU resource it creates and releases them before unload; nothing
  crosses the DLL boundary but the read-only path table baked into the
  module itself. The editor and hosts never load resources.
- A missing or malformed resource fails `create` with the resource path
  in the error — the same contract as an invalid document, surfaced in
  quickfix.

# 7. Editor integration

- The manifest is a tracked document type in the runtime: validated on
  edit like scene documents (`nveditor.document_valid` /
  `document_invalid`), with schema checks (unknown keys, authored `id`
  rejection, duplicate handles, missing files) reported as diagnostics
  with exact ranges.
- Under `docs/entity_documents.md`, entity fields that name resources
  (by handle name) are validated by the project registry against the
  manifest; a dangling reference is a document diagnostic, not a module
  crash.
- Editing the manifest and running `:kalam build reload` is the complete
  iteration loop: sync, regenerate, rebuild, hot reload — resources
  included, no editor restart.

# 8. Testing

- Emitter: golden-output tests for a fixture manifest; authored-id
  rejection; collision diagnostics; deterministic regeneration (bytes
  equal across two runs); generated-header C and C++ compile smokes.
- Identity: handle inference table (paths → names), hash stability, the
  case-exactness rule.
- Staging: fixture vault → res sync (copy, skip-on-hash, `--check`
  drift); missing-file error listing; POST_BUILD staging verified the
  same way the runtime deployment test checks `nvim-runtime`.
- DDS parser: header validation, format coverage, truncated-file and
  wrong-magic failures — unit tests with byte fixtures, no GPU.
- Module: WARP create/render with a manifest shader and texture; missing
  resource fails create with the path in the error; reload with a
  changed shader file renders differently (the existing reload harness
  pattern).
- ABI: an old-struct module (no `project_root`) still loads under the
  new host — alongside the version test from `docs/entity_documents.md`.

# 9. Milestones

- **R1 — manifest and tables**: schema, emitter, generated-header smokes,
  editor validation of the manifest document. No runtime behavior change.
- **R2 — staging**: `res_stage`, vault sync, POST_BUILD staging for both
  hosts, `:kalam res sync`.
- **R3 — module loading**: `Game_Create_Desc` extension, shader-from-file
  (embedded string deleted), DDS textures, WARP and reload coverage.
  This is the demo: edit the cube's HLSL in a split, `:kalam build
  reload`, new shading — no C++ rebuild of the renderer logic.
- **R4 — entity references**: resource fields in author schemas validated
  against the manifest through the project registry (lands with or after
  E1 of `docs/entity_documents.md`).

# 10. Risks

- Scope creep toward gin's full converter zoo (atlases, image resize,
  anim conventions). The copy-only rule and the deferred-dependency list
  are the fence; each converter is its own future decision.
- DDS-only textures are a workflow tax (authoring tools export PNG).
  Accepted for revision 1; the explicit follow-up decision is vendoring
  stb_image in the staging tool (converting at sync time, keeping the
  module dependency-free) rather than in the module.
- The `Game_Create_Desc` extension is the second additive ABI change;
  it reuses the degradation-test pattern the highlight interaction
  establishes.
- Manifest-order determinism must survive QVariant/JSON map iteration —
  the emitter reads arrays positionally and never iterates maps for
  output order.

# 11. Acceptance criteria

1. A fixture manifest generates identical tables on repeated runs; an
   authored `id`, a duplicate handle, and a missing file each fail with
   a diagnostic naming the entry.
2. `nveditor` and the standalone host both start with `res/` staged
   beside them, and the module renders using a manifest-declared shader
   file and DDS texture with the embedded shader string deleted.
3. Editing the shader file and running `:kalam build reload` changes the
   rendered result without restarting the editor.
4. A manifest error shows up in quickfix with an exact range while
   editing `res.manifest.json`.
5. The full test suite, including the new emitter/staging/DDS/module
   suites, passes; the source tree stays clean after a full build.
