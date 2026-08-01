#include "d3d11_viewport_host.h"
#include "module_loader.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

enum
{
    Reload_Render_Schema_Capacity = 4,
    Reload_Render_Field_Capacity = 16,
    Reload_Render_String_Capacity = 4096,
    Reload_Render_Path_Capacity = 32768,
    Reload_Render_Scene_Capacity = 16384
};

typedef struct Reload_Render_Backing
{
    Author_Schema_View schemas[Reload_Render_Schema_Capacity];
    Author_Field_Desc fields[Reload_Render_Field_Capacity];
    char strings[Reload_Render_String_Capacity];
    wchar_t path[Reload_Render_Path_Capacity];
} Reload_Render_Backing;

typedef struct Reload_Render_Frame
{
    uint64_t hash;
    uint32_t changed_pixel_count;
    uint32_t width;
    uint32_t height;
} Reload_Render_Frame;

typedef struct Reload_Render_Context
{
    uint32_t failure_count;
    uint32_t clear_context_count;
    uint32_t hot_path_counter;
    uint32_t log_message_count;
    uint64_t next_generation;
    const wchar_t* hot_directory;
    const Author_Document_State* document;
    D3D11_Viewport_Host* viewport;
    Module_Loader_Slot active;
    Module_Loader_Slot candidate;
    Reload_Render_Backing active_backing;
    Reload_Render_Backing candidate_backing;
} Reload_Render_Context;

static String_View
reload_render_view(const char* text)
{
    String_View result;
    result.data = text;
    result.size = text != 0 ? (uint64_t)strlen(text) : 0;
    return result;
}

static uint32_t
reload_render_view_equals(String_View value, const char* expected)
{
    const String_View expected_view = reload_render_view(expected);
    return value.data != 0 && value.size == expected_view.size &&
        memcmp(value.data, expected_view.data, (size_t)value.size) == 0;
}

static void
reload_render_check(
    Reload_Render_Context* context,
    uint32_t condition,
    const char* message)
{
    if (condition == 0)
    {
        context->failure_count += 1;
        fputs("FAIL: ", stderr);
        fputs(message, stderr);
        fputc(10, stderr);
    }
}

static void
reload_render_log(void* user, uint32_t level, String_View message)
{
    Reload_Render_Context* context = (Reload_Render_Context*)user;
    (void)level;
    (void)message;
    if (context != 0)
    {
        context->log_message_count += 1;
    }
}

static void
reload_render_clear_context(void* user)
{
    Reload_Render_Context* context = (Reload_Render_Context*)user;
    if (context != 0)
    {
        context->clear_context_count += 1;
        d3d11_viewport_host_clear_bindings(context->viewport, 1);
    }
}

static Module_Loader_Storage
reload_render_storage(Reload_Render_Backing* backing)
{
    Module_Loader_Storage result;
    memset(&result, 0, sizeof(result));
    result.schemas = backing->schemas;
    result.schema_capacity = Reload_Render_Schema_Capacity;
    result.fields = backing->fields;
    result.field_capacity = Reload_Render_Field_Capacity;
    result.strings = backing->strings;
    result.string_capacity = Reload_Render_String_Capacity;
    result.loaded_path = backing->path;
    result.loaded_path_capacity = Reload_Render_Path_Capacity;
    return result;
}

static uint32_t
reload_render_read_scene(
    Reload_Render_Context* context,
    const wchar_t* path,
    char* bytes,
    uint32_t capacity,
    uint32_t* out_size)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER file_size;
    DWORD bytes_read = 0;
    uint32_t result = 0;

    *out_size = 0;
    memset(&file_size, 0, sizeof(file_size));
    file = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        0,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        0);
    if (file == INVALID_HANDLE_VALUE)
    {
        reload_render_check(
            context,
            0,
            "canonical examples/cube.scene.json must open");
        return 0;
    }
    if (!GetFileSizeEx(file, &file_size) ||
        file_size.QuadPart <= 0 || file_size.QuadPart >= capacity)
    {
        reload_render_check(
            context,
            0,
            "canonical scene must fit the fixed test buffer");
        goto cleanup;
    }
    if (!ReadFile(file, bytes, (DWORD)file_size.QuadPart,
            &bytes_read, 0) ||
        bytes_read != (DWORD)file_size.QuadPart)
    {
        reload_render_check(context, 0, "canonical scene read must complete");
        goto cleanup;
    }
    bytes[bytes_read] = 0;
    *out_size = bytes_read;
    result = 1;

