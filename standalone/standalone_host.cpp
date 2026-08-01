#include "standalone_host.h"

#include "d3d11_viewport_host.h"
#include "frame_scheduler.h"
#include "input_latch.h"
#include "module_loader.h"

#if !defined(_WIN32)
#error The Kalam standalone host requires Win32.
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>

#include <limits.h>
#include <stdio.h>
#include <strsafe.h>
#include <string.h>

enum
{
    Standalone_Path_Capacity = 32768,
    Standalone_Module_Schema_Capacity = 16,
    Standalone_Module_Field_Capacity = 128,
    Standalone_Module_String_Capacity = 64 * 1024,
    Standalone_Max_Document_Size = 64 * 1024 * 1024,
    Standalone_Default_Width = 1280,
    Standalone_Default_Height = 720,
    Standalone_Smoke_Width = 320,
    Standalone_Smoke_Height = 180,
    Standalone_Default_Smoke_Frame_Count = 4
};

typedef uint32_t Standalone_Parse_Result;

enum
{
    Standalone_Parse_Ok = 0,
    Standalone_Parse_Help = 1,
    Standalone_Parse_Error = 2
};

typedef struct Standalone_Options
{
    const wchar_t* module_path;
    const wchar_t* hot_directory;
    const wchar_t* scene_path;
    const wchar_t* post_process_path;

    uint32_t frame_limit;
    uint32_t frame_limit_enabled;
    uint32_t smoke;
    uint32_t warp;

    wchar_t default_module_path[Standalone_Path_Capacity];
} Standalone_Options;

typedef struct Standalone_Document_Owner
{
    char* json_allocation;
    char* path_utf8_allocation;
    Author_Object_Source root_object;
} Standalone_Document_Owner;

typedef struct Standalone_Module_Buffers
{
    Author_Schema_View schemas[Standalone_Module_Schema_Capacity];
    Author_Field_Desc fields[Standalone_Module_Field_Capacity];
    char strings[Standalone_Module_String_Capacity];
    wchar_t loaded_path[Standalone_Path_Capacity];
} Standalone_Module_Buffers;

typedef struct Standalone_Host
{
    HWND window;
    HINSTANCE instance;
    ATOM window_class;

    D3D11_Viewport_Host viewport;
    Frame_Scheduler scheduler;
    Input_Latch input;

    Module_Loader_Slot active_module;
    Module_Loader_Slot candidate_module;
    Standalone_Module_Buffers active_module_buffers;
    Standalone_Module_Buffers candidate_module_buffers;

    Author_Document_State documents[2];
    Standalone_Document_Owner document_owners[2];
    uint32_t document_count;

    uint32_t running;
    uint32_t fatal_window_error;
    uint32_t viewport_initialized;
    uint32_t pointer_position_valid;
    uint32_t captured_mouse_buttons;
    uint32_t clock_reset_requested;

    uint64_t fixed_update_count;
    uint64_t rendered_frame_count;
    uint64_t present_call_count;

    /*
     * UTF-8 project root for manifest resources. The shipping layout
     * stages res/ beside the executable, so the root is the executable's
     * directory (docs/resource_manifest.md section 5).
     */
    char project_root[Standalone_Path_Capacity];
    uint32_t project_root_size;
} Standalone_Host;

static const wchar_t Standalone_Window_Class_Name[] =
    L"KalamStandaloneD3D11Window";

static const char Standalone_Scene_Schema[] = "nveditor.scene/v1";
static const char Standalone_Post_Process_Schema[] =
    "nveditor.post_process/v1";
static const char Standalone_Root_JSON_Path[] = "$";

static volatile LONG standalone_hot_copy_counter;

static uint64_t
standalone_string_size(const char* text)
{
    uint64_t size = 0;

    while (text != 0 && text[size] != '\0')
    {
        size += 1;
    }
    return size;
}

static String_View
standalone_string_view(const char* text)
{
    String_View result;
    result.data = text;
    result.size = standalone_string_size(text);
    return result;
}

static uint32_t
standalone_wide_equal(const wchar_t* left, const wchar_t* right)
{
    if (left == 0 || right == 0)
    {
        return left == right;
    }
    return wcscmp(left, right) == 0;
}

static void
standalone_print_usage(void)
{
    fwprintf(
        stdout,
        L"Usage: game.exe [options]\n"
        L"  --module <game_module.dll>  Canonical module (default: beside game.exe)\n"
        L"  --hot-dir <directory>       Directory for the unique temporary DLL copy\n"
        L"  --scene <cube.scene.json>   Apply a scene snapshot at startup\n"
        L"  --post <post_process.json>  Apply a post-process snapshot at startup\n"
        L"  --frames <positive-count>   Exit after this many presentation frames\n"
        L"  --warp                      Use the D3D11 WARP software adapter\n"
        L"  --smoke                     Hidden WARP run (defaults to four frames)\n"
        L"  --help                      Show this message\n");
}

static uint32_t
standalone_parse_positive_u32(
    const wchar_t* text,
    uint32_t* out_value)
{
    uint64_t value = 0;
    uint32_t index = 0;

    if (out_value != 0)
    {
        *out_value = 0;
    }
    if (text == 0 || text[0] == L'\0' || out_value == 0)
    {
        return 0;
    }

    while (text[index] != L'\0')
    {
        uint32_t digit = 0;
        if (text[index] < L'0' || text[index] > L'9')
        {
            return 0;
        }

        digit = (uint32_t)(text[index] - L'0');
        value = value * 10u + digit;
        if (value > UINT32_MAX)
        {
            return 0;
        }
        index += 1;
    }

    if (value == 0)
    {
        return 0;
    }
    *out_value = (uint32_t)value;
    return 1;
}

static uint32_t
standalone_default_module_path(
    wchar_t* output,
    uint32_t output_capacity)
{
    DWORD path_size = 0;
    uint32_t index = 0;
    uint32_t filename_index = 0;

    if (output == 0 || output_capacity == 0)
    {
        return 0;
    }

    path_size = GetModuleFileNameW(0, output, output_capacity);
    if (path_size == 0 || path_size >= output_capacity)
    {
        output[0] = L'\0';
        return 0;
    }

    filename_index = 0;
    for (index = 0; index < path_size; index += 1)
    {
        if (output[index] == L'\\' || output[index] == L'/')
        {
            filename_index = index + 1;
        }
    }

    return SUCCEEDED(
        StringCchCopyW(
            output + filename_index,
            output_capacity - filename_index,
            L"game_module.dll"));
}

