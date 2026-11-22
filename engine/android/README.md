# Android renderer

The debug APK runs the Rust engine through SDL3's Android activity using bgfx. It requires an arm64 device with Android 9 (API 28) or newer. Vulkan is the default backend. The experimental OpenGL ES 3 path uses the same Slang shaders and requires sampled RGBA16F render targets. SDL supplies the Java activity, native entry point and input. The surface callback gives the renderer a counted ANativeWindow reference so it can release and rebuild device resources when the surface changes.

Use JDK 17, the Android command-line SDK manager, a C++ host build environment and Python 3. Set ANDROID_HOME and JAVA_HOME. Install the Android components through the SDK manager (review its license prompts), then the Rust target:

```powershell
sdkmanager "platform-tools" "platforms;android-35" "build-tools;35.0.0" "ndk;27.2.12479018" "cmake;3.31.6"
rustup target add aarch64-linux-android
```

From the renderer worktree run:

```powershell
cargo xtask doctor --target android
cargo xtask build --target android
cargo xtask run --target android --smoke
cargo xtask run --target android --capture engine/build/captures/android
```

The Android helper uses the Gradle 8.12 wrapper and Java sources from the repository-pinned SDL checkout and Android Gradle Plugin 8.7.3. Set KALAM_SDL_SOURCE for an existing checkout at that revision. A clean host fetches it at build time. Build packages one arm64-v8a APK at `engine/android/app/build/outputs/apk/debug/app-debug.apk`. The host compares its included material, environment and Vulkan shader records against the source assets. Building does not need a connected device. Enable USB debugging and authorize the host to run on a device. Set ANDROID_SERIAL to select a device when several are attached.

The smoke runner waits for the native exit code. Capture mode saves the GPU readback to the supplied prefix plus `.tga`. For runtime diagnostics use `adb logcat -s SDL Kalam AndroidRuntime`. An interactive run should orbit on a one-finger drag. Rotate during rendering, switch Home, then resume the app several times. Verify that frames return, orbit still responds, and the renderer reports no stale handles or device errors. Record the device, Vulkan renderer, surface dimensions and frame timing alongside the fixed-scene capture.

To select OpenGL ES, run `cargo xtask run --target android --backend gles --smoke`.
The APK contains both Vulkan and GLSL ES shader packages. All four GLSL ES programs
compiled and linked in a host WebGL2 context. Android presentation and capture
still need verification on a test device.
