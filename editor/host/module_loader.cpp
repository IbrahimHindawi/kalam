#include "module_loader.h"

#if !defined(_WIN32)
#error The Kalam hot-module loader requires Win32.
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <float.h>
#include <string.h>

enum
{
    Module_Loader_Max_Win32_Path = 32768
};

static void
module_loader_zero_error(Module_Loader_Error* error)
{
    if (error != 0)
    {
        memset(error, 0, sizeof(*error));
    }
}

static uint32_t
module_loader_copy_error_text(
    const char* source,
    uint64_t source_size,
    char* destination,
    uint32_t destination_capacity,
    uint32_t* out_size)
{
    uint64_t copy_size = source_size;
    uint32_t was_truncated = 0;

    if (out_size != 0)
    {
        *out_size = 0;
    }
    if (destination == 0 || destination_capacity == 0)
    {
        return source_size != 0;
    }

    destination[0] = '\0';
    if (source == 0)
    {
        return source_size != 0;
    }

    if (copy_size >= destination_capacity)
    {
        copy_size = destination_capacity - 1;
        was_truncated = 1;
    }
    if (copy_size != 0)
    {
        memcpy(destination, source, (size_t)copy_size);
    }
    destination[copy_size] = '\0';
    if (out_size != 0)
    {
        *out_size = (uint32_t)copy_size;
    }
    return was_truncated;
}

static Module_Loader_Result
module_loader_fail_literal(
    Module_Loader_Error* error,
    Module_Loader_Result result,
    Module_Loader_Stage stage,
    uint32_t win32_error,
    const char* message)
{
    module_loader_zero_error(error);
    if (error != 0)
    {
        uint64_t message_size = 0;
        while (message != 0 && message[message_size] != '\0')
        {
            message_size += 1;
        }

        error->result = result;
        error->stage = stage;
        error->win32_error = win32_error;
        error->message_was_truncated = module_loader_copy_error_text(
            message,
            message_size,
            error->message,
            Module_Loader_Error_Message_Capacity,
            &error->message_size);
    }
    return result;
}

static Module_Loader_Result
module_loader_fail_game(
    Module_Loader_Error* error,
    Module_Loader_Result result,
    Module_Loader_Stage stage,
    Game_Result game_result,
    uint32_t item_index,
    const Game_Error* game_error,
    const char* fallback_message)
{
    uint64_t fallback_size = 0;

    module_loader_zero_error(error);
    if (error == 0)
    {
        return result;
    }

    error->result = result;
    error->stage = stage;
    error->game_result = game_result;

    if (stage == Module_Loader_Stage_Query_Schema ||
        stage == Module_Loader_Stage_Validate_Schema ||
        stage == Module_Loader_Stage_Copy_Schema)
    {
        error->schema_index = item_index;
    }
    else if (stage == Module_Loader_Stage_Apply_Document)
    {
        error->document_index = item_index;
    }

    if (game_error != 0 &&
        game_error->message.data != 0 &&
        game_error->message.size != 0)
    {
        error->message_was_truncated = module_loader_copy_error_text(
            game_error->message.data,
            game_error->message.size,
            error->message,
            Module_Loader_Error_Message_Capacity,
            &error->message_size);
    }
    else
    {
        while (fallback_message != 0 && fallback_message[fallback_size] != '\0')
        {
            fallback_size += 1;
        }
        error->message_was_truncated = module_loader_copy_error_text(
            fallback_message,
            fallback_size,
            error->message,
            Module_Loader_Error_Message_Capacity,
            &error->message_size);
    }

    if (game_error != 0 &&
        game_error->json_path.data != 0 &&
        game_error->json_path.size != 0)
    {
        error->json_path_was_truncated = module_loader_copy_error_text(
            game_error->json_path.data,
            game_error->json_path.size,
            error->json_path,
            Module_Loader_Error_JSON_Path_Capacity,
            &error->json_path_size);
    }
    return result;
}

static void
module_loader_clear_schema_storage(Module_Loader_Slot* slot)
{
    uint32_t schema_index = 0;
    uint32_t field_index = 0;

    if (slot == 0)
    {
        return;
    }

    for (schema_index = 0;
         schema_index < slot->schema_count &&
         slot->storage.schemas != 0;
         schema_index += 1)
    {
        memset(
            &slot->storage.schemas[schema_index],
            0,
            sizeof(slot->storage.schemas[schema_index]));
    }
    for (field_index = 0;
         field_index < slot->field_count &&
         slot->storage.fields != 0;
         field_index += 1)
    {
        memset(
            &slot->storage.fields[field_index],
            0,
            sizeof(slot->storage.fields[field_index]));
    }
    if (slot->storage.strings != 0 && slot->string_size != 0)
    {
        memset(
            slot->storage.strings,
            0,
            (size_t)slot->string_size);
    }

    slot->schemas = slot->storage.schemas;
    slot->schema_count = 0;
    slot->field_count = 0;
    slot->string_size = 0;
}

