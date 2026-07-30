# Kalam author-schema reflection tool

`reflection_tool` is a focused build-time generator. It scans explicit
line-oriented annotations embedded in C or C++ comments and emits one plain
header containing `Author_Field_Desc` and `Author_Schema_View` tables.

It does not parse C++, infer runtime fields, scan directories, or write into the
source tree. The caller supplies every input file and the output path.

## Annotation grammar

Leading horizontal whitespace is allowed. Every annotation occupies one
physical line and begins with `//@kalam`. Attributes have a fixed order so
malformed input is rejected instead of guessed.

```text
//@kalam schema symbol=<c_identifier> id="<schema-id>/vN" version=<u32> flags=<u32>
//@kalam field key="<stable-key>" name="<display-name>" path="<json-path>" group="<group>" kind=<kind> semantic=<semantic> min=[<f>,<f>,<f>,<f>] max=[<f>,<f>,<f>,<f>] step=[<f>,<f>,<f>,<f>] flags=<u32> id=<auto|u64>
//@kalam end
```

A `field` belongs to the open `schema`. Schema blocks cannot nest and must end
explicitly. Non-annotation source lines are ignored.

Supported field kinds are:

```text
F32 I32 Bool Vec2 Vec3 Vec4 Color3 Color4 Euler_Degrees3
Transform Enum String Asset_Reference
```

Supported semantics are:

```text
None Color Position Rotation Scale Transform Angle Percentage Asset
```

Quoted values accept UTF-8 bytes plus `\"`, `\\`, and `\xHH`. Control bytes and
embedded NUL are rejected. Float tokens use a locale-independent decimal
subset: an optional sign, decimal digits, an optional fractional part, and an
optional decimal exponent. Each range vector always has four components;
unused components are conventionally zero.

Example:

```cpp
//@kalam schema symbol=Scene id="nveditor.scene/v1" version=1 flags=0
typedef struct Authored_Scene
{
    //@kalam field key="scene.cube.color" name="color" path="$.cube.color" group="cube" kind=Color3 semantic=Color min=[0,0,0,0] max=[1,1,1,0] step=[0.01,0.01,0.01,0] flags=0 id=auto
    float cube_color[3];
} Authored_Scene;
//@kalam end
```

`id=auto` hashes the exact decoded stable key with FNV-1a-64. An explicit
nonzero decimal or hexadecimal `u64` is an identity override for intentional
renames. Within a schema, duplicate stable keys and duplicate IDs are errors.
Two different automatic keys producing the same FNV-1a-64 value are reported
as a collision, never resolved by ordering. Schema IDs and C symbols must be
unique across all input files. The numeric schema version must match the
trailing `/vN` suffix.

Generated schemas are sorted by case-sensitive schema ID. Generated fields are
sorted by numeric field ID and then stable key. Source order and command-line
input order therefore do not affect valid output.

## Command line and build integration

```text
reflection_tool --output <build-path>/author_schemas.generated.h <input>...
```

The output includes `author_schema.h`, so the consuming target exposes only the
repository's `bridge/` directory as an include path. The output path must be in
the build tree; its parent directory must already exist.

The exact CMake/Ninja custom-command contract is:

```cmake
add_custom_command(
    OUTPUT  "${generated_author_schema}"
    COMMAND reflection_tool
            --output "${generated_author_schema}"
            ${authored_schema_sources}
    DEPENDS reflection_tool
            ${authored_schema_sources}
    VERBATIM
)
```

Inputs are the built `reflection_tool` executable and each explicitly listed
annotation source. The sole generated output is the caller-specified header.
No source-tree glob, local Gin path, sidecar database, or hidden output is used.
The writer compares bytes first, then writes and flushes a same-directory
temporary file before atomic replacement, so an unchanged generation preserves
the existing file timestamp.