cleanup:
    CloseHandle(file);
    return result;
}

static void
reload_render_make_hot_path(
    Reload_Render_Context* context,
    wchar_t* destination,
    uint32_t destination_capacity)
{
    const int written = _snwprintf_s(
        destination,
        destination_capacity,
        _TRUNCATE,
        L"%ls\\real_cube_hot_%lu_%llu_%u.dll",
        context->hot_directory,
        (unsigned long)GetCurrentProcessId(),
        (unsigned long long)GetTickCount64(),
        context->hot_path_counter);
    context->hot_path_counter += 1;
    reload_render_check(
        context,
        written > 0,
        "unique hot DLL path must fit the Win32 path buffer");
}

static Module_Loader_Prepare_Request
reload_render_request(
    Reload_Render_Context* context,
    const wchar_t* canonical_path,
    const wchar_t* hot_path)
{
    Module_Loader_Prepare_Request request;
    memset(&request, 0, sizeof(request));
    request.struct_size = sizeof(request);
    request.build_generation = context->next_generation++;
    request.canonical_dll_path = canonical_path;
    request.unique_hot_dll_path = hot_path;
    request.create_desc.struct_size = sizeof(Game_Create_Desc);
    request.create_desc.host_api_version = Host_API_Version;
    request.create_desc.d3d11_device = context->viewport->device;
    request.create_desc.d3d11_immediate_context =
        context->viewport->immediate_context;
    request.create_desc.host_user = context;
    request.create_desc.host_log = reload_render_log;
    request.create_desc.project_root.data = KALAM_TEST_PROJECT_ROOT;
    request.create_desc.project_root.size =
        (uint64_t)(sizeof(KALAM_TEST_PROJECT_ROOT) - 1);
    request.documents = context->document;
    request.document_count = 1;
    request.clear_context_bindings = reload_render_clear_context;
    request.clear_context_user = context;
    return request;
}

static void
reload_render_release_unknown(void** object)
{
    IUnknown* unknown = 0;
    if (object != 0 && *object != 0)
    {
        unknown = (IUnknown*)*object;
        unknown->Release();
        *object = 0;
    }
}

static uint32_t
reload_render_hash_pixels(
    Reload_Render_Context* context,
    const D3D11_MAPPED_SUBRESOURCE* mapped,
    const D3D11_TEXTURE2D_DESC* desc,
    Reload_Render_Frame* out_frame)
{
    const uint8_t* background = (const uint8_t*)mapped->pData;
    uint64_t hash = UINT64_C(14695981039346656037);
    uint32_t changed = 0;
    uint32_t y = 0;
    for (y = 0; y < desc->Height; y += 1)
    {
        const uint8_t* row = (const uint8_t*)mapped->pData +
            (size_t)y * mapped->RowPitch;
        uint32_t x = 0;
        for (x = 0; x < desc->Width; x += 1)
        {
            const uint8_t* pixel = row + (size_t)x * 4;
            int delta = (int)pixel[0] - (int)background[0];
            int component = (int)pixel[1] - (int)background[1];
            uint32_t byte_index = 0;
            delta = delta < 0 ? -delta : delta;
            component = component < 0 ? -component : component;
            delta += component;
            component = (int)pixel[2] - (int)background[2];
            delta += component < 0 ? -component : component;
            changed += delta >= 24 ? 1u : 0u;
            for (byte_index = 0; byte_index < 4; byte_index += 1)
            {
                hash ^= pixel[byte_index];
                hash *= UINT64_C(1099511628211);
            }
        }
    }
    reload_render_check(context, changed > 100 &&
        changed < desc->Width * desc->Height,
        "readback must contain cube pixels over a background");
    out_frame->hash = hash;
    out_frame->changed_pixel_count = changed;
    out_frame->width = desc->Width;
    out_frame->height = desc->Height;
    return changed > 100 && changed < desc->Width * desc->Height;
}

