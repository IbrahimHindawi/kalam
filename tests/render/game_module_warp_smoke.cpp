#include "d3d11_viewport_host.h"
#include "game_api.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t test_failure_count;

static const char test_scene_json[] =
    "{"
    "\"$schema\":\"nveditor.scene/v1\","
    "\"cube\":{"
    "\"color\":[0.95,0.15,0.05,1.0],"
    "\"transform\":{"
    "\"translation\":[0.0,0.0,0.0],"
    "\"rotation_degrees\":[25.0,35.0,0.0],"
    "\"scale\":[0.9,0.9,0.9]"
    "}}}";

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

static void
test_print_game_error(const char* operation, const Game_Error* error)
{
    printf("%s failed", operation);
    if (error != 0 && error->message.data != 0)
    {
        printf(": ");
        fwrite(
            error->message.data,
            1,
            (size_t)error->message.size,
            stdout);
    }
    if (error != 0 &&
        error->json_path.data != 0 &&
        error->json_path.size != 0)
    {
        printf(" at ");
        fwrite(
            error->json_path.data,
            1,
            (size_t)error->json_path.size,
            stdout);
    }
    printf("\n");
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

static String_View
test_string_view(const char* text)
{
    String_View result;
    result.data = text;
    result.size = text != 0 ? (uint64_t)strlen(text) : 0;
    return result;
}

static uint32_t
test_string_view_equal(String_View view, const char* text)
{
    uint64_t text_size = text != 0 ? (uint64_t)strlen(text) : 0;
    return view.size == text_size &&
        (text_size == 0 ||
         (view.data != 0 &&
          memcmp(view.data, text, (size_t)text_size) == 0));
}

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

static HMODULE
test_load_game_module(const char* utf8_path)
{
    wchar_t wide_path[32768];
    int character_count = 0;

    memset(wide_path, 0, sizeof(wide_path));
    if (utf8_path == 0)
    {
        test_fail("game module path argument is present", __FILE__, __LINE__);
        return 0;
    }

    character_count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8_path,
        -1,
        wide_path,
        (int)(sizeof(wide_path) / sizeof(wide_path[0])));
    if (character_count == 0)
    {
        test_fail("game module path is valid UTF-8", __FILE__, __LINE__);
        return 0;
    }
    return LoadLibraryW(wide_path);
}

static uint32_t
test_resolve_game_get_api(
    HMODULE module,
    Game_Get_API_Function* out_function)
{
    FARPROC address = 0;

    if (module == 0 || out_function == 0)
    {
        return 0;
    }

    *out_function = 0;
    address = GetProcAddress(module, "game_get_api");
    if (address == 0 || sizeof(address) != sizeof(*out_function))
    {
        return 0;
    }
    memcpy(out_function, &address, sizeof(address));
    return *out_function != 0;
}

