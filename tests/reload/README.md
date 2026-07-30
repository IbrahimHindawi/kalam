# Transactional reload tests

This directory is both a standalone CMake project and an `add_subdirectory`
test graph. It never writes a hot DLL into the source tree.

## Target graph

```text
reload_module_loader (STATIC)
  sources: editor/host/module_loader.cpp
  public headers: editor/host, bridge
  Win32 imports: kernel32

reload_fake_valid_a (SHARED) ┐
reload_fake_valid_b (SHARED) │
reload_fake_missing_entry    │
reload_fake_invalid_version  │
reload_fake_invalid_size     │
reload_fake_invalid_pointer  ├─> reload_module_loader_tests
reload_fake_invalid_create   │      -> reload_module_loader
reload_fake_invalid_schema_query
reload_fake_invalid_schema_duplicate
reload_fake_invalid_schema_field
reload_fake_invalid_apply    ┘

CTest: reload.transactional_candidate_swap
```

The loader and test executable explicitly require only `kernel32` for
`GetFullPathNameW`, `CopyFileW`, `LoadLibraryW`, `GetProcAddress`,
`FreeLibrary`, `DeleteFileW`, and the test-side file/process checks. Fake
modules use only the compiler runtime.

The test proves:

- missing entry point and API version/size/pointer failures preserve module A;
- create, schema-query, duplicate-schema, invalid-field, and document-apply
  failures destroy/unload only the candidate and preserve module A;
- all module error text is copied before unload;
- schema arrays and all nested strings live in caller storage (the fake module
  poisons its source strings before `prepare` returns);
- `prepare` and frame-boundary `commit` are distinct;
- A-to-B commit unloads A before publishing B;
- a stale prepared generation cannot replace a newer active module;
- active copies cannot be deleted, while unloaded copies remain until explicit
  deletion.

## Standalone invocation

```powershell
cmake -S tests/reload -B build/reload-clang -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build/reload-clang
ctest --test-dir build/reload-clang --output-on-failure
```
