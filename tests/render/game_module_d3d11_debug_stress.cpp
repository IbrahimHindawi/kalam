#include "d3d11_viewport_host.h"
#include "game_api.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi.h>

#include <stdio.h>
#include <string.h>

enum
{
    Test_Cycle_Count = 12,
    Test_Live_Entry_Capacity = 64,
    Test_Description_Capacity = 256,
    Test_Skip_Return_Code = 77
};

typedef struct Test_Live_Entry
{
    D3D11_MESSAGE_ID id;
    uint32_t count;
    char description[Test_Description_Capacity];
} Test_Live_Entry;

typedef struct Test_Live_Snapshot
{
    Test_Live_Entry entries[Test_Live_Entry_Capacity];
    uint32_t entry_count;
    uint32_t detail_count;
    uint32_t summary_count;
    uint64_t raw_message_count;
} Test_Live_Snapshot;

static const char test_scene_json[] =
    "{"
    "\"$schema\":\"nveditor.scene/v1\","
    "\"cube\":{"
    "\"color\":[0.82,0.26,0.11,1.0],"
    "\"transform\":{"
    "\"translation\":[0.0,0.0,0.0],"
    "\"rotation_degrees\":[20.0,35.0,0.0],"
    "\"scale\":[0.9,0.9,0.9]"
    "}}}";

static const uint32_t test_cycle_widths[Test_Cycle_Count] =
{
    337, 421, 289, 512, 365, 448,
    313, 476, 351, 404, 278, 492
};

static const uint32_t test_cycle_heights[Test_Cycle_Count] =
{
    191, 237, 173, 288, 211, 252,
    187, 269, 203, 228, 167, 277
};