static void
module_loader_make_empty(Module_Loader_Slot* slot)
{
    if (slot == 0)
    {
        return;
    }

    module_loader_clear_schema_storage(slot);
    slot->native_library = 0;
    slot->game = 0;
    memset(&slot->api, 0, sizeof(slot->api));
    slot->build_generation = 0;
    slot->loaded_path_size = 0;
    slot->state = Module_Loader_Slot_Empty;

    if (slot->storage.loaded_path != 0 &&
        slot->storage.loaded_path_capacity != 0)
    {
        slot->storage.loaded_path[0] = L'\0';
    }
}

static uint32_t
module_loader_storage_is_valid(const Module_Loader_Storage* storage)
{
    if (storage == 0)
    {
        return 0;
    }
    if ((storage->schemas == 0) != (storage->schema_capacity == 0))
    {
        return 0;
    }
    if ((storage->fields == 0) != (storage->field_capacity == 0))
    {
        return 0;
    }
    if ((storage->strings == 0) != (storage->string_capacity == 0))
    {
        return 0;
    }
    if (storage->loaded_path == 0 ||
        storage->loaded_path_capacity == 0 ||
        storage->loaded_path_capacity > Module_Loader_Max_Win32_Path)
    {
        return 0;
    }
#if UINTPTR_MAX < UINT64_MAX
    if (storage->string_capacity > UINTPTR_MAX)
    {
        return 0;
    }
#endif
    return 1;
}

static Module_Loader_Result
module_loader_resolve_path(
    const wchar_t* input,
    wchar_t* output,
    uint32_t output_capacity,
    uint32_t* out_size,
    Module_Loader_Error* error)
{
    DWORD result = 0;
    DWORD win32_error = 0;

    if (out_size != 0)
    {
        *out_size = 0;
    }
    if (input == 0 || input[0] == L'\0' ||
        output == 0 || output_capacity == 0)
    {
        return module_loader_fail_literal(
            error,
            Module_Loader_Result_Invalid_Argument,
            Module_Loader_Stage_Resolve_Path,
            ERROR_INVALID_PARAMETER,
            "module path is empty or its destination buffer is invalid");
    }

    SetLastError(ERROR_SUCCESS);
    result = GetFullPathNameW(input, output_capacity, output, 0);
    if (result == 0)
    {
        win32_error = GetLastError();
        return module_loader_fail_literal(
            error,
            Module_Loader_Result_Path_Resolution_Failed,
            Module_Loader_Stage_Resolve_Path,
            win32_error,
            "GetFullPathNameW failed for a module path");
    }
    if (result >= output_capacity)
    {
        output[0] = L'\0';
        return module_loader_fail_literal(
            error,
            Module_Loader_Result_Storage_Too_Small,
            Module_Loader_Stage_Resolve_Path,
            ERROR_INSUFFICIENT_BUFFER,
            "module path storage is too small");
    }

    if (out_size != 0)
    {
        *out_size = (uint32_t)result;
    }
    return Module_Loader_Result_Ok;
}

static uint32_t
module_loader_utf8_is_valid(String_View value, uint32_t require_nonempty)
{
    uint64_t index = 0;

    if ((value.size != 0 && value.data == 0) ||
        (require_nonempty != 0 && value.size == 0))
    {
        return 0;
    }

    while (index < value.size)
    {
        const uint8_t first = (uint8_t)value.data[index];
        uint32_t continuation_count = 0;
        uint32_t code_point = 0;
        uint32_t continuation_index = 0;

        if (first == 0)
        {
            return 0;
        }
        if (first <= 0x7f)
        {
            index += 1;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf)
        {
            continuation_count = 1;
            code_point = first & 0x1f;
        }
        else if (first >= 0xe0 && first <= 0xef)
        {
            continuation_count = 2;
            code_point = first & 0x0f;
        }
        else if (first >= 0xf0 && first <= 0xf4)
        {
            continuation_count = 3;
            code_point = first & 0x07;
        }
        else
        {
            return 0;
        }

        if (value.size - index <= continuation_count)
        {
            return 0;
        }
        for (continuation_index = 0;
             continuation_index < continuation_count;
             continuation_index += 1)
        {
            const uint8_t continuation =
                (uint8_t)value.data[index + 1 + continuation_index];
            if ((continuation & 0xc0) != 0x80)
            {
                return 0;
            }
            code_point = (code_point << 6) | (continuation & 0x3f);
        }

        if ((continuation_count == 2 && code_point < 0x800) ||
            (continuation_count == 3 && code_point < 0x10000) ||
            (code_point >= 0xd800 && code_point <= 0xdfff) ||
            code_point > 0x10ffff)
        {
            return 0;
        }
        index += 1 + continuation_count;
    }
    return 1;
}

static uint32_t
module_loader_string_equal(String_View left, String_View right)
{
    if (left.size != right.size)
    {
        return 0;
    }
    if (left.size == 0)
    {
        return 1;
    }
    if (left.data == 0 || right.data == 0)
    {
        return 0;
    }
    return memcmp(left.data, right.data, (size_t)left.size) == 0;
}

