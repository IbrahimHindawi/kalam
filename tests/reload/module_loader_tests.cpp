#include "module_loader.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

enum
{
    Reload_Test_Schema_Capacity = 4,
    Reload_Test_Field_Capacity = 16,
    Reload_Test_String_Capacity = 4096,
    Reload_Test_Path_Capacity = 32768
};

typedef struct Reload_Test_Backing
{
    Author_Schema_View schemas[Reload_Test_Schema_Capacity];
    Author_Field_Desc fields[Reload_Test_Field_Capacity];
    char strings[Reload_Test_String_Capacity];
    wchar_t path[Reload_Test_Path_Capacity];
} Reload_Test_Backing;

typedef struct Reload_Test_Context
{
    uint32_t failure_count;
    uint32_t clear_context_count;
    uint32_t hot_path_counter;
    uint64_t next_generation;

    const wchar_t* hot_directory;
    const Author_Document_State* document;

    Module_Loader_Slot active;
    Module_Loader_Slot candidate;
    Reload_Test_Backing active_backing;
    Reload_Test_Backing candidate_backing;
} Reload_Test_Context;

static String_View
reload_test_view(const char* text)
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

static uint32_t
reload_test_string_equals(String_View value, const char* expected)
{
    String_View expected_view = reload_test_view(expected);
    return value.data != 0 &&
        value.size == expected_view.size &&
        memcmp(value.data, expected_view.data, (size_t)value.size) == 0;
}

static void
reload_test_check(
    Reload_Test_Context* context,
    uint32_t condition,
    const char* message)
{
    if (condition == 0)
    {
        context->failure_count += 1;
        fprintf(stderr, "FAIL: %s\n", message);
    }
}

static void
reload_test_clear_context(void* user)
{
    Reload_Test_Context* context = (Reload_Test_Context*)user;
    if (context != 0)
    {
        context->clear_context_count += 1;
    }
}

static Module_Loader_Storage
reload_test_storage(Reload_Test_Backing* backing)
{
    Module_Loader_Storage result;
    memset(&result, 0, sizeof(result));
    result.schemas = backing->schemas;
    result.schema_capacity = Reload_Test_Schema_Capacity;
    result.fields = backing->fields;
    result.field_capacity = Reload_Test_Field_Capacity;
    result.strings = backing->strings;
    result.string_capacity = Reload_Test_String_Capacity;
    result.loaded_path = backing->path;
    result.loaded_path_capacity = Reload_Test_Path_Capacity;
    return result;
}

static uint32_t
reload_test_pointer_in_bytes(
    const void* pointer,
    const void* storage,
    uint64_t storage_size)
{
    const uintptr_t address = (uintptr_t)pointer;
    const uintptr_t first = (uintptr_t)storage;
    return pointer != 0 &&
        address >= first &&
        address - first < storage_size;
}

static uint32_t
reload_test_api_is_zero(const Game_API* api)
{
    Game_API zero;
    memset(&zero, 0, sizeof(zero));
    return api != 0 && memcmp(api, &zero, sizeof(zero)) == 0;
}

