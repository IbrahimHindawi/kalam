#include "d3d11_viewport_host.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <string.h>

static void
d3d11_viewport_zero_host(D3D11_Viewport_Host* host)
{
    memset(host, 0, sizeof(*host));
}

static void
d3d11_viewport_release_unknown(void** object)
{
    IUnknown* unknown = 0;

    if (object == 0 || *object == 0)
    {
        return;
    }

    unknown = (IUnknown*)*object;
    unknown->Release();
    *object = 0;
}

static D3D11_Viewport_Result
d3d11_viewport_report(
    D3D11_Viewport_Host* host,
    D3D11_Viewport_Result result,
    HRESULT hresult)
{
    if (host != 0)
    {
        host->last_result = result;
        host->last_hresult = (int32_t)hresult;
    }
    return result;
}

static uint32_t
d3d11_viewport_is_device_removed_hresult(HRESULT hresult)
{
    return hresult == DXGI_ERROR_DEVICE_REMOVED ||
        hresult == DXGI_ERROR_DEVICE_RESET ||
        hresult == DXGI_ERROR_DEVICE_HUNG ||
        hresult == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

static D3D11_Viewport_Result
d3d11_viewport_report_device_failure(
    D3D11_Viewport_Host* host,
    D3D11_Viewport_Result ordinary_result,
    HRESULT hresult)
{
    ID3D11Device* device = 0;
    HRESULT removed_reason = hresult;

    if (!d3d11_viewport_is_device_removed_hresult(hresult))
    {
        return d3d11_viewport_report(host, ordinary_result, hresult);
    }

    if (host != 0)
    {
        host->device_lost = 1;
        device = (ID3D11Device*)host->device;
        if (device != 0)
        {
            removed_reason = device->GetDeviceRemovedReason();
        }
        host->device_removed_reason = (int32_t)removed_reason;
    }

    return d3d11_viewport_report(
        host,
        D3D11_Viewport_Result_Device_Removed,
        hresult);
}

static HRESULT
d3d11_viewport_create_device_attempt(
    D3D11_Viewport_Host* host,
    D3D_DRIVER_TYPE driver_type,
    UINT creation_flags,
    const D3D_FEATURE_LEVEL* levels,
    UINT level_count,
    ID3D11Device** out_device,
    D3D_FEATURE_LEVEL* out_feature_level,
    ID3D11DeviceContext** out_context)
{
    host->device_create_attempt_count += 1;
    return D3D11CreateDevice(
        0,
        driver_type,
        0,
        creation_flags,
        levels,
        level_count,
        D3D11_SDK_VERSION,
        out_device,
        out_feature_level,
        out_context);
}

static HRESULT
d3d11_viewport_create_device(
    D3D11_Viewport_Host* host,
    const D3D11_Viewport_Create_Desc* desc,
    ID3D11Device** out_device,
    D3D_FEATURE_LEVEL* out_feature_level,
    ID3D11DeviceContext** out_context)
{
    static const D3D_FEATURE_LEVEL requested_levels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    D3D_DRIVER_TYPE driver_type = D3D_DRIVER_TYPE_HARDWARE;
    UINT base_flags =
        D3D11_CREATE_DEVICE_BGRA_SUPPORT |
        D3D11_CREATE_DEVICE_SINGLETHREADED;
    UINT creation_flags = base_flags;
    uint32_t debug_requested = 0;
    uint32_t debug_required = 0;
    uint32_t flags_pass = 0;
    HRESULT hresult = E_FAIL;

    if (desc->driver == D3D11_Viewport_Driver_Warp)
    {
        driver_type = D3D_DRIVER_TYPE_WARP;
    }

    debug_requested =
        (desc->flags & D3D11_Viewport_Create_Enable_Debug_Layer) != 0 ||
        (desc->flags & D3D11_Viewport_Create_Require_Debug_Layer) != 0;
    debug_required =
        (desc->flags & D3D11_Viewport_Create_Require_Debug_Layer) != 0;

    if (debug_requested)
    {
        creation_flags |= D3D11_CREATE_DEVICE_DEBUG;
    }

    for (flags_pass = 0; flags_pass < 2; flags_pass += 1)
    {
        hresult = d3d11_viewport_create_device_attempt(
            host,
            driver_type,
            creation_flags,
            requested_levels,
            (UINT)(sizeof(requested_levels) / sizeof(requested_levels[0])),
            out_device,
            out_feature_level,
            out_context);

        if (hresult == E_INVALIDARG)
        {
            host->used_feature_level_retry = 1;
            hresult = d3d11_viewport_create_device_attempt(
                host,
                driver_type,
                creation_flags,
                requested_levels + 1,
                (UINT)(sizeof(requested_levels) / sizeof(requested_levels[0]) - 1),
                out_device,
                out_feature_level,
                out_context);
        }

        if (SUCCEEDED(hresult))
        {
            host->device_creation_flags = creation_flags;
            host->debug_layer_enabled =
                (creation_flags & D3D11_CREATE_DEVICE_DEBUG) != 0;
            return hresult;
        }

        if ((creation_flags & D3D11_CREATE_DEVICE_DEBUG) == 0 ||
            debug_required ||
            hresult != DXGI_ERROR_SDK_COMPONENT_MISSING)
        {
            break;
        }

        d3d11_viewport_release_unknown((void**)out_context);
        d3d11_viewport_release_unknown((void**)out_device);
        creation_flags = base_flags;
        host->debug_layer_fallback = 1;
    }

    return hresult;
}

static HRESULT
d3d11_viewport_query_factory(
    ID3D11Device* device,
    IDXGIFactory1** out_factory)
{
    IDXGIDevice* dxgi_device = 0;
    IDXGIAdapter* adapter = 0;
    HRESULT hresult = E_FAIL;

    hresult = device->QueryInterface(
        __uuidof(IDXGIDevice),
        (void**)&dxgi_device);

    if (FAILED(hresult))
    {
        return hresult;
    }

    hresult = dxgi_device->GetAdapter(&adapter);
    if (SUCCEEDED(hresult))
    {
        hresult = adapter->GetParent(
            __uuidof(IDXGIFactory1),
            (void**)out_factory);
    }

    d3d11_viewport_release_unknown((void**)&adapter);
    d3d11_viewport_release_unknown((void**)&dxgi_device);
    return hresult;
}

void
d3d11_viewport_host_clear_bindings(
    D3D11_Viewport_Host* host,
    uint32_t flush_context)
{
    ID3D11DeviceContext* context = 0;

    if (host == 0)
    {
        return;
    }

    context = (ID3D11DeviceContext*)host->immediate_context;
    if (context == 0)
    {
        return;
    }

    context->OMSetRenderTargets(0, 0, 0);
    context->ClearState();
    if (flush_context)
    {
        context->Flush();
    }
}

static void
d3d11_viewport_release_targets(D3D11_Viewport_Host* host)
{
    d3d11_viewport_host_clear_bindings(host, 0);
    d3d11_viewport_release_unknown(&host->depth_stencil_view);
    d3d11_viewport_release_unknown(&host->depth_texture);
    d3d11_viewport_release_unknown(&host->render_target_view);
    host->target_available = 0;
}

static D3D11_Viewport_Result
d3d11_viewport_create_swap_chain(
    D3D11_Viewport_Host* host,
    uint32_t width,
    uint32_t height)
{
    IDXGIFactory1* factory = (IDXGIFactory1*)host->dxgi_factory;
    ID3D11Device* device = (ID3D11Device*)host->device;
    IDXGISwapChain* swap_chain = 0;
    DXGI_SWAP_CHAIN_DESC swap_desc;
    HRESULT hresult = E_FAIL;

    memset(&swap_desc, 0, sizeof(swap_desc));
    swap_desc.BufferDesc.Width = width;
    swap_desc.BufferDesc.Height = height;
    swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = 2;
    swap_desc.OutputWindow = (HWND)host->window_handle;
    swap_desc.Windowed = TRUE;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hresult = factory->CreateSwapChain(device, &swap_desc, &swap_chain);
    if (FAILED(hresult))
    {
        return d3d11_viewport_report_device_failure(
            host,
            D3D11_Viewport_Result_Swap_Chain_Create_Failed,
            hresult);
    }

    hresult = factory->MakeWindowAssociation(
        (HWND)host->window_handle,
        DXGI_MWA_NO_ALT_ENTER);

    if (FAILED(hresult))
    {
        d3d11_viewport_release_unknown((void**)&swap_chain);
        return d3d11_viewport_report(
            host,
            D3D11_Viewport_Result_Swap_Chain_Create_Failed,
            hresult);
    }

    host->swap_chain = swap_chain;
    return D3D11_Viewport_Result_Ok;
}

static D3D11_Viewport_Result
d3d11_viewport_create_targets(
    D3D11_Viewport_Host* host,
    uint32_t width,
    uint32_t height)
{
    ID3D11Device* device = (ID3D11Device*)host->device;
    IDXGISwapChain* swap_chain = (IDXGISwapChain*)host->swap_chain;
    ID3D11Texture2D* back_buffer = 0;
    ID3D11RenderTargetView* render_target_view = 0;
    ID3D11Texture2D* depth_texture = 0;
    ID3D11DepthStencilView* depth_stencil_view = 0;
    D3D11_TEXTURE2D_DESC depth_desc;
    HRESULT hresult = E_FAIL;
    D3D11_Viewport_Result result = D3D11_Viewport_Result_Ok;

    hresult = swap_chain->GetBuffer(
        0,
        __uuidof(ID3D11Texture2D),
        (void**)&back_buffer);
    if (FAILED(hresult))
    {
        result = d3d11_viewport_report_device_failure(
            host,
            D3D11_Viewport_Result_Back_Buffer_Failed,
            hresult);
        goto cleanup;
    }

    hresult = device->CreateRenderTargetView(
        back_buffer,
        0,
        &render_target_view);
    if (FAILED(hresult))
    {
        result = d3d11_viewport_report_device_failure(
            host,
            D3D11_Viewport_Result_Render_Target_Create_Failed,
            hresult);
        goto cleanup;
    }

    memset(&depth_desc, 0, sizeof(depth_desc));
    depth_desc.Width = width;
    depth_desc.Height = height;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hresult = device->CreateTexture2D(
        &depth_desc,
        0,
        &depth_texture);
    if (FAILED(hresult))
    {
        result = d3d11_viewport_report_device_failure(
            host,
            D3D11_Viewport_Result_Depth_Texture_Create_Failed,
            hresult);
        goto cleanup;
    }

    hresult = device->CreateDepthStencilView(
        depth_texture,
        0,
        &depth_stencil_view);
    if (FAILED(hresult))
    {
        result = d3d11_viewport_report_device_failure(
            host,
            D3D11_Viewport_Result_Depth_View_Create_Failed,
            hresult);
        goto cleanup;
    }

    host->render_target_view = render_target_view;
    host->depth_texture = depth_texture;
    host->depth_stencil_view = depth_stencil_view;
    host->target_available = 1;
    render_target_view = 0;
    depth_texture = 0;
    depth_stencil_view = 0;
    result = d3d11_viewport_report(
        host,
        D3D11_Viewport_Result_Ok,
        S_OK);

cleanup:
    d3d11_viewport_release_unknown((void**)&depth_stencil_view);
    d3d11_viewport_release_unknown((void**)&depth_texture);
    d3d11_viewport_release_unknown((void**)&render_target_view);
    d3d11_viewport_release_unknown((void**)&back_buffer);
    return result;
}

static D3D11_Viewport_Result
d3d11_viewport_advance_generation(D3D11_Viewport_Host* host)
{
    if (host->target_generation == UINT64_MAX)
    {
        return d3d11_viewport_report(
            host,
            D3D11_Viewport_Result_Generation_Exhausted,
            E_FAIL);
    }

    host->target_generation += 1;
    return D3D11_Viewport_Result_Ok;
}

D3D11_Viewport_Result
d3d11_viewport_host_create(
    D3D11_Viewport_Host* out_host,
    const D3D11_Viewport_Create_Desc* desc)
{
    ID3D11Device* device = 0;
    ID3D11DeviceContext* context = 0;
    IDXGIFactory1* factory = 0;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_10_0;
    HRESULT hresult = E_FAIL;
    D3D11_Viewport_Result result = D3D11_Viewport_Result_Ok;

    if (out_host == 0 || desc == 0)
    {
        return D3D11_Viewport_Result_Invalid_Argument;
    }

    if (out_host->initialized ||
        out_host->device != 0 ||
        out_host->immediate_context != 0 ||
        out_host->swap_chain != 0)
    {
        return d3d11_viewport_report(
            out_host,
            D3D11_Viewport_Result_Invalid_State,
            E_UNEXPECTED);
    }

    d3d11_viewport_zero_host(out_host);

    if (desc->struct_size < sizeof(D3D11_Viewport_Create_Desc) ||
        desc->window_handle == 0 ||
        !IsWindow((HWND)desc->window_handle) ||
        (desc->driver != D3D11_Viewport_Driver_Hardware &&
         desc->driver != D3D11_Viewport_Driver_Warp))
    {
        return d3d11_viewport_report(
            out_host,
            D3D11_Viewport_Result_Invalid_Argument,
            E_INVALIDARG);
    }

    out_host->window_handle = desc->window_handle;
    out_host->driver = desc->driver;

    hresult = d3d11_viewport_create_device(
        out_host,
        desc,
        &device,
        &feature_level,
        &context);
    if (FAILED(hresult))
    {
        d3d11_viewport_release_unknown((void**)&context);
        d3d11_viewport_release_unknown((void**)&device);
        return d3d11_viewport_report(
            out_host,
            D3D11_Viewport_Result_Device_Create_Failed,
            hresult);
    }

    hresult = d3d11_viewport_query_factory(device, &factory);
    if (FAILED(hresult))
    {
        d3d11_viewport_release_unknown((void**)&context);
        d3d11_viewport_release_unknown((void**)&device);
        return d3d11_viewport_report(
            out_host,
            D3D11_Viewport_Result_Factory_Query_Failed,
            hresult);
    }

    out_host->device = device;
    out_host->immediate_context = context;
    out_host->dxgi_factory = factory;
    out_host->feature_level = (uint32_t)feature_level;
    out_host->initialized = 1;
    out_host->width = desc->width;
    out_host->height = desc->height;

    if (desc->width == 0 || desc->height == 0)
    {
        return d3d11_viewport_report(
            out_host,
            D3D11_Viewport_Result_Ok,
            S_OK);
    }

    result = d3d11_viewport_create_swap_chain(
        out_host,
        desc->width,
        desc->height);
    if (result != D3D11_Viewport_Result_Ok)
    {
        d3d11_viewport_host_destroy(out_host);
        return result;
    }

    result = d3d11_viewport_create_targets(
        out_host,
        desc->width,
        desc->height);
    if (result != D3D11_Viewport_Result_Ok)
    {
        d3d11_viewport_host_destroy(out_host);
        return result;
    }

    result = d3d11_viewport_advance_generation(out_host);
    if (result != D3D11_Viewport_Result_Ok)
    {
        d3d11_viewport_host_destroy(out_host);
        return result;
    }

    return d3d11_viewport_report(
        out_host,
        D3D11_Viewport_Result_Ok,
        S_OK);
}

void
d3d11_viewport_host_destroy(D3D11_Viewport_Host* host)
{
    IDXGISwapChain* swap_chain = 0;

    if (host == 0)
    {
        return;
    }

    d3d11_viewport_host_clear_bindings(host, 1);
    d3d11_viewport_release_targets(host);

    swap_chain = (IDXGISwapChain*)host->swap_chain;
    if (swap_chain != 0)
    {
        swap_chain->SetFullscreenState(FALSE, 0);
    }

    d3d11_viewport_release_unknown(&host->swap_chain);
    d3d11_viewport_release_unknown(&host->dxgi_factory);
    d3d11_viewport_release_unknown(&host->immediate_context);
    d3d11_viewport_release_unknown(&host->device);
    d3d11_viewport_zero_host(host);
}

D3D11_Viewport_Result
d3d11_viewport_host_resize(
    D3D11_Viewport_Host* host,
    uint32_t width,
    uint32_t height)
{
    IDXGISwapChain* swap_chain = 0;
    HRESULT hresult = E_FAIL;
    D3D11_Viewport_Result result = D3D11_Viewport_Result_Ok;
    uint32_t needs_new_generation = 0;

    if (host == 0 || !host->initialized || host->device_lost)
    {
        return d3d11_viewport_report(
            host,
            D3D11_Viewport_Result_Invalid_State,
            E_UNEXPECTED);
    }

    if (host->width == width &&
        host->height == height &&
        host->target_available)
    {
        return d3d11_viewport_report(
            host,
            D3D11_Viewport_Result_Ok,
            S_OK);
    }

    if (host->width == 0 && host->height == 0 &&
        width == 0 && height == 0 &&
        !host->target_available)
    {
        return d3d11_viewport_report(
            host,
            D3D11_Viewport_Result_Skipped,
            S_OK);
    }

    needs_new_generation =
        host->target_available || host->target_generation != 0;

    if (needs_new_generation)
    {
        result = d3d11_viewport_advance_generation(host);
        if (result != D3D11_Viewport_Result_Ok)
        {
            return result;
        }
    }

    d3d11_viewport_release_targets(host);
    host->width = width;
    host->height = height;
    host->occluded = 0;

    if (width == 0 || height == 0)
    {
        return d3d11_viewport_report(
            host,
            D3D11_Viewport_Result_Skipped,
            S_OK);
    }

    swap_chain = (IDXGISwapChain*)host->swap_chain;
    if (swap_chain == 0)
    {
        result = d3d11_viewport_create_swap_chain(host, width, height);
        if (result != D3D11_Viewport_Result_Ok)
        {
            return result;
        }
    }
    else
    {
        hresult = swap_chain->ResizeBuffers(
            0,
            width,
            height,
            DXGI_FORMAT_UNKNOWN,
            0);
        if (FAILED(hresult))
        {
            return d3d11_viewport_report_device_failure(
                host,
                D3D11_Viewport_Result_Resize_Failed,
                hresult);
        }
    }

    result = d3d11_viewport_create_targets(host, width, height);
    if (result != D3D11_Viewport_Result_Ok)
    {
        return result;
    }

    if (!needs_new_generation)
    {
        result = d3d11_viewport_advance_generation(host);
        if (result != D3D11_Viewport_Result_Ok)
        {
            d3d11_viewport_release_targets(host);
            return result;
        }
    }

    return d3d11_viewport_report(
        host,
        D3D11_Viewport_Result_Ok,
        S_OK);
}

D3D11_Viewport_Result
d3d11_viewport_host_clear(
    D3D11_Viewport_Host* host,
    const float color_rgba[4],
    float depth,
    uint8_t stencil)
{
    ID3D11DeviceContext* context = 0;
    ID3D11RenderTargetView* render_target_view = 0;
    ID3D11DepthStencilView* depth_stencil_view = 0;

    if (host == 0 || !host->initialized || color_rgba == 0)
    {
        return d3d11_viewport_report(
            host,
            D3D11_Viewport_Result_Invalid_Argument,
            E_INVALIDARG);
    }

    if (!host->target_available ||
        host->width == 0 ||
        host->height == 0 ||
        host->occluded ||
        host->device_lost)
    {
        return d3d11_viewport_report(
            host,
            D3D11_Viewport_Result_Skipped,
            S_OK);
    }

    if (depth < 0.0f || depth > 1.0f)
    {
        return d3d11_viewport_report(
            host,
            D3D11_Viewport_Result_Invalid_Argument,
            E_INVALIDARG);
    }

    context = (ID3D11DeviceContext*)host->immediate_context;
    render_target_view = (ID3D11RenderTargetView*)host->render_target_view;
    depth_stencil_view = (ID3D11DepthStencilView*)host->depth_stencil_view;

    context->ClearRenderTargetView(render_target_view, color_rgba);
    context->ClearDepthStencilView(
        depth_stencil_view,
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
        depth,
        stencil);

    return d3d11_viewport_report(
        host,
        D3D11_Viewport_Result_Ok,
        S_OK);
}

D3D11_Viewport_Result
d3d11_viewport_host_present(
    D3D11_Viewport_Host* host,
    uint32_t sync_interval)
{
    IDXGISwapChain* swap_chain = 0;
    HRESULT hresult = S_OK;

    if (host == 0 || !host->initialized || sync_interval > 4)
    {
        return d3d11_viewport_report(
            host,
            D3D11_Viewport_Result_Invalid_Argument,
            E_INVALIDARG);
    }

    if (!host->target_available ||
        host->width == 0 ||
        host->height == 0 ||
        host->device_lost)
    {
        return d3d11_viewport_report(
            host,
            D3D11_Viewport_Result_Skipped,
            S_OK);
    }

    swap_chain = (IDXGISwapChain*)host->swap_chain;
    if (host->occluded)
    {
        hresult = swap_chain->Present(0, DXGI_PRESENT_TEST);
        if (hresult == DXGI_STATUS_OCCLUDED)
        {
            return d3d11_viewport_report(
                host,
                D3D11_Viewport_Result_Skipped,
                hresult);
        }

        if (FAILED(hresult))
        {
            return d3d11_viewport_report_device_failure(
                host,
                D3D11_Viewport_Result_Present_Failed,
                hresult);
        }

        host->occluded = 0;
    }

    hresult = swap_chain->Present(sync_interval, 0);
    if (hresult == DXGI_STATUS_OCCLUDED)
    {
        host->occluded = 1;
        return d3d11_viewport_report(
            host,
            D3D11_Viewport_Result_Skipped,
            hresult);
    }

    if (FAILED(hresult))
    {
        return d3d11_viewport_report_device_failure(
            host,
            D3D11_Viewport_Result_Present_Failed,
            hresult);
    }

    return d3d11_viewport_report(
        host,
        D3D11_Viewport_Result_Ok,
        hresult);
}

uint32_t
d3d11_viewport_host_get_render_target(
    const D3D11_Viewport_Host* host,
    Host_Render_Target* out_target)
{
    if (out_target == 0)
    {
        return 0;
    }

    memset(out_target, 0, sizeof(*out_target));

    if (host == 0 ||
        !host->initialized ||
        !host->target_available ||
        host->occluded ||
        host->device_lost ||
        host->width == 0 ||
        host->height == 0)
    {
        return 0;
    }

    out_target->d3d11_render_target_view = host->render_target_view;
    out_target->d3d11_depth_stencil_view = host->depth_stencil_view;
    out_target->width = host->width;
    out_target->height = host->height;
    out_target->generation = host->target_generation;
    return 1;
}

const char*
d3d11_viewport_result_string(D3D11_Viewport_Result result)
{
    switch (result)
    {
        case D3D11_Viewport_Result_Ok: return "ok";
        case D3D11_Viewport_Result_Skipped: return "skipped";
        case D3D11_Viewport_Result_Invalid_Argument: return "invalid argument";
        case D3D11_Viewport_Result_Invalid_State: return "invalid state";
        case D3D11_Viewport_Result_Generation_Exhausted: return "generation exhausted";
        case D3D11_Viewport_Result_Device_Create_Failed: return "device creation failed";
        case D3D11_Viewport_Result_Factory_Query_Failed: return "factory query failed";
        case D3D11_Viewport_Result_Swap_Chain_Create_Failed: return "swap chain creation failed";
        case D3D11_Viewport_Result_Back_Buffer_Failed: return "back buffer query failed";
        case D3D11_Viewport_Result_Render_Target_Create_Failed: return "render target creation failed";
        case D3D11_Viewport_Result_Depth_Texture_Create_Failed: return "depth texture creation failed";
        case D3D11_Viewport_Result_Depth_View_Create_Failed: return "depth view creation failed";
        case D3D11_Viewport_Result_Resize_Failed: return "resize failed";
        case D3D11_Viewport_Result_Device_Removed: return "device removed";
        case D3D11_Viewport_Result_Present_Failed: return "present failed";
        default: return "unknown D3D11 viewport result";
    }
}