static uint32_t
module_loader_schema_version_matches(
    String_View schema_id,
    uint32_t schema_version)
{
    uint64_t slash_index = schema_id.size;
    uint64_t cursor = 0;
    uint64_t parsed_version = 0;

    if (schema_version == 0 || schema_id.size < 4)
    {
        return 0;
    }

    while (slash_index > 0)
    {
        slash_index -= 1;
        if (schema_id.data[slash_index] == '/')
        {
            break;
        }
    }
    if (schema_id.data[slash_index] != '/' ||
        slash_index + 2 >= schema_id.size ||
        schema_id.data[slash_index + 1] != 'v')
    {
        return 0;
    }

    cursor = slash_index + 2;
    while (cursor < schema_id.size)
    {
        const uint8_t byte = (uint8_t)schema_id.data[cursor];
        const uint32_t digit = (uint32_t)(byte - (uint8_t)'0');
        if (byte < (uint8_t)'0' || byte > (uint8_t)'9')
        {
            return 0;
        }
        if (parsed_version > ((UINT32_MAX - digit) / 10))
        {
            return 0;
        }
        parsed_version = parsed_version * 10 + digit;
        cursor += 1;
    }
    return parsed_version == schema_version;
}

static uint32_t
module_loader_float_is_finite(float value)
{
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000);
}

static uint32_t
module_loader_field_is_valid(
    const Author_Field_Desc* field,
    const Author_Field_Desc* prior_fields,
    uint32_t prior_field_count)
{
    uint32_t index = 0;
    uint32_t component = 0;
    const uint32_t known_flags =
        Author_Field_Flag_Context_Edit_Supported |
        Author_Field_Flag_Viewport_Gizmo_Supported;

    if (field == 0 ||
        field->id == 0 ||
        !module_loader_utf8_is_valid(field->name, 1) ||
        !module_loader_utf8_is_valid(field->json_path, 1) ||
        !module_loader_utf8_is_valid(field->group, 0) ||
        field->json_path.data[0] != '$' ||
        field->kind > Author_Field_Asset_Reference ||
        field->semantic > Author_Semantic_Asset ||
        (field->flags & ~known_flags) != 0)
    {
        return 0;
    }

    for (index = 0; index < prior_field_count; index += 1)
    {
        if (prior_fields[index].id == field->id)
        {
            return 0;
        }
    }

    for (component = 0; component < 4; component += 1)
    {
        if (!module_loader_float_is_finite(field->minimum[component]) ||
            !module_loader_float_is_finite(field->maximum[component]) ||
            !module_loader_float_is_finite(field->step[component]) ||
            field->minimum[component] > field->maximum[component] ||
            field->step[component] < 0.0f)
        {
            return 0;
        }
    }
    return 1;
}

static Module_Loader_Result
module_loader_copy_string(
    Module_Loader_Slot* slot,
    String_View source,
    uint32_t require_nonempty,
    String_View* destination,
    Module_Loader_Error* error,
    uint32_t schema_index,
    uint32_t field_index)
{
    uint64_t required_size = 0;
    char* target = 0;

    if (slot == 0 || destination == 0 ||
        !module_loader_utf8_is_valid(source, require_nonempty))
    {
        Module_Loader_Result result = module_loader_fail_literal(
            error,
            Module_Loader_Result_Schema_Invalid,
            Module_Loader_Stage_Validate_Schema,
            0,
            "author schema contains an invalid UTF-8 string view");
        if (error != 0)
        {
            error->schema_index = schema_index;
            error->field_index = field_index;
        }
        return result;
    }

    if (source.size == UINT64_MAX)
    {
        if (error != 0)
        {
            error->schema_index = schema_index;
            error->field_index = field_index;
        }
        return module_loader_fail_literal(
            error,
            Module_Loader_Result_Storage_Too_Small,
            Module_Loader_Stage_Copy_Schema,
            0,
            "author schema string size overflows");
    }
    required_size = source.size + 1;
    if (slot->string_size > slot->storage.string_capacity ||
        required_size > slot->storage.string_capacity - slot->string_size)
    {
        Module_Loader_Result result = module_loader_fail_literal(
            error,
            Module_Loader_Result_Storage_Too_Small,
            Module_Loader_Stage_Copy_Schema,
            0,
            "caller-owned author schema string storage is too small");
        if (error != 0)
        {
            error->schema_index = schema_index;
            error->field_index = field_index;
        }
        return result;
    }
#if UINTPTR_MAX < UINT64_MAX
    if (slot->string_size > UINTPTR_MAX ||
        required_size > UINTPTR_MAX ||
        slot->string_size + required_size > UINTPTR_MAX)
    {
        Module_Loader_Result result = module_loader_fail_literal(
            error,
            Module_Loader_Result_Storage_Too_Small,
            Module_Loader_Stage_Copy_Schema,
            0,
            "author schema string storage exceeds the host address space");
        if (error != 0)
        {
            error->schema_index = schema_index;
            error->field_index = field_index;
        }
        return result;
    }
#endif

    target = slot->storage.strings + (size_t)slot->string_size;
    if (source.size != 0)
    {
        memcpy(target, source.data, (size_t)source.size);
    }
    target[(size_t)source.size] = '\0';

    destination->data = target;
    destination->size = source.size;
    slot->string_size += required_size;
    return Module_Loader_Result_Ok;
}