static void
test_print_hresult(const char* operation, HRESULT hresult)
{
    printf(
        "%s failed with HRESULT 0x%08x\n",
        operation,
        (unsigned int)(uint32_t)hresult);
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

static String_View
test_string_view(const char* text)
{
    String_View result;
    result.data = text;
    result.size = text != 0 ? (uint64_t)strlen(text) : 0;
    return result;
}

static uint32_t
test_module_path_to_wide(
    const char* utf8_path,
    wchar_t* out_path,
    uint32_t out_path_capacity)
{
    wchar_t input_path[32768];
    DWORD full_path_size = 0;
    int converted_size = 0;

    if (utf8_path == 0 ||
        out_path == 0 ||
        out_path_capacity == 0)
    {
        return 0;
    }

    memset(input_path, 0, sizeof(input_path));
    memset(
        out_path,
        0,
        (size_t)out_path_capacity * sizeof(out_path[0]));

    converted_size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8_path,
        -1,
        input_path,
        (int)(sizeof(input_path) / sizeof(input_path[0])));
    if (converted_size == 0)
    {
        return 0;
    }

    full_path_size = GetFullPathNameW(
        input_path,
        out_path_capacity,
        out_path,
        0);
    return full_path_size != 0 &&
        full_path_size < out_path_capacity;
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
test_is_live_summary(D3D11_MESSAGE_ID id)
{
    return id == D3D11_MESSAGE_ID_LIVE_OBJECT_SUMMARY ||
        id == D3D11_MESSAGE_ID_LIVE_OBJECT_SUMMARY_WIN7;
}

static uint32_t
test_add_live_message(
    Test_Live_Snapshot* snapshot,
    const D3D11_MESSAGE* message)
{
    Test_Live_Entry* entry = 0;
    uint32_t index = 0;
    size_t copy_size = 0;

    if (snapshot == 0 || message == 0)
    {
        return 0;
    }

    if (test_is_live_summary(message->ID))
    {
        snapshot->summary_count += 1;
        return 1;
    }

    for (index = 0; index < snapshot->entry_count; index += 1)
    {
        if (snapshot->entries[index].id == message->ID)
        {
            snapshot->entries[index].count += 1;
            snapshot->detail_count += 1;
            return 1;
        }
    }

    if (snapshot->entry_count >= Test_Live_Entry_Capacity)
    {
        printf(
            "live-object snapshot exceeded %u distinct message IDs\n",
            (unsigned int)Test_Live_Entry_Capacity);
        return 0;
    }

    entry = &snapshot->entries[snapshot->entry_count];
    snapshot->entry_count += 1;
    entry->id = message->ID;
    entry->count = 1;

    if (message->pDescription != 0 &&
        message->DescriptionByteLength != 0)
    {
        copy_size = message->DescriptionByteLength;
        if (copy_size >= sizeof(entry->description))
        {
            copy_size = sizeof(entry->description) - 1;
        }
        memcpy(
            entry->description,
            message->pDescription,
            copy_size);
        entry->description[copy_size] = '\0';
    }

    snapshot->detail_count += 1;
    return 1;
}

static uint32_t
test_capture_live_snapshot(
    ID3D11DeviceContext* context,
    ID3D11Debug* debug,
    ID3D11InfoQueue* info_queue,
    Test_Live_Snapshot* out_snapshot)
{
    D3D11_MESSAGE* message = 0;
    UINT64 discarded_before = 0;
    UINT64 discarded_after = 0;
    UINT64 message_count = 0;
    UINT64 message_index = 0;
    SIZE_T message_size = 0;
    HRESULT hresult = E_FAIL;
    uint32_t result = 0;

    if (context == 0 ||
        debug == 0 ||
        info_queue == 0 ||
        out_snapshot == 0)
    {
        return 0;
    }

    memset(out_snapshot, 0, sizeof(*out_snapshot));
    context->ClearState();
    context->Flush();

    info_queue->ClearStoredMessages();
    discarded_before =
        info_queue->GetNumMessagesDiscardedByMessageCountLimit();

    hresult = debug->ReportLiveDeviceObjects(
        (D3D11_RLDO_FLAGS)(
            D3D11_RLDO_SUMMARY |
            D3D11_RLDO_DETAIL |
            D3D11_RLDO_IGNORE_INTERNAL));
    if (FAILED(hresult))
    {
        test_print_hresult(
            "ID3D11Debug::ReportLiveDeviceObjects",
            hresult);
        return 0;
    }

    discarded_after =
        info_queue->GetNumMessagesDiscardedByMessageCountLimit();
    if (discarded_after != discarded_before)
    {
        printf(
            "D3D11 info queue discarded live-object details "
            "(before=%llu, after=%llu)\n",
            (unsigned long long)discarded_before,
            (unsigned long long)discarded_after);
        return 0;
    }

    message_count =
        info_queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    out_snapshot->raw_message_count = (uint64_t)message_count;
    if (message_count == 0)
    {
        printf(
            "D3D11 debug layer produced no live-object report messages\n");
        return 0;
    }

    for (message_index = 0;
         message_index < message_count;
         message_index += 1)
    {
        message_size = 0;
        hresult = info_queue->GetMessage(
            message_index,
            0,
            &message_size);
        if (FAILED(hresult) ||
            message_size < sizeof(D3D11_MESSAGE))
        {
            test_print_hresult(
                "ID3D11InfoQueue::GetMessage(size)",
                hresult);
            goto cleanup;
        }

        message = (D3D11_MESSAGE*)HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            message_size);
        if (message == 0)
        {
            printf(
                "could not allocate %llu bytes for a D3D11 message\n",
                (unsigned long long)message_size);
            goto cleanup;
        }

        hresult = info_queue->GetMessage(
            message_index,
            message,
            &message_size);
        if (FAILED(hresult))
        {
            test_print_hresult(
                "ID3D11InfoQueue::GetMessage",
                hresult);
            goto cleanup;
        }

        if (!test_add_live_message(out_snapshot, message))
        {
            goto cleanup;
        }

        HeapFree(GetProcessHeap(), 0, message);
        message = 0;
    }

    if (out_snapshot->summary_count == 0 ||
        out_snapshot->detail_count == 0)
    {
        printf(
            "incomplete live-object report: summaries=%u, details=%u\n",
            (unsigned int)out_snapshot->summary_count,
            (unsigned int)out_snapshot->detail_count);
        goto cleanup;
    }

    result = 1;

cleanup:
    if (message != 0)
    {
        HeapFree(GetProcessHeap(), 0, message);
    }
    return result;
}

static uint32_t
test_snapshot_count_for_id(
    const Test_Live_Snapshot* snapshot,
    D3D11_MESSAGE_ID id)
{
    uint32_t index = 0;

    for (index = 0; index < snapshot->entry_count; index += 1)
    {
        if (snapshot->entries[index].id == id)
        {
            return snapshot->entries[index].count;
        }
    }
    return 0;
}

static const char*
test_snapshot_description_for_id(
    const Test_Live_Snapshot* snapshot,
    D3D11_MESSAGE_ID id)
{
    uint32_t index = 0;

    for (index = 0; index < snapshot->entry_count; index += 1)
    {
        if (snapshot->entries[index].id == id)
        {
            return snapshot->entries[index].description;
        }
    }
    return "";
}

