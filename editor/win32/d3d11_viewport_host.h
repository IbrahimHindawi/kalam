#ifndef KALAM_EDITOR_WIN32_D3D11_VIEWPORT_HOST_H
#define KALAM_EDITOR_WIN32_D3D11_VIEWPORT_HOST_H

#include <stdint.h>

#include "game_api.h"

typedef uint32_t D3D11_Viewport_Result;

enum
{
    D3D11_Viewport_Result_Ok = 0,
    D3D11_Viewport_Result_Skipped,
    D3D11_Viewport_Result_Invalid_Argument,
    D3D11_Viewport_Result_Invalid_State,
    D3D11_Viewport_Result_Generation_Exhausted,
    D3D11_Viewport_Result_Device_Create_Failed,
    D3D11_Viewport_Result_Factory_Query_Failed,
    D3D11_Viewport_Result_Swap_Chain_Create_Failed,
    D3D11_Viewport_Result_Back_Buffer_Failed,
    D3D11_Viewport_Result_Render_Target_Create_Failed,
    D3D11_Viewport_Result_Depth_Texture_Create_Failed,
    D3D11_Viewport_Result_Depth_View_Create_Failed,
    D3D11_Viewport_Result_Resize_Failed,
    D3D11_Viewport_Result_Device_Removed,
    D3D11_Viewport_Result_Present_Failed
};

typedef uint32_t D3D11_Viewport_Driver;

enum
{
    D3D11_Viewport_Driver_Hardware = 0,
    D3D11_Viewport_Driver_Warp = 1
};

enum
{
    D3D11_Viewport_Create_Enable_Debug_Layer = 1u << 0,
    D3D11_Viewport_Create_Require_Debug_Layer = 1u << 1
};

typedef struct D3D11_Viewport_Create_Desc
{
    uint32_t struct_size;
    uint32_t flags;

    void* window_handle; // Borrowed HWND, owned by the Qt or Win32 host.
    uint32_t width;
    uint32_t height;

    D3D11_Viewport_Driver driver;
    uint32_t reserved;
} D3D11_Viewport_Create_Desc;

/*
 * Stable host-owned D3D11 state. COM pointers are kept opaque here so this
 * header cannot leak D3D11/DXGI types into bridge or simulation headers.
 * The host is single-threaded and all functions must be called by its owner.
 */
typedef struct D3D11_Viewport_Host
{
    void* window_handle;

    void* device;
    void* immediate_context;
    void* dxgi_factory;
    void* swap_chain;

    void* render_target_view;
    void* depth_texture;
    void* depth_stencil_view;

    uint32_t width;
    uint32_t height;
    uint64_t target_generation;

    uint32_t feature_level;
    uint32_t device_creation_flags;
    uint32_t device_create_attempt_count;
    D3D11_Viewport_Driver driver;

    uint32_t initialized;
    uint32_t target_available;
    uint32_t occluded;
    uint32_t device_lost;

    uint32_t used_feature_level_retry;
    uint32_t debug_layer_enabled;
    uint32_t debug_layer_fallback;
    uint32_t reserved;

    int32_t last_hresult;
    int32_t device_removed_reason;
    D3D11_Viewport_Result last_result;
    uint32_t reserved_result;
} D3D11_Viewport_Host;

#if defined(__cplusplus)
extern "C" {
#endif

D3D11_Viewport_Result d3d11_viewport_host_create(
    D3D11_Viewport_Host* out_host,
    const D3D11_Viewport_Create_Desc* desc);

void d3d11_viewport_host_destroy(D3D11_Viewport_Host* host);

D3D11_Viewport_Result d3d11_viewport_host_resize(
    D3D11_Viewport_Host* host,
    uint32_t width,
    uint32_t height);

D3D11_Viewport_Result d3d11_viewport_host_clear(
    D3D11_Viewport_Host* host,
    const float color_rgba[4],
    float depth,
    uint8_t stencil);

D3D11_Viewport_Result d3d11_viewport_host_present(
    D3D11_Viewport_Host* host,
    uint32_t sync_interval);

void d3d11_viewport_host_clear_bindings(
    D3D11_Viewport_Host* host,
    uint32_t flush_context);

uint32_t d3d11_viewport_host_get_render_target(
    const D3D11_Viewport_Host* host,
    Host_Render_Target* out_target);

const char* d3d11_viewport_result_string(D3D11_Viewport_Result result);

#if defined(__cplusplus)
}
#endif

#endif