static Module_Loader_Result
module_loader_copy_schema(
    Module_Loader_Slot* slot,
    const Author_Schema_View* source,
    uint32_t schema_index,
    Module_Loader_Error* error)
{
    Author_Schema_View* destination = 0;
    uint32_t field_index = 0;
    uint32_t prior_schema_index = 0;
    const uint32_t known_flags =
        Author_Schema_Flag_Full_Form_Supported |
        Author_Schema_Flag_Context_Edit_Supported |
        Author_Schema_Flag_Viewport_Gizmo_Supported;

    if (slot == 0 || source == 0 ||
        !module_loader_utf8_is_valid(source->schema_id, 1) ||
        !module_loader_schema_version_matches(
            source->schema_id,
            source->schema_version) ||
        (source->field_count != 0 && source->fields == 0) ||
        (source->flags & ~known_flags) != 0)
    {
        Module_Loader_Result result = module_loader_fail_literal(
            error,
            Module_Loader_Result_Schema_Invalid,
            Module_Loader_Stage_Validate_Schema,
            0,
            "author schema header is invalid");
        if (error != 0)
        {
            error->schema_index = schema_index;
        }
        return result;
    }

    for (prior_schema_index = 0;
         prior_schema_index < slot->schema_count;
         prior_schema_index += 1)
    {
        if (module_loader_string_equal(
                slot->storage.schemas[prior_schema_index].schema_id,
                source->schema_id))
        {
            Module_Loader_Result result = module_loader_fail_literal(
                error,
                Module_Loader_Result_Schema_Invalid,
                Module_Loader_Stage_Validate_Schema,
                0,
                "candidate module exposes a duplicate author schema ID");
            if (error != 0)
            {
                error->schema_index = schema_index;
            }
            return result;
        }
    }

    if (slot->schema_count >= slot->storage.schema_capacity ||
        source->field_count >
            slot->storage.field_capacity - slot->field_count)
    {
        Module_Loader_Result result = module_loader_fail_literal(
            error,
            Module_Loader_Result_Storage_Too_Small,
            Module_Loader_Stage_Copy_Schema,
            0,
            "caller-owned author schema descriptor storage is too small");
        if (error != 0)
        {
            error->schema_index = schema_index;
        }
        return result;
    }

    destination = &slot->storage.schemas[slot->schema_count];
    memset(destination, 0, sizeof(*destination));
    destination->schema_version = source->schema_version;
    destination->flags = source->flags;
    destination->field_count = source->field_count;
    destination->fields =
        source->field_count == 0
            ? 0
            : &slot->storage.fields[slot->field_count];

    {
        Module_Loader_Result result = module_loader_copy_string(
            slot,
            source->schema_id,
            1,
            &destination->schema_id,
            error,
            schema_index,
            UINT32_MAX);
        if (result != Module_Loader_Result_Ok)
        {
            return result;
        }
    }

    for (field_index = 0; field_index < source->field_count; field_index += 1)
    {
        const Author_Field_Desc* source_field = &source->fields[field_index];
        Author_Field_Desc* destination_field =
            &slot->storage.fields[slot->field_count];
        Module_Loader_Result result = Module_Loader_Result_Ok;

        if (!module_loader_field_is_valid(
                source_field,
                destination->fields,
                field_index))
        {
            result = module_loader_fail_literal(
                error,
                Module_Loader_Result_Schema_Invalid,
                Module_Loader_Stage_Validate_Schema,
                0,
                "author field descriptor is invalid or has a duplicate ID");
            if (error != 0)
            {
                error->schema_index = schema_index;
                error->field_index = field_index;
            }
            return result;
        }

        memset(destination_field, 0, sizeof(*destination_field));
        *destination_field = *source_field;
        destination_field->name.data = 0;
        destination_field->name.size = 0;
        destination_field->json_path.data = 0;
        destination_field->json_path.size = 0;
        destination_field->group.data = 0;
        destination_field->group.size = 0;

        result = module_loader_copy_string(
            slot,
            source_field->name,
            1,
            &destination_field->name,
            error,
            schema_index,
            field_index);
        if (result == Module_Loader_Result_Ok)
        {
            result = module_loader_copy_string(
                slot,
                source_field->json_path,
                1,
                &destination_field->json_path,
                error,
                schema_index,
                field_index);
        }
        if (result == Module_Loader_Result_Ok)
        {
            result = module_loader_copy_string(
                slot,
                source_field->group,
                0,
                &destination_field->group,
                error,
                schema_index,
                field_index);
        }
        if (result != Module_Loader_Result_Ok)
        {
            return result;
        }
        slot->field_count += 1;
    }

    slot->schema_count += 1;
    return Module_Loader_Result_Ok;
}

static uint32_t
module_loader_api_is_complete(const Game_API* api)
{
    return api != 0 &&
        api->create != 0 &&
        api->destroy != 0 &&
        api->fixed_update != 0 &&
        api->render != 0 &&
        api->apply_document != 0 &&
        api->remove_document != 0 &&
        api->get_author_schema_count != 0 &&
        api->get_author_schema != 0 &&
        api->read_author_field != 0 &&
        api->preview_author_field != 0;
}

