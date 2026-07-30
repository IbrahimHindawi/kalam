#ifndef KALAM_TESTS_RELOAD_FAKE_MODULE_IMPL_H
#define KALAM_TESTS_RELOAD_FAKE_MODULE_IMPL_H

#include "game_api.h"

#include <string.h>

#ifndef KALAM_RELOAD_FAKE_MARKER
#define KALAM_RELOAD_FAKE_MARKER 99
#endif

#ifndef KALAM_RELOAD_FAKE_FAILURE
#define KALAM_RELOAD_FAKE_FAILURE 0
#endif

#define KALAM_RELOAD_FAKE_FAILURE_NONE 0
#define KALAM_RELOAD_FAKE_FAILURE_API_VERSION 1
#define KALAM_RELOAD_FAKE_FAILURE_API_SIZE 2
#define KALAM_RELOAD_FAKE_FAILURE_API_POINTER 3
#define KALAM_RELOAD_FAKE_FAILURE_CREATE 4
#define KALAM_RELOAD_FAKE_FAILURE_SCHEMA_QUERY 5
#define KALAM_RELOAD_FAKE_FAILURE_SCHEMA_DUPLICATE 6
#define KALAM_RELOAD_FAKE_FAILURE_APPLY 7
#define KALAM_RELOAD_FAKE_FAILURE_NEGOTIATION 8
#define KALAM_RELOAD_FAKE_FAILURE_SCHEMA_FIELD 9

typedef struct Reload_Fake_State
{
    uint32_t is_created;
    uint32_t has_document;
    Author_Document_Id document_id;
    uint64_t document_revision;
} Reload_Fake_State;

static Reload_Fake_State g_reload_fake_state;

/*
 * apply_document deliberately overwrites these module-owned schema bytes after
 * the loader has enumerated them. A shallow-copying host therefore fails the
 * tests even while the candidate DLL is still loaded.
 */
static char g_reload_fake_schema_id[] = "reload.test/v1";
static char g_reload_fake_field_name[] = "module_marker";
static char g_reload_fake_field_path[] = "$.module_marker";
static char g_reload_fake_field_group[] = "Reload";

static String_View
reload_fake_const_view(const char* text, uint64_t size)
{
    String_View result;
    result.data = text;
    result.size = size;
    return result;
}

static void
reload_fake_set_error(
    Game_Error* error,
    const char* message,
    uint64_t message_size,
    const char* json_path,
    uint64_t json_path_size)
{
    if (error != 0)
    {
        error->message = reload_fake_const_view(message, message_size);
        error->json_path = reload_fake_const_view(json_path, json_path_size);
    }
}

#if KALAM_RELOAD_FAKE_FAILURE != KALAM_RELOAD_FAKE_FAILURE_APPLY
static uint32_t
reload_fake_view_equals(
    String_View value,
    const char* expected,
    uint64_t expected_size)
{
    return value.data != 0 &&
        value.size == expected_size &&
        memcmp(value.data, expected, (size_t)expected_size) == 0;
}
#endif

static Game_Result
reload_fake_create(
    const Game_Create_Desc* desc,
    void** out_game,
    Game_Error* out_error)
{
    if (out_game != 0)
    {
        *out_game = 0;
    }
    reload_fake_set_error(out_error, "", 0, "", 0);
    if (desc == 0 || out_game == 0 ||
        desc->struct_size != sizeof(Game_Create_Desc) ||
        desc->host_api_version != Host_API_Version)
    {
        return Game_Result_Invalid_Argument;
    }

#if KALAM_RELOAD_FAKE_FAILURE == KALAM_RELOAD_FAKE_FAILURE_CREATE
    static const char message[] = "fake candidate create failure";
    reload_fake_set_error(
        out_error,
        message,
        sizeof(message) - 1,
        "",
        0);
    return Game_Result_Create_Failed;
#else
    memset(&g_reload_fake_state, 0, sizeof(g_reload_fake_state));
    g_reload_fake_state.is_created = 1;
    *out_game = &g_reload_fake_state;
    return Game_Result_Ok;
#endif
}

static void
reload_fake_destroy(void* game)
{
    Reload_Fake_State* state = (Reload_Fake_State*)game;
    if (state != 0)
    {
        memset(state, 0, sizeof(*state));
    }
}

static void
reload_fake_fixed_update(
    void* game,
    const Game_Input* input,
    float fixed_delta_time)
{
    (void)game;
    (void)input;
    (void)fixed_delta_time;
}