static Standalone_Parse_Result
standalone_parse_options(
    int argument_count,
    wchar_t** arguments,
    Standalone_Options* options)
{
    int index = 0;

    if (options == 0 || argument_count < 0 ||
        (argument_count != 0 && arguments == 0))
    {
        return Standalone_Parse_Error;
    }

    memset(options, 0, sizeof(*options));
    if (!standalone_default_module_path(
            options->default_module_path,
            Standalone_Path_Capacity))
    {
        fwprintf(stderr, L"Could not derive game_module.dll beside game.exe.\n");
        return Standalone_Parse_Error;
    }
    options->module_path = options->default_module_path;

    for (index = 1; index < argument_count; index += 1)
    {
        const wchar_t* argument = arguments[index];

        if (standalone_wide_equal(argument, L"--help") ||
            standalone_wide_equal(argument, L"-h"))
        {
            return Standalone_Parse_Help;
        }
        if (standalone_wide_equal(argument, L"--smoke"))
        {
            options->smoke = 1;
            options->warp = 1;
            continue;
        }
        if (standalone_wide_equal(argument, L"--warp"))
        {
            options->warp = 1;
            continue;
        }

        if (index + 1 >= argument_count)
        {
            fwprintf(stderr, L"Option '%ls' requires a value.\n", argument);
            return Standalone_Parse_Error;
        }

        if (standalone_wide_equal(argument, L"--module"))
        {
            options->module_path = arguments[index + 1];
        }
        else if (standalone_wide_equal(argument, L"--hot-dir"))
        {
            options->hot_directory = arguments[index + 1];
        }
        else if (standalone_wide_equal(argument, L"--scene"))
        {
            options->scene_path = arguments[index + 1];
        }
        else if (standalone_wide_equal(argument, L"--post"))
        {
            options->post_process_path = arguments[index + 1];
        }
        else if (standalone_wide_equal(argument, L"--frames"))
        {
            if (!standalone_parse_positive_u32(
                    arguments[index + 1],
                    &options->frame_limit))
            {
                fwprintf(
                    stderr,
                    L"--frames requires a positive 32-bit integer.\n");
                return Standalone_Parse_Error;
            }
            options->frame_limit_enabled = 1;
        }
        else
        {
            fwprintf(stderr, L"Unknown option '%ls'.\n", argument);
            return Standalone_Parse_Error;
        }
        index += 1;
    }

    if (options->smoke && !options->frame_limit_enabled)
    {
        options->frame_limit = Standalone_Default_Smoke_Frame_Count;
        options->frame_limit_enabled = 1;
    }
    return Standalone_Parse_Ok;
}

static uint32_t
standalone_ensure_directory(const wchar_t* path)
{
    DWORD attributes = 0;

    if (path == 0 || path[0] == L'\0')
    {
        return 0;
    }

    attributes = GetFileAttributesW(path);
    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    if (!CreateDirectoryW(path, 0) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
    {
        return 0;
    }

    attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static uint32_t
standalone_make_hot_copy_path(
    const wchar_t* canonical_input,
    const wchar_t* requested_hot_directory,
    wchar_t* canonical_output,
    wchar_t* hot_output)
{
    wchar_t hot_directory[Standalone_Path_Capacity];
    DWORD canonical_size = 0;
    DWORD hot_directory_size = 0;
    DWORD index = 0;
    DWORD directory_size = 0;
    DWORD process_id = GetCurrentProcessId();
    FILETIME file_time;
    LONG counter = 0;
    const wchar_t* separator = L"\\";
    HRESULT format_result = E_FAIL;

    memset(hot_directory, 0, sizeof(hot_directory));
    memset(&file_time, 0, sizeof(file_time));
    if (canonical_input == 0 || canonical_input[0] == L'\0' ||
        canonical_output == 0 || hot_output == 0)
    {
        return 0;
    }

    canonical_size = GetFullPathNameW(
        canonical_input,
        Standalone_Path_Capacity,
        canonical_output,
        0);
    if (canonical_size == 0 ||
        canonical_size >= Standalone_Path_Capacity)
    {
        return 0;
    }

    if (requested_hot_directory != 0)
    {
        hot_directory_size = GetFullPathNameW(
            requested_hot_directory,
            Standalone_Path_Capacity,
            hot_directory,
            0);
        if (hot_directory_size == 0 ||
            hot_directory_size >= Standalone_Path_Capacity ||
            !standalone_ensure_directory(hot_directory))
        {
            return 0;
        }
    }
    else
    {
        for (index = 0; index < canonical_size; index += 1)
        {
            if (canonical_output[index] == L'\\' ||
                canonical_output[index] == L'/')
            {
                directory_size = index;
            }
        }
        if (directory_size == 0 ||
            directory_size >= Standalone_Path_Capacity)
        {
            return 0;
        }

        memcpy(
            hot_directory,
            canonical_output,
            (size_t)directory_size * sizeof(wchar_t));
        hot_directory[directory_size] = L'\0';
        hot_directory_size = directory_size;
    }

    if (hot_directory_size != 0 &&
        (hot_directory[hot_directory_size - 1] == L'\\' ||
         hot_directory[hot_directory_size - 1] == L'/'))
    {
        separator = L"";
    }

    GetSystemTimeAsFileTime(&file_time);
    counter = InterlockedIncrement(&standalone_hot_copy_counter);
    format_result = StringCchPrintfW(
        hot_output,
        Standalone_Path_Capacity,
        L"%ls%lsgame_module.hot.%08lX.%08lX%08lX.%08lX.dll",
        hot_directory,
        separator,
        process_id,
        file_time.dwHighDateTime,
        file_time.dwLowDateTime,
        (DWORD)counter);
    return SUCCEEDED(format_result);
}

static void
standalone_print_string_view(String_View value)
{
    uint64_t size = value.size;

    if (value.data == 0)
    {
        return;
    }
    if (size > 4096)
    {
        size = 4096;
    }
    if (size != 0)
    {
        fwrite(value.data, 1, (size_t)size, stderr);
    }
    if (size != value.size)
    {
        fputs("...", stderr);
    }
}

static void
standalone_host_log(
    void* host_user,
    uint32_t level,
    String_View message)
{
    (void)host_user;
    fprintf(stderr, "[game:%u] ", (unsigned int)level);
    standalone_print_string_view(message);
    fputc('\n', stderr);
}

static void
standalone_print_game_error(
    const char* operation,
    Game_Result result,
    const Game_Error* error)
{
    fprintf(
        stderr,
        "%s failed (game result %u)",
        operation,
        (unsigned int)result);
    if (error != 0 &&
        error->message.data != 0 &&
        error->message.size != 0)
    {
        fputs(": ", stderr);
        standalone_print_string_view(error->message);
    }
    if (error != 0 &&
        error->json_path.data != 0 &&
        error->json_path.size != 0)
    {
        fputs(" at ", stderr);
        standalone_print_string_view(error->json_path);
    }
    fputc('\n', stderr);
}

static void
standalone_print_module_error(
    const char* operation,
    Module_Loader_Result result,
    const Module_Loader_Error* error)
{
    fprintf(
        stderr,
        "%s failed: %s",
        operation,
        module_loader_result_string(result));
    if (error != 0)
    {
        fprintf(
            stderr,
            " (stage %u, Win32 %u, game %u)",
            (unsigned int)error->stage,
            (unsigned int)error->win32_error,
            (unsigned int)error->game_result);
        if (error->message_size != 0)
        {
            fputs(": ", stderr);
            fwrite(
                error->message,
                1,
                (size_t)error->message_size,
                stderr);
        }
        if (error->json_path_size != 0)
        {
            fputs(" at ", stderr);
            fwrite(
                error->json_path,
                1,
                (size_t)error->json_path_size,
                stderr);
        }
    }
    fputc('\n', stderr);
}

static uint32_t
standalone_wide_path_to_utf8(
    const wchar_t* path,
    char** out_allocation,
    String_View* out_view)
{
    int required_size = 0;
    int converted_size = 0;
    char* allocation = 0;

    if (out_allocation != 0)
    {
        *out_allocation = 0;
    }
    if (out_view != 0)
    {
        memset(out_view, 0, sizeof(*out_view));
    }
    if (path == 0 || out_allocation == 0 || out_view == 0)
    {
        return 0;
    }

    required_size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        path,
        -1,
        0,
        0,
        0,
        0);
    if (required_size <= 1)
    {
        return 0;
    }

    allocation = (char*)HeapAlloc(
        GetProcessHeap(),
        0,
        (SIZE_T)required_size);
    if (allocation == 0)
    {
        return 0;
    }

    converted_size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        path,
        -1,
        allocation,
        required_size,
        0,
        0);
    if (converted_size != required_size)
    {
        HeapFree(GetProcessHeap(), 0, allocation);
        return 0;
    }

    *out_allocation = allocation;
    out_view->data = allocation;
    out_view->size = (uint64_t)(converted_size - 1);
    return 1;
}