static Module_Loader_Result
module_loader_unload(
    Module_Loader_Slot* slot,
    Module_Loader_Clear_Context_Function clear_context_bindings,
    void* clear_context_user,
    Module_Loader_Error* error)
{
    HMODULE library = 0;

    if (slot == 0)
    {
        return module_loader_fail_literal(
            error,
            Module_Loader_Result_Invalid_Argument,
            Module_Loader_Stage_Unload,
            ERROR_INVALID_PARAMETER,
            "module slot is null");
    }

    if (slot->native_library == 0)
    {
        module_loader_clear_schema_storage(slot);
        slot->game = 0;
        memset(&slot->api, 0, sizeof(slot->api));
        slot->build_generation = 0;
        slot->state =
            slot->loaded_path_size == 0
                ? Module_Loader_Slot_Empty
                : Module_Loader_Slot_Inactive_Copy;
        return Module_Loader_Result_Ok;
    }

    if (clear_context_bindings != 0)
    {
        clear_context_bindings(clear_context_user);
    }

    if (slot->game != 0 && slot->api.destroy != 0)
    {
        slot->api.destroy(slot->game);
    }
    slot->game = 0;
    module_loader_clear_schema_storage(slot);

    /*
     * Erase every DLL function pointer before FreeLibrary. No API or schema
     * view in the slot can refer into the image after the unload succeeds.
     */
    memset(&slot->api, 0, sizeof(slot->api));
    library = (HMODULE)slot->native_library;

    SetLastError(ERROR_SUCCESS);
    if (!FreeLibrary(library))
    {
        const DWORD win32_error = GetLastError();
        slot->state = Module_Loader_Slot_Unload_Pending;
        return module_loader_fail_literal(
            error,
            Module_Loader_Result_Unload_Failed,
            Module_Loader_Stage_Unload,
            win32_error,
            "FreeLibrary failed; copied DLL remains protected from deletion");
    }

    library = 0;
    slot->native_library = 0;
    slot->build_generation = 0;
    slot->state =
        slot->loaded_path_size == 0
            ? Module_Loader_Slot_Empty
            : Module_Loader_Slot_Inactive_Copy;
    return Module_Loader_Result_Ok;
}

static Module_Loader_Result
module_loader_rollback_candidate(
    Module_Loader_Slot* candidate,
    const Module_Loader_Prepare_Request* request,
    Module_Loader_Error* error,
    Module_Loader_Result original_result)
{
    Module_Loader_Error unload_error;
    Module_Loader_Result unload_result = Module_Loader_Result_Ok;

    memset(&unload_error, 0, sizeof(unload_error));
    unload_result = module_loader_unload(
        candidate,
        request == 0 ? 0 : request->clear_context_bindings,
        request == 0 ? 0 : request->clear_context_user,
        &unload_error);
    if (unload_result != Module_Loader_Result_Ok)
    {
        if (error != 0)
        {
            *error = unload_error;
        }
        return unload_result;
    }
    return original_result;
}

Module_Loader_Result
module_loader_slot_init(
    Module_Loader_Slot* slot,
    const Module_Loader_Storage* storage,
    Module_Loader_Error* out_error)
{
    module_loader_zero_error(out_error);
    if (slot == 0 || !module_loader_storage_is_valid(storage))
    {
        return module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Invalid_Argument,
            Module_Loader_Stage_None,
            ERROR_INVALID_PARAMETER,
            "module slot or caller-owned storage is invalid");
    }

    memset(slot, 0, sizeof(*slot));
    slot->storage = *storage;
    slot->schemas = storage->schemas;
    slot->state = Module_Loader_Slot_Empty;
    slot->storage.loaded_path[0] = L'\0';
    return Module_Loader_Result_Ok;
}

