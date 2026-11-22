# Kalam renderer plan

Ship one fixed reference scene on Windows x64/D3D11 and Android arm64/Vulkan. The original implementation started from a moving-sprite prototype. Cargo and Rust std own scene data, material validation and the frame loop. SDL3 owns windows, events and native surface lifetime; bgfx owns the device, passes, submission and presentation. Use Slang for all authored shaders. Use Filament's [PBR reference](https://google.github.io/filament/Filament.md.html) and [reference sources at v1.67.0](https://github.com/google/filament/tree/7f717f8f922fadb03101a76ba0112d5ec88751c0/shaders/src) to select and validate material conventions.

## Bring up both platforms

1. Use the pinned SDL3 revision, matching bgfx, bx, bimg, shaderc and a pinned Slang compiler. Produce a Windows executable and an APK with packaged data. Doctor reports tools, Rust targets, and device capabilities.
2. Pass HWND or ANativeWindow to bgfx. Run events on the platform event thread, handle physical pixel dimensions, minimization, pause/resume and surface replacement. Rebuild lost GPU resources from retained asset descriptions. Mouse drag and one-finger drag orbit the camera.
3. Compile each Slang stage offline through the bgfx packager. Record compiler identities, entries, vertex layout, uniforms, and target. Gate native backend support on a textured, transformed indexed mesh and a capture with the expected orientation and color. Neither deployed app needs shader compilers.

## Scene and resource contract

Use typed generational handles, renderer-owned resources, one explicitly unloadable scene group, and flat render packets allocated in frame scratch. Uploads must outlive submission via owned copies. A JSON material descriptor has a version, stable asset references, base color factor, metallic and roughness factors, and optional texture references. Validate new materials before replacing live data; errors preserve the previous revision.

## Render milestones

1. Direct forward lighting with GGX distribution, correlated Smith visibility, Schlick Fresnel and Lambert diffuse. Keep albedo in sRGB textures and material parameters in linear channels.
2. Scene-linear HDR, fixed exposure, explicit tone curve and a single display transfer.
3. Baked diffuse irradiance, roughness-filtered cube mips and split-sum BRDF lookup; one directional shadow map with explicit bias and filtering.
4. Screen-space alpha sprites over the tonemapped output. A fixed-camera scene includes a metallic/roughness sweep, asymmetric texture, ground receiver, and sprite. Expose application inputs without requiring a particular ECS.

## Release evidence

Build from pinned inputs. Test material replacement and failed edits, stale handle generations, repeated scene reload and resource baselines. Validate selected shader samples against CPU reference values, roughness endpoints, texture color spaces, sprite blending, shadow orientation and tolerant image comparisons. Exercise Windows resize/minimize and Android touch/rotation/background/foreground on actual devices. Record backend, GPU/driver, output resolution and frame timings before defining performance budgets. Future desktop ports must pass the same checks before being declared supported.

Use the implementation README for current commands and qualification status. This plan defines desired behavior, not a record that any particular hardware check has passed.
