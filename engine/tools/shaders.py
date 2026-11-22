"""Compile authored Slang offline to the pinned bgfx shader package format."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

ENGINE = Path(__file__).resolve().parents[1]
CACHE = ENGINE.parent / "build-shader-tooling"
OUT = ENGINE / "build/assets/shaders"
TOOLS = {
    "slang": (
        "https://github.com/shader-slang/slang/releases/download/v2025.24.1/slang-2025.24.1-windows-x86_64.zip",
        "fdf3709f6be625ee42ba84fae9cb632ebcec279be505923fba61a0249610b854",
        "bin/slangc.exe", "-version", "2025.24.1",
    ),
    "bgfx": (
        "https://github.com/bkaradzic/bgfx.cmake/releases/download/v1.168.9637-616/bgfx.cmake.binary.windows.v1.168.9637-616.zip",
        "f77b8587c131029725a5c4dbc151ffc80a8074ed8b8d91b9ae3b6f3e7367f8fc",
        "bgfx.cmake/bin/shaderc.exe", "--version", "1.19.168",
    ),
}
ATTR = {"POSITION": "a_position", "NORMAL": "a_normal", "COLOR0": "a_color0",
        "TEXCOORD0": "a_texcoord0", "TEXCOORD1": "a_texcoord1", "TEXCOORD2": "a_texcoord2"}
TARGETS = {"windows": ("dx11", "s_5_0"), "android": ("spirv", "spirv"), "android-gles": ("essl", "300_es")}
PROGRAMS = {
    "pbr": (
        ["POSITION", "NORMAL", "TEXCOORD0"],
        ["u_modelViewProj", "u_model", "u_lightViewProj"],
        ["u_eye", "u_lightDir", "u_lightColor", "u_cameraFlags", "u_baseColor", "u_material",
         "s_baseColor", "s_material", "s_shadow", "s_irradiance", "s_prefilter", "s_dfg", "s_normal"],
    ),
    "depth": (["POSITION"], ["u_modelViewProj"], []),
    "tone": (["POSITION", "TEXCOORD0"], [], ["u_exposure", "u_outlineColor", "s_color"]),
    "sprite": (["POSITION", "TEXCOORD0"], [], ["u_tint", "s_color"]),
    "editor_ui": (["POSITION", "COLOR0", "TEXCOORD0"], [], ["s_color"]),
}
for name in ("pbr", "depth"):
    attrs, vs, fs = PROGRAMS[name]
    PROGRAMS[name + "_skin"] = (attrs + ["TEXCOORD1", "TEXCOORD2"], vs + ["s_skinPalette"], fs)


def tool(name, bootstrap):
    url, digest, relative, version_flag, version = TOOLS[name]
    exe = CACHE / name / relative
    if not exe.is_file() and bootstrap:
        if os.name != "nt":
            raise ValueError("Shader tool bootstrap requires Windows x64")
        CACHE.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=CACHE) as tmp:
            archive = Path(tmp) / "release.zip"
            urllib.request.urlretrieve(url, archive)
            if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
                raise ValueError(f"{name} release checksum mismatch")
            with zipfile.ZipFile(archive) as zipped:
                zipped.extractall(Path(tmp) / "release")
            (Path(tmp) / "release").rename(CACHE / name)
    if not exe.is_file():
        raise ValueError(f"Missing {exe}. Run python engine/tools/shaders.py --bootstrap")
    diagnostic = subprocess.check_output([exe, version_flag], stderr=subprocess.STDOUT, text=True)
    if version not in diagnostic:
        raise ValueError(f"Unexpected {name} version: {diagnostic.strip()}")
    return exe


def stage_abi(code, stage, attrs, varyings):
    # Slang lowers a cbuffer to a nested struct. bgfx reflects top-level
    # uniforms. Adapt binding declarations and identifiers only.
    block = re.compile(
        r"struct (SLANG_ParameterGroup_\w+)\s*\{([^{}]*)\};\s*"
        r"cbuffer (\w+)\s*:\s*register\(b0\)\s*\{\s*\1\s+\3;\s*\}", re.S
    )
    m = block.search(code)
    if m:
        declarations = []
        for line in m[2].splitlines():
            if not line.strip():
                continue
            field = re.fullmatch(r"\s*(float4(?:x4)?)\s+(u_\w+);\s*", line)
            if not field:
                raise ValueError(f"Unsupported bgfx material declaration: {line}")
            if f"{m[3]}.{field[2]}" in code:
                declarations.append(f"uniform {field[1]} {field[2]};")
        code = code[:m.start()] + "\n".join(declarations) + code[m.end():]
        code = re.sub(rf"\b{m[3]}\.(u_\w+)", r"\1", code)
    if re.search(r"\bcbuffer\b", code):
        raise ValueError("Unsupported cbuffer layout emitted by Slang")
    entry = f"{stage}Main"
    signature = re.search(rf"\b{entry}\(([^()]*)\)", code)
    if not signature:
        raise ValueError(f"Slang output has no {entry} entry point")
    code = re.sub(rf"\b{entry}\(", "main(", code)
    if stage == "vs":
        found = re.findall(r"\b(\w+)\s*:\s*(POSITION|NORMAL|COLOR0|TEXCOORD[012])\b", signature[1])
        if [semantic for _, semantic in found] != attrs:
            raise ValueError(f"Unexpected vertex inputs: {found}")
        for var, semantic in found:
            code = re.sub(rf"\b{var}\b", ATTR[semantic], code)
    inputs = [ATTR[a] for a in attrs] if stage == "vs" else varyings
    outputs = varyings if stage == "vs" else []
    directives = "".join("$" + direction + " " + ", ".join(names) + "\n"
                         for direction, names in [("input", inputs), ("output", outputs)] if names)
    return directives + code


def es_stage(code, stage, attrs, varyings, expected):
    """Adapt Slang's GLSL interface to bgfx and GLSL ES 3.00."""
    code = re.sub(r"^#version \d+\s*|^layout\((?:row|column)_major\) (?:uniform|buffer);\s*$", "", code, flags=re.M)
    uniform_block = re.compile(r"layout\(binding = \d+\)\s*layout\(std140\) uniform block_\w+\s*\{([^{}]*)\}(\w+);")
    block = uniform_block.search(code)
    current = set()
    if block:
        fields = re.findall(r"\b(vec4|mat4x4) (u_\w+);", block[1])
        declarations = []
        for ty, name in fields:
            qualified = rf"\b{block[2]}\.{name}\b"
            if re.search(qualified, code):
                current.add(name)
                declarations.append(f"uniform {ty} {name};")
                # Slang represents matrices transposed in its GLSL expressions.
                code = re.sub(qualified, f"transpose({name})" if ty == "mat4x4" else name, code)
        code = uniform_block.sub("\n".join(declarations), code)
    for dim, texture in re.findall(r"uniform texture(2D|Cube) (s_\w+Texture);", code):
        name = texture.removesuffix("Texture")
        sampler = name + "Sampler"
        constructor = rf"sampler{dim}\(\s*{texture}\s*,\s*{sampler}\s*\)"
        code, uses = re.subn(constructor, name, code)
        replacement = f"uniform highp sampler{dim} {name};" if uses else ""
        code = re.sub(rf"layout\(binding = \d+\)\s*uniform texture{dim} {texture};", replacement, code)
        code = re.sub(rf"layout\(binding = \d+\)\s*uniform sampler {sampler};", "", code)
        if uses:
            current.add(name)
    if current != set(expected):
        raise ValueError(f"GLSL ES bindings differ: expected {expected}, active {sorted(current)}")
    names = ([ATTR[a] for a in attrs], varyings) if stage == "vs" else (varyings, ["kh_color"])
    for direction, exports in zip(("in", "out"), names):
        interface = re.compile(rf"layout\(location = (\d+)\)\s*{direction} (\w+) (\w+);")
        matches = list(interface.finditer(code))
        if sorted(int(match[1]) for match in matches) != list(range(len(exports))):
            raise ValueError(f"Unexpected GLSL ES {stage} {direction} locations")
        for match in matches:
            slot, ty, old = match.groups()
            name = exports[int(slot)]
            layout = "layout(location = 0) " if stage == "fs" and direction == "out" else ""
            code = code.replace(match[0], f"{layout}{direction} {ty} {name};")
            code = re.sub(rf"\b{old}\b", name, code)
    if stage == "vs":
        # Keep Kalam's light-space depth at 0..1 while converting raster clip depth.
        code = re.sub(r"(gl_Position\s*=[^;]+;)", r"\1 gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;", code)
    if re.search(r"\blayout\((?:binding|std140)|\buniform (?:sampler |texture)", code):
        raise ValueError("Unsupported resource declaration in GLSL ES")
    directives = "".join(f"${label} {', '.join(values)}\n"
                         for label, values in zip(("input", "output"), (names[0], varyings if stage == "vs" else []))
                         if values)
    return directives + "#version 300 es\nprecision highp float;\nprecision highp int;\n" + code