Module_Loader_Result
module_loader_prepare(
    Module_Loader_Slot* candidate,
    const Module_Loader_Prepare_Request* request,
    Module_Loader_Error* out_error)
{
    wchar_t canonical_path[Module_Loader_Max_Win32_Path];
    uint32_t canonical_path_size = 0;
    Module_Loader_Result result = Module_Loader_Result_Ok;
    HMODULE library = 0;
    FARPROC raw_entry = 0;
    Game_Get_API_Function get_api = 0;
    Game_Error game_error;
    Game_Result game_result = Game_Result_Ok;
    uint32_t schema_count = 0;
    uint32_t schema_index = 0;
    uint32_t document_index = 0;

    module_loader_zero_error(out_error);
    memset(canonical_path, 0, sizeof(canonical_path));
    memset(&game_error, 0, sizeof(game_error));

    if (candidate == 0 || request == 0 ||
        request->struct_size != sizeof(*request) ||
        request->flags != 0 ||
        request->reserved != 0 ||
        request->build_generation == 0 ||
        request->canonical_dll_path == 0 ||
        request->unique_hot_dll_path == 0 ||
        request->create_desc.struct_size != sizeof(Game_Create_Desc) ||
        request->create_desc.host_api_version != Host_API_Version ||
        (request->document_count != 0 && request->documents == 0))
    {
        return module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Invalid_Argument,
            Module_Loader_Stage_None,
            ERROR_INVALID_PARAMETER,
            "candidate prepare request is invalid");
    }
    if (candidate->state != Module_Loader_Slot_Empty ||
        candidate->native_library != 0 ||
        candidate->game != 0 ||
        candidate->loaded_path_size != 0 ||
        !module_loader_storage_is_valid(&candidate->storage))
    {
        return module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Invalid_State,
            Module_Loader_Stage_None,
            ERROR_INVALID_STATE,
            "candidate slot is not empty");
    }

    result = module_loader_resolve_path(
        request->canonical_dll_path,
        canonical_path,
        Module_Loader_Max_Win32_Path,
        &canonical_path_size,
        out_error);
    if (result != Module_Loader_Result_Ok)
    {
        return result;
    }

    result = module_loader_resolve_path(
        request->unique_hot_dll_path,
        candidate->storage.loaded_path,
        candidate->storage.loaded_path_capacity,
        &candidate->loaded_path_size,
        out_error);
    if (result != Module_Loader_Result_Ok)
    {
        module_loader_make_empty(candidate);
        return result;
    }

    (void)canonical_path_size;
    if (CompareStringOrdinal(
            canonical_path,
            -1,
            candidate->storage.loaded_path,
            -1,
            TRUE) == CSTR_EQUAL)
    {
        module_loader_make_empty(candidate);
        return module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Hot_Path_Not_Unique,
            Module_Loader_Stage_Copy,
            ERROR_ALREADY_EXISTS,
            "canonical and hot-load DLL paths must be different");
    }

    SetLastError(ERROR_SUCCESS);
    if (!CopyFileW(
            canonical_path,
            candidate->storage.loaded_path,
            TRUE))
    {
        const DWORD win32_error = GetLastError();
        module_loader_make_empty(candidate);
        return module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Copy_Failed,
            Module_Loader_Stage_Copy,
            win32_error,
            "CopyFileW could not create the unique hot-load DLL");
    }

    candidate->state = Module_Loader_Slot_Inactive_Copy;
    candidate->build_generation = request->build_generation;

    SetLastError(ERROR_SUCCESS);
    library = LoadLibraryW(candidate->storage.loaded_path);
    if (library == 0)
    {
        const DWORD win32_error = GetLastError();
        return module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Load_Failed,
            Module_Loader_Stage_Load,
            win32_error,
            "LoadLibraryW failed for the copied candidate DLL");
    }
    candidate->native_library = (void*)library;

    SetLastError(ERROR_SUCCESS);
    raw_entry = GetProcAddress(library, "game_get_api");
    if (raw_entry == 0)
    {
        const DWORD win32_error = GetLastError();
        result = module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Entry_Point_Missing,
            Module_Loader_Stage_Resolve_Entry,
            win32_error,
            "candidate DLL does not export game_get_api");
        return module_loader_rollback_candidate(
            candidate,
            request,
            out_error,
            result);
    }

    static_assert(
        sizeof(get_api) == sizeof(raw_entry),
        "Win32 function pointers must match FARPROC size");
    memcpy(&get_api, &raw_entry, sizeof(get_api));
    raw_entry = 0;

    memset(&candidate->api, 0, sizeof(candidate->api));
    game_result = get_api(
        Host_API_Version,
        (uint32_t)sizeof(Game_API),
        &candidate->api,
        &game_error);
    get_api = 0;

    if (game_result != Game_Result_Ok)
    {
        result = module_loader_fail_game(
            out_error,
            Module_Loader_Result_Negotiation_Failed,
            Module_Loader_Stage_Negotiate_API,
            game_result,
            0,
            &game_error,
            "candidate API negotiation failed");
        memset(&game_error, 0, sizeof(game_error));
        return module_loader_rollback_candidate(
            candidate,
            request,
            out_error,
            result);
    }
    memset(&game_error, 0, sizeof(game_error));

    if (candidate->api.api_version != Game_API_Version)
    {
        result = module_loader_fail_literal(
            out_error,
            Module_Loader_Result_API_Version_Mismatch,
            Module_Loader_Stage_Validate_API,
            0,
            "candidate Game_API version does not match the host");
        return module_loader_rollback_candidate(
            candidate,
            request,
            out_error,
            result);
    }
    if (candidate->api.struct_size != sizeof(Game_API))
    {
        result = module_loader_fail_literal(
            out_error,
            Module_Loader_Result_API_Size_Mismatch,
            Module_Loader_Stage_Validate_API,
            0,
            "candidate Game_API struct size does not match the full host table");
        return module_loader_rollback_candidate(
            candidate,
            request,
            out_error,
            result);
    }
    if (!module_loader_api_is_complete(&candidate->api))
    {
        result = module_loader_fail_literal(
            out_error,
            Module_Loader_Result_API_Function_Missing,
            Module_Loader_Stage_Validate_API,
            0,
            "candidate Game_API is missing a required function pointer");
        return module_loader_rollback_candidate(
            candidate,
            request,
            out_error,
            result);
    }

    candidate->game = 0;
    game_result = candidate->api.create(
        &request->create_desc,
        &candidate->game,
        &game_error);
    if (game_result != Game_Result_Ok)
    {
        result = module_loader_fail_game(
            out_error,
            Module_Loader_Result_Create_Failed,
            Module_Loader_Stage_Create,
            game_result,
            0,
            &game_error,
            "candidate module creation failed");
        memset(&game_error, 0, sizeof(game_error));
        return module_loader_rollback_candidate(
            candidate,
            request,
            out_error,
            result);
    }
    memset(&game_error, 0, sizeof(game_error));
    if (candidate->game == 0)
    {
        result = module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Create_Returned_Null,
            Module_Loader_Stage_Create,
            0,
            "candidate create succeeded without returning runtime state");
        return module_loader_rollback_candidate(
            candidate,
            request,
            out_error,
            result);
    }

    schema_count = candidate->api.get_author_schema_count();
    if (schema_count > candidate->storage.schema_capacity)
    {
        result = module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Storage_Too_Small,
            Module_Loader_Stage_Query_Schema_Count,
            0,
            "candidate exposes more schemas than caller-owned storage can hold");
        return module_loader_rollback_candidate(
            candidate,
            request,
            out_error,
            result);
    }

    for (schema_index = 0; schema_index < schema_count; schema_index += 1)
    {
        Author_Schema_View schema_view;
        memset(&schema_view, 0, sizeof(schema_view));
        memset(&game_error, 0, sizeof(game_error));

        game_result = candidate->api.get_author_schema(
            schema_index,
            &schema_view,
            &game_error);
        if (game_result != Game_Result_Ok)
        {
            result = module_loader_fail_game(
                out_error,
                Module_Loader_Result_Schema_Query_Failed,
                Module_Loader_Stage_Query_Schema,
                game_result,
                schema_index,
                &game_error,
                "candidate author schema query failed");
            memset(&schema_view, 0, sizeof(schema_view));
            memset(&game_error, 0, sizeof(game_error));
            return module_loader_rollback_candidate(
                candidate,
                request,
                out_error,
                result);
        }
        memset(&game_error, 0, sizeof(game_error));

        result = module_loader_copy_schema(
            candidate,
            &schema_view,
            schema_index,
            out_error);
        memset(&schema_view, 0, sizeof(schema_view));
        if (result != Module_Loader_Result_Ok)
        {
            return module_loader_rollback_candidate(
                candidate,
                request,
                out_error,
                result);
        }
    }

    for (document_index = 0;
         document_index < request->document_count;
         document_index += 1)
    {
        memset(&game_error, 0, sizeof(game_error));
        game_result = candidate->api.apply_document(
            candidate->game,
            &request->documents[document_index],
            &game_error);
        if (game_result != Game_Result_Ok)
        {
            result = module_loader_fail_game(
                out_error,
                Module_Loader_Result_Document_Apply_Failed,
                Module_Loader_Stage_Apply_Document,
                game_result,
                document_index,
                &game_error,
                "candidate rejected a last-known-valid document");
            memset(&game_error, 0, sizeof(game_error));
            return module_loader_rollback_candidate(
                candidate,
                request,
                out_error,
                result);
        }
        memset(&game_error, 0, sizeof(game_error));
    }

    candidate->state = Module_Loader_Slot_Prepared;
    module_loader_zero_error(out_error);
    return Module_Loader_Result_Ok;
}

