"""Host entry points behind cargo xtask. All paths are relative to this checkout."""
import argparse
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ENGINE = Path(__file__).resolve().parents[1]
BUILD = ENGINE / "build"
PY = sys.executable
SHADERS = ENGINE / "tools/shaders.py"
ANDROID = ENGINE / "android/tool.py"


def execute(*args, cwd=ENGINE):
    subprocess.run(list(map(str,args)), cwd=cwd, check=True)


def shaders(target):
    execute(PY, SHADERS, "--target", target)
    if target == "android":
        execute(PY, SHADERS, "--target", "android-gles")


def windows(vulkan, editor=False, scene=None, game_manifest=None):
    game = scene if not game_manifest and scene and re.fullmatch(r"[A-Za-z0-9_-]{1,64}", scene) and (
        ENGINE.parent / "samples" / scene / "window" / "Cargo.toml"
    ).is_file() else None
    cargo_args = []
    output = BUILD
    library = None
    if game_manifest:
        import tomllib

        with game_manifest.open("rb") as manifest:
            project = tomllib.load(manifest)
        game = project.get("package", {}).get("name", "")
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", game):
            raise ValueError("--game-manifest must point to a game package with a valid name")
        library = project.get("lib", {}).get("name", game.replace("-", "_"))
        if not re.fullmatch(r"[A-Za-z0-9_]+", library):
            raise ValueError("The game library name must be a Rust crate name")
        if "staticlib" not in project.get("lib", {}).get("crate-type", []):
            raise ValueError("The game package must declare crate-type = [\"staticlib\"]")
        cargo_args = ["--manifest-path", game_manifest, "--lib"]
        output = game_manifest.parent / "target" / "kalam"
    package = game or "kalam-host"
    suffix = f"-{game}" if game else ""
    dest = output / ("windows-editor-gpu" if editor else "windows")
    rust = output / ("windows-rust" + suffix)
    features = ["--features", "editor"] if editor else []
    execute("cargo", "build", "--release", "--locked", "-p", package, "--target-dir", rust,
            *cargo_args, *features, cwd=game_manifest.parent if game_manifest else ENGINE)
    flags = ["-DKALAM_EDITOR=ON" if editor else "-DKALAM_EDITOR=OFF"]
    tool_package = ENGINE.parent / "build-shader-tooling/bgfx/bgfx.cmake/lib/cmake/bgfx"
    if tool_package.is_dir() and not os.getenv("bgfx_DIR"):
        flags.append(f"-Dbgfx_DIR={tool_package}")
    for name in ("SDL3_DIR","bgfx_DIR","FETCHCONTENT_SOURCE_DIR_SDL3","FETCHCONTENT_SOURCE_DIR_BGFX_CMAKE"):
        if os.getenv(name):
            flags.append(f"-D{name}={os.environ[name]}")
    if editor and os.getenv("FETCHCONTENT_SOURCE_DIR_KH_IMGUI"):
        flags.append(f"-DFETCHCONTENT_SOURCE_DIR_KH_IMGUI={os.environ['FETCHCONTENT_SOURCE_DIR_KH_IMGUI']}")
    execute("cmake","-S",ENGINE/"native","-B",dest,"-G","Ninja","-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_C_COMPILER=clang","-DCMAKE_CXX_COMPILER=clang++",
            f"-DKALAM_RUST_LIB={rust / 'release' / ((library or package.replace('-', '_')) + '.lib')}",
            f"-DKALAM_GAME_NAME={game or ''}", *flags)
    execute("cmake","--build",dest,"--config","Release","--target","kalam")
    content = [] if game_manifest else [ENGINE/"assets"]
    for src in content + [BUILD/"assets/shaders/dx11"] + ([BUILD/"assets/shaders/spirv"] if vulkan else []):
        key = Path("shaders")/src.name if src.name in ("dx11","spirv") else Path()
        shutil.copytree(src,dest/"assets"/key,dirs_exist_ok=True)
    return dest / f"{game or 'kalam'}.exe"


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command",choices=("doctor","shaders","build","run","sim","editor"))
    parser.add_argument("--target",choices=("windows","android","all"))
    parser.add_argument("--scene",help="scene name for Windows run or headless sim")
    parser.add_argument("--script",type=Path,help="JSON command file for sim")
    parser.add_argument("--asset-root",type=Path,help="override the content directory for sim, editor or an external game")
    parser.add_argument("--game-manifest",type=Path,help="Cargo.toml of an external Windows game")
    parser.add_argument("--lens",action="store_true",help="open the ephemeral scene panel at startup")
    parser.add_argument("--smoke",action="store_true")
    parser.add_argument("--gui", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--backend",choices=("dx11","vulkan","gles"),help="backend override for run or doctor")
    parser.add_argument("--lifecycle-test",action="store_true",help="Windows resize, minimize and restore test")
    parser.add_argument("--capture",type=Path, help="TGA capture prefix")
    args=parser.parse_args()
    game_manifest = args.game_manifest.resolve() if args.game_manifest else None
    game_assets = None
    if game_manifest:
        if args.command not in ("build", "run", "editor") or args.target not in (None, "windows"):
            parser.error("--game-manifest requires Windows build, run or editor")
        if not game_manifest.is_file():
            parser.error(f"Game manifest does not exist: {game_manifest}")
        game_assets = args.asset_root.resolve() if args.asset_root else game_manifest.parent / "assets"
        if args.command != "build" and not game_assets.is_dir():
            parser.error(f"Game assets directory does not exist: {game_assets}. Set --asset-root to its path.")
    if args.gui and args.command != "editor":
        parser.error("--gui is only available with editor")
    if args.command == "sim":
        if not args.script or args.target or args.backend or args.capture or args.smoke or args.lifecycle_test or args.lens:
            parser.error("sim requires --script and accepts --scene and --asset-root")
        assets = args.asset_root.resolve() if args.asset_root else ENGINE/"assets"
        execute("cargo","run","--quiet","--locked","-p","kalam-runtime","--bin","kalam-sim","--",
                assets,args.scene or "room",args.script.resolve())
        return 0
    if args.command == "editor":
        if args.target not in (None, "windows") or args.backend or args.script or args.lens or args.lifecycle_test:
            parser.error("editor accepts --scene, --asset-root, --capture and --smoke on Windows")
        shaders("windows")
        binary = windows(False, editor=True, scene=args.scene, game_manifest=game_manifest)
        environment = os.environ.copy()
        root = game_assets or (args.asset_root.resolve() if args.asset_root else ENGINE/"assets")
        cwd = game_manifest.parent if game_manifest else ENGINE
        scene = args.scene or (None if game_manifest else "entities")
        launch = [binary, "--scene", scene] if scene else [binary]
        if args.capture:
            capture = args.capture.resolve()
            capture.parent.mkdir(parents=True, exist_ok=True)
            launch.extend(("--capture", capture))
        if args.smoke:
            with tempfile.TemporaryDirectory(dir=binary.parent.parent, prefix="editor-smoke-") as temporary:
                copy = Path(temporary) / "assets"
                shutil.copytree(root, copy)
                environment["KALAM_CONTENT_ROOT"] = str(copy)
                subprocess.run([*launch, "--smoke"], cwd=cwd, env=environment, check=True)
        else:
            environment["KALAM_CONTENT_ROOT"] = str(root)
            subprocess.run(launch, cwd=cwd, env=environment, check=True)
        return 0
    if args.script or (args.asset_root and not game_manifest):
        parser.error("--script requires sim, and --asset-root requires sim, editor or --game-manifest")
    target=args.target or ("all" if args.command in ("doctor","shaders") else "windows")
    if args.lens and (target != "windows" or args.command != "run"):
        parser.error("--lens requires run --target windows")
    if args.scene and (target != "windows" or args.command != "run"):
        parser.error("--scene requires run --target windows")
    if args.backend == "gles" and target != "android":
        parser.error("GLES is available only on Android")
    if args.command == "doctor":
        ok=True
        for tool in ("cargo","rustc","cmake","ninja","clang","clang++","git"):
            found=shutil.which(tool)
            print(f"{tool}: {found or 'missing'}", flush=True)
            ok &= bool(found)
        ok &= subprocess.call([PY,str(SHADERS),"--target",target,"--doctor"])==0
        if target in ("android","all"):
            ok &= subprocess.call([PY,str(ANDROID),"doctor"] + (["--gles"] if args.backend=="gles" else []))==0
        return 0 if ok else 1
    if args.command=="shaders":
        shaders(target)
        return 0
    if target=="all":
        parser.error("build and run take one target")
    if args.capture and args.command!="run":
        parser.error("--capture requires run")
    if (args.backend in ("dx11","vulkan") or args.lifecycle_test) and (target!="windows" or args.command!="run"):
        parser.error("--backend and --lifecycle-test require run --target windows")
    shaders("all" if args.backend=="vulkan" else target)
    if target=="android":
        execute(PY,ANDROID,args.command,*(["--gles"] if args.backend=="gles" else []), *(["--smoke"] if args.smoke else []), *(["--capture",args.capture.resolve()] if args.capture else []))
    else:
        binary=windows(args.backend=="vulkan", scene=args.scene, game_manifest=game_manifest)
        if args.command=="run":
            options=["--smoke"] if args.smoke else []
            if args.lens:
                options+=["--lens"]
            if args.scene:
                options+=["--scene",args.scene]
            if args.backend:
                options+=["--backend",args.backend]
            if args.lifecycle_test:
                options+=["--lifecycle-test"]
            if args.capture:
                args.capture.parent.mkdir(parents=True,exist_ok=True)
                options+=["--capture",str(args.capture.resolve())]
            if game_manifest:
                environment = os.environ.copy()
                environment["KALAM_CONTENT_ROOT"] = str(game_assets)
                subprocess.run([binary, *options], cwd=game_manifest.parent, env=environment, check=True)
            else:
                execute(binary,*options)
    return 0


if __name__=="__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