static uint32_t
test_live_snapshots_equal(
    const Test_Live_Snapshot* baseline,
    const Test_Live_Snapshot* after,
    uint32_t cycle_index)
{
    uint32_t index = 0;
    uint32_t equal = 1;

    if (baseline->detail_count != after->detail_count)
    {
        printf(
            "cycle %u live-object total changed: baseline=%u, after=%u\n",
            (unsigned int)cycle_index,
            (unsigned int)baseline->detail_count,
            (unsigned int)after->detail_count);
        equal = 0;
    }

    for (index = 0; index < baseline->entry_count; index += 1)
    {
        const Test_Live_Entry* entry = &baseline->entries[index];
        uint32_t after_count =
            test_snapshot_count_for_id(after, entry->id);
        if (entry->count != after_count)
        {
            printf(
                "cycle %u live detail ID %u changed: "
                "baseline=%u, after=%u\n"
                "  baseline sample: %s\n"
                "  after sample: %s\n",
                (unsigned int)cycle_index,
                (unsigned int)(uint32_t)entry->id,
                (unsigned int)entry->count,
                (unsigned int)after_count,
                entry->description,
                test_snapshot_description_for_id(after, entry->id));
            equal = 0;
        }
    }

    for (index = 0; index < after->entry_count; index += 1)
    {
        const Test_Live_Entry* entry = &after->entries[index];
        if (test_snapshot_count_for_id(baseline, entry->id) == 0)
        {
            printf(
                "cycle %u introduced live detail ID %u, count=%u\n"
                "  after sample: %s\n",
                (unsigned int)cycle_index,
                (unsigned int)(uint32_t)entry->id,
                (unsigned int)entry->count,
                entry->description);
            equal = 0;
        }
    }

    return equal;
}

static LRESULT CALLBACK
test_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