def linkage(entry, stage):
    """Use reflected semantics and types to derive comparable bgfx stage hashes."""
    params = [entry["result"]] if stage == "vs" else entry["parameters"]
    names = []
    for param in params:
        fields = param["type"].get("fields", [param])
        for field in fields:
            semantic = field["semanticName"]
            if semantic.startswith("SV_"):
                continue
            ty = field["type"]
            width = ty.get("elementCount", 1)
            scalar = ty.get("elementType", ty)["scalarType"]
            names.append(f"{semantic}{field.get('semanticIndex', 0)}_{scalar}_{width}")
    return names


def inspect_record(path, stage, expected, attrs, reflected, backend):
    data = path.read_bytes()
    if data[:4] != (b"VSH" if stage == "vs" else b"FSH") + bytes([14]):
        raise ValueError(f"Invalid bgfx header: {path}")
    in_hash, out_hash, count = struct.unpack_from("<II8xH", data, 4)
    offset = 22
    bindings = {}
    for _ in range(count):
        n = data[offset]
        name = data[offset+1:offset+1+n].decode()
        offset += 1+n
        kind, num, reg, slots, _, dim, _ = struct.unpack_from("<BHHHBBH", data, offset)
        offset += 11
        bindings[name] = {"typeFlags": kind, "count": num, "register": reg,
                          "slots": slots, "dimension": dim}
    if set(bindings) != set(expected):
        raise ValueError(f"{path.name}: expected {expected}, reflected {list(bindings)}")
    size, = struct.unpack_from("<I", data, offset)
    if not size or offset + 4 + size >= len(data):
        raise ValueError(f"{path.name}: invalid shader payload")
    if stage == "vs":
        attr_ids = {"POSITION": 1, "NORMAL": 2, "COLOR0": 5,
                    "TEXCOORD0": 16, "TEXCOORD1": 17, "TEXCOORD2": 18}
        start = offset + 5 + size
        actual = struct.unpack_from(f"<{data[start]}H", data, start + 1)
        if list(actual) != [attr_ids[a] for a in attrs]:
            raise ValueError(f"{path.name}: invalid vertex attribute mapping {actual}")
    for binding in reflected.get("bindings", []):
        b = binding["binding"]
        if b.get("used") and b["kind"] in ("samplerState", "shaderResource"):
            name = binding["name"].removesuffix("Sampler").removesuffix("Texture")
            slot = bindings[name]["register"] - (2 if backend == "spirv" else 0)
            if b["index"] != slot:
                raise ValueError(f"{path.name}: {name} wrong texture slot {slot}")
    return {"inputHash": in_hash, "outputHash": out_hash, "uniforms": bindings,
            "sha256": hashlib.sha256(data).hexdigest()}