static uint32_t
standalone_read_document(
    const wchar_t* path,
    const char* schema_id,
    Author_Document_Id document_id,
    Author_Object_Id root_object_id,
    Author_Document_State* out_document,
    Standalone_Document_Owner* out_owner)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER file_size;
    DWORD bytes_read = 0;
    char* json = 0;
    const char* json_view = 0;
    uint64_t json_view_size = 0;
    uint32_t result = 0;

    memset(&file_size, 0, sizeof(file_size));
    if (out_document != 0)
    {
        memset(out_document, 0, sizeof(*out_document));
    }
    if (out_owner != 0)
    {
        memset(out_owner, 0, sizeof(*out_owner));
    }
    if (path == 0 || path[0] == L'\0' ||
        schema_id == 0 ||
        document_id == 0 ||
        root_object_id == 0 ||
        out_document == 0 ||
        out_owner == 0)
    {
        return 0;
    }

    file = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        0,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        0);
    if (file == INVALID_HANDLE_VALUE)
    {
        fwprintf(
            stderr,
            L"Could not open authored document '%ls' (Win32 %lu).\n",
            path,
            GetLastError());
        goto cleanup;
    }

    if (!GetFileSizeEx(file, &file_size) ||
        file_size.QuadPart <= 0 ||
        file_size.QuadPart > Standalone_Max_Document_Size)
    {
        fwprintf(
            stderr,
            L"Authored document '%ls' is empty, unreadable, or larger than %u bytes.\n",
            path,
            (unsigned int)Standalone_Max_Document_Size);
        goto cleanup;
    }

    json = (char*)HeapAlloc(
        GetProcessHeap(),
        0,
        (SIZE_T)file_size.QuadPart + 1u);
    if (json == 0)
    {
        fwprintf(
            stderr,
            L"Could not allocate storage for authored document '%ls'.\n",
            path);
        goto cleanup;
    }

    if (!ReadFile(
            file,
            json,
            (DWORD)file_size.QuadPart,
            &bytes_read,
            0) ||
        bytes_read != (DWORD)file_size.QuadPart)
    {
        fwprintf(
            stderr,
            L"Could not completely read authored document '%ls' (Win32 %lu).\n",
            path,
            GetLastError());
        goto cleanup;
    }
    json[bytes_read] = '\0';

    if (!standalone_wide_path_to_utf8(
            path,
            &out_owner->path_utf8_allocation,
            &out_document->project_relative_path))
    {
        fwprintf(
            stderr,
            L"Could not convert authored document path '%ls' to UTF-8.\n",
            path);
        goto cleanup;
    }

    json_view = json;
    json_view_size = bytes_read;
    if (json_view_size >= 3 &&
        (uint8_t)json_view[0] == 0xef &&
        (uint8_t)json_view[1] == 0xbb &&
        (uint8_t)json_view[2] == 0xbf)
    {
        json_view += 3;
        json_view_size -= 3;
    }

    out_owner->json_allocation = json;
    out_owner->root_object.object_id = root_object_id;
    out_owner->root_object.json_path =
        standalone_string_view(Standalone_Root_JSON_Path);

    out_document->struct_size = (uint32_t)sizeof(*out_document);
    out_document->document_id = document_id;
    out_document->root_object_id = root_object_id;
    out_document->revision = 1;
    out_document->schema_id = standalone_string_view(schema_id);
    out_document->json_utf8.data = json_view;
    out_document->json_utf8.size = json_view_size;
    out_document->objects = &out_owner->root_object;
    out_document->object_count = 1;

    json = 0;
    result = 1;

cleanup:
    if (file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(file);
    }
    if (json != 0)
    {
        HeapFree(GetProcessHeap(), 0, json);
    }
    if (!result)
    {
        if (out_owner->path_utf8_allocation != 0)
        {
            HeapFree(
                GetProcessHeap(),
                0,
                out_owner->path_utf8_allocation);
        }
        memset(out_document, 0, sizeof(*out_document));
        memset(out_owner, 0, sizeof(*out_owner));
    }
    return result;
}

static void
standalone_release_documents(Standalone_Host* host)
{
    uint32_t index = 0;

    if (host == 0)
    {
        return;
    }

    for (index = 0; index < 2; index += 1)
    {
        if (host->document_owners[index].json_allocation != 0)
        {
            HeapFree(
                GetProcessHeap(),
                0,
                host->document_owners[index].json_allocation);
        }
        if (host->document_owners[index].path_utf8_allocation != 0)
        {
            HeapFree(
                GetProcessHeap(),
                0,
                host->document_owners[index].path_utf8_allocation);
        }
        memset(
            &host->document_owners[index],
            0,
            sizeof(host->document_owners[index]));
        memset(&host->documents[index], 0, sizeof(host->documents[index]));
    }
    host->document_count = 0;
}

static void
standalone_clear_context_bindings(void* user)
{
    D3D11_Viewport_Host* viewport = (D3D11_Viewport_Host*)user;

    if (viewport != 0)
    {
        d3d11_viewport_host_clear_bindings(viewport, 1);
    }
}

