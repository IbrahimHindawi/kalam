"""Build and deploy Kalam's SDL3 arm64 Android application from the host."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid
import zipfile

ROOT = Path(__file__).resolve().parent
ENGINE = ROOT.parent
WORKTREE = ENGINE.parent
SDL_PIN = "877399b2b2cf21e67554ed9046410f268ce1d1b2"
TARGET = "aarch64-linux-android"
NDK = "27.2.12479018"
CMAKE = "3.31.6"
APP = "org.kalam.renderer"
ACTIVITY = APP + "/.KalamActivity"
APK = ROOT / "app/build/outputs/apk/debug/app-debug.apk"
WIN = os.name == "nt"
EXE = ".exe" if WIN else ""


def check(cmd, **kwargs):
    return subprocess.run([str(arg) for arg in cmd], check=True, **kwargs)


def output(cmd):
    return check(cmd, text=True, capture_output=True).stdout.strip()


def sdk_root():
    for path in [os.getenv("ANDROID_HOME"), os.getenv("ANDROID_SDK_ROOT"),
                 Path(os.getenv("LOCALAPPDATA", "~")) / "Android/Sdk",
                 Path.home() / "Android/Sdk", Path.home() / "Library/Android/sdk",
                 ENGINE / "build/android-sdk"]:
        if path and Path(path).is_dir():
            return Path(path).resolve()
    return ENGINE / "build/android-sdk"


def java_home():
    home = os.getenv("JAVA_HOME")
    if home:
        return Path(home)
    java = shutil.which("javac")
    if java:
        return Path(java).resolve().parent.parent
    for base in (ENGINE / "build/android-sdk/jdk",):
        for javac in base.glob("*/bin/javac" + EXE):
            return javac.parent.parent
    return None


def sdl_source():
    explicit = os.getenv("KALAM_SDL_SOURCE")
    if explicit:
        return Path(explicit).resolve()
    common = Path(output(["git", "-C", WORKTREE, "rev-parse", "--path-format=absolute", "--git-common-dir"]))
    for origin in (WORKTREE, common.parent):
        for path in (origin / "vendor/SDL", origin / "build/_deps/sdl3-src"):
            if (path / "android-project/gradlew").is_file():
                return path.resolve()
    return ENGINE / "build/sources/SDL"


def ensure_sdl(path):
    if not path.exists() and not os.getenv("KALAM_SDL_SOURCE"):
        path.parent.mkdir(parents=True, exist_ok=True)
        check(["git", "init", path])
        check(["git", "-C", path, "fetch", "--depth", "1",
               "https://github.com/libsdl-org/SDL.git", SDL_PIN])
        check(["git", "-C", path, "checkout", "--detach", "FETCH_HEAD"])
    if output(["git", "-C", path, "rev-parse", "HEAD"]) != SDL_PIN:
        raise ValueError(f"SDL source must be pinned at {SDL_PIN}: {path}")


def tools(sdk):
    return {
        "SDK platform API 35": sdk / "platforms/android-35/android.jar",
        "build-tools 35.0.0": sdk / ("build-tools/35.0.0/aapt2" + EXE),
        f"NDK {NDK}": sdk / "ndk" / NDK / "source.properties",
        f"CMake {CMAKE}": sdk / "cmake" / CMAKE / "bin" / ("cmake" + EXE),
        "adb": sdk / "platform-tools" / ("adb" + EXE),
    }


def device(adb, serial, gles):
    lines = output([adb, "devices"]).splitlines()[1:]
    authorized = [line.split()[0] for line in lines if line.split()[1:] == ["device"]]
    if serial:
        if serial not in authorized:
            raise ValueError(f"device {serial} not authorized; adb devices: {lines}")
    elif len(authorized) == 1:
        serial = authorized[0]
    else:
        raise ValueError(f"select a connected, authorized device (--serial); adb devices: {lines}")
    command = [adb, "-s", serial]
    abi = output(command + ["shell", "getprop", "ro.product.cpu.abilist"])
    api = output(command + ["shell", "getprop", "ro.build.version.sdk"])
    features = output(command + ["shell", "pm", "list", "features"]).splitlines()
    vulkan = next((v for v in features if v.startswith("feature:android.hardware.vulkan.version=")), "")
    model = output(command + ["shell", "getprop", "ro.product.model"])
    print(f"Device {serial}: {model}, Android API {api}, ABI {abi}, {vulkan or 'no Vulkan'}")
    if "arm64-v8a" not in abi.split(",") or int(api) < 28:
        raise ValueError("requires arm64-v8a and API >= 28")
    if gles:
        version = output(command + ["shell", "getprop", "ro.opengles.version"])
        if version and int(version) < 0x30000:
            raise ValueError("requires OpenGL ES 3.0 or newer")
        return command
    if not vulkan:
        raise ValueError("requires Vulkan")
    # Some Android versions expose a real physical-device query through the
    # system GPU service. The renderer checks again on the selected bgfx device.
    query = subprocess.run(command + ["shell", "cmd", "gpu", "vkjson"], capture_output=True, text=True)
    try:
        devices = json.loads(query.stdout)["devices"]
    except (ValueError, KeyError):
        devices = []
    compatible = False
    for gpu in devices:
        props = gpu["properties"]
        # VkFormat R16G16B16A16_SFLOAT = 97, D16_UNORM = 124.
        features = {int(fmt): int(f["optimalTilingFeatures"]) for fmt, f in gpu["formats"]}
        hdr = (features.get(97, 0) & 0x81) == 0x81  # sampled image and color attachment
        depth = bool(features.get(124, 0) & 0x200)  # depth/stencil attachment
        compatible |= hdr and depth
        print(f"{props['deviceName']}, driver {int(props['driverVersion'])}, "
              f"RGBA16F sampled render target={hdr}, D16 depth target={depth}")
    if devices and not compatible:
        raise ValueError("no Vulkan device supports the scene's HDR and depth formats")
    return command


def verify_apk(shader_dirs):
    shaders = [f"{p}.{s}.bin" for p in ("pbr", "depth", "tone", "sprite") for s in ("vs", "fs")]
    assets = {f"assets/shaders/{folder.name}/{name}": folder / name
              for folder in shader_dirs for name in [*shaders, "manifest.json"]}
    source = ENGINE / "assets"
    for asset in source.rglob("*"):
        if asset.is_file():
            assets["assets/" + asset.relative_to(source).as_posix()] = asset
    with zipfile.ZipFile(APK) as apk:
        for name in ("libmain.so", "libSDL3.so"):
            apk.getinfo(f"lib/arm64-v8a/{name}")
        for name, file in assets.items():
            if apk.read(name) != file.read_bytes():
                raise ValueError(f"APK contains stale asset {name}")


def launch(adb, smoke, capture, gles):
    run_id = uuid.uuid4().hex
    check(adb + ["install", "-r", APK])
    check(adb + ["shell", "am", "start", "-W", "-S", "-n", ACTIVITY, "--es", "run_id", run_id,
                 "--ez", "smoke", str(smoke).lower(), "--ez", "capture", str(bool(capture)).lower(),
                 "--ez", "gles", str(gles).lower()])
    if not smoke and not capture:
        return
    # The activity launch result only confirms Java startup. Wait for this run's native exit.
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        messages = output(adb + ["logcat", "-d", "-s", "Kalam:I"])
        result = next((line for line in messages.splitlines() if f"run={run_id}" in line), None)
        if result:
            print(result)
            if f"native exit 0 run={run_id}" not in result:
                raise ValueError("native Android run failed; inspect adb logcat -s SDL Kalam AndroidRuntime")
            break
        time.sleep(0.5)
    else:
        raise ValueError("native Android run timed out; inspect adb logcat -s SDL Kalam AndroidRuntime")
    if capture:
        path = f"files/capture-{run_id}.tga"
        data = check(adb + ["exec-out", "run-as", APP, "cat", path], capture_output=True).stdout
        if len(data) < 18 or data[2] not in (2, 10) or data[16] not in (24, 32):
            raise ValueError("native run did not produce a TGA capture")
        destination = Path(str(capture) + ".tga")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        check(adb + ["shell", "run-as", APP, "rm", path])
        print(f"Captured {destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["doctor", "build", "run"])
    parser.add_argument("--serial", default=os.getenv("ANDROID_SERIAL"))
    parser.add_argument("--gles", action="store_true", help="select experimental OpenGL ES")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--capture", type=Path, help="copy the native TGA capture to <prefix>.tga")
    args = parser.parse_args()
    sdk = sdk_root()
    required = tools(sdk)
    jdk = java_home()
    installed = set(output(["rustup", "target", "list", "--installed"]).splitlines())
    valid = True
    for label, path in required.items():
        print(f"{label}: {path} ({'ok' if path.is_file() else 'missing'})", flush=True)
        valid &= path.is_file()
    print(f"JDK: {jdk or 'missing'}; Rust {TARGET}: {'ok' if TARGET in installed else 'missing'}", flush=True)
    valid &= bool(jdk and (jdk / "bin" / ("javac" + EXE)).is_file()) and TARGET in installed
    adb = None
    if args.command != "build" and required["adb"].exists():
        adb = device(required["adb"], args.serial, args.gles)
    if not valid:
        raise ValueError("host dependencies missing; see engine/android/README.md")
    if args.command == "doctor":
        return
    sdl = sdl_source()
    ensure_sdl(sdl)
    env = dict(os.environ, ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk), JAVA_HOME=str(jdk))
    target = (ENGINE / "build/android-rust").resolve()
    check(["cargo", "build", "--locked", "--target", TARGET, "--release", "--target-dir", target,
           "-p", "kalam-host"], cwd=WORKTREE, env=env)
    rust_lib = target / TARGET / "release/libkalam_host.a"
    shaders = [ENGINE / "build/assets/shaders" / backend for backend in ("spirv", "essl")]
    for folder in shaders:
        if not (folder / "manifest.json").is_file():
            raise ValueError(f"missing {folder.name} shader assets; run cargo xtask shaders --target android")
    wrapper = sdl / "android-project" / ("gradlew.bat" if WIN else "gradlew")
    check([wrapper, "--project-dir", ROOT, f"-PkalamSdl={sdl}", f"-PkalamRustLib={rust_lib}",
           ":app:assembleDebug"], env=env)
    verify_apk(shaders)
    print(f"Built {APK}")
    if args.command == "run":
        launch(adb, args.smoke, args.capture, args.gles)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"Android: {error}", file=sys.stderr)
        sys.exit(1)