static int
test_reload_resize_debug_stress(const char* module_path)
{
    static const wchar_t class_name[] =
        L"KalamGameModuleD3D11DebugStressWindow";
    HINSTANCE instance = GetModuleHandleW(0);
    WNDCLASSEXW window_class;
    ATOM class_atom = 0;
    HWND window = 0;
    wchar_t wide_module_path[32768];
    D3D11_Viewport_Host host;
    D3D11_Viewport_Create_Desc viewport_desc;
    D3D11_Viewport_Result viewport_result =
        D3D11_Viewport_Result_Ok;
    ID3D11Device* device = 0;
    ID3D11DeviceContext* context = 0;
    ID3D11Debug* debug = 0;
    ID3D11InfoQueue* info_queue = 0;
    HRESULT hresult = E_FAIL;
    uint32_t host_created = 0;
    uint32_t cycle_index = 0;
    uint32_t unload_count = 0;
    uint32_t generation_advance_count = 0;
    int result = 1;

    memset(&window_class, 0, sizeof(window_class));
    memset(wide_module_path, 0, sizeof(wide_module_path));
    memset(&host, 0, sizeof(host));
    memset(&viewport_desc, 0, sizeof(viewport_desc));

    if (!test_module_path_to_wide(
            module_path,
            wide_module_path,
            (uint32_t)(
                sizeof(wide_module_path) /
                sizeof(wide_module_path[0]))))
    {
        printf("game module path is not valid UTF-8 or is too long\n");
        goto cleanup;
    }

    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = test_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    class_atom = RegisterClassExW(&window_class);
    if (class_atom == 0)
    {
        printf(
            "RegisterClassExW failed with Win32 error %lu\n",
            (unsigned long)GetLastError());
        goto cleanup;
    }

    window = CreateWindowExW(
        0,
        class_name,
        L"Kalam hidden D3D11 debug stress",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        360,
        240,
        0,
        0,
        instance,
        0);
    if (window == 0)
    {
        printf(
            "CreateWindowExW failed with Win32 error %lu\n",
            (unsigned long)GetLastError());
        goto cleanup;
    }

    viewport_desc.struct_size = (uint32_t)sizeof(viewport_desc);
    viewport_desc.flags =
        D3D11_Viewport_Create_Enable_Debug_Layer |
        D3D11_Viewport_Create_Require_Debug_Layer;
    viewport_desc.window_handle = window;
    viewport_desc.width = 320;
    viewport_desc.height = 180;
    viewport_desc.driver = D3D11_Viewport_Driver_Warp;
    viewport_result =
        d3d11_viewport_host_create(&host, &viewport_desc);
    if (viewport_result != D3D11_Viewport_Result_Ok)
    {
        hresult = (HRESULT)host.last_hresult;
        if (viewport_result ==
                D3D11_Viewport_Result_Device_Create_Failed &&
            hresult == DXGI_ERROR_SDK_COMPONENT_MISSING)
        {
            printf(
                "SKIP: D3D11 debug layer is unavailable "
                "(Windows Graphics Tools is not installed; "
                "HRESULT 0x%08x)\n",
                (unsigned int)(uint32_t)hresult);
            result = Test_Skip_Return_Code;
            goto cleanup;
        }

        printf(
            "required WARP debug viewport create failed: %s "
            "(HRESULT 0x%08x)\n",
            d3d11_viewport_result_string(viewport_result),
            (unsigned int)(uint32_t)hresult);
        goto cleanup;
    }
    host_created = 1;

    if (!host.debug_layer_enabled ||
        (host.device_creation_flags &
         D3D11_CREATE_DEVICE_DEBUG) == 0)
    {
        printf("viewport did not retain the required D3D11 debug layer\n");
        goto cleanup;
    }

    device = (ID3D11Device*)host.device;
    context = (ID3D11DeviceContext*)host.immediate_context;
    hresult = device->QueryInterface(
        __uuidof(ID3D11Debug),
        (void**)&debug);
    if (hresult == E_NOINTERFACE)
    {
        printf(
            "SKIP: D3D11 debug interfaces are unavailable even though "
            "debug device creation succeeded\n");
        result = Test_Skip_Return_Code;
        goto cleanup;
    }
    if (FAILED(hresult))
    {
        test_print_hresult(
            "ID3D11Device::QueryInterface(ID3D11Debug)",
            hresult);
        goto cleanup;
    }

    hresult = device->QueryInterface(
        __uuidof(ID3D11InfoQueue),
        (void**)&info_queue);
    if (hresult == E_NOINTERFACE)
    {
        printf("SKIP: ID3D11InfoQueue is unavailable\n");
        result = Test_Skip_Return_Code;
        goto cleanup;
    }
    if (FAILED(hresult))
    {
        test_print_hresult(
            "ID3D11Device::QueryInterface(ID3D11InfoQueue)",
            hresult);
        goto cleanup;
    }

    info_queue->ClearStorageFilter();
    info_queue->ClearRetrievalFilter();
    hresult = info_queue->SetMessageCountLimit(4096);
    if (FAILED(hresult))
    {
        test_print_hresult(
            "ID3D11InfoQueue::SetMessageCountLimit",
            hresult);
        goto cleanup;
    }

    printf(
        "D3D11 WARP debug layer enabled; starting %u "
        "reload/resize cycles\n",
        (unsigned int)Test_Cycle_Count);

    for (cycle_index = 0;
         cycle_index < Test_Cycle_Count;
         cycle_index += 1)
    {
        HMODULE module = 0;
        HMODULE module_lookup = 0;
        Game_Get_API_Function get_api = 0;
        Game_API api;
        Game_Error error;
        Game_Create_Desc game_desc;
        Author_Object_Source objects[1];
        Author_Document_State document;
        Host_Render_Target target;
        Test_Live_Snapshot baseline;
        Test_Live_Snapshot active;
        Test_Live_Snapshot after;
        void* game = 0;
        uint64_t generation_before_resize = 0;
        Game_Result game_result = Game_Result_Ok;
        uint32_t cycle_ok = 1;

        memset(&api, 0, sizeof(api));
        memset(&error, 0, sizeof(error));
        memset(&game_desc, 0, sizeof(game_desc));
        memset(objects, 0, sizeof(objects));
        memset(&document, 0, sizeof(document));
        memset(&target, 0, sizeof(target));
        memset(&baseline, 0, sizeof(baseline));
        memset(&active, 0, sizeof(active));
        memset(&after, 0, sizeof(after));

        if (!test_capture_live_snapshot(
                context,
                debug,
                info_queue,
                &baseline))
        {
            cycle_ok = 0;
            goto cycle_cleanup;
        }

        module = LoadLibraryW(wide_module_path);
        if (module == 0)
        {
            printf(
                "cycle %u LoadLibraryW failed with Win32 error %lu\n",
                (unsigned int)(cycle_index + 1),
                (unsigned long)GetLastError());
            cycle_ok = 0;
            goto cycle_cleanup;
        }

        module_lookup = GetModuleHandleW(wide_module_path);
        if (module_lookup != module)
        {
            printf(
                "cycle %u could not confirm loaded module handle\n",
                (unsigned int)(cycle_index + 1));
            cycle_ok = 0;
            goto cycle_cleanup;
        }

        if (!test_resolve_game_get_api(module, &get_api))
        {
            printf(
                "cycle %u could not resolve game_get_api\n",
                (unsigned int)(cycle_index + 1));
            cycle_ok = 0;
            goto cycle_cleanup;
        }

        game_result = get_api(
            Host_API_Version,
            (uint32_t)sizeof(api),
            &api,
            &error);
        if (game_result != Game_Result_Ok ||
            api.api_version != Game_API_Version ||
            api.struct_size != (uint32_t)sizeof(api) ||
            api.create == 0 ||
            api.destroy == 0 ||
            api.render == 0 ||
            api.apply_document == 0)
        {
            test_print_game_error("game_get_api", &error);
            cycle_ok = 0;
            goto cycle_cleanup;
        }

        game_desc.struct_size = (uint32_t)sizeof(game_desc);
        game_desc.host_api_version = Host_API_Version;
        game_desc.d3d11_device = host.device;
        game_desc.d3d11_immediate_context =
            host.immediate_context;
        game_desc.project_root.data = KALAM_TEST_PROJECT_ROOT;
        game_desc.project_root.size =
            (uint64_t)(sizeof(KALAM_TEST_PROJECT_ROOT) - 1);
        game_result = api.create(
            &game_desc,
            &game,
            &error);
        if (game_result != Game_Result_Ok || game == 0)
        {
            test_print_game_error("Game_API.create", &error);
            cycle_ok = 0;
            goto cycle_cleanup;
        }

        objects[0].object_id =
            UINT64_C(0x1020304050607080);
        objects[0].json_path = test_string_view("$");

        document.struct_size = (uint32_t)sizeof(document);
        document.document_id =
            UINT64_C(0x1122334455667788);
        document.root_object_id = objects[0].object_id;
        document.revision = (uint64_t)cycle_index + 1;
        document.schema_id =
            test_string_view("nveditor.scene/v1");
        document.project_relative_path =
            test_string_view("examples/cube.scene.json");
        document.json_utf8 = test_string_view(test_scene_json);
        document.objects = objects;
        document.object_count = 1;

        game_result = api.apply_document(
            game,
            &document,
            &error);
        if (game_result != Game_Result_Ok)
        {
            test_print_game_error(
                "Game_API.apply_document",
                &error);
            cycle_ok = 0;
            goto cycle_cleanup;
        }

        if (!d3d11_viewport_host_get_render_target(
                &host,
                &target))
        {
            printf(
                "cycle %u initial render target is unavailable\n",
                (unsigned int)(cycle_index + 1));
            cycle_ok = 0;
            goto cycle_cleanup;
        }
        game_result = api.render(game, &target, 0.25f, &error);
        if (game_result != Game_Result_Ok)
        {
            test_print_game_error(
                "Game_API.render(before resize)",
                &error);
            cycle_ok = 0;
            goto cycle_cleanup;
        }

        generation_before_resize = target.generation;
        viewport_result = d3d11_viewport_host_resize(
            &host,
            test_cycle_widths[cycle_index],
            test_cycle_heights[cycle_index]);
        if (viewport_result != D3D11_Viewport_Result_Ok)
        {
            printf(
                "cycle %u viewport resize failed: %s "
                "(HRESULT 0x%08x)\n",
                (unsigned int)(cycle_index + 1),
                d3d11_viewport_result_string(viewport_result),
                (unsigned int)(uint32_t)host.last_hresult);
            cycle_ok = 0;
            goto cycle_cleanup;
        }
        if (!d3d11_viewport_host_get_render_target(
                &host,
                &target) ||
            target.generation <= generation_before_resize)
        {
            printf(
                "cycle %u resize generation did not advance "
                "(before=%llu, after=%llu)\n",
                (unsigned int)(cycle_index + 1),
                (unsigned long long)generation_before_resize,
                (unsigned long long)target.generation);
            cycle_ok = 0;
            goto cycle_cleanup;
        }
        generation_advance_count += 1;

        game_result = api.render(game, &target, 0.75f, &error);
        if (game_result != Game_Result_Ok)
        {
            test_print_game_error(
                "Game_API.render(after resize)",
                &error);
            cycle_ok = 0;
            goto cycle_cleanup;
        }

        if (!test_capture_live_snapshot(
                context,
                debug,
                info_queue,
                &active))
        {
            cycle_ok = 0;
            goto cycle_cleanup;
        }
        if (active.detail_count <= baseline.detail_count)
        {
            printf(
                "cycle %u module resources were not visible to "
                "the D3D11 live-object report "
                "(baseline=%u, active=%u)\n",
                (unsigned int)(cycle_index + 1),
                (unsigned int)baseline.detail_count,
                (unsigned int)active.detail_count);
            cycle_ok = 0;
            goto cycle_cleanup;
        }

cycle_cleanup:
        if (game != 0 && api.destroy != 0)
        {
            api.destroy(game);
            game = 0;
        }
        d3d11_viewport_host_clear_bindings(&host, 1);

        if (module != 0)
        {
            if (!FreeLibrary(module))
            {
                printf(
                    "cycle %u FreeLibrary failed with Win32 error %lu\n",
                    (unsigned int)(cycle_index + 1),
                    (unsigned long)GetLastError());
                cycle_ok = 0;
            }
            else
            {
                unload_count += 1;
                module = 0;
                get_api = 0;
                memset(&api, 0, sizeof(api));

                if (GetModuleHandleW(wide_module_path) != 0)
                {
                    printf(
                        "cycle %u game_module.dll remains loaded "
                        "after FreeLibrary\n",
                        (unsigned int)(cycle_index + 1));
                    cycle_ok = 0;
                }
            }
        }

        if (cycle_ok &&
            !test_capture_live_snapshot(
                context,
                debug,
                info_queue,
                &after))
        {
            cycle_ok = 0;
        }
        if (cycle_ok &&
            !test_live_snapshots_equal(
                &baseline,
                &after,
                cycle_index + 1))
        {
            cycle_ok = 0;
        }

        if (!cycle_ok)
        {
            if (module != 0)
            {
                (void)FreeLibrary(module);
            }
            goto cleanup;
        }

        printf(
            "cycle %u/%u: generation=%llu, host live=%u, "
            "module live=%u (+%u), post-unload live=%u\n",
            (unsigned int)(cycle_index + 1),
            (unsigned int)Test_Cycle_Count,
            (unsigned long long)target.generation,
            (unsigned int)baseline.detail_count,
            (unsigned int)active.detail_count,
            (unsigned int)(
                active.detail_count - baseline.detail_count),
            (unsigned int)after.detail_count);
    }

    if (unload_count != Test_Cycle_Count ||
        generation_advance_count != Test_Cycle_Count)
    {
        printf(
            "stress counters are incomplete: unloads=%u, "
            "generation advances=%u, expected=%u\n",
            (unsigned int)unload_count,
            (unsigned int)generation_advance_count,
            (unsigned int)Test_Cycle_Count);
        goto cleanup;
    }

    printf(
        "verified %u successful DLL unloads, %u resize-generation "
        "advances, and no growth in D3D11 live-object type counts\n",
        (unsigned int)unload_count,
        (unsigned int)generation_advance_count);
    result = 0;

cleanup:
    if (info_queue != 0)
    {
        info_queue->ClearStoredMessages();
        info_queue->Release();
    }
    if (debug != 0)
    {
        debug->Release();
    }
    if (host_created)
    {
        d3d11_viewport_host_destroy(&host);
    }
    if (window != 0 && !DestroyWindow(window) && result == 0)
    {
        printf(
            "DestroyWindow failed with Win32 error %lu\n",
            (unsigned long)GetLastError());
        result = 1;
    }
    if (class_atom != 0 &&
        !UnregisterClassW(class_name, instance) &&
        result == 0)
    {
        printf(
            "UnregisterClassW failed with Win32 error %lu\n",
            (unsigned long)GetLastError());
        result = 1;
    }
    return result;
}

int
main(int argument_count, char** arguments)
{
    int result = 0;

    if (argument_count != 2)
    {
        printf(
            "usage: game_module_d3d11_debug_stress.exe "
            "<absolute-game-module-dll-path>\n");
        return 2;
    }

    printf("[ RUN      ] game_module_reload_resize_debug\n");
    result = test_reload_resize_debug_stress(arguments[1]);
    if (result == Test_Skip_Return_Code)
    {
        printf("[  SKIPPED ] game_module_reload_resize_debug\n");
        return result;
    }
    if (result != 0)
    {
        printf("[  FAILED  ] game_module_reload_resize_debug\n");
        return result;
    }

    printf("[       OK ] game_module_reload_resize_debug\n");
    return 0;
}