Module_Loader_Result
module_loader_commit(
    Module_Loader_Slot* active,
    Module_Loader_Slot* prepared,
    Module_Loader_Error* out_error)
{
    Module_Loader_Slot old_slot;
    Module_Loader_Slot new_slot;
    Module_Loader_Result result = Module_Loader_Result_Ok;

    module_loader_zero_error(out_error);
    memset(&old_slot, 0, sizeof(old_slot));
    memset(&new_slot, 0, sizeof(new_slot));

    if (active == 0 || prepared == 0 || active == prepared ||
        prepared->state != Module_Loader_Slot_Prepared ||
        prepared->native_library == 0 ||
        prepared->game == 0 ||
        !module_loader_api_is_complete(&prepared->api) ||
        !module_loader_storage_is_valid(&prepared->storage) ||
        !module_loader_storage_is_valid(&active->storage) ||
        (active->state != Module_Loader_Slot_Empty &&
         active->state != Module_Loader_Slot_Active) ||
        (active->state == Module_Loader_Slot_Active &&
         (active->native_library == 0 ||
          active->game == 0 ||
          !module_loader_api_is_complete(&active->api))))
    {
        return module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Invalid_State,
            Module_Loader_Stage_None,
            ERROR_INVALID_STATE,
            "commit requires distinct empty/active and prepared slots");
    }
    if (active->state == Module_Loader_Slot_Active &&
        prepared->build_generation <= active->build_generation)
    {
        return module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Stale_Generation,
            Module_Loader_Stage_None,
            0,
            "an older or duplicate build generation cannot replace the active module");
    }

    if (active->state == Module_Loader_Slot_Active)
    {
        /*
         * The caller has already cleared context bindings. Supplying no
         * callback here keeps prepare and the frame-boundary commit separate.
         */
        result = module_loader_unload(active, 0, 0, out_error);
        if (result != Module_Loader_Result_Ok)
        {
            return result;
        }
    }

    old_slot = *active;
    new_slot = *prepared;
    new_slot.state = Module_Loader_Slot_Active;

    *active = new_slot;
    *prepared = old_slot;

    memset(&old_slot, 0, sizeof(old_slot));
    memset(&new_slot, 0, sizeof(new_slot));
    module_loader_zero_error(out_error);
    return Module_Loader_Result_Ok;
}