static uint32_t
reload_render_capture(
    Reload_Render_Context* context,
    Reload_Render_Frame* out_frame)
{
    ID3D11Device* device = (ID3D11Device*)context->viewport->device;
    ID3D11DeviceContext* immediate_context =
        (ID3D11DeviceContext*)context->viewport->immediate_context;
    IDXGISwapChain* swap_chain =
        (IDXGISwapChain*)context->viewport->swap_chain;
    ID3D11Texture2D* back_buffer = 0;
    ID3D11Texture2D* staging = 0;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hresult = E_FAIL;
    uint32_t is_mapped = 0;
    uint32_t result = 0;

    memset(&desc, 0, sizeof(desc));
    memset(&mapped, 0, sizeof(mapped));
    memset(out_frame, 0, sizeof(*out_frame));
    hresult = swap_chain->GetBuffer(
        0, __uuidof(ID3D11Texture2D), (void**)&back_buffer);
    if (FAILED(hresult))
    {
        reload_render_check(context, 0, "swap-chain readback must open");
        goto cleanup;
    }
    back_buffer->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        desc.SampleDesc.Count != 1 || desc.Width == 0 || desc.Height == 0)
    {
        reload_render_check(context, 0, "readback must be nonempty RGBA8");
        goto cleanup;
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    hresult = device->CreateTexture2D(&desc, 0, &staging);
    if (FAILED(hresult))
    {
        reload_render_check(context, 0, "staging texture creation must succeed");
        goto cleanup;
    }
    immediate_context->CopyResource(staging, back_buffer);
    hresult = immediate_context->Map(
        staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hresult))
    {
        reload_render_check(context, 0, "staging texture map must succeed");
        goto cleanup;
    }
    is_mapped = 1;
    if (mapped.pData == 0 || mapped.RowPitch < desc.Width * 4)
    {
        reload_render_check(context, 0, "mapped rows must contain all pixels");
        goto cleanup;
    }
    result = reload_render_hash_pixels(context, &mapped, &desc, out_frame);

cleanup:
    if (is_mapped)
    {
        immediate_context->Unmap(staging, 0);
    }
    reload_render_release_unknown((void**)&staging);
    reload_render_release_unknown((void**)&back_buffer);
    return result;
}

static uint32_t
reload_render_active_frame(
    Reload_Render_Context* context,
    Reload_Render_Frame* out_frame)
{
    Host_Render_Target target;
    Game_Error error;
    Game_Result result = Game_Result_Ok;
    memset(&target, 0, sizeof(target));
    memset(&error, 0, sizeof(error));
    if (!module_loader_slot_is_callable(&context->active))
    {
        reload_render_check(context, 0, "active real module must be callable");
        return 0;
    }
    if (!d3d11_viewport_host_get_render_target(context->viewport, &target))
    {
        reload_render_check(context, 0, "WARP render target must be available");
        return 0;
    }
    result = context->active.api.render(
        context->active.game, &target, 0.0f, &error);
    if (result != Game_Result_Ok)
    {
        reload_render_check(context, 0, "active real cube render must succeed");
        return 0;
    }
    return reload_render_capture(context, out_frame);
}

static Author_Field_Id
reload_render_find_color_field(const Module_Loader_Slot* slot)
{
    uint32_t schema_index = 0;
    for (schema_index = 0; schema_index < slot->schema_count; schema_index += 1)
    {
        const Author_Schema_View* schema = &slot->schemas[schema_index];
        uint32_t field_index = 0;
        if (!reload_render_view_equals(schema->schema_id,
                "nveditor.scene/v1"))
        {
            continue;
        }
        for (field_index = 0; field_index < schema->field_count; field_index += 1)
        {
            if (reload_render_view_equals(
                    schema->fields[field_index].json_path,
                    "$.cube.color"))
            {
                return schema->fields[field_index].id;
            }
        }
    }
    return 0;
}

static uint32_t
reload_render_delete_inactive(
    Reload_Render_Context* context,
    Module_Loader_Slot* slot)
{
    Module_Loader_Error error;
    Module_Loader_Result result = Module_Loader_Result_Ok;
    memset(&error, 0, sizeof(error));
    if (slot->state == Module_Loader_Slot_Empty)
    {
        return 1;
    }
    if (slot->state != Module_Loader_Slot_Inactive_Copy)
    {
        reload_render_check(context, 0, "unloaded DLL must be an inactive copy");
        return 0;
    }
    result = module_loader_delete_inactive_copy(slot, &error);
    reload_render_check(context, result == Module_Loader_Result_Ok &&
        slot->state == Module_Loader_Slot_Empty,
        "inactive hot DLL deletion must succeed");
    return result == Module_Loader_Result_Ok;
}

