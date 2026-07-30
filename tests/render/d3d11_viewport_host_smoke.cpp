#include "d3d11_viewport_host.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <stdio.h>
#include <string.h>

static uint32_t test_failure_count;

static void
test_fail(const char* expression, const char* file, int line)
{
    printf("%s(%d): check failed: %s\n", file, line, expression);
    test_failure_count += 1;
}

static void
test_fail_hresult(
    const char* operation,
    HRESULT hresult,
    const char* file,
    int line)
{
    printf(
        "%s(%d): %s failed with HRESULT 0x%08x\n",
        file,
        line,
        operation,
        (unsigned int)(uint32_t)hresult);
    test_failure_count += 1;
}

#define TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            test_fail(#expression, __FILE__, __LINE__); \
        } \
    } while (0)

#define TEST_REQUIRE_CLEANUP(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            test_fail(#expression, __FILE__, __LINE__); \
            goto cleanup; \
        } \
    } while (0)

static void
test_release_unknown(void** object)
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

static uint32_t
test_color_channel_near(uint8_t actual, uint8_t expected)
{
    int delta = (int)actual - (int)expected;
    if (delta < 0)
    {
        delta = -delta;
    }
    return delta <= 2;
}

static uint32_t
test_read_back_clear(
    D3D11_Viewport_Host* host,
    uint8_t expected_red,
    uint8_t expected_green,
    uint8_t expected_blue,
    uint8_t expected_alpha)
{
    ID3D11Device* device = (ID3D11Device*)host->device;
    ID3D11DeviceContext* context =
        (ID3D11DeviceContext*)host->immediate_context;
    IDXGISwapChain* swap_chain = (IDXGISwapChain*)host->swap_chain;
    ID3D11Texture2D* back_buffer = 0;
    ID3D11Texture2D* staging_texture = 0;
    D3D11_TEXTURE2D_DESC texture_desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    const uint8_t* pixel = 0;
    HRESULT hresult = E_FAIL;
    uint32_t result = 0;
    uint32_t mapped_texture = 0;

    memset(&texture_desc, 0, sizeof(texture_desc));
    memset(&mapped, 0, sizeof(mapped));

    hresult = swap_chain->GetBuffer(
        0,
        __uuidof(ID3D11Texture2D),
        (void**)&back_buffer);
    if (FAILED(hresult))
    {
        test_fail_hresult("IDXGISwapChain::GetBuffer", hresult, __FILE__, __LINE__);
        goto cleanup;
    }

    back_buffer->GetDesc(&texture_desc);
    if (texture_desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        texture_desc.SampleDesc.Count != 1)
    {
        test_fail("expected single-sample RGBA8 back buffer", __FILE__, __LINE__);
        goto cleanup;
    }

    texture_desc.Usage = D3D11_USAGE_STAGING;
    texture_desc.BindFlags = 0;
    texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    texture_desc.MiscFlags = 0;

    hresult = device->CreateTexture2D(
        &texture_desc,
        0,
        &staging_texture);
    if (FAILED(hresult))
    {
        test_fail_hresult("ID3D11Device::CreateTexture2D", hresult, __FILE__, __LINE__);
        goto cleanup;
    }

    context->CopyResource(staging_texture, back_buffer);
    hresult = context->Map(
        staging_texture,
        0,
        D3D11_MAP_READ,
        0,
        &mapped);
    if (FAILED(hresult))
    {
        test_fail_hresult("ID3D11DeviceContext::Map", hresult, __FILE__, __LINE__);
        goto cleanup;
    }
    mapped_texture = 1;

    if (mapped.pData == 0 || mapped.RowPitch < 4)
    {
        test_fail("mapped back buffer contains a pixel", __FILE__, __LINE__);
        goto cleanup;
    }

    pixel = (const uint8_t*)mapped.pData;
    if (!test_color_channel_near(pixel[0], expected_red) ||
        !test_color_channel_near(pixel[1], expected_green) ||
        !test_color_channel_near(pixel[2], expected_blue) ||
        !test_color_channel_near(pixel[3], expected_alpha))
    {
        printf(
            "%s(%d): unexpected clear pixel RGBA=(%u,%u,%u,%u)\n",
            __FILE__,
            __LINE__,
            (unsigned int)pixel[0],
            (unsigned int)pixel[1],
            (unsigned int)pixel[2],
            (unsigned int)pixel[3]);
        test_failure_count += 1;
        goto cleanup;
    }

    result = 1;

cleanup:
    if (mapped_texture)
    {
        context->Unmap(staging_texture, 0);
    }
    test_release_unknown((void**)&staging_texture);
    test_release_unknown((void**)&back_buffer);
    return result;
}

