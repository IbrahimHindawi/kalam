"""Refresh checked-in distribution notices after changing a dependency pin."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ENGINE = Path(__file__).resolve().parents[1]
OUT = ENGINE / "assets" / "notices"


def read(path):
    return path.read_text(encoding="utf-8-sig").strip()


def render(sections):
    return "\n\n".join(f"{title}\n{'='*len(title)}\n{body}" for title, body in sections) + "\n"


def native(args, rust_licenses):
    items = [
        ("SDL3 (877399b2)", args.sdl / "LICENSE.txt"),
        ("SDL HIDAPI (BSD option)", args.sdl / "src/hidapi/LICENSE-bsd.txt"),
        ("SDL YUV conversion", args.sdl / "src/video/yuv2rgb/LICENSE"),
        ("bgfx (0762725e)", args.bgfx / "LICENSE"),
        ("bx (f8508dc0)", args.bx / "LICENSE"),
        ("bimg (9bdb5805)", args.bimg / "LICENSE"),
        ("TinySTL", args.bx / "include/tinystl/LICENSE"),
        ("Microsoft DirectX headers", args.bgfx / "3rdparty/directx-headers/LICENSE"),
        ("Android native app glue", args.bgfx / "3rdparty/native_app_glue/LICENSE"),
        ("L-SMASH", args.bgfx / "3rdparty/l-smash/LICENSE"),
        ("stb", args.bimg / "3rdparty/stb/LICENSE"),
        ("miniz", args.bimg / "3rdparty/tinyexr/deps/miniz/LICENSE"),
        ("Arm ASTC encoder", args.bimg / "3rdparty/astc-encoder/LICENSE.txt"),
        ("LLVM exception (Android libc++)", rust_licenses / "LLVM-exception.txt"),
    ]
    sections = []
    for label, file in items:
        text = read(file)
        if label.startswith("Arm ASTC"):
            text = "Copyright 2011-2024 Arm Limited\n\n" + text
        sections.append((label, text))
    fdlibm = read(args.sdl / "src/libm/s_sin.c").split("/*", 1)[1].split("*/", 1)[0]
    sections.append(("SDL fdlibm", fdlibm.strip()))
    h264 = read(args.bgfx / "3rdparty/h264/h264.h").partition("//Copyright(c)")[2]
    if not h264:
        raise ValueError("Missing h264 license in bgfx sources")
    sections.append(("bgfx H.264 parser", "Copyright(c)" + h264.replace("\n//", "\n")))
    sections.append(("Khronos Vulkan headers", "Copyright 2014-2026 The Khronos Group Inc.\n"
                     "Apache-2.0 OR MIT. The Apache-2.0 text is reproduced above."))
    return render(sections)


def rust_deps():
    meta = json.loads(subprocess.check_output(["cargo", "metadata", "--locked", "--format-version=1"],
                                              cwd=ENGINE.parent))
    sections = []
    for p in sorted(meta["packages"], key=lambda p: (p["name"], p["version"])):
        if not p["source"]:
            continue
        crate = Path(p["manifest_path"]).parent
        mit = crate / "LICENSE-MIT"
        if not mit.is_file() and p["license"] == "MIT":
            mit = crate / "LICENSE"
        for file in [mit, *crate.glob("LICENSE-UNICODE*"), *crate.glob("NOTICE*")]:
            sections.append((f"{p['name']} {p['version']} / {file.name}", read(file)))
    return render(sections)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for dep in ("sdl", "bgfx", "bx", "bimg"):
        parser.add_argument("--"+dep, required=True, type=Path)
    args = parser.parse_args()
    sysroot = Path(subprocess.check_output(["rustc", "--print", "sysroot"], text=True).strip())
    docs = sysroot / "share/doc/rust"
    version = subprocess.check_output(["rustc", "--version"], text=True).strip()
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "native.txt").write_text(native(args, docs / "licenses"), encoding="utf-8")
    (OUT / "crates.txt").write_text(rust_deps(), encoding="utf-8")
    shutil.copyfile(docs / "COPYRIGHT-library.html", OUT / "rust-std.html")
    lock = hashlib.sha256((ENGINE.parent / "Cargo.lock").read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    (OUT / "versions.txt").write_text(f"{version}\nCargo.lock (LF) SHA256 {lock}\n"
                                     "Native library revisions are listed in native.txt.\n", encoding="utf-8")


if __name__ == "__main__":
    main()