static Game_Result
reload_fake_render(
    void* game,
    const Host_Render_Target* target,
    float interpolation_alpha,
    Game_Error* out_error)
{
    (void)target;
    (void)interpolation_alpha;
    (void)out_error;
    return game == &g_reload_fake_state &&
        g_reload_fake_state.is_created != 0
            ? Game_Result_Ok
            : Game_Result_Invalid_Argument;
}

static Game_Result
reload_fake_apply_document(
    void* game,
    const Author_Document_State* document,
    Game_Error* out_error)
{
    Reload_Fake_State* state = (Reload_Fake_State*)game;
    reload_fake_set_error(out_error, "", 0, "", 0);
    if (state != &g_reload_fake_state ||
        state->is_created == 0 ||
        document == 0 ||
        document->struct_size != sizeof(Author_Document_State) ||
        document->document_id == 0)
    {
        return Game_Result_Invalid_Argument;
    }

#if KALAM_RELOAD_FAKE_FAILURE == KALAM_RELOAD_FAKE_FAILURE_APPLY
    static const char invalid_message[] = "fake document apply failure";
    static const char invalid_path[] = "$";
    reload_fake_set_error(
        out_error,
        invalid_message,
        sizeof(invalid_message) - 1,
        invalid_path,
        sizeof(invalid_path) - 1);
    return Game_Result_Invalid_Document;
#else
    static const char schema_id[] = "reload.test/v1";
    if (!reload_fake_view_equals(
            document->schema_id,
            schema_id,
            sizeof(schema_id) - 1))
    {
        return Game_Result_Invalid_Argument;
    }

    state->has_document = 1;
    state->document_id = document->document_id;
    state->document_revision = document->revision;

    memset(g_reload_fake_schema_id, '!', sizeof(g_reload_fake_schema_id) - 1);
    memset(g_reload_fake_field_name, '!', sizeof(g_reload_fake_field_name) - 1);
    memset(g_reload_fake_field_path, '!', sizeof(g_reload_fake_field_path) - 1);
    memset(g_reload_fake_field_group, '!', sizeof(g_reload_fake_field_group) - 1);
    return Game_Result_Ok;
#endif
}

static Game_Result
reload_fake_remove_document(
    void* game,
    Author_Document_Id document_id,
    Game_Error* out_error)
{
    Reload_Fake_State* state = (Reload_Fake_State*)game;
    (void)out_error;

    if (state != &g_reload_fake_state ||
        state->has_document == 0 ||
        state->document_id != document_id)
    {
        return Game_Result_Invalid_Argument;
    }
    state->has_document = 0;
    state->document_id = 0;
    state->document_revision = 0;
    return Game_Result_Ok;
}

static uint32_t
reload_fake_get_author_schema_count(void)
{
#if KALAM_RELOAD_FAKE_FAILURE == KALAM_RELOAD_FAKE_FAILURE_SCHEMA_DUPLICATE
    return 2;
#else
    return 1;
#endif
}

static Game_Result
reload_fake_get_author_schema(
    uint32_t schema_index,
    Author_Schema_View* out_schema,
    Game_Error* out_error)
{
    reload_fake_set_error(out_error, "", 0, "", 0);
#if KALAM_RELOAD_FAKE_FAILURE == KALAM_RELOAD_FAKE_FAILURE_SCHEMA_QUERY
    static const char failure_message[] = "fake schema query failure";
    (void)schema_index;
    (void)out_schema;
    reload_fake_set_error(
        out_error,
        failure_message,
        sizeof(failure_message) - 1,
        "",
        0);
    return Game_Result_Internal_Error;
#else
    static Author_Field_Desc field;
    if (out_schema == 0 ||
        schema_index >= reload_fake_get_author_schema_count())
    {
        return Game_Result_Invalid_Argument;
    }

    memset(&field, 0, sizeof(field));
#if KALAM_RELOAD_FAKE_FAILURE == KALAM_RELOAD_FAKE_FAILURE_SCHEMA_FIELD
    field.id = 0;
#else
    field.id = UINT64_C(0x8f759f813fe682b1);
#endif
    field.name.data = g_reload_fake_field_name;
    field.name.size = sizeof(g_reload_fake_field_name) - 1;
    field.json_path.data = g_reload_fake_field_path;
    field.json_path.size = sizeof(g_reload_fake_field_path) - 1;
    field.group.data = g_reload_fake_field_group;
    field.group.size = sizeof(g_reload_fake_field_group) - 1;
    field.kind = Author_Field_F32;
    field.semantic = Author_Semantic_None;
    field.minimum[0] = 0.0f;
    field.maximum[0] = 100.0f;
    field.step[0] = 0.25f;
    field.flags = Author_Field_Flag_Context_Edit_Supported;

    memset(out_schema, 0, sizeof(*out_schema));
    out_schema->schema_id.data = g_reload_fake_schema_id;
    out_schema->schema_id.size = sizeof(g_reload_fake_schema_id) - 1;
    out_schema->schema_version = 1;
    out_schema->fields = &field;
    out_schema->field_count = 1;
    out_schema->flags =
        Author_Schema_Flag_Full_Form_Supported |
        Author_Schema_Flag_Context_Edit_Supported;
    return Game_Result_Ok;
#endif
}