Module_Loader_Result
module_loader_release(
    Module_Loader_Slot* slot,
    Module_Loader_Clear_Context_Function clear_context_bindings,
    void* clear_context_user,
    Module_Loader_Error* out_error)
{
    module_loader_zero_error(out_error);
    if (slot == 0)
    {
        return module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Invalid_Argument,
            Module_Loader_Stage_Unload,
            ERROR_INVALID_PARAMETER,
            "module slot is null");
    }

    if (slot->state == Module_Loader_Slot_Empty ||
        slot->state == Module_Loader_Slot_Inactive_Copy)
    {
        return Module_Loader_Result_Ok;
    }
    if (slot->state != Module_Loader_Slot_Prepared &&
        slot->state != Module_Loader_Slot_Active &&
        slot->state != Module_Loader_Slot_Unload_Pending)
    {
        return module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Invalid_State,
            Module_Loader_Stage_Unload,
            ERROR_INVALID_STATE,
            "module slot is not releasable");
    }

    return module_loader_unload(
        slot,
        clear_context_bindings,
        clear_context_user,
        out_error);
}

Module_Loader_Result
module_loader_delete_inactive_copy(
    Module_Loader_Slot* slot,
    Module_Loader_Error* out_error)
{
    module_loader_zero_error(out_error);
    if (slot == 0 ||
        slot->state != Module_Loader_Slot_Inactive_Copy ||
        slot->native_library != 0 ||
        slot->loaded_path_size == 0 ||
        slot->storage.loaded_path == 0)
    {
        return module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Invalid_State,
            Module_Loader_Stage_Delete,
            ERROR_INVALID_STATE,
            "only an unloaded inactive hot copy can be deleted");
    }

    SetLastError(ERROR_SUCCESS);
    if (!DeleteFileW(slot->storage.loaded_path))
    {
        const DWORD win32_error = GetLastError();
        if (win32_error != ERROR_FILE_NOT_FOUND &&
            win32_error != ERROR_PATH_NOT_FOUND)
        {
            return module_loader_fail_literal(
                out_error,
                Module_Loader_Result_Delete_Failed,
                Module_Loader_Stage_Delete,
                win32_error,
                "DeleteFileW failed for an inactive hot copy");
        }
    }

    module_loader_make_empty(slot);
    module_loader_zero_error(out_error);
    return Module_Loader_Result_Ok;
}

Module_Loader_Result
module_loader_forget_inactive_copy(
    Module_Loader_Slot* slot,
    Module_Loader_Error* out_error)
{
    module_loader_zero_error(out_error);
    if (slot == 0 ||
        slot->state != Module_Loader_Slot_Inactive_Copy ||
        slot->native_library != 0)
    {
        return module_loader_fail_literal(
            out_error,
            Module_Loader_Result_Invalid_State,
            Module_Loader_Stage_None,
            ERROR_INVALID_STATE,
            "only an unloaded inactive hot copy can be forgotten");
    }

    module_loader_make_empty(slot);
    return Module_Loader_Result_Ok;
}

uint32_t
module_loader_slot_is_callable(const Module_Loader_Slot* slot)
{
    return slot != 0 &&
        (slot->state == Module_Loader_Slot_Prepared ||
         slot->state == Module_Loader_Slot_Active) &&
        slot->native_library != 0 &&
        slot->game != 0 &&
        module_loader_api_is_complete(&slot->api);
}

const char*
module_loader_result_string(Module_Loader_Result result)
{
    switch (result)
    {
        case Module_Loader_Result_Ok: return "ok";
        case Module_Loader_Result_Invalid_Argument: return "invalid argument";
        case Module_Loader_Result_Invalid_State: return "invalid state";
        case Module_Loader_Result_Storage_Too_Small: return "storage too small";
        case Module_Loader_Result_Stale_Generation: return "stale generation";
        case Module_Loader_Result_Path_Resolution_Failed: return "path resolution failed";
        case Module_Loader_Result_Hot_Path_Not_Unique: return "hot path is not unique";
        case Module_Loader_Result_Copy_Failed: return "DLL copy failed";
        case Module_Loader_Result_Load_Failed: return "DLL load failed";
        case Module_Loader_Result_Entry_Point_Missing: return "entry point missing";
        case Module_Loader_Result_Negotiation_Failed: return "API negotiation failed";
        case Module_Loader_Result_API_Version_Mismatch: return "API version mismatch";
        case Module_Loader_Result_API_Size_Mismatch: return "API size mismatch";
        case Module_Loader_Result_API_Function_Missing: return "API function missing";
        case Module_Loader_Result_Create_Failed: return "candidate create failed";
        case Module_Loader_Result_Create_Returned_Null: return "candidate create returned null";
        case Module_Loader_Result_Schema_Query_Failed: return "schema query failed";
        case Module_Loader_Result_Schema_Invalid: return "schema invalid";
        case Module_Loader_Result_Document_Apply_Failed: return "document apply failed";
        case Module_Loader_Result_Unload_Failed: return "DLL unload failed";
        case Module_Loader_Result_Delete_Failed: return "inactive copy deletion failed";
        default: return "unknown module loader result";
    }
}