static Module_Loader_Storage
standalone_module_storage(Standalone_Module_Buffers* buffers)
{
    Module_Loader_Storage storage;
    memset(&storage, 0, sizeof(storage));

    if (buffers != 0)
    {
        storage.schemas = buffers->schemas;
        storage.schema_capacity = Standalone_Module_Schema_Capacity;
        storage.fields = buffers->fields;
        storage.field_capacity = Standalone_Module_Field_Capacity;
        storage.strings = buffers->strings;
        storage.string_capacity = Standalone_Module_String_Capacity;
        storage.loaded_path = buffers->loaded_path;
        storage.loaded_path_capacity = Standalone_Path_Capacity;
    }
    return storage;
}

static uint32_t
standalone_initialize_module_slots(Standalone_Host* host)
{
    Module_Loader_Storage active_storage;
    Module_Loader_Storage candidate_storage;
    Module_Loader_Error error;
    Module_Loader_Result result = Module_Loader_Result_Ok;

    if (host == 0)
    {
        return 0;
    }

    memset(&error, 0, sizeof(error));
    active_storage =
        standalone_module_storage(&host->active_module_buffers);
    candidate_storage =
        standalone_module_storage(&host->candidate_module_buffers);

    result = module_loader_slot_init(
        &host->active_module,
        &active_storage,
        &error);
    if (result != Module_Loader_Result_Ok)
    {
        standalone_print_module_error(
            "active module-slot initialization",
            result,
            &error);
        return 0;
    }

    result = module_loader_slot_init(
        &host->candidate_module,
        &candidate_storage,
        &error);
    if (result != Module_Loader_Result_Ok)
    {
        standalone_print_module_error(
            "candidate module-slot initialization",
            result,
            &error);
        return 0;
    }
    return 1;
}

static uint32_t
standalone_load_game_module(
    Standalone_Host* host,
    const wchar_t* canonical_path,
    const wchar_t* hot_path)
{
    Module_Loader_Prepare_Request request;
    Module_Loader_Error error;
    Module_Loader_Result result = Module_Loader_Result_Ok;

    if (host == 0 || canonical_path == 0 || hot_path == 0)
    {
        return 0;
    }

    memset(&request, 0, sizeof(request));
    memset(&error, 0, sizeof(error));
    request.struct_size = (uint32_t)sizeof(request);
    request.build_generation = 1;
    request.canonical_dll_path = canonical_path;
    request.unique_hot_dll_path = hot_path;
    request.create_desc.struct_size =
        (uint32_t)sizeof(request.create_desc);
    request.create_desc.host_api_version = Host_API_Version;
    request.create_desc.d3d11_device = host->viewport.device;
    request.create_desc.d3d11_immediate_context =
        host->viewport.immediate_context;
    request.create_desc.host_user = host;
    request.create_desc.host_log = standalone_host_log;
    request.create_desc.project_root.data = host->project_root;
    request.create_desc.project_root.size =
        (uint64_t)host->project_root_size;
    request.documents = host->documents;
    request.document_count = host->document_count;
    request.clear_context_bindings =
        standalone_clear_context_bindings;
    request.clear_context_user = &host->viewport;

    result = module_loader_prepare(
        &host->candidate_module,
        &request,
        &error);
    if (result != Module_Loader_Result_Ok)
    {
        standalone_print_module_error(
            "game-module candidate preparation",
            result,
            &error);
        return 0;
    }

    d3d11_viewport_host_clear_bindings(&host->viewport, 0);
    result = module_loader_commit(
        &host->active_module,
        &host->candidate_module,
        &error);
    if (result != Module_Loader_Result_Ok)
    {
        standalone_print_module_error(
            "game-module candidate commit",
            result,
            &error);
        return 0;
    }

    frame_scheduler_reset_backlog(&host->scheduler);
    host->clock_reset_requested = 1;
    return module_loader_slot_is_callable(&host->active_module);
}

static uint32_t
standalone_release_module_slot(
    Module_Loader_Slot* slot,
    D3D11_Viewport_Host* viewport)
{
    Module_Loader_Error error;
    Module_Loader_Result result = Module_Loader_Result_Ok;
    uint32_t success = 1;

    memset(&error, 0, sizeof(error));
    if (slot == 0)
    {
        return 0;
    }

    if (slot->state == Module_Loader_Slot_Prepared ||
        slot->state == Module_Loader_Slot_Active ||
        slot->state == Module_Loader_Slot_Unload_Pending)
    {
        result = module_loader_release(
            slot,
            standalone_clear_context_bindings,
            viewport,
            &error);
        if (result != Module_Loader_Result_Ok)
        {
            standalone_print_module_error(
                "game-module release",
                result,
                &error);
            success = 0;
        }
    }

    if (slot->state == Module_Loader_Slot_Inactive_Copy)
    {
        memset(&error, 0, sizeof(error));
        result = module_loader_delete_inactive_copy(slot, &error);
        if (result != Module_Loader_Result_Ok)
        {
            standalone_print_module_error(
                "hot-copy deletion",
                result,
                &error);
            success = 0;
        }
    }
    return success;
}