static void
reload_test_check_schema_copy(
    Reload_Test_Context* context,
    const Module_Loader_Slot* slot)
{
    const Author_Schema_View* schema = 0;
    const Author_Field_Desc* field = 0;

    reload_test_check(
        context,
        slot->schema_count == 1,
        "candidate must expose one copied schema");
    if (slot->schema_count != 1)
    {
        return;
    }

    schema = &slot->schemas[0];
    reload_test_check(
        context,
        reload_test_string_equals(schema->schema_id, "reload.test/v1"),
        "schema ID must survive module-owned source poisoning");
    reload_test_check(
        context,
        reload_test_pointer_in_bytes(
            schema->schema_id.data,
            slot->storage.strings,
            slot->storage.string_capacity),
        "schema ID must point into host-owned string storage");
    reload_test_check(
        context,
        schema->fields == slot->storage.fields,
        "field table must point into host-owned field storage");
    reload_test_check(
        context,
        schema->field_count == 1,
        "copied schema must retain its field count");
    if (schema->field_count != 1)
    {
        return;
    }

    field = &schema->fields[0];
    reload_test_check(
        context,
        reload_test_string_equals(field->name, "module_marker"),
        "field name must be deep-copied");
    reload_test_check(
        context,
        reload_test_string_equals(field->json_path, "$.module_marker"),
        "field JSON path must be deep-copied");
    reload_test_check(
        context,
        reload_test_string_equals(field->group, "Reload"),
        "field group must be deep-copied");
    reload_test_check(
        context,
        reload_test_pointer_in_bytes(
            field->name.data,
            slot->storage.strings,
            slot->storage.string_capacity) &&
        reload_test_pointer_in_bytes(
            field->json_path.data,
            slot->storage.strings,
            slot->storage.string_capacity) &&
        reload_test_pointer_in_bytes(
            field->group.data,
            slot->storage.strings,
            slot->storage.string_capacity),
        "every nested field string must point into host-owned storage");
}

static void
reload_test_make_hot_path(
    Reload_Test_Context* context,
    wchar_t* destination,
    uint32_t destination_capacity)
{
    const DWORD process_id = GetCurrentProcessId();
    const ULONGLONG tick = GetTickCount64();
    const int result = _snwprintf_s(
        destination,
        destination_capacity,
        _TRUNCATE,
        L"%ls\\game_module_hot_%lu_%llu_%u.dll",
        context->hot_directory,
        (unsigned long)process_id,
        (unsigned long long)tick,
        context->hot_path_counter);

    context->hot_path_counter += 1;
    reload_test_check(
        context,
        result > 0,
        "test hot path must fit the Win32 long-path buffer");
}

static Module_Loader_Prepare_Request
reload_test_request(
    Reload_Test_Context* context,
    const wchar_t* canonical_path,
    const wchar_t* hot_path,
    uint64_t generation)
{
    Module_Loader_Prepare_Request request;
    memset(&request, 0, sizeof(request));
    request.struct_size = sizeof(request);
    request.build_generation = generation;
    request.canonical_dll_path = canonical_path;
    request.unique_hot_dll_path = hot_path;
    request.create_desc.struct_size = sizeof(Game_Create_Desc);
    request.create_desc.host_api_version = Host_API_Version;
    request.documents = context->document;
    request.document_count = 1;
    request.clear_context_bindings = reload_test_clear_context;
    request.clear_context_user = context;
    return request;
}

static float
reload_test_read_marker(
    Reload_Test_Context* context,
    const Module_Loader_Slot* slot)
{
    Author_Value value;
    Game_Error error;
    const Game_Result result = slot->api.read_author_field(
        slot->game,
        context->document->document_id,
        context->document->root_object_id,
        UINT64_C(0x8f759f813fe682b1),
        &value,
        &error);

    reload_test_check(
        context,
        result == Game_Result_Ok,
        "active module marker read must succeed");
    return result == Game_Result_Ok ? value.data.f32[0] : -1.0f;
}

static void
reload_test_delete_candidate_copy(Reload_Test_Context* context)
{
    Module_Loader_Error error;
    wchar_t copied_path[Reload_Test_Path_Capacity];
    const DWORD attributes_before =
        GetFileAttributesW(context->candidate.storage.loaded_path);

    wcscpy_s(
        copied_path,
        Reload_Test_Path_Capacity,
        context->candidate.storage.loaded_path);
    reload_test_check(
        context,
        context->candidate.state == Module_Loader_Slot_Inactive_Copy,
        "failed or superseded candidate must retain an inactive-copy state");
    reload_test_check(
        context,
        attributes_before != INVALID_FILE_ATTRIBUTES,
        "hot copy must remain on disk until explicit deletion");
    reload_test_check(
        context,
        module_loader_delete_inactive_copy(
            &context->candidate,
            &error) == Module_Loader_Result_Ok,
        "explicit inactive-copy deletion must succeed");
    reload_test_check(
        context,
        context->candidate.state == Module_Loader_Slot_Empty,
        "deleted inactive-copy slot must become reusable");
    reload_test_check(
        context,
        GetFileAttributesW(copied_path) == INVALID_FILE_ATTRIBUTES,
        "explicit inactive-copy deletion must remove the copied DLL");
}