static uint32_t
reload_render_failed_candidate(
    Reload_Render_Context* context,
    const wchar_t* canonical_path,
    Module_Loader_Result expected_result,
    Module_Loader_Stage expected_stage,
    const char* stage_name,
    const Reload_Render_Frame* baseline)
{
    wchar_t hot_path[Reload_Render_Path_Capacity];
    Module_Loader_Prepare_Request request;
    Module_Loader_Error error;
    Reload_Render_Frame frame;
    Game_API active_api;
    void* active_library = context->active.native_library;
    void* active_game = context->active.game;
    const uint64_t active_generation = context->active.build_generation;
    const uint32_t clear_before = context->clear_context_count;
    Module_Loader_Result result = Module_Loader_Result_Ok;

    memset(hot_path, 0, sizeof(hot_path));
    memset(&request, 0, sizeof(request));
    memset(&error, 0, sizeof(error));
    memset(&frame, 0, sizeof(frame));
    active_api = context->active.api;
    reload_render_make_hot_path(
        context, hot_path, Reload_Render_Path_Capacity);
    request = reload_render_request(context, canonical_path, hot_path);
    result = module_loader_prepare(&context->candidate, &request, &error);

    reload_render_check(context, result == expected_result &&
        error.result == expected_result && error.stage == expected_stage,
        "candidate must reject at the requested transactional stage");
    reload_render_check(context,
        context->candidate.native_library == 0 &&
        context->candidate.game == 0 &&
        !module_loader_slot_is_callable(&context->candidate),
        "rejected candidate must leave no callable state");
    reload_render_check(context,
        context->clear_context_count == clear_before + 1,
        "candidate rollback must clear D3D bindings exactly once");
    reload_render_check(context,
        context->active.state == Module_Loader_Slot_Active &&
        context->active.native_library == active_library &&
        context->active.game == active_game &&
        context->active.build_generation == active_generation &&
        memcmp(&context->active.api, &active_api, sizeof(active_api)) == 0,
        "rejection must not mutate the active real module");

    if (!reload_render_active_frame(context, &frame))
    {
        return 0;
    }
    reload_render_check(context,
        frame.width == baseline->width && frame.height == baseline->height &&
        frame.changed_pixel_count == baseline->changed_pixel_count &&
        frame.hash == baseline->hash,
        "post-rejection cube framebuffer must match baseline exactly");
    printf("[ preserved ] %-12s cube_pixels=%u hash=%016llx",
        stage_name,
        (unsigned int)frame.changed_pixel_count,
        (unsigned long long)frame.hash);
    fputc(10, stdout);
    return reload_render_delete_inactive(context, &context->candidate);
}

static LRESULT CALLBACK
reload_render_window_proc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