static LRESULT CALLBACK
test_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

static void
test_hidden_window_warp_viewport(void)
{
    static const wchar_t class_name[] =
        L"KalamD3D11ViewportHostSmokeWindow";

    HINSTANCE instance = GetModuleHandleW(0);
    WNDCLASSEXW window_class;
    ATOM class_atom = 0;
    HWND window = 0;
    D3D11_Viewport_Host host = {};
    D3D11_Viewport_Create_Desc create_desc = {};
    Host_Render_Target target = {};
    D3D11_Viewport_Result viewport_result = D3D11_Viewport_Result_Ok;
    float first_clear[4] = {0.125f, 0.25f, 0.5f, 1.0f};
    float second_clear[4] = {0.75f, 0.125f, 0.25f, 1.0f};
    uint64_t initial_generation = 0;
    uint64_t resized_generation = 0;
    uint64_t zero_size_generation = 0;
    uint32_t host_created = 0;

    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = test_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;

    class_atom = RegisterClassExW(&window_class);
    TEST_REQUIRE_CLEANUP(class_atom != 0);

    window = CreateWindowExW(
        0,
        class_name,
        L"Kalam hidden WARP smoke",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        360,
        240,
        0,
        0,
        instance,
        0);
    TEST_REQUIRE_CLEANUP(window != 0);
    TEST_CHECK(!IsWindowVisible(window));

    create_desc.struct_size = sizeof(create_desc);
    create_desc.window_handle = window;
    create_desc.width = 320;
    create_desc.height = 180;
    create_desc.driver = D3D11_Viewport_Driver_Warp;

    viewport_result = d3d11_viewport_host_create(&host, &create_desc);
    if (viewport_result != D3D11_Viewport_Result_Ok)
    {
        printf(
            "D3D11 viewport create failed: %s (HRESULT 0x%08x)\n",
            d3d11_viewport_result_string(viewport_result),
            (unsigned int)(uint32_t)host.last_hresult);
        test_failure_count += 1;
        goto cleanup;
    }
    host_created = 1;

    TEST_CHECK(host.initialized);
    TEST_CHECK(host.driver == D3D11_Viewport_Driver_Warp);
    TEST_CHECK(host.device != 0);
    TEST_CHECK(host.immediate_context != 0);
    TEST_CHECK(host.dxgi_factory != 0);
    TEST_CHECK(host.swap_chain != 0);
    TEST_CHECK(host.render_target_view != 0);
    TEST_CHECK(host.depth_texture != 0);
    TEST_CHECK(host.depth_stencil_view != 0);
    TEST_CHECK(host.device_create_attempt_count >= 1);
    TEST_CHECK(host.feature_level >= (uint32_t)D3D_FEATURE_LEVEL_10_0);

    TEST_REQUIRE_CLEANUP(
        d3d11_viewport_host_get_render_target(&host, &target));
    host.occluded = 1;
    TEST_CHECK(!d3d11_viewport_host_get_render_target(&host, &target));
    TEST_CHECK(target.d3d11_render_target_view == 0);
    host.occluded = 0;
    TEST_REQUIRE_CLEANUP(
        d3d11_viewport_host_get_render_target(&host, &target));
    TEST_CHECK(target.d3d11_render_target_view == host.render_target_view);
    TEST_CHECK(target.d3d11_depth_stencil_view == host.depth_stencil_view);
    TEST_CHECK(target.width == 320);
    TEST_CHECK(target.height == 180);
    TEST_CHECK(target.generation != 0);
    initial_generation = target.generation;

    TEST_REQUIRE_CLEANUP(
        d3d11_viewport_host_clear(
            &host,
            first_clear,
            1.0f,
            0) ==
        D3D11_Viewport_Result_Ok);
    TEST_REQUIRE_CLEANUP(test_read_back_clear(&host, 32, 64, 128, 255));

    viewport_result = d3d11_viewport_host_present(&host, 0);
    TEST_CHECK(
        viewport_result == D3D11_Viewport_Result_Ok ||
        viewport_result == D3D11_Viewport_Result_Skipped);

    TEST_REQUIRE_CLEANUP(
        d3d11_viewport_host_resize(&host, 640, 360) ==
        D3D11_Viewport_Result_Ok);
    TEST_REQUIRE_CLEANUP(
        d3d11_viewport_host_get_render_target(&host, &target));
    TEST_CHECK(target.width == 640);
    TEST_CHECK(target.height == 360);
    TEST_CHECK(target.generation > initial_generation);
    resized_generation = target.generation;

    TEST_REQUIRE_CLEANUP(
        d3d11_viewport_host_clear(
            &host,
            second_clear,
            0.5f,
            7) ==
        D3D11_Viewport_Result_Ok);
    TEST_REQUIRE_CLEANUP(test_read_back_clear(&host, 191, 32, 64, 255));

    // A redundant resize does not invalidate target-sized module resources.
    TEST_CHECK(
        d3d11_viewport_host_resize(&host, 640, 360) ==
        D3D11_Viewport_Result_Ok);
    TEST_CHECK(host.target_generation == resized_generation);

    TEST_CHECK(
        d3d11_viewport_host_resize(&host, 0, 0) ==
        D3D11_Viewport_Result_Skipped);
    TEST_CHECK(host.target_generation > resized_generation);
    zero_size_generation = host.target_generation;
    TEST_CHECK(!d3d11_viewport_host_get_render_target(&host, &target));
    TEST_CHECK(target.d3d11_render_target_view == 0);
    TEST_CHECK(target.d3d11_depth_stencil_view == 0);
    TEST_CHECK(target.width == 0);
    TEST_CHECK(target.height == 0);
    TEST_CHECK(
        d3d11_viewport_host_clear(&host, first_clear, 1.0f, 0) ==
        D3D11_Viewport_Result_Skipped);
    TEST_CHECK(
        d3d11_viewport_host_present(&host, 0) ==
        D3D11_Viewport_Result_Skipped);

    TEST_REQUIRE_CLEANUP(
        d3d11_viewport_host_resize(&host, 128, 96) ==
        D3D11_Viewport_Result_Ok);
    TEST_REQUIRE_CLEANUP(
        d3d11_viewport_host_get_render_target(&host, &target));
    TEST_CHECK(target.width == 128);
    TEST_CHECK(target.height == 96);
    TEST_CHECK(target.generation > zero_size_generation);
    TEST_CHECK(
        d3d11_viewport_host_clear(&host, first_clear, 1.0f, 0) ==
        D3D11_Viewport_Result_Ok);

    d3d11_viewport_host_clear_bindings(&host, 1);
    TEST_CHECK(d3d11_viewport_host_get_render_target(&host, &target));

cleanup:
    if (host_created)
    {
        d3d11_viewport_host_destroy(&host);
        TEST_CHECK(!host.initialized);
        TEST_CHECK(host.device == 0);
        TEST_CHECK(host.immediate_context == 0);
        TEST_CHECK(host.dxgi_factory == 0);
        TEST_CHECK(host.swap_chain == 0);
        TEST_CHECK(host.render_target_view == 0);
        TEST_CHECK(host.depth_texture == 0);
        TEST_CHECK(host.depth_stencil_view == 0);

        // Teardown is explicitly idempotent.
        d3d11_viewport_host_destroy(&host);
    }

    if (window != 0)
    {
        TEST_CHECK(DestroyWindow(window));
    }

    if (class_atom != 0)
    {
        TEST_CHECK(UnregisterClassW(class_name, instance));
    }
}

int
main(void)
{
    printf("[ RUN      ] hidden_window_warp_viewport\n");
    test_hidden_window_warp_viewport();

    if (test_failure_count != 0)
    {
        printf("[  FAILED  ] hidden_window_warp_viewport\n");
        printf("%u D3D11 viewport check(s) failed\n", test_failure_count);
        return 1;
    }

    printf("[       OK ] hidden_window_warp_viewport\n");
    printf("D3D11 WARP viewport smoke test passed\n");
    return 0;
}