static void
reload_test_failed_candidate(
    Reload_Test_Context* context,
    const wchar_t* canonical_path,
    Module_Loader_Result expected_result,
    const char* expected_module_error)
{
    wchar_t hot_path[Reload_Test_Path_Capacity];
    Module_Loader_Prepare_Request request;
    Module_Loader_Error error;
    Module_Loader_Result result = Module_Loader_Result_Ok;
    const uint32_t clear_count_before = context->clear_context_count;

    reload_test_make_hot_path(
        context,
        hot_path,
        Reload_Test_Path_Capacity);
    request = reload_test_request(
        context,
        canonical_path,
        hot_path,
        context->next_generation);
    context->next_generation += 1;

    result = module_loader_prepare(
        &context->candidate,
        &request,
        &error);
    reload_test_check(
        context,
        result == expected_result,
        "candidate must fail at the expected transactional stage");
    reload_test_check(
        context,
        context->candidate.native_library == 0 &&
        context->candidate.game == 0 &&
        reload_test_api_is_zero(&context->candidate.api) != 0 &&
        module_loader_slot_is_callable(&context->candidate) == 0,
        "failed candidate must leave no loaded handle, runtime, or callable API");
    reload_test_check(
        context,
        context->clear_context_count == clear_count_before + 1,
        "failed loaded candidate must clear context bindings before unload");
    reload_test_check(
        context,
        module_loader_slot_is_callable(&context->active) != 0 &&
        reload_test_read_marker(context, &context->active) == 1.0f,
        "every candidate failure must preserve active module A");

    if (expected_module_error != 0)
    {
        reload_test_check(
            context,
            strcmp(error.message, expected_module_error) == 0,
            "module-owned failure text must survive candidate unload");
    }

    reload_test_delete_candidate_copy(context);
}