def inspect_es_record(path, stage, code, expected):
    # shaderc --raw records the stage hashes and GLSL source. The GL backend
    # reflects uniforms and vertex attributes from the linked program.
    data = path.read_bytes()
    magic = b"VSH" if stage == "vs" else b"FSH"
    in_hash, out_hash, count, size = struct.unpack_from("<II8xHI", data, 4)
    source = code[code.index("#version"):].strip().encode()
    if (data[:4] != magic + bytes([14]) or count != 0 or
            size != len(data) - 27 or data[-1] != 0 or data[26:-1].strip() != source):
        raise ValueError("Unexpected GLSL ES shader record")
    return {"inputHash": in_hash, "outputHash": out_hash, "uniforms": expected,
            "sha256": hashlib.sha256(data).hexdigest()}


def build(platform, slang, shaderc, staging):
    backend, profile = TARGETS[platform]
    folder = staging / backend
    folder.mkdir()
    manifest = {"backend": backend, "profile": profile, "slang": TOOLS["slang"][-1],
                "bgfxToolRelease": "v1.168.9637-616", "stages": {}}
    with tempfile.TemporaryDirectory(dir=staging) as temp:
        es = backend == "essl"
        generated = Path(temp) / ("generated.glsl" if es else "generated.hlsl")
        reflection = Path(temp) / "reflection.json"
        (Path(temp) / "varying.def.sc").write_text("// bgfx raw path gets linkage from stage directives.\n")
        for name, (attrs, vs_bindings, fs_bindings) in PROGRAMS.items():
            source = ENGINE / "shaders" / (name + ".slang")
            for stage, kind in (("vs", "vertex"), ("fs", "fragment")):
                output = folder / f"{name}.{stage}.bin"
                try:
                    subprocess.run([slang, source, "-entry", stage+"Main", "-stage", kind,
                                    "-target", "glsl" if es else "hlsl", "-no-mangle", "-line-directive-mode", "none",
                                    "-matrix-layout-column-major", "-reflection-json", reflection, "-o", generated] +
                                   (["-DKALAM_GLES=1", "-fvk-b-shift", "20", "0", "-fvk-s-shift", "10", "0", "-fvk-t-shift", "0", "0"] if es else []), check=True)
                    entry = json.loads(reflection.read_bytes())["entryPoints"][0]
                    varyings = linkage(entry, stage)
                    expected = vs_bindings if stage == "vs" else fs_bindings
                    source_code = generated.read_text()
                    code = es_stage(source_code, stage, attrs, varyings, expected) if es else stage_abi(source_code, stage, attrs, varyings)
                    generated.write_bytes(code.encode("utf-8"))
                    subprocess.run([shaderc, "--raw", "--type", kind, "--platform", "android" if es else platform,
                                    "-p", profile, "-O", "3", "-f", generated, "-o", output],
                                   check=True)
                    if es:
                        meta = inspect_es_record(output, stage, code, expected)
                    else:
                        meta = inspect_record(output, stage, expected, attrs, entry, backend)
                except (ValueError, subprocess.CalledProcessError) as exc:
                    raise ValueError(f"{platform}/{name}.{stage} ({source}): {exc}") from exc
                meta.update(entry=stage+"Main", linkage=varyings, attributes=attrs if stage == "vs" else [])
                manifest["stages"][output.name] = meta
            if (manifest["stages"][name+".vs.bin"]["outputHash"] !=
                    manifest["stages"][name+".fs.bin"]["inputHash"]):
                raise ValueError(f"{backend}/{name}: stage linkage mismatch")
    (folder / "manifest.json").write_text(json.dumps(manifest, indent=2)+"\n", encoding="utf-8")
    return folder


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=[*TARGETS, "all"], default="all")
    parser.add_argument("--bootstrap", action="store_true", help="Install verified host tools")
    parser.add_argument("--doctor", action="store_true", help="Verify executable versions")
    args = parser.parse_args()
    slang = tool("slang", args.bootstrap)
    shaderc = tool("bgfx", args.bootstrap)
    if args.doctor:
        print(f"Slang {TOOLS['slang'][-1]}: {slang}\nbgfx shaderc {TOOLS['bgfx'][-1]}: {shaderc}")
        return
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=OUT.parent) as temp:
        folders = [build(p, slang, shaderc, Path(temp)) for p in
                   (TARGETS if args.target == "all" else [args.target])]
        for folder in folders:
            dest = OUT / folder.name
            old = Path(temp) / (folder.name+".previous")
            if dest.exists():
                dest.rename(old)
            try:
                folder.rename(dest)
            except OSError:
                if old.exists():
                    old.rename(dest)
                raise
            print(f"Packaged {dest}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError, struct.error) as error:
        sys.exit(f"shader build failed: {error}")