int
wmain(int argument_count, wchar_t** arguments)
{
    enum
    {
        Argument_Hot_Directory = 1,
        Argument_Real_Module = 2,
        Argument_Canonical_Scene = 3,
        Argument_Invalid_Version = 4,
        Argument_Invalid_Create = 5,
        Argument_Invalid_Schema = 6,
        Argument_Invalid_Apply = 7,
        Required_Argument_Count = 8
    };
    static const wchar_t class_name[] =
        L"KalamRealModuleReloadRenderWindow";
    HINSTANCE instance = GetModuleHandleW(0);
    WNDCLASSEXW window_class;
    ATOM class_atom = 0;
    HWND window = 0;
    D3D11_Viewport_Host viewport;
    D3D11_Viewport_Create_Desc viewport_desc;
    Reload_Render_Context context;
    Module_Loader_Storage active_storage;
    Module_Loader_Storage candidate_storage;
    Module_Loader_Prepare_Request request;
    Module_Loader_Error loader_error;
    Module_Loader_Result loader_result = Module_Loader_Result_Ok;
    Author_Object_Source root_object;
    Author_Document_State document;
    Author_Value color_value;
    Game_Error game_error;
    Reload_Render_Frame baseline;
    wchar_t hot_path[Reload_Render_Path_Capacity];
    char scene_json[Reload_Render_Scene_Capacity];
    uint32_t scene_json_size = 0;
    Author_Field_Id color_field_id = 0;
    uint32_t viewport_created = 0;

    if (argument_count != Required_Argument_Count)
    {
        fputs("expected seven test arguments", stderr);
        fputc(10, stderr);
        return 2;
    }
    memset(&window_class, 0, sizeof(window_class));
    memset(&viewport, 0, sizeof(viewport));
    memset(&viewport_desc, 0, sizeof(viewport_desc));
    memset(&context, 0, sizeof(context));
    memset(&active_storage, 0, sizeof(active_storage));
    memset(&candidate_storage, 0, sizeof(candidate_storage));
    memset(&request, 0, sizeof(request));
    memset(&loader_error, 0, sizeof(loader_error));
    memset(&root_object, 0, sizeof(root_object));
    memset(&document, 0, sizeof(document));
    memset(&color_value, 0, sizeof(color_value));
    memset(&game_error, 0, sizeof(game_error));
    memset(&baseline, 0, sizeof(baseline));
    memset(hot_path, 0, sizeof(hot_path));
    memset(scene_json, 0, sizeof(scene_json));

    context.hot_directory = arguments[Argument_Hot_Directory];
    context.next_generation = 1;
    context.viewport = &viewport;
    if (!reload_render_read_scene(&context,
            arguments[Argument_Canonical_Scene], scene_json,
            Reload_Render_Scene_Capacity, &scene_json_size))
    {
        goto cleanup;
    }

    root_object.object_id = UINT64_C(0x1020304050607080);
    root_object.source_id = reload_render_view("");
    root_object.json_path = reload_render_view("$");
    document.struct_size = sizeof(document);
    document.document_id = UINT64_C(0x1122334455667788);
    document.root_object_id = root_object.object_id;
    document.revision = 1;
    document.schema_id = reload_render_view("nveditor.scene/v1");
    document.project_relative_path =
        reload_render_view("examples/cube.scene.json");
    document.json_utf8.data = scene_json;
    document.json_utf8.size = scene_json_size;
    document.objects = &root_object;
    document.object_count = 1;
    context.document = &document;

    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = reload_render_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    class_atom = RegisterClassExW(&window_class);
    reload_render_check(&context, class_atom != 0,
        "hidden WARP window class registration must succeed");
    if (class_atom == 0)
    {
        goto cleanup;
    }
    window = CreateWindowExW(
        0, class_name, L"Kalam real-module rejection render test",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 360, 240,
        0, 0, instance, 0);
    reload_render_check(&context, window != 0 && !IsWindowVisible(window),
        "WARP test window must be created and remain hidden");
    if (window == 0)
    {
        goto cleanup;
    }
    viewport_desc.struct_size = sizeof(viewport_desc);
    viewport_desc.window_handle = window;
    viewport_desc.width = 320;
    viewport_desc.height = 180;
    viewport_desc.driver = D3D11_Viewport_Driver_Warp;
    reload_render_check(&context,
        d3d11_viewport_host_create(&viewport, &viewport_desc) ==
            D3D11_Viewport_Result_Ok,
        "hidden D3D11 WARP viewport creation must succeed");
    if (!viewport.initialized)
    {
        goto cleanup;
    }
    viewport_created = 1;

    active_storage = reload_render_storage(&context.active_backing);
    candidate_storage = reload_render_storage(&context.candidate_backing);
    reload_render_check(&context,
        module_loader_slot_init(&context.active, &active_storage,
            &loader_error) == Module_Loader_Result_Ok,
        "active loader slot initialization must succeed");
    reload_render_check(&context,
        module_loader_slot_init(&context.candidate, &candidate_storage,
            &loader_error) == Module_Loader_Result_Ok,
        "candidate loader slot initialization must succeed");
    if (context.active.state != Module_Loader_Slot_Empty ||
        context.candidate.state != Module_Loader_Slot_Empty)
    {
        goto cleanup;
    }

    reload_render_make_hot_path(
        &context, hot_path, Reload_Render_Path_Capacity);
    request = reload_render_request(
        &context, arguments[Argument_Real_Module], hot_path);
    loader_result = module_loader_prepare(
        &context.candidate, &request, &loader_error);
    reload_render_check(&context,
        loader_result == Module_Loader_Result_Ok &&
        context.candidate.state == Module_Loader_Slot_Prepared &&
        context.candidate.schema_count >= 2,
        "real game_module must prepare with canonical scene and schemas");
    if (loader_result != Module_Loader_Result_Ok)
    {
        fputs("real module prepare failed: ", stderr);
        fputs(loader_error.message, stderr);
        fputc(10, stderr);
        goto cleanup;
    }
    loader_result = module_loader_commit(
        &context.active, &context.candidate, &loader_error);
    reload_render_check(&context,
        loader_result == Module_Loader_Result_Ok &&
        context.active.state == Module_Loader_Slot_Active &&
        context.candidate.state == Module_Loader_Slot_Empty,
        "real game_module must become active");
    if (loader_result != Module_Loader_Result_Ok)
    {
        goto cleanup;
    }

    color_field_id = reload_render_find_color_field(&context.active);
    reload_render_check(&context, color_field_id != 0,
        "copied real schema must expose $.cube.color");
    if (color_field_id == 0)
    {
        goto cleanup;
    }
    loader_result = context.active.api.read_author_field(
        context.active.game,
        document.document_id,
        document.root_object_id,
        color_field_id,
        &color_value,
        &game_error);
    reload_render_check(&context,
        loader_result == Game_Result_Ok &&
        color_value.kind == Author_Field_Color4 &&
        color_value.data.f32[0] > 0.11999f &&
        color_value.data.f32[0] < 0.12001f &&
        color_value.data.f32[1] > 0.57999f &&
        color_value.data.f32[1] < 0.58001f &&
        color_value.data.f32[2] > 0.94999f &&
        color_value.data.f32[2] < 0.95001f,
        "real module must contain canonical cube.scene.json color");

    if (!reload_render_active_frame(&context, &baseline))
    {
        goto cleanup;
    }
    printf("[ baseline  ] real cube    cube_pixels=%u hash=%016llx",
        (unsigned int)baseline.changed_pixel_count,
        (unsigned long long)baseline.hash);
    fputc(10, stdout);
    if (!reload_render_failed_candidate(
            &context, arguments[Argument_Invalid_Version],
            Module_Loader_Result_API_Version_Mismatch,
            Module_Loader_Stage_Validate_API,
            "API version", &baseline))
    {
        goto cleanup;
    }
    if (!reload_render_failed_candidate(
            &context, arguments[Argument_Invalid_Create],
            Module_Loader_Result_Create_Failed,
            Module_Loader_Stage_Create,
            "create", &baseline))
    {
        goto cleanup;
    }
    if (!reload_render_failed_candidate(
            &context, arguments[Argument_Invalid_Schema],
            Module_Loader_Result_Schema_Invalid,
            Module_Loader_Stage_Validate_Schema,
            "schema", &baseline))
    {
        goto cleanup;
    }
    if (!reload_render_failed_candidate(
            &context, arguments[Argument_Invalid_Apply],
            Module_Loader_Result_Document_Apply_Failed,
            Module_Loader_Stage_Apply_Document,
            "apply", &baseline))
    {
        goto cleanup;
    }

cleanup:
    if (context.candidate.state == Module_Loader_Slot_Prepared ||
        context.candidate.state == Module_Loader_Slot_Active ||
        context.candidate.state == Module_Loader_Slot_Unload_Pending)
    {
        memset(&loader_error, 0, sizeof(loader_error));
        reload_render_check(&context,
            module_loader_release(
                &context.candidate,
                reload_render_clear_context,
                &context,
                &loader_error) == Module_Loader_Result_Ok,
            "candidate cleanup release must succeed");
    }
    (void)reload_render_delete_inactive(&context, &context.candidate);
    if (context.active.state == Module_Loader_Slot_Prepared ||
        context.active.state == Module_Loader_Slot_Active ||
        context.active.state == Module_Loader_Slot_Unload_Pending)
    {
        memset(&loader_error, 0, sizeof(loader_error));
        reload_render_check(&context,
            module_loader_release(
                &context.active,
                reload_render_clear_context,
                &context,
                &loader_error) == Module_Loader_Result_Ok,
            "active real module shutdown release must succeed");
    }
    (void)reload_render_delete_inactive(&context, &context.active);
    if (viewport_created)
    {
        d3d11_viewport_host_destroy(&viewport);
    }
    if (window != 0)
    {
        reload_render_check(&context, DestroyWindow(window) != 0,
            "hidden WARP window destruction must succeed");
    }
    if (class_atom != 0)
    {
        reload_render_check(&context,
            UnregisterClassW(class_name, instance) != 0,
            "hidden WARP class unregister must succeed");
    }
    if (context.failure_count != 0)
    {
        fprintf(stderr, "%u real-module rejection render checks failed",
            (unsigned int)context.failure_count);
        fputc(10, stderr);
        return 1;
    }
    fputs("real game_module remained callable and rendered the unchanged ",
        stdout);
    fputs("canonical cube after all four candidate rejection stages", stdout);
    fputc(10, stdout);
    return 0;
}
