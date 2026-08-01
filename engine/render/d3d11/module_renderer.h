#ifndef KALAM_ENGINE_RENDER_D3D11_MODULE_RENDERER_H
#define KALAM_ENGINE_RENDER_D3D11_MODULE_RENDERER_H

#include <stdint.h>

typedef uint32_t Module_Renderer_Result;

enum
{
    Module_Renderer_Result_Ok = 0,
    Module_Renderer_Result_Invalid_Argument,
    Module_Renderer_Result_Shader_Compile_Failed,
    Module_Renderer_Result_Resource_Create_Failed,
    Module_Renderer_Result_Device_Removed,
    Module_Renderer_Result_Resource_Missing,
    Module_Renderer_Result_Resource_Invalid
};

/*
 * A render target is borrowed for one render call. The renderer never retains,
 * releases, resizes, or presents either view.
 */
typedef struct Module_Render_Target
{
    void* d3d11_render_target_view;
    void* d3d11_depth_stencil_view;
    uint32_t width;
    uint32_t height;
    uint64_t generation;
} Module_Render_Target;

typedef struct Module_Render_Cube
{
    float translation[3];
    float rotation_degrees[3];
    float scale[3];
    float color[4];
    uint32_t draw_transform_gizmo;
} Module_Render_Cube;

typedef struct Module_Render_Post_Process
{
    float exposure;
    float bloom_threshold;
    float vignette;
    float reserved;
    float tint[4];
} Module_Render_Post_Process;

/*
 * All game-specific GPU objects are owned by this structure. device and
 * immediate_context are borrowed from the stable host and are never AddRef'd
 * or released by the renderer.
 */
typedef struct Module_Renderer
{
    void* device;
    void* immediate_context;

    void* vertex_shader;
    void* pixel_shader;
    void* input_layout;
    void* vertex_buffer;
    void* index_buffer;
    void* constant_buffer;
    void* rasterizer_state;
    void* depth_enabled_state;
    void* depth_disabled_state;
    void* blend_state;

    void* surface_texture;
    void* surface_texture_view;
    void* surface_sampler;

    int32_t last_hresult;
    uint32_t initialized;

    /*
     * The manifest resource that failed to load, when create returns
     * Resource_Missing or Resource_Invalid. Points at generated table
     * data with static lifetime.
     */
    const char* failed_resource_path;
} Module_Renderer;

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * project_root resolves the manifest resource paths baked into the
 * generated tables (docs/resource_manifest.md section 6). It is borrowed
 * for the duration of the call only. A null or empty root, a missing
 * file, or an unreadable resource fails create with the offending path in
 * failed_resource_path.
 */
Module_Renderer_Result module_renderer_create(
    Module_Renderer* out_renderer,
    void* borrowed_d3d11_device,
    void* borrowed_d3d11_immediate_context,
    const char* project_root,
    uint64_t project_root_size);

void module_renderer_destroy(Module_Renderer* renderer);

Module_Renderer_Result module_renderer_render_cube(
    Module_Renderer* renderer,
    const Module_Render_Target* target,
    const Module_Render_Cube* cube,
    const Module_Render_Post_Process* post_process);

const char* module_renderer_result_string(Module_Renderer_Result result);

#if defined(__cplusplus)
}
#endif

#endif
