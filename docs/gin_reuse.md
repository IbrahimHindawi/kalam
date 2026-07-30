# Gin reference provenance

Kalam uses Gin only as a read-only design reference. The audited baseline is
commit `c6090099b01b3610cddbb14a6dc7c61d593f6dc9`. No Kalam target, configure
step, generated file, or runtime path depends on the local Gin checkout.

The entries below record material concepts extracted from that baseline. Code
is rewritten into Kalam-owned modules to satisfy the stricter ownership,
dependency, failure-reporting, and test requirements in `spec_1.md`.

| Gin baseline source | Extracted concept | Kalam destination | Assumptions removed and intentional changes | Replacement evidence |
| --- | --- | --- | --- | --- |
| `src/core.h:13-24` | Small fixed-width engine vocabulary | `engine/core/core_types.h` | ABI headers use `<stdint.h>` names directly. Kalam does not carry Gin's broad includes, metadata registry, `size_t` alias, or C++-`bool`-based ABI values. | Core compile tests and standalone C/C++ bridge-header tests. |
| `src/string8.h:6-35`, `src/string8.c` | Pointer-plus-byte-count string view | `engine/core/string_view.h` | Views are immutable UTF-8 bytes with no NUL promise. No cast-away constness; capacity arithmetic and zero-size cases are checked. | Core string-view tests and ABI layout tests. |
| `src/core_templates.h:15-127` | POD `array<T>`/`vec<T>` shapes and arena-backed free functions | `engine/core/core_templates.h` | Checked multiplication/doubling, explicit failure, non-aborting bounds access, invariant preservation, and bounded copy on resize. Gin's unsafe arena realloc and no-op destroy pattern are not copied. | Vector growth/shrink, overflow, OOM, and copy-bound tests. |
| `src/saha.h:15-49,82-201`; `src/os.h:4-9` | Reserved virtual memory, on-demand commit, temporary marks, and named lifetimes | `engine/core/arena.h`, `engine/core/arena_win32.cpp` | Explicit configurable reserve/initial commit/commit granularity; checks before all arithmetic; capacity checked before commit; reported OOM; validated nested LIFO scopes; decommit on large discard; `VirtualFree(MEM_RELEASE)` destruction; repeatable initialization. The 1-TiB reservation, commit-before-bound check, unsafe realloc, unchecked pop/set-position, and no-op destroy are intentionally rejected. | Arena alignment/growth/overflow/exhaustion/scope/reset/release tests. |
| `tools/reflect_gen.cpp` and its CMake custom command | Focused compile-time metadata generation | `reflection/reflection_tool.cpp`, `reflection/main.cpp`, and the root `author_schema_generation` target | Kalam defines a narrow authored-field grammar, stable hash IDs, explicit collision and malformed-annotation failures, deterministic ordering, atomic write-if-content-changed output, and build-tree-only generation. Gin metadata tables and ordinal identities are not copied. | Reflection unit tests, deterministic regeneration checks, generated C/C++ header smoke tests, and the source-tree/no-op build tests. |
| `src/gin_win32.c` | QPC-driven frame timing and Win32 input translation patterns | `editor/host/frame_scheduler.cpp`, `editor/host/input_latch.cpp`, and `standalone/standalone_host.cpp` | Scheduler and input state are host-owned; press and release edges survive zero-tick frames; catch-up is bounded; focus loss synthesizes releases; reload, pause, occlusion, and debugger-sized stalls reset backlog. Process globals and virtual-key values never cross the game ABI. | Scheduler/input unit tests and standalone key-translation and hidden-WARP parity tests. |
| `src/dx11ops.c`, `src/dx11ops.h` | Explicit COM teardown, deferred resize, and partial-initialization cleanup | `editor/win32/d3d11_viewport_host.cpp` and `engine/render/d3d11/module_renderer.cpp` | Device, context, swap chain, back buffer, resize, occlusion, and Present remain in the stable host. Shaders, buffers, target-sized resources, post-processing, and gizmo rendering belong to the hot module and are released before DLL unload. No combined renderer singleton or copied Gin content remains. | Hidden-window WARP create/render/resize/destroy coverage and repeated debug-layer reload/resize live-object snapshots. |

These are audited design extractions, not copied source. Add another table entry
whenever a future implementation materially adopts a Gin concept.