static uint32_t
test_read_back_non_background(
    D3D11_Viewport_Host* host,
    uint32_t* out_changed_pixel_count)
{
    ID3D11Device* device = (ID3D11Device*)host->device;
    ID3D11DeviceContext* context =
        (ID3D11DeviceContext*)host->immediate_context;
    IDXGISwapChain* swap_chain = (IDXGISwapChain*)host->swap_chain;
    ID3D11Texture2D* back_buffer = 0;
    ID3D11Texture2D* staging_texture = 0;
    D3D11_TEXTURE2D_DESC texture_desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    const uint8_t* background = 0;
    HRESULT hresult = E_FAIL;
    uint32_t changed_pixel_count = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t mapped_texture = 0;
    uint32_t result = 0;

    if (out_changed_pixel_count != 0)
    {
        *out_changed_pixel_count = 0;
    }
    memset(&texture_desc, 0, sizeof(texture_desc));
    memset(&mapped, 0, sizeof(mapped));

    hresult = swap_chain->GetBuffer(
        0,
        __uuidof(ID3D11Texture2D),
        (void**)&back_buffer);
    if (FAILED(hresult))
    {
        test_fail_hresult(
            "IDXGISwapChain::GetBuffer",
            hresult,
            __FILE__,
            __LINE__);
        goto cleanup;
    }

    back_buffer->GetDesc(&texture_desc);
    if (texture_desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        texture_desc.SampleDesc.Count != 1 ||
        texture_desc.Width == 0 ||
        texture_desc.Height == 0)
    {
        test_fail(
            "expected a nonempty, single-sample RGBA8 back buffer",
            __FILE__,
            __LINE__);
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
        test_fail_hresult(
            "ID3D11Device::CreateTexture2D",
            hresult,
            __FILE__,
            __LINE__);
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
        test_fail_hresult(
            "ID3D11DeviceContext::Map",
            hresult,
            __FILE__,
            __LINE__);
        goto cleanup;
    }
    mapped_texture = 1;

    if (mapped.pData == 0 ||
        mapped.RowPitch < texture_desc.Width * 4)
    {
        test_fail(
            "mapped back buffer contains complete rows",
            __FILE__,
            __LINE__);
        goto cleanup;
    }

    background = (const uint8_t*)mapped.pData;
    for (y = 0; y < texture_desc.Height; y += 1)
    {
        const uint8_t* row =
            (const uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch;
        for (x = 0; x < texture_desc.Width; x += 1)
        {
            const uint8_t* pixel = row + (size_t)x * 4;
            int red_delta = (int)pixel[0] - (int)background[0];
            int green_delta = (int)pixel[1] - (int)background[1];
            int blue_delta = (int)pixel[2] - (int)background[2];
            if (red_delta < 0)
            {
                red_delta = -red_delta;
            }
            if (green_delta < 0)
            {
                green_delta = -green_delta;
            }
            if (blue_delta < 0)
            {
                blue_delta = -blue_delta;
            }
            if (red_delta + green_delta + blue_delta >= 24)
            {
                changed_pixel_count += 1;
            }
        }
    }

    if (changed_pixel_count == 0 ||
        changed_pixel_count >= texture_desc.Width * texture_desc.Height)
    {
        printf(
            "%s(%d): expected cube pixels over a background; changed=%u\n",
            __FILE__,
            __LINE__,
            (unsigned int)changed_pixel_count);
        test_failure_count += 1;
        goto cleanup;
    }

    if (out_changed_pixel_count != 0)
    {
        *out_changed_pixel_count = changed_pixel_count;
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
test_log(void* host_user, uint32_t level, String_View message)
{
    uint32_t* message_count = (uint32_t*)host_user;
    (void)level;

    if (message_count != 0)
    {
        *message_count += 1;
    }
    printf("[ module  ] ");
    if (message.data != 0)
    {
        fwrite(message.data, 1, (size_t)message.size, stdout);
    }
    printf("\n");
}

static void
test_hidden_warp_game_module(const char* module_path)
{
    static const wchar_t class_name[] =
        L"KalamGameModuleWarpSmokeWindow";
    HINSTANCE instance = GetModuleHandleW(0);
    WNDCLASSEXW window_class;
    ATOM class_atom = 0;
    HWND window = 0;
    HMODULE module = 0;
    Game_Get_API_Function get_api = 0;
    Game_API api;
    Game_API rejected_api;
    Game_Error error;
    Game_Create_Desc game_create_desc;
    Game_Input input;
    Author_Object_Source objects[1];
    Author_Document_State document;
    Author_Value field_value;
    Author_Value preview_value;
    Game_Viewport_Interaction viewport_interaction;
    Game_Viewport_Interaction_Result viewport_interaction_result;
    Author_Schema_View schema;
    Author_Field_Id color_field_id = 0;
    D3D11_Viewport_Host host;
    D3D11_Viewport_Create_Desc viewport_create_desc;
    Host_Render_Target target;
    void* game = 0;
    Game_Result game_result = Game_Result_Ok;
    D3D11_Viewport_Result viewport_result = D3D11_Viewport_Result_Ok;
    uint64_t first_generation = 0;
    uint32_t schema_index = 0;
    uint32_t changed_pixel_count = 0;
    uint32_t log_message_count = 0;
    uint32_t host_created = 0;

    memset(&window_class, 0, sizeof(window_class));
    memset(&api, 0, sizeof(api));
    memset(&rejected_api, 0xcd, sizeof(rejected_api));
    memset(&error, 0, sizeof(error));
    memset(&game_create_desc, 0, sizeof(game_create_desc));
    memset(&input, 0, sizeof(input));
    memset(objects, 0, sizeof(objects));
    memset(&document, 0, sizeof(document));
    memset(&field_value, 0, sizeof(field_value));
    memset(&preview_value, 0, sizeof(preview_value));
    memset(&viewport_interaction, 0, sizeof(viewport_interaction));
    memset(
        &viewport_interaction_result,
        0,
        sizeof(viewport_interaction_result));
    memset(&schema, 0, sizeof(schema));
    memset(&host, 0, sizeof(host));
    memset(&viewport_create_desc, 0, sizeof(viewport_create_desc));
    memset(&target, 0, sizeof(target));

    module = test_load_game_module(module_path);
    if (module == 0)
    {
        printf(
            "LoadLibraryW failed for %s (Win32 error %lu)\n",
            module_path != 0 ? module_path : "(null)",
            (unsigned long)GetLastError());
        test_failure_count += 1;
        goto cleanup;
    }
    TEST_REQUIRE_CLEANUP(test_resolve_game_get_api(module, &get_api));

    game_result = get_api(
        Host_API_Version + 1,
        (uint32_t)sizeof(rejected_api),
        &rejected_api,
        &error);
    TEST_CHECK(game_result == Game_Result_Invalid_Argument);
    {
        const uint8_t* bytes = (const uint8_t*)&rejected_api;
        uint32_t byte_index = 0;
        for (byte_index = 0;
             byte_index < (uint32_t)sizeof(rejected_api);
             byte_index += 1)
        {
            TEST_CHECK(bytes[byte_index] == 0);
        }
    }

    game_result = get_api(
        Host_API_Version,
        (uint32_t)sizeof(api),
        &api,
        &error);
    if (game_result != Game_Result_Ok)
    {
        test_print_game_error("game_get_api", &error);
        test_failure_count += 1;
        goto cleanup;
    }
    TEST_CHECK(api.api_version == Game_API_Version);
    TEST_CHECK(api.struct_size == (uint32_t)sizeof(api));
    TEST_REQUIRE_CLEANUP(api.create != 0);
    TEST_REQUIRE_CLEANUP(api.destroy != 0);
    TEST_REQUIRE_CLEANUP(api.fixed_update != 0);
    TEST_REQUIRE_CLEANUP(api.render != 0);
    TEST_REQUIRE_CLEANUP(api.apply_document != 0);
    TEST_REQUIRE_CLEANUP(api.remove_document != 0);
    TEST_REQUIRE_CLEANUP(api.get_author_schema_count != 0);
    TEST_REQUIRE_CLEANUP(api.get_author_schema != 0);
    TEST_REQUIRE_CLEANUP(api.read_author_field != 0);
    TEST_REQUIRE_CLEANUP(api.preview_author_field != 0);
    TEST_REQUIRE_CLEANUP(api.viewport_interaction != 0);

    TEST_CHECK(api.get_author_schema_count() >= 2);
    for (schema_index = 0;
         schema_index < api.get_author_schema_count();
         schema_index += 1)
    {
        uint32_t field_index = 0;
        memset(&schema, 0, sizeof(schema));
        game_result = api.get_author_schema(
            schema_index,
            &schema,
            &error);
        if (game_result != Game_Result_Ok)
        {
            test_print_game_error("get_author_schema", &error);
            test_failure_count += 1;
            goto cleanup;
        }
        TEST_CHECK(schema.schema_id.data != 0);
        TEST_CHECK(schema.schema_id.size != 0);
        TEST_CHECK(schema.fields != 0);
        TEST_CHECK(schema.field_count != 0);

        if (test_string_view_equal(
                schema.schema_id,
                "nveditor.scene/v1"))
        {
            for (field_index = 0;
                 field_index < schema.field_count;
                 field_index += 1)
            {
                if (test_string_view_equal(
                        schema.fields[field_index].json_path,
                        "$.cube.color"))
                {
                    color_field_id = schema.fields[field_index].id;
                }
            }
        }
    }
    TEST_REQUIRE_CLEANUP(color_field_id != 0);

    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = test_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    class_atom = RegisterClassExW(&window_class);
    TEST_REQUIRE_CLEANUP(class_atom != 0);

    window = CreateWindowExW(
        0,
        class_name,
        L"Kalam hidden WARP game module smoke",
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

    viewport_create_desc.struct_size =
        (uint32_t)sizeof(viewport_create_desc);
    viewport_create_desc.window_handle = window;
    viewport_create_desc.width = 320;
    viewport_create_desc.height = 180;
    viewport_create_desc.driver = D3D11_Viewport_Driver_Warp;
    viewport_result =
        d3d11_viewport_host_create(&host, &viewport_create_desc);
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

    game_create_desc.struct_size =
        (uint32_t)sizeof(game_create_desc);
    game_create_desc.host_api_version = Host_API_Version;
    game_create_desc.d3d11_device = host.device;
    game_create_desc.d3d11_immediate_context =
        host.immediate_context;
    game_create_desc.host_user = &log_message_count;
    game_create_desc.host_log = test_log;
    game_create_desc.project_root.data = KALAM_TEST_PROJECT_ROOT;
    game_create_desc.project_root.size =
        (uint64_t)(sizeof(KALAM_TEST_PROJECT_ROOT) - 1);
    game_result = api.create(&game_create_desc, &game, &error);
    if (game_result != Game_Result_Ok)
    {
        test_print_game_error("Game_API.create", &error);
        test_failure_count += 1;
        goto cleanup;
    }
    TEST_REQUIRE_CLEANUP(game != 0);
    TEST_CHECK(log_message_count == 1);

    objects[0].object_id = UINT64_C(0x1020304050607080);
    objects[0].json_path = test_string_view("$");
    document.struct_size = (uint32_t)sizeof(document);
    document.document_id = UINT64_C(0x1122334455667788);
    document.root_object_id = objects[0].object_id;
    document.revision = 1;
    document.schema_id = test_string_view("nveditor.scene/v1");
    document.project_relative_path =
        test_string_view("examples/cube.scene.json");
    document.json_utf8 = test_string_view(test_scene_json);
    document.objects = objects;
    document.object_count = 1;

    game_result = api.apply_document(game, &document, &error);
    if (game_result != Game_Result_Ok)
    {
        test_print_game_error("Game_API.apply_document", &error);
        test_failure_count += 1;
        goto cleanup;
    }

    game_result = api.read_author_field(
        game,
        document.document_id,
        document.root_object_id,
        color_field_id,
        &field_value,
        &error);
    TEST_CHECK(game_result == Game_Result_Ok);
    TEST_CHECK(field_value.kind == Author_Field_Color4);
    TEST_CHECK(field_value.data.f32[0] == 0.95f);
    TEST_CHECK(field_value.data.f32[1] == 0.15f);

    viewport_interaction.struct_size =
        (uint32_t)sizeof(viewport_interaction);
    viewport_interaction.kind = Game_Viewport_Interaction_Select;
    viewport_interaction.pointer_x_pixels = 160;
    viewport_interaction.pointer_y_pixels = 90;
    viewport_interaction.viewport_width = 320;
    viewport_interaction.viewport_height = 180;
    game_result = api.viewport_interaction(
        game,
        &viewport_interaction,
        &viewport_interaction_result,
        &error);
    TEST_CHECK(game_result == Game_Result_Ok);
    TEST_CHECK(
        (viewport_interaction_result.flags &
         Game_Viewport_Result_Has_Selection) != 0);
    TEST_CHECK(
        viewport_interaction_result.object_id == document.root_object_id);
    TEST_CHECK(viewport_interaction_result.field_id != 0);

    viewport_interaction.kind = Game_Viewport_Interaction_Gizmo_Begin;
    viewport_interaction.pointer_x_pixels = 205;
    game_result = api.viewport_interaction(
        game,
        &viewport_interaction,
        &viewport_interaction_result,
        &error);
    TEST_CHECK(game_result == Game_Result_Ok);
    TEST_CHECK(
        (viewport_interaction_result.flags &
         Game_Viewport_Result_Gizmo_Active) != 0);

    viewport_interaction.kind = Game_Viewport_Interaction_Gizmo_Update;
    viewport_interaction.pointer_delta_x_pixels = 10;
    viewport_interaction.pointer_delta_y_pixels = 0;
    game_result = api.viewport_interaction(
        game,
        &viewport_interaction,
        &viewport_interaction_result,
        &error);
    TEST_CHECK(game_result == Game_Result_Ok);
    TEST_CHECK(
        viewport_interaction_result.value.kind == Author_Field_Transform);
    TEST_CHECK(
        fabsf(
            viewport_interaction_result.value.data.transform.translation[0] -
            0.1f) < 0.00001f);

    viewport_interaction.kind = Game_Viewport_Interaction_Gizmo_Cancel;
    game_result = api.viewport_interaction(
        game,
        &viewport_interaction,
        &viewport_interaction_result,
        &error);
    TEST_CHECK(game_result == Game_Result_Ok);
    TEST_CHECK(
        (viewport_interaction_result.flags &
         Game_Viewport_Result_Cancelled) != 0);
    TEST_CHECK(
        viewport_interaction_result.value.data.transform.translation[0] ==
        0.0f);

    viewport_interaction.kind = Game_Viewport_Interaction_Gizmo_Begin;
    viewport_interaction.pointer_delta_x_pixels = 0;
    game_result = api.viewport_interaction(
        game,
        &viewport_interaction,
        &viewport_interaction_result,
        &error);
    TEST_CHECK(game_result == Game_Result_Ok);
    viewport_interaction.kind = Game_Viewport_Interaction_Gizmo_Update;
    viewport_interaction.pointer_delta_x_pixels = 10;
    game_result = api.viewport_interaction(
        game,
        &viewport_interaction,
        &viewport_interaction_result,
        &error);
    TEST_CHECK(game_result == Game_Result_Ok);
    viewport_interaction.kind = Game_Viewport_Interaction_Gizmo_Commit;
    viewport_interaction.pointer_delta_x_pixels = 0;
    game_result = api.viewport_interaction(
        game,
        &viewport_interaction,
        &viewport_interaction_result,
        &error);
    TEST_CHECK(game_result == Game_Result_Ok);
    TEST_CHECK(
        (viewport_interaction_result.flags &
         Game_Viewport_Result_Commit_Requested) != 0);
    TEST_CHECK(
        fabsf(
            viewport_interaction_result.value.data.transform.translation[0] -
            0.1f) < 0.00001f);

    preview_value.kind = Author_Field_Color4;
    preview_value.data.f32[0] = 0.10f;
    preview_value.data.f32[1] = 0.85f;
    preview_value.data.f32[2] = 0.25f;
    preview_value.data.f32[3] = 1.00f;
    game_result = api.preview_author_field(
        game,
        document.document_id,
        document.root_object_id,
        color_field_id,
        &preview_value,
        &error);
    TEST_CHECK(game_result == Game_Result_Ok);

    input.struct_size = (uint32_t)sizeof(input);
    input.fixed_tick_index = 1;
    api.fixed_update(game, &input, 1.0f / 60.0f);

    TEST_REQUIRE_CLEANUP(
        d3d11_viewport_host_get_render_target(&host, &target));
    first_generation = target.generation;
    game_result = api.render(game, &target, 0.5f, &error);
    if (game_result != Game_Result_Ok)
    {
        test_print_game_error("Game_API.render", &error);
        test_failure_count += 1;
        goto cleanup;
    }
    TEST_REQUIRE_CLEANUP(
        test_read_back_non_background(
            &host,
            &changed_pixel_count));
    TEST_CHECK(changed_pixel_count > 100);

    TEST_REQUIRE_CLEANUP(
        d3d11_viewport_host_resize(&host, 400, 240) ==
        D3D11_Viewport_Result_Ok);
    TEST_REQUIRE_CLEANUP(
        d3d11_viewport_host_get_render_target(&host, &target));
    TEST_CHECK(target.generation > first_generation);
    game_result = api.render(game, &target, 0.0f, &error);
    if (game_result != Game_Result_Ok)
    {
        test_print_game_error("Game_API.render after resize", &error);
        test_failure_count += 1;
        goto cleanup;
    }
    TEST_REQUIRE_CLEANUP(
        test_read_back_non_background(
            &host,
            &changed_pixel_count));
    TEST_CHECK(changed_pixel_count > 100);

    game_result = api.remove_document(
        game,
        document.document_id,
        &error);
    TEST_CHECK(game_result == Game_Result_Ok);

cleanup:
    if (game != 0 && api.destroy != 0)
    {
        api.destroy(game);
        game = 0;
    }

    if (host_created)
    {
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
    if (module != 0)
    {
        TEST_CHECK(FreeLibrary(module));
    }
}

int
main(int argument_count, char** arguments)
{
    if (argument_count != 2)
    {
        printf(
            "usage: game_module_warp_smoke.exe "
            "<absolute-game-module-dll-path>\n");
        return 2;
    }

    printf("[ RUN      ] hidden_warp_game_module\n");
    test_hidden_warp_game_module(arguments[1]);

    if (test_failure_count != 0)
    {
        printf("[  FAILED  ] hidden_warp_game_module\n");
        printf(
            "%u game module WARP check(s) failed\n",
            (unsigned int)test_failure_count);
        return 1;
    }

    printf("[       OK ] hidden_warp_game_module\n");
    printf("Game module D3D11 WARP smoke test passed\n");
    return 0;
}