Game_Key
standalone_translate_win32_key(
    uint32_t virtual_key,
    uint32_t scan_code,
    uint32_t is_extended)
{
    switch (virtual_key)
    {
        case 'A': return Game_Key_A;
        case 'B': return Game_Key_B;
        case 'C': return Game_Key_C;
        case 'D': return Game_Key_D;
        case 'E': return Game_Key_E;
        case 'F': return Game_Key_F;
        case 'G': return Game_Key_G;
        case 'H': return Game_Key_H;
        case 'I': return Game_Key_I;
        case 'J': return Game_Key_J;
        case 'K': return Game_Key_K;
        case 'L': return Game_Key_L;
        case 'M': return Game_Key_M;
        case 'N': return Game_Key_N;
        case 'O': return Game_Key_O;
        case 'P': return Game_Key_P;
        case 'Q': return Game_Key_Q;
        case 'R': return Game_Key_R;
        case 'S': return Game_Key_S;
        case 'T': return Game_Key_T;
        case 'U': return Game_Key_U;
        case 'V': return Game_Key_V;
        case 'W': return Game_Key_W;
        case 'X': return Game_Key_X;
        case 'Y': return Game_Key_Y;
        case 'Z': return Game_Key_Z;

        case '0': return Game_Key_Digit_0;
        case '1': return Game_Key_Digit_1;
        case '2': return Game_Key_Digit_2;
        case '3': return Game_Key_Digit_3;
        case '4': return Game_Key_Digit_4;
        case '5': return Game_Key_Digit_5;
        case '6': return Game_Key_Digit_6;
        case '7': return Game_Key_Digit_7;
        case '8': return Game_Key_Digit_8;
        case '9': return Game_Key_Digit_9;

        case VK_ESCAPE: return Game_Key_Escape;
        case VK_RETURN:
            return is_extended
                ? Game_Key_Keypad_Enter
                : Game_Key_Enter;
        case VK_TAB: return Game_Key_Tab;
        case VK_BACK: return Game_Key_Backspace;
        case VK_SPACE: return Game_Key_Space;

        case VK_LEFT:
            return !is_extended ? Game_Key_Keypad_4 : Game_Key_Left;
        case VK_RIGHT:
            return !is_extended ? Game_Key_Keypad_6 : Game_Key_Right;
        case VK_UP:
            return !is_extended ? Game_Key_Keypad_8 : Game_Key_Up;
        case VK_DOWN:
            return !is_extended ? Game_Key_Keypad_2 : Game_Key_Down;
        case VK_INSERT:
            return !is_extended ? Game_Key_Keypad_0 : Game_Key_Insert;
        case VK_DELETE:
            return !is_extended
                ? Game_Key_Keypad_Decimal
                : Game_Key_Delete;
        case VK_HOME:
            return !is_extended ? Game_Key_Keypad_7 : Game_Key_Home;
        case VK_END:
            return !is_extended ? Game_Key_Keypad_1 : Game_Key_End;
        case VK_PRIOR:
            return !is_extended
                ? Game_Key_Keypad_9
                : Game_Key_Page_Up;
        case VK_NEXT:
            return !is_extended
                ? Game_Key_Keypad_3
                : Game_Key_Page_Down;
        case VK_CLEAR:
            return !is_extended ? Game_Key_Keypad_5 : Game_Key_Unknown;

        case VK_LSHIFT: return Game_Key_Left_Shift;
        case VK_RSHIFT: return Game_Key_Right_Shift;
        case VK_SHIFT:
        {
            UINT mapped_key = MapVirtualKeyW(
                scan_code,
                MAPVK_VSC_TO_VK_EX);
            if (mapped_key == VK_LSHIFT)
            {
                return Game_Key_Left_Shift;
            }
            if (mapped_key == VK_RSHIFT)
            {
                return Game_Key_Right_Shift;
            }
            return Game_Key_Unknown;
        }
        case VK_LCONTROL: return Game_Key_Left_Control;
        case VK_RCONTROL: return Game_Key_Right_Control;
        case VK_CONTROL:
            return is_extended
                ? Game_Key_Right_Control
                : Game_Key_Left_Control;
        case VK_LMENU: return Game_Key_Left_Alt;
        case VK_RMENU: return Game_Key_Right_Alt;
        case VK_MENU:
            return is_extended
                ? Game_Key_Right_Alt
                : Game_Key_Left_Alt;
        case VK_LWIN: return Game_Key_Left_Super;
        case VK_RWIN: return Game_Key_Right_Super;

        case VK_CAPITAL: return Game_Key_Caps_Lock;
        case VK_NUMLOCK: return Game_Key_Num_Lock;
        case VK_SCROLL: return Game_Key_Scroll_Lock;
        case VK_SNAPSHOT: return Game_Key_Print_Screen;
        case VK_PAUSE: return Game_Key_Pause;

        case VK_F1: return Game_Key_F1;
        case VK_F2: return Game_Key_F2;
        case VK_F3: return Game_Key_F3;
        case VK_F4: return Game_Key_F4;
        case VK_F5: return Game_Key_F5;
        case VK_F6: return Game_Key_F6;
        case VK_F7: return Game_Key_F7;
        case VK_F8: return Game_Key_F8;
        case VK_F9: return Game_Key_F9;
        case VK_F10: return Game_Key_F10;
        case VK_F11: return Game_Key_F11;
        case VK_F12: return Game_Key_F12;
        case VK_F13: return Game_Key_F13;
        case VK_F14: return Game_Key_F14;
        case VK_F15: return Game_Key_F15;
        case VK_F16: return Game_Key_F16;
        case VK_F17: return Game_Key_F17;
        case VK_F18: return Game_Key_F18;
        case VK_F19: return Game_Key_F19;
        case VK_F20: return Game_Key_F20;
        case VK_F21: return Game_Key_F21;
        case VK_F22: return Game_Key_F22;
        case VK_F23: return Game_Key_F23;
        case VK_F24: return Game_Key_F24;

        case VK_OEM_7: return Game_Key_Apostrophe;
        case VK_OEM_COMMA: return Game_Key_Comma;
        case VK_OEM_MINUS: return Game_Key_Minus;
        case VK_OEM_PERIOD: return Game_Key_Period;
        case VK_OEM_2: return Game_Key_Slash;
        case VK_OEM_1: return Game_Key_Semicolon;
        case VK_OEM_PLUS: return Game_Key_Equal;
        case VK_OEM_4: return Game_Key_Left_Bracket;
        case VK_OEM_5: return Game_Key_Backslash;
        case VK_OEM_6: return Game_Key_Right_Bracket;
        case VK_OEM_3: return Game_Key_Grave_Accent;

        case VK_NUMPAD0: return Game_Key_Keypad_0;
        case VK_NUMPAD1: return Game_Key_Keypad_1;
        case VK_NUMPAD2: return Game_Key_Keypad_2;
        case VK_NUMPAD3: return Game_Key_Keypad_3;
        case VK_NUMPAD4: return Game_Key_Keypad_4;
        case VK_NUMPAD5: return Game_Key_Keypad_5;
        case VK_NUMPAD6: return Game_Key_Keypad_6;
        case VK_NUMPAD7: return Game_Key_Keypad_7;
        case VK_NUMPAD8: return Game_Key_Keypad_8;
        case VK_NUMPAD9: return Game_Key_Keypad_9;
        case VK_DECIMAL: return Game_Key_Keypad_Decimal;
        case VK_SEPARATOR: return Game_Key_Keypad_Decimal;
        case VK_DIVIDE: return Game_Key_Keypad_Divide;
        case VK_MULTIPLY: return Game_Key_Keypad_Multiply;
        case VK_SUBTRACT: return Game_Key_Keypad_Subtract;
        case VK_ADD: return Game_Key_Keypad_Add;

#if defined(VK_OEM_NEC_EQUAL)
        case VK_OEM_NEC_EQUAL: return Game_Key_Keypad_Equal;
#endif

        default: return Game_Key_Unknown;
    }
}

static Game_Mouse_Button
standalone_translate_mouse_button(UINT message, WPARAM word_parameter)
{
    switch (message)
    {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            return Game_Mouse_Button_Left;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return Game_Mouse_Button_Right;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return Game_Mouse_Button_Middle;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            return GET_XBUTTON_WPARAM(word_parameter) == XBUTTON1
                ? Game_Mouse_Button_Auxiliary_1
                : Game_Mouse_Button_Auxiliary_2;
        default:
            return Game_Mouse_Button_Count;
    }
}