int
wmain(int argument_count, wchar_t** arguments)
{
    Reload_Test_Context context;
    Module_Loader_Storage active_storage;
    Module_Loader_Storage candidate_storage;
    Module_Loader_Error error;
    Author_Object_Source root;
    Author_Document_State document;
    wchar_t hot_path[Reload_Test_Path_Capacity];
    Module_Loader_Prepare_Request request;
    uint32_t clear_before = 0;

    enum
    {
        Argument_Hot_Directory = 1,
        Argument_Valid_A = 2,
        Argument_Valid_B = 3,
        Argument_Missing_Entry = 4,
        Argument_Invalid_Version = 5,
        Argument_Invalid_Size = 6,
        Argument_Invalid_Pointer = 7,
        Argument_Invalid_Create = 8,
        Argument_Invalid_Schema_Query = 9,
        Argument_Invalid_Schema_Duplicate = 10,
        Argument_Invalid_Schema_Field = 11,
        Argument_Invalid_Apply = 12,
        Required_Argument_Count = 13
    };

    if (argument_count != Required_Argument_Count)
    {
        fwprintf(
            stderr,
            L"expected %d arguments, received %d\n",
            Required_Argument_Count - 1,
            argument_count - 1);
        return 2;
    }

    memset(&context, 0, sizeof(context));
    memset(&root, 0, sizeof(root));
    memset(&document, 0, sizeof(document));

    root.object_id = UINT64_C(0x2001);
    root.source_id = reload_test_view("root");
    root.json_path = reload_test_view("$");

    document.struct_size = sizeof(document);
    document.document_id = UINT64_C(0x1001);
    document.root_object_id = root.object_id;
    document.revision = 73;
    document.schema_id = reload_test_view("reload.test/v1");
    document.project_relative_path = reload_test_view("tests/reload.json");
    document.json_utf8 =
        reload_test_view("{\"$schema\":\"reload.test/v1\",\"module_marker\":1}");
    document.objects = &root;
    document.object_count = 1;

    context.hot_directory = arguments[Argument_Hot_Directory];
    context.document = &document;
    context.next_generation = 1;

    active_storage = reload_test_storage(&context.active_backing);
    candidate_storage = reload_test_storage(&context.candidate_backing);
    reload_test_check(
        &context,
        module_loader_slot_init(
            &context.active,
            &active_storage,
            &error) == Module_Loader_Result_Ok,
        "active loader slot initialization must succeed");
    reload_test_check(
        &context,
        module_loader_slot_init(
            &context.candidate,
            &candidate_storage,
            &error) == Module_Loader_Result_Ok,
        "candidate loader slot initialization must succeed");

    /* Initial transactional load of A. */
    reload_test_make_hot_path(
        &context,
        hot_path,
        Reload_Test_Path_Capacity);
    request = reload_test_request(
        &context,
        arguments[Argument_Valid_A],
        hot_path,
        context.next_generation);
    context.next_generation += 1;
    reload_test_check(
        &context,
        module_loader_prepare(
            &context.candidate,
            &request,
            &error) == Module_Loader_Result_Ok,
        "valid module A prepare must succeed");
    reload_test_check_schema_copy(&context, &context.candidate);
    reload_test_check(
        &context,
        reload_test_read_marker(&context, &context.candidate) == 1.0f,
        "prepared module A must receive the last-valid snapshot synchronously");
    reload_test_check(
        &context,
        module_loader_commit(
            &context.active,
            &context.candidate,
            &error) == Module_Loader_Result_Ok,
        "initial module A commit must succeed");
    reload_test_check(
        &context,
        context.active.state == Module_Loader_Slot_Active &&
        context.candidate.state == Module_Loader_Slot_Empty,
        "initial commit must publish A and return one empty slot");

    /* Deletion is impossible while the copy is active. */
    reload_test_check(
        &context,
        module_loader_delete_inactive_copy(
            &context.active,
            &error) == Module_Loader_Result_Invalid_State,
        "active copied DLL must be protected from explicit deletion");
    reload_test_check(
        &context,
        GetFileAttributesW(context.active.storage.loaded_path) !=
            INVALID_FILE_ATTRIBUTES,
        "rejected active-copy deletion must leave the DLL on disk");

    reload_test_failed_candidate(
        &context,
        arguments[Argument_Missing_Entry],
        Module_Loader_Result_Entry_Point_Missing,
        0);
    reload_test_failed_candidate(
        &context,
        arguments[Argument_Invalid_Version],
        Module_Loader_Result_API_Version_Mismatch,
        0);
    reload_test_failed_candidate(
        &context,
        arguments[Argument_Invalid_Size],
        Module_Loader_Result_API_Size_Mismatch,
        0);
    reload_test_failed_candidate(
        &context,
        arguments[Argument_Invalid_Pointer],
        Module_Loader_Result_API_Function_Missing,
        0);
    reload_test_failed_candidate(
        &context,
        arguments[Argument_Invalid_Create],
        Module_Loader_Result_Create_Failed,
        "fake candidate create failure");
    reload_test_failed_candidate(
        &context,
        arguments[Argument_Invalid_Schema_Query],
        Module_Loader_Result_Schema_Query_Failed,
        "fake schema query failure");
    reload_test_failed_candidate(
        &context,
        arguments[Argument_Invalid_Schema_Duplicate],
        Module_Loader_Result_Schema_Invalid,
        0);
    reload_test_failed_candidate(
        &context,
        arguments[Argument_Invalid_Schema_Field],
        Module_Loader_Result_Schema_Invalid,
        0);
    reload_test_failed_candidate(
        &context,
        arguments[Argument_Invalid_Apply],
        Module_Loader_Result_Document_Apply_Failed,
        "fake document apply failure");

    /*
     * Prepare B while A remains callable, then let the host perform its
     * frame-boundary/context-clear work before commit.
     */
    reload_test_make_hot_path(
        &context,
        hot_path,
        Reload_Test_Path_Capacity);
    request = reload_test_request(
        &context,
        arguments[Argument_Valid_B],
        hot_path,
        context.next_generation);
    context.next_generation += 1;
    reload_test_check(
        &context,
        module_loader_prepare(
            &context.candidate,
            &request,
            &error) == Module_Loader_Result_Ok,
        "valid module B prepare must succeed");
    reload_test_check(
        &context,
        reload_test_read_marker(&context, &context.active) == 1.0f,
        "module A must remain callable throughout B preparation");
    reload_test_check(
        &context,
        reload_test_read_marker(&context, &context.candidate) == 2.0f,
        "prepared module B must receive the last-valid snapshot");
    reload_test_check_schema_copy(&context, &context.candidate);

    reload_test_clear_context(&context);
    reload_test_check(
        &context,
        module_loader_commit(
            &context.active,
            &context.candidate,
            &error) == Module_Loader_Result_Ok,
        "A to B commit must succeed");
    reload_test_check(
        &context,
        reload_test_read_marker(&context, &context.active) == 2.0f,
        "successful commit must publish module B");
    reload_test_check_schema_copy(&context, &context.active);
    reload_test_check(
        &context,
        context.candidate.state == Module_Loader_Slot_Inactive_Copy,
        "successful replacement must return unloaded A as an inactive copy");
    reload_test_delete_candidate_copy(&context);

    /*
     * A prepared older generation must not replace newer active B. Both remain
     * callable until the caller explicitly discards the stale candidate.
     */
    reload_test_make_hot_path(
        &context,
        hot_path,
        Reload_Test_Path_Capacity);
    request = reload_test_request(
        &context,
        arguments[Argument_Valid_A],
        hot_path,
        1);
    reload_test_check(
        &context,
        module_loader_prepare(
            &context.candidate,
            &request,
            &error) == Module_Loader_Result_Ok,
        "older valid candidate may finish preparation");
    reload_test_check(
        &context,
        module_loader_commit(
            &context.active,
            &context.candidate,
            &error) == Module_Loader_Result_Stale_Generation,
        "stale prepared generation must not replace active B");
    reload_test_check(
        &context,
        reload_test_read_marker(&context, &context.active) == 2.0f &&
        reload_test_read_marker(&context, &context.candidate) == 1.0f,
        "stale-commit rejection must preserve active B and prepared A");
    clear_before = context.clear_context_count;
    reload_test_check(
        &context,
        module_loader_release(
            &context.candidate,
            reload_test_clear_context,
            &context,
            &error) == Module_Loader_Result_Ok,
        "explicit stale-candidate release must succeed");
    reload_test_check(
        &context,
        context.clear_context_count == clear_before + 1,
        "stale-candidate release must clear context bindings before unload");
    reload_test_delete_candidate_copy(&context);

    clear_before = context.clear_context_count;
    reload_test_check(
        &context,
        module_loader_release(
            &context.active,
            reload_test_clear_context,
            &context,
            &error) == Module_Loader_Result_Ok,
        "active B shutdown release must succeed");
    reload_test_check(
        &context,
        context.clear_context_count == clear_before + 1 &&
        context.active.state == Module_Loader_Slot_Inactive_Copy &&
        context.active.native_library == 0 &&
        context.active.game == 0 &&
        reload_test_api_is_zero(&context.active.api) != 0 &&
        context.active.schema_count == 0 &&
        module_loader_slot_is_callable(&context.active) == 0,
        "shutdown must clear bindings and leave no DLL pointers after unload");

    /*
     * The helper operates on the candidate slot, so swap the two empty/inactive
     * values without touching their external backing ownership.
     */
    {
        Module_Loader_Slot temporary = context.candidate;
        context.candidate = context.active;
        context.active = temporary;
        memset(&temporary, 0, sizeof(temporary));
    }
    reload_test_delete_candidate_copy(&context);

    if (context.failure_count != 0)
    {
        fprintf(
            stderr,
            "%u transactional reload checks failed\n",
            context.failure_count);
        return 1;
    }

    printf("transactional module reload checks passed\n");
    return 0;
}
