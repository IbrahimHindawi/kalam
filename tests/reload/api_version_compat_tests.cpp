/*
 * Game_API version compatibility (docs/entity_documents.md section 6).
 *
 * Version differences are struct_size-guarded additions, so a module built
 * against an older Game_API must still load and stay callable under a
 * newer host; only a module newer than the host is rejected. This runs in
 * its own process with its own loader slot so no other reload scenario's
 * state can mask the result.
 */

#include "module_loader.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

enum
{
    Compat_Schema_Capacity = 4,
    Compat_Field_Capacity = 16,
    Compat_String_Capacity = 4096,
    Compat_Path_Capacity = 32768
};

typedef struct Compat_Backing
{
    Author_Schema_View schemas[Compat_Schema_Capacity];
    Author_Field_Desc fields[Compat_Field_Capacity];
    char strings[Compat_String_Capacity];
    wchar_t path[Compat_Path_Capacity];
} Compat_Backing;

static uint32_t compat_failure_count = 0;

static void
compat_check(uint32_t condition, const char* message)
{
    if (condition == 0)
    {
        compat_failure_count += 1;
        fprintf(stderr, "FAIL: %s\n", message);
    }
}

static void
compat_clear_context(void* user)
{
    (void)user;
}

static String_View
compat_view(const char* text)
{
    String_View result;
    uint64_t size = 0;
    while (text != 0 && text[size] != '\0')
    {
        size += 1;
    }
    result.data = text;
    result.size = size;
    return result;
}

/*
 * Loads one module into a freshly initialized slot and reports the
 * prepare result plus the negotiated API version.
 */
static Module_Loader_Result
compat_prepare_once(
    const wchar_t* module_path,
    const wchar_t* hot_directory,
    uint32_t attempt,
    uint32_t* out_api_version,
    uint32_t* out_callable)
{
    Compat_Backing backing;
    Module_Loader_Slot slot;
    Module_Loader_Storage storage;
    Module_Loader_Prepare_Request request;
    Module_Loader_Error error;
    Author_Object_Source root;
    Author_Document_State document;
    wchar_t hot_path[Compat_Path_Capacity];
    Module_Loader_Result result = Module_Loader_Result_Ok;

    memset(&backing, 0, sizeof(backing));
    memset(&slot, 0, sizeof(slot));
    memset(&storage, 0, sizeof(storage));
    memset(&request, 0, sizeof(request));
    memset(&error, 0, sizeof(error));
    memset(&root, 0, sizeof(root));
    memset(&document, 0, sizeof(document));
    *out_api_version = 0;
    *out_callable = 0;

    storage.schemas = backing.schemas;
    storage.schema_capacity = Compat_Schema_Capacity;
    storage.fields = backing.fields;
    storage.field_capacity = Compat_Field_Capacity;
    storage.strings = backing.strings;
    storage.string_capacity = Compat_String_Capacity;
    storage.loaded_path = backing.path;
    storage.loaded_path_capacity = Compat_Path_Capacity;

    if (module_loader_slot_init(&slot, &storage, &error) !=
        Module_Loader_Result_Ok)
    {
        compat_check(0, "loader slot initialization must succeed");
        return Module_Loader_Result_Invalid_State;
    }

    root.object_id = UINT64_C(0x2001);
    root.source_id = compat_view("root");
    root.json_path = compat_view("$");

    document.struct_size = sizeof(document);
    document.document_id = UINT64_C(0x1001);
    document.root_object_id = root.object_id;
    document.revision = 73;
    document.schema_id = compat_view("reload.test/v1");
    document.project_relative_path = compat_view("tests/reload.json");
    document.json_utf8 =
        compat_view("{\"$schema\":\"reload.test/v1\",\"module_marker\":1}");
    document.objects = &root;
    document.object_count = 1;

    _snwprintf_s(
        hot_path,
        Compat_Path_Capacity,
        _TRUNCATE,
        L"%ls\\api_compat_%lu_%u.dll",
        hot_directory,
        (unsigned long)GetCurrentProcessId(),
        attempt);

    request.struct_size = sizeof(request);
    request.build_generation = 1;
    request.canonical_dll_path = module_path;
    request.unique_hot_dll_path = hot_path;
    request.create_desc.struct_size = sizeof(Game_Create_Desc);
    request.create_desc.host_api_version = Host_API_Version;
    request.create_desc.project_root.data = KALAM_TEST_PROJECT_ROOT;
    request.create_desc.project_root.size =
        (uint64_t)(sizeof(KALAM_TEST_PROJECT_ROOT) - 1);
    request.documents = &document;
    request.document_count = 1;
    request.clear_context_bindings = compat_clear_context;
    request.clear_context_user = 0;

    result = module_loader_prepare(&slot, &request, &error);
    if (result == Module_Loader_Result_Ok)
    {
        *out_api_version = slot.api.api_version;
        *out_callable = module_loader_slot_is_callable(&slot);
        memset(&error, 0, sizeof(error));
        (void)module_loader_release(
            &slot,
            compat_clear_context,
            0,
            &error);
    }
    else
    {
        fprintf(
            stderr,
            "prepare stage=%u message=%s\n",
            (unsigned)error.stage,
            error.message);
    }
    return result;
}

int
wmain(int argument_count, wchar_t** arguments)
{
    uint32_t api_version = 0;
    uint32_t callable = 0;
    Module_Loader_Result result = Module_Loader_Result_Ok;

    enum
    {
        Argument_Hot_Directory = 1,
        Argument_Older_Module = 2,
        Argument_Newer_Module = 3,
        Required_Argument_Count = 4
    };

    if (argument_count != Required_Argument_Count)
    {
        fwprintf(
            stderr,
            L"usage: api_version_compat_tests <hot-dir> <older> <newer>\n");
        return 2;
    }

    /* An older module loads, reports its own version, and is callable. */
    result = compat_prepare_once(
        arguments[Argument_Older_Module],
        arguments[Argument_Hot_Directory],
        1,
        &api_version,
        &callable);
    compat_check(
        result == Module_Loader_Result_Ok,
        "an older Game_API version must still prepare");
    compat_check(
        api_version == 1,
        "the older module reports its own API version");
    compat_check(
        callable != 0,
        "the older module is callable");

    /* A module newer than the host is still refused. */
    api_version = 0;
    callable = 0;
    result = compat_prepare_once(
        arguments[Argument_Newer_Module],
        arguments[Argument_Hot_Directory],
        2,
        &api_version,
        &callable);
    compat_check(
        result == Module_Loader_Result_API_Version_Mismatch,
        "a newer Game_API version must be rejected");

    if (compat_failure_count == 0)
    {
        printf("all Game_API version compatibility checks passed\n");
        return 0;
    }
    fprintf(
        stderr,
        "%u Game_API version compatibility checks failed\n",
        compat_failure_count);
    return 1;
}