static uint32_t
standalone_mouse_button_mask(Game_Mouse_Button button)
{
    if (button >= Game_Mouse_Button_Count)
    {
        return 0;
    }
    return UINT32_C(1) << button;
}

static LRESULT CALLBACK
standalone_window_procedure(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter)
{
    Standalone_Host* host =
        (Standalone_Host*)GetWindowLongPtrW(window, GWLP_USERDATA);

    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW* create =
            (const CREATESTRUCTW*)long_parameter;
        host = (Standalone_Host*)create->lpCreateParams;
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            (LONG_PTR)host);
    }

    if (host == 0)
    {
        return DefWindowProcW(
            window,
            message,
            word_parameter,
            long_parameter);
    }

    switch (message)
    {
        case WM_CLOSE:
            host->running = 0;
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            host->running = 0;
            host->window = 0;
            PostQuitMessage(0);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT paint;
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }

        case WM_SIZE:
            if (host->viewport_initialized)
            {
                D3D11_Viewport_Result resize_result =
                    d3d11_viewport_host_resize(
                        &host->viewport,
                        (uint32_t)LOWORD(long_parameter),
                        (uint32_t)HIWORD(long_parameter));
                if (resize_result != D3D11_Viewport_Result_Ok &&
                    resize_result != D3D11_Viewport_Result_Skipped)
                {
                    host->fatal_window_error = resize_result;
                    host->running = 0;
                }
            }
            return 0;

        case WM_ENTERSIZEMOVE:
            frame_scheduler_reset_backlog(&host->scheduler);
            host->clock_reset_requested = 1;
            return 0;

        case WM_EXITSIZEMOVE:
            frame_scheduler_reset_backlog(&host->scheduler);
            host->clock_reset_requested = 1;
            return 0;

        case WM_SETFOCUS:
            input_latch_focus_gained(&host->input);
            host->pointer_position_valid = 0;
            frame_scheduler_reset_backlog(&host->scheduler);
            host->clock_reset_requested = 1;
            return 0;

        case WM_KILLFOCUS:
            input_latch_focus_lost(&host->input);
            host->captured_mouse_buttons = 0;
            if (GetCapture() == window)
            {
                ReleaseCapture();
            }
            frame_scheduler_reset_backlog(&host->scheduler);
            host->clock_reset_requested = 1;
            return 0;

        case WM_CAPTURECHANGED:
        {
            uint32_t button = 0;
            for (button = 0;
                 button < Game_Mouse_Button_Count;
                 button += 1)
            {
                input_latch_set_mouse_button(
                    &host->input,
                    button,
                    0);
            }
            host->captured_mouse_buttons = 0;
            return 0;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
        {
            uint32_t scan_code =
                ((uint32_t)(uintptr_t)long_parameter >> 16) & 0xffu;
            uint32_t is_extended =
                ((uint32_t)(uintptr_t)long_parameter >> 24) & 1u;
            uint32_t is_down =
                message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            Game_Key key = Game_Key_Unknown;

            if (message == WM_SYSKEYDOWN &&
                word_parameter == VK_F4 &&
                (((uint32_t)(uintptr_t)long_parameter >> 29) & 1u))
            {
                SendMessageW(window, WM_CLOSE, 0, 0);
                return 0;
            }

            key = standalone_translate_win32_key(
                (uint32_t)word_parameter,
                scan_code,
                is_extended);
            if (key != Game_Key_Unknown)
            {
                input_latch_set_key(&host->input, key, is_down);
                return 0;
            }
            break;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP:
        {
            Game_Mouse_Button button =
                standalone_translate_mouse_button(
                    message,
                    word_parameter);
            uint32_t is_down =
                message == WM_LBUTTONDOWN ||
                message == WM_RBUTTONDOWN ||
                message == WM_MBUTTONDOWN ||
                message == WM_XBUTTONDOWN;
            uint32_t mask = standalone_mouse_button_mask(button);

            input_latch_set_mouse_button(
                &host->input,
                button,
                is_down);
            if (is_down)
            {
                host->captured_mouse_buttons |= mask;
                SetCapture(window);
                SetFocus(window);
            }
            else
            {
                host->captured_mouse_buttons &= ~mask;
                if (host->captured_mouse_buttons == 0 &&
                    GetCapture() == window)
                {
                    ReleaseCapture();
                }
            }
            return message == WM_XBUTTONDOWN ||
                message == WM_XBUTTONUP
                    ? TRUE
                    : 0;
        }

        case WM_MOUSEMOVE:
        {
            int32_t x = (int32_t)GET_X_LPARAM(long_parameter);
            int32_t y = (int32_t)GET_Y_LPARAM(long_parameter);
            if (host->pointer_position_valid)
            {
                input_latch_add_pointer_delta(
                    &host->input,
                    x - host->input.pointer_x_pixels,
                    y - host->input.pointer_y_pixels);
            }
            input_latch_set_pointer_position(&host->input, x, y);
            host->pointer_position_valid = 1;
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            POINT point;
            point.x = GET_X_LPARAM(long_parameter);
            point.y = GET_Y_LPARAM(long_parameter);
            if (ScreenToClient(window, &point))
            {
                input_latch_set_pointer_position(
                    &host->input,
                    point.x,
                    point.y);
                host->pointer_position_valid = 1;
            }
            input_latch_add_wheel_delta(
                &host->input,
                (float)GET_WHEEL_DELTA_WPARAM(word_parameter) /
                    (float)WHEEL_DELTA);
            return 0;
        }

        default:
            break;
    }

    return DefWindowProcW(
        window,
        message,
        word_parameter,
        long_parameter);
}

static uint32_t
standalone_create_window(
    Standalone_Host* host,
    uint32_t width,
    uint32_t height,
    uint32_t hidden)
{
    WNDCLASSEXW window_class;
    RECT window_rectangle;
    DWORD style = WS_OVERLAPPEDWINDOW;
    int window_width = 0;
    int window_height = 0;

    if (host == 0 || width == 0 || height == 0)
    {
        return 0;
    }

    memset(&window_class, 0, sizeof(window_class));
    memset(&window_rectangle, 0, sizeof(window_rectangle));
    host->instance = GetModuleHandleW(0);

    window_class.cbSize = (UINT)sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    window_class.lpfnWndProc = standalone_window_procedure;
    window_class.hInstance = host->instance;
    window_class.hCursor = LoadCursorW(
        0,
        MAKEINTRESOURCEW(32512));
    window_class.lpszClassName = Standalone_Window_Class_Name;
    host->window_class = RegisterClassExW(&window_class);
    if (host->window_class == 0)
    {
        fwprintf(
            stderr,
            L"RegisterClassExW failed (Win32 %lu).\n",
            GetLastError());
        return 0;
    }

    window_rectangle.right = (LONG)width;
    window_rectangle.bottom = (LONG)height;
    if (!AdjustWindowRectEx(
            &window_rectangle,
            style,
            FALSE,
            0))
    {
        fwprintf(
            stderr,
            L"AdjustWindowRectEx failed (Win32 %lu).\n",
            GetLastError());
        return 0;
    }
    window_width =
        (int)(window_rectangle.right - window_rectangle.left);
    window_height =
        (int)(window_rectangle.bottom - window_rectangle.top);

    host->window = CreateWindowExW(
        0,
        Standalone_Window_Class_Name,
        L"Kalam",
        style,
        hidden ? 0 : CW_USEDEFAULT,
        hidden ? 0 : CW_USEDEFAULT,
        window_width,
        window_height,
        0,
        0,
        host->instance,
        host);
    if (host->window == 0)
    {
        fwprintf(
            stderr,
            L"CreateWindowExW failed (Win32 %lu).\n",
            GetLastError());
        return 0;
    }

    if (!hidden)
    {
        ShowWindow(host->window, SW_SHOWDEFAULT);
        UpdateWindow(host->window);
        SetFocus(host->window);
    }
    return 1;
}