static Game_Result
reload_fake_read_author_field(
    void* game,
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    Author_Field_Id field_id,
    Author_Value* out_value,
    Game_Error* out_error)
{
    Reload_Fake_State* state = (Reload_Fake_State*)game;
    (void)object_id;
    (void)out_error;

    if (state != &g_reload_fake_state ||
        state->has_document == 0 ||
        state->document_id != document_id ||
        field_id != UINT64_C(0x8f759f813fe682b1) ||
        out_value == 0)
    {
        return Game_Result_Invalid_Argument;
    }

    memset(out_value, 0, sizeof(*out_value));
    out_value->kind = Author_Field_F32;
    out_value->data.f32[0] = (float)KALAM_RELOAD_FAKE_MARKER;
    return Game_Result_Ok;
}

static Game_Result
reload_fake_preview_author_field(
    void* game,
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    Author_Field_Id field_id,
    const Author_Value* value,
    Game_Error* out_error)
{
    Reload_Fake_State* state = (Reload_Fake_State*)game;
    (void)object_id;
    (void)out_error;

    if (state != &g_reload_fake_state ||
        state->has_document == 0 ||
        state->document_id != document_id ||
        field_id != UINT64_C(0x8f759f813fe682b1) ||
        value == 0 ||
        value->kind != Author_Field_F32)
    {
        return Game_Result_Invalid_Argument;
    }
    return Game_Result_Ok;
}

KALAM_GAME_API_EXPORT
Game_Result
game_get_api(
    uint32_t host_api_version,
    uint32_t host_game_api_struct_size,
    Game_API* out_api,
    Game_Error* out_error)
{
    reload_fake_set_error(out_error, "", 0, "", 0);
    if (host_api_version != Host_API_Version ||
        host_game_api_struct_size != sizeof(Game_API) ||
        out_api == 0)
    {
        return Game_Result_Invalid_Argument;
    }

#if KALAM_RELOAD_FAKE_FAILURE == KALAM_RELOAD_FAKE_FAILURE_NEGOTIATION
    static const char negotiation_message[] = "fake API negotiation failure";
    reload_fake_set_error(
        out_error,
        negotiation_message,
        sizeof(negotiation_message) - 1,
        "",
        0);
    return Game_Result_Internal_Error;
#else
    memset(out_api, 0, sizeof(*out_api));
    out_api->api_version = Game_API_Version;
    out_api->struct_size = sizeof(Game_API);
    out_api->create = reload_fake_create;
    out_api->destroy = reload_fake_destroy;
    out_api->fixed_update = reload_fake_fixed_update;
    out_api->render = reload_fake_render;
    out_api->apply_document = reload_fake_apply_document;
    out_api->remove_document = reload_fake_remove_document;
    out_api->get_author_schema_count = reload_fake_get_author_schema_count;
    out_api->get_author_schema = reload_fake_get_author_schema;
    out_api->read_author_field = reload_fake_read_author_field;
    out_api->preview_author_field = reload_fake_preview_author_field;

#if KALAM_RELOAD_FAKE_FAILURE == KALAM_RELOAD_FAKE_FAILURE_API_VERSION
    out_api->api_version = Game_API_Version + 1;
#elif KALAM_RELOAD_FAKE_FAILURE == KALAM_RELOAD_FAKE_FAILURE_API_SIZE
    out_api->struct_size =
        (uint32_t)(sizeof(Game_API) - sizeof(out_api->preview_author_field));
#elif KALAM_RELOAD_FAKE_FAILURE == KALAM_RELOAD_FAKE_FAILURE_API_POINTER
    out_api->render = 0;
#endif
    return Game_Result_Ok;
#endif
}

#endif /* KALAM_TESTS_RELOAD_FAKE_MODULE_IMPL_H */