static uint32_t
standalone_pump_messages(Standalone_Host* host)
{
    MSG message;

    memset(&message, 0, sizeof(message));
    if (host == 0)
    {
        return 0;
    }

    while (PeekMessageW(&message, 0, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            host->running = 0;
            return 0;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return host->running;
}

static uint32_t
standalone_run_frames(
    Standalone_Host* host,
    const Standalone_Options* options)
{
    LARGE_INTEGER performance_frequency;
    LARGE_INTEGER previous_counter;
    LARGE_INTEGER current_counter;
    uint64_t presentation_frame_count = 0;

    memset(&performance_frequency, 0, sizeof(performance_frequency));
    memset(&previous_counter, 0, sizeof(previous_counter));
    memset(&current_counter, 0, sizeof(current_counter));
    if (host == 0 || options == 0 ||
        !module_loader_slot_is_callable(&host->active_module))
    {
        return 0;
    }

    if (!QueryPerformanceFrequency(&performance_frequency) ||
        performance_frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&previous_counter))
    {
        fputs("The Win32 performance counter is unavailable.\n", stderr);
        return 0;
    }

    host->running = 1;
    while (host->running)
    {
        Frame_Schedule schedule;
        double measured_delta_seconds = 0.0;
        uint32_t update_index = 0;
        D3D11_Viewport_Result viewport_result =
            D3D11_Viewport_Result_Ok;
        Host_Render_Target render_target;
        Game_Error game_error;
        Game_Result game_result = Game_Result_Ok;
        const float clear_color[4] =
            {0.025f, 0.035f, 0.055f, 1.0f};

        memset(&schedule, 0, sizeof(schedule));
        memset(&render_target, 0, sizeof(render_target));
        memset(&game_error, 0, sizeof(game_error));

        standalone_pump_messages(host);
        if (!host->running)
        {
            break;
        }
        if (host->fatal_window_error != 0)
        {
            fprintf(
                stderr,
                "Viewport resize failed: %s.\n",
                d3d11_viewport_result_string(
                    host->fatal_window_error));
            return 0;
        }

        if (!QueryPerformanceCounter(&current_counter))
        {
            fputs("QueryPerformanceCounter failed.\n", stderr);
            return 0;
        }

        if (host->clock_reset_requested)
        {
            previous_counter = current_counter;
            host->clock_reset_requested = 0;
        }
        else
        {
            measured_delta_seconds =
                (double)(
                    current_counter.QuadPart -
                    previous_counter.QuadPart) /
                (double)performance_frequency.QuadPart;
            previous_counter = current_counter;
        }

        /*
         * A short hidden test must prove fixed_update rather than depending on
         * how quickly the CI worker loops. Production always uses QPC.
         */
        if (options->smoke)
        {
            measured_delta_seconds =
                frame_scheduler_fixed_delta_seconds();
        }

        schedule = frame_scheduler_advance(
            &host->scheduler,
            measured_delta_seconds);
        for (update_index = 0;
             update_index < schedule.update_count;
             update_index += 1)
        {
            Game_Input input;
            memset(&input, 0, sizeof(input));
            input_latch_consume(
                &host->input,
                schedule.first_fixed_tick_index + update_index,
                &input);
            host->active_module.api.fixed_update(
                host->active_module.game,
                &input,
                schedule.fixed_delta_time);
            host->fixed_update_count += 1;
        }

        viewport_result = d3d11_viewport_host_clear(
            &host->viewport,
            clear_color,
            1.0f,
            0);
        if (viewport_result != D3D11_Viewport_Result_Ok &&
            viewport_result != D3D11_Viewport_Result_Skipped)
        {
            fprintf(
                stderr,
                "Viewport clear failed: %s (HRESULT 0x%08x).\n",
                d3d11_viewport_result_string(viewport_result),
                (unsigned int)(uint32_t)host->viewport.last_hresult);
            return 0;
        }

        if (viewport_result == D3D11_Viewport_Result_Ok &&
            d3d11_viewport_host_get_render_target(
                &host->viewport,
                &render_target))
        {
            game_result = host->active_module.api.render(
                host->active_module.game,
                &render_target,
                schedule.interpolation_alpha,
                &game_error);
            if (game_result != Game_Result_Ok)
            {
                standalone_print_game_error(
                    "Game_API.render",
                    game_result,
                    &game_error);
                return 0;
            }
            host->rendered_frame_count += 1;

            viewport_result = d3d11_viewport_host_present(
                &host->viewport,
                options->smoke ? 0u : 1u);
            host->present_call_count += 1;
            if (viewport_result != D3D11_Viewport_Result_Ok &&
                viewport_result != D3D11_Viewport_Result_Skipped)
            {
                fprintf(
                    stderr,
                    "Viewport present failed: %s (HRESULT 0x%08x).\n",
                    d3d11_viewport_result_string(viewport_result),
                    (unsigned int)(uint32_t)host->viewport.last_hresult);
                return 0;
            }
        }

        presentation_frame_count += 1;
        if (options->frame_limit_enabled &&
            presentation_frame_count >= options->frame_limit)
        {
            host->running = 0;
        }
        else if (host->viewport.occluded ||
                 !host->viewport.target_available)
        {
            Sleep(10);
        }
    }

    if (options->smoke &&
        (host->fixed_update_count == 0 ||
         host->rendered_frame_count == 0 ||
         host->present_call_count == 0))
    {
        fprintf(
            stderr,
            "Smoke run was incomplete: ticks=%llu renders=%llu presents=%llu.\n",
            (unsigned long long)host->fixed_update_count,
            (unsigned long long)host->rendered_frame_count,
            (unsigned long long)host->present_call_count);
        return 0;
    }
    return 1;
}

int
standalone_run(int argument_count, wchar_t** arguments)
{
    Standalone_Options options;
    Standalone_Parse_Result parse_result = Standalone_Parse_Ok;
    Standalone_Host* host = 0;
    wchar_t canonical_module_path[Standalone_Path_Capacity];
    wchar_t hot_module_path[Standalone_Path_Capacity];
    D3D11_Viewport_Create_Desc viewport_desc;
    D3D11_Viewport_Result viewport_result =
        D3D11_Viewport_Result_Ok;
    RECT client_rectangle;
    uint32_t initial_width = Standalone_Default_Width;
    uint32_t initial_height = Standalone_Default_Height;
    uint32_t clean_shutdown = 1;
    int exit_code = 1;

    memset(&options, 0, sizeof(options));
    memset(canonical_module_path, 0, sizeof(canonical_module_path));
    memset(hot_module_path, 0, sizeof(hot_module_path));
    memset(&viewport_desc, 0, sizeof(viewport_desc));
    memset(&client_rectangle, 0, sizeof(client_rectangle));

    parse_result = standalone_parse_options(
        argument_count,
        arguments,
        &options);
    if (parse_result == Standalone_Parse_Help)
    {
        standalone_print_usage();
        return 0;
    }
    if (parse_result != Standalone_Parse_Ok)
    {
        standalone_print_usage();
        return 2;
    }

    host = (Standalone_Host*)HeapAlloc(
        GetProcessHeap(),
        HEAP_ZERO_MEMORY,
        sizeof(*host));
    if (host == 0)
    {
        fputs("Could not allocate standalone-host state.\n", stderr);
        return 1;
    }
    frame_scheduler_init(&host->scheduler);
    input_latch_init(&host->input);

    /* res/ is staged beside the executable; that directory is the root. */
    {
        DWORD length = GetModuleFileNameA(
            0,
            host->project_root,
            (DWORD)sizeof(host->project_root));
        if (length == 0 || length >= sizeof(host->project_root))
        {
            fputs("Could not resolve the executable directory.\n", stderr);
            HeapFree(GetProcessHeap(), 0, host);
            return 1;
        }
        while (length > 0 &&
               host->project_root[length - 1] != '\\' &&
               host->project_root[length - 1] != '/')
        {
            --length;
        }
        while (length > 1 &&
               (host->project_root[length - 1] == '\\' ||
                host->project_root[length - 1] == '/'))
        {
            --length;
        }
        host->project_root[length] = 0;
        host->project_root_size = (uint32_t)length;
    }

    if (options.smoke)
    {
        initial_width = Standalone_Smoke_Width;
        initial_height = Standalone_Smoke_Height;
    }
    if (!standalone_create_window(
            host,
            initial_width,
            initial_height,
            options.smoke))
    {
        goto cleanup;
    }

    if (!GetClientRect(host->window, &client_rectangle) ||
        client_rectangle.right <= client_rectangle.left ||
        client_rectangle.bottom <= client_rectangle.top)
    {
        fwprintf(
            stderr,
            L"Could not determine the standalone client size (Win32 %lu).\n",
            GetLastError());
        goto cleanup;
    }

    viewport_desc.struct_size = (uint32_t)sizeof(viewport_desc);
    viewport_desc.window_handle = host->window;
    viewport_desc.width =
        (uint32_t)(client_rectangle.right - client_rectangle.left);
    viewport_desc.height =
        (uint32_t)(client_rectangle.bottom - client_rectangle.top);
    viewport_desc.driver = options.warp
        ? D3D11_Viewport_Driver_Warp
        : D3D11_Viewport_Driver_Hardware;

    viewport_result = d3d11_viewport_host_create(
        &host->viewport,
        &viewport_desc);
    if (viewport_result != D3D11_Viewport_Result_Ok)
    {
        fprintf(
            stderr,
            "D3D11 viewport creation failed: %s (HRESULT 0x%08x).\n",
            d3d11_viewport_result_string(viewport_result),
            (unsigned int)(uint32_t)host->viewport.last_hresult);
        goto cleanup;
    }
    host->viewport_initialized = 1;

    if (options.scene_path != 0)
    {
        if (!standalone_read_document(
                options.scene_path,
                Standalone_Scene_Schema,
                UINT64_C(0x1001),
                UINT64_C(0x1002),
                &host->documents[host->document_count],
                &host->document_owners[host->document_count]))
        {
            goto cleanup;
        }
        host->document_count += 1;
    }
    if (options.post_process_path != 0)
    {
        if (!standalone_read_document(
                options.post_process_path,
                Standalone_Post_Process_Schema,
                UINT64_C(0x2001),
                UINT64_C(0x2002),
                &host->documents[host->document_count],
                &host->document_owners[host->document_count]))
        {
            goto cleanup;
        }
        host->document_count += 1;
    }

    if (!standalone_initialize_module_slots(host))
    {
        goto cleanup;
    }
    if (!standalone_make_hot_copy_path(
            options.module_path,
            options.hot_directory,
            canonical_module_path,
            hot_module_path))
    {
        fwprintf(
            stderr,
            L"Could not resolve the canonical module or unique hot-copy path (Win32 %lu).\n",
            GetLastError());
        goto cleanup;
    }
    if (!standalone_load_game_module(
            host,
            canonical_module_path,
            hot_module_path))
    {
        goto cleanup;
    }

    if (!standalone_run_frames(host, &options))
    {
        goto cleanup;
    }
    exit_code = 0;

cleanup:
    if (host != 0)
    {
        /*
         * The module releases all module-owned GPU resources before the stable
         * host tears down its device/context. Each copied DLL is deleted only
         * after FreeLibrary has completed.
         */
        if (!standalone_release_module_slot(
                &host->active_module,
                &host->viewport))
        {
            clean_shutdown = 0;
        }
        if (!standalone_release_module_slot(
                &host->candidate_module,
                &host->viewport))
        {
            clean_shutdown = 0;
        }

        standalone_release_documents(host);

        if (host->viewport_initialized)
        {
            d3d11_viewport_host_destroy(&host->viewport);
            host->viewport_initialized = 0;
        }
        if (host->window != 0)
        {
            if (!DestroyWindow(host->window))
            {
                clean_shutdown = 0;
            }
            host->window = 0;
        }
        if (host->window_class != 0)
        {
            if (!UnregisterClassW(
                    Standalone_Window_Class_Name,
                    host->instance))
            {
                clean_shutdown = 0;
            }
            host->window_class = 0;
        }

        if (options.smoke)
        {
            fprintf(
                stdout,
                "standalone smoke: ticks=%llu renders=%llu presents=%llu teardown=%s\n",
                (unsigned long long)host->fixed_update_count,
                (unsigned long long)host->rendered_frame_count,
                (unsigned long long)host->present_call_count,
                clean_shutdown ? "clean" : "failed");
        }
        HeapFree(GetProcessHeap(), 0, host);
    }

    if (!clean_shutdown)
    {
        exit_code = 1;
    }
    return exit_code;
}
