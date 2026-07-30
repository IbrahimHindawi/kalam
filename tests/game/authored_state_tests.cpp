#include "authored_state.h"

#include <stdio.h>
#include <string.h>

static uint32_t test_failure_count;

static void
test_fail(const char* expression, int line)
{
    printf("authored_state_tests.cpp(%d): check failed: %s\n", line, expression);
    test_failure_count += 1;
}

#define TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            test_fail(#expression, __LINE__); \
        } \
    } while (0)

static String_View
test_view(const char* text)
{
    String_View result;
    result.data = text;
    result.size = (uint64_t)strlen(text);
    return result;
}

static Author_Document_State
test_document(
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    uint64_t revision,
    const char* schema_id,
    const char* path,
    const char* json,
    Author_Object_Source* root)
{
    Author_Document_State result;
    memset(&result, 0, sizeof(result));
    memset(root, 0, sizeof(*root));

    root->object_id = object_id;
    root->json_path = test_view("$");

    result.struct_size = (uint32_t)sizeof(result);
    result.document_id = document_id;
    result.root_object_id = object_id;
    result.revision = revision;
    result.schema_id = test_view(schema_id);
    result.project_relative_path = test_view(path);
    result.json_utf8 = test_view(json);
    result.objects = root;
    result.object_count = 1;
    return result;
}

static const char* Valid_Scene_Json =
    "{"
    "\"$schema\":\"nveditor.scene/v1\","
    "\"cube\":{"
    "\"color\":[0.12,0.58,0.95,1],"
    "\"transform\":{"
    "\"translation\":[1,2,3],"
    "\"rotation_degrees\":[25,35,0],"
    "\"scale\":[1,1,1]"
    "}}}";

static const char* Valid_Post_Json =
    "{"
    "\"$schema\":\"nveditor.post_process/v1\","
    "\"exposure\":1,"
    "\"bloom_threshold\":0.8,"
    "\"vignette\":0.15,"
    "\"tint\":[1,0.5,0.25,1]"
    "}";

static void
test_valid_scene_apply_read_preview_remove(void)
{
    Authored_State state;
    Author_Object_Source root;
    Author_Document_State document;
    Author_Value value;
    Author_Value preview;
    Game_Error error;
    Game_Result result;

    authored_state_init(&state);
    document = test_document(
        10,
        20,
        1,
        "nveditor.scene/v1",
        "examples/cube.scene.json",
        Valid_Scene_Json,
        &root);

    result = authored_state_apply_document(&state, &document, &error);
    TEST_CHECK(result == Game_Result_Ok);
    TEST_CHECK(state.document_count == 1);
    TEST_CHECK(authored_state_latest_scene(&state) != 0);
    TEST_CHECK(authored_state_latest_scene(&state)->cube_color[1] == 0.58f);

    memset(&value, 0, sizeof(value));
    result = authored_state_read_field(
        &state,
        10,
        20,
        Author_Field_Id_Scene_Translation,
        &value,
        &error);
    TEST_CHECK(result == Game_Result_Ok);
    TEST_CHECK(value.kind == Author_Field_Vec3);
    TEST_CHECK(value.data.f32[0] == 1.0f);
    TEST_CHECK(value.data.f32[1] == 2.0f);
    TEST_CHECK(value.data.f32[2] == 3.0f);

    memset(&preview, 0, sizeof(preview));
    preview.kind = Author_Field_Color4;
    preview.data.f32[0] = 1.0f;
    preview.data.f32[1] = 0.0f;
    preview.data.f32[2] = 0.0f;
    preview.data.f32[3] = 1.0f;
    result = authored_state_preview_field(
        &state,
        10,
        20,
        Author_Field_Id_Scene_Cube_Color,
        &preview,
        &error);
    TEST_CHECK(result == Game_Result_Ok);
    TEST_CHECK(authored_state_latest_scene(&state)->cube_color[0] == 1.0f);
    TEST_CHECK(authored_state_latest_scene(&state)->cube_color[1] == 0.0f);

    preview.data.f32[0] = 2.0f;
    result = authored_state_preview_field(
        &state,
        10,
        20,
        Author_Field_Id_Scene_Cube_Color,
        &preview,
        &error);
    TEST_CHECK(result == Game_Result_Invalid_Argument);
    TEST_CHECK(authored_state_latest_scene(&state)->cube_color[0] == 1.0f);

    result = authored_state_remove_document(&state, 10, &error);
    TEST_CHECK(result == Game_Result_Ok);
    TEST_CHECK(state.document_count == 0);
    TEST_CHECK(authored_state_latest_scene(&state) == 0);
}

static void
test_failed_apply_preserves_last_valid_state(void)
{
    Authored_State state;
    Author_Object_Source objects[2];
    Author_Document_State document;
    Game_Error error;
    Game_Result result;
    const char* duplicate_json =
        "{"
        "\"$schema\":\"nveditor.scene/v1\","
        "\"$schema\":\"nveditor.scene/v1\","
        "\"cube\":{"
        "\"color\":[1,1,1,1],"
        "\"transform\":{"
        "\"translation\":[0,0,0],"
        "\"rotation_degrees\":[0,0,0],"
        "\"scale\":[1,1,1]"
        "}}}";
    const char* out_of_range_json =
        "{"
        "\"$schema\":\"nveditor.scene/v1\","
        "\"cube\":{"
        "\"color\":[9,1,1,1],"
        "\"transform\":{"
        "\"translation\":[0,0,0],"
        "\"rotation_degrees\":[0,0,0],"
        "\"scale\":[1,1,1]"
        "}}}";

    authored_state_init(&state);
    document = test_document(
        1,
        2,
        1,
        "nveditor.scene/v1",
        "scene.json",
        Valid_Scene_Json,
        &objects[0]);
    TEST_CHECK(
        authored_state_apply_document(&state, &document, &error) ==
        Game_Result_Ok);

    document.revision = 2;
    document.json_utf8 = test_view(duplicate_json);
    result = authored_state_apply_document(&state, &document, &error);
    TEST_CHECK(result == Game_Result_Invalid_Document);
    TEST_CHECK(state.documents[0].revision == 1);
    TEST_CHECK(authored_state_latest_scene(&state)->cube_color[0] == 0.12f);

    document.revision = 3;
    document.json_utf8 = test_view(out_of_range_json);
    result = authored_state_apply_document(&state, &document, &error);
    TEST_CHECK(result == Game_Result_Invalid_Document);
    TEST_CHECK(state.documents[0].revision == 1);

    document.revision = 4;
    document.json_utf8 = test_view(
        "{\"$schema\":\"nveditor.unknown/v1\"}");
    document.schema_id = test_view("nveditor.unknown/v1");
    result = authored_state_apply_document(&state, &document, &error);
    TEST_CHECK(result == Game_Result_Unsupported_Schema);
    TEST_CHECK(state.documents[0].revision == 1);

    document = test_document(
        1,
        2,
        5,
        "nveditor.scene/v1",
        "scene.json",
        Valid_Scene_Json,
        &objects[0]);
    objects[1].object_id = 3;
    objects[1].source_id = test_view("cube");
    objects[1].json_path = test_view("$.cube");
    document.objects = objects;
    document.object_count = 2;
    result = authored_state_apply_document(&state, &document, &error);
    TEST_CHECK(result == Game_Result_Invalid_Document);
    TEST_CHECK(state.documents[0].revision == 1);
}

static void
test_post_process_and_snapshot_validation(void)
{
    Authored_State state;
    Author_Object_Source objects[3];
    Author_Document_State document;
    Author_Value value;
    Game_Error error;
    Game_Result result;

    authored_state_init(&state);
    document = test_document(
        30,
        40,
        1,
        "nveditor.post_process/v1",
        "examples/post_process.json",
        Valid_Post_Json,
        &objects[0]);

    result = authored_state_apply_document(&state, &document, &error);
    TEST_CHECK(result == Game_Result_Ok);
    TEST_CHECK(authored_state_latest_post_process(&state) != 0);
    TEST_CHECK(
        authored_state_latest_post_process(&state)->bloom_threshold == 0.8f);

    result = authored_state_read_field(
        &state,
        30,
        40,
        Author_Field_Id_Post_Tint,
        &value,
        &error);
    TEST_CHECK(result == Game_Result_Ok);
    TEST_CHECK(value.kind == Author_Field_Color4);
    TEST_CHECK(value.data.f32[1] == 0.5f);

    document.document_id = 31;
    document.revision = 2;
    document.objects = objects;
    document.object_count = 3;
    objects[1].object_id = 41;
    objects[1].json_path = test_view("$.first");
    objects[1].source_id = test_view("duplicate");
    objects[2].object_id = 42;
    objects[2].json_path = test_view("$.second");
    objects[2].source_id = test_view("duplicate");
    result = authored_state_apply_document(&state, &document, &error);
    TEST_CHECK(result == Game_Result_Invalid_Document);
    TEST_CHECK(
        error.message.size == strlen("duplicate authored source ID"));
    TEST_CHECK(
        memcmp(
            error.message.data,
            "duplicate authored source ID",
            strlen("duplicate authored source ID")) == 0);
    TEST_CHECK(state.document_count == 1);

    document = test_document(
        32,
        42,
        1,
        "nveditor.scene/v1",
        "post.json",
        Valid_Post_Json,
        &objects[0]);
    result = authored_state_apply_document(&state, &document, &error);
    TEST_CHECK(result == Game_Result_Invalid_Document);
    TEST_CHECK(state.document_count == 1);
}

static void
test_json_unicode_and_number_rules(void)
{
    Authored_State state;
    Author_Object_Source root;
    Author_Document_State document;
    Game_Error error;
    const char* escaped_schema =
        "{"
        "\"\\u0024schema\":\"nveditor.scene/v1\","
        "\"cube\":{"
        "\"color\":[1e-1,2e-1,3e-1,1],"
        "\"transform\":{"
        "\"translation\":[-0,0.0,1E2],"
        "\"rotation_degrees\":[0,0,0],"
        "\"scale\":[1,1,1]"
        "}}}";
    const char* leading_zero =
        "{"
        "\"$schema\":\"nveditor.post_process/v1\","
        "\"exposure\":01,"
        "\"bloom_threshold\":0.8,"
        "\"vignette\":0.1,"
        "\"tint\":[1,1,1,1]"
        "}";

    authored_state_init(&state);
    document = test_document(
        50,
        60,
        1,
        "nveditor.scene/v1",
        "unicode.scene.json",
        escaped_schema,
        &root);
    TEST_CHECK(
        authored_state_apply_document(&state, &document, &error) ==
        Game_Result_Ok);
    TEST_CHECK(authored_state_latest_scene(&state)->cube_color[0] == 0.1f);
    TEST_CHECK(
        authored_state_latest_scene(&state)
            ->cube_transform.translation[2] == 100.0f);

    document = test_document(
        51,
        61,
        1,
        "nveditor.post_process/v1",
        "invalid.json",
        leading_zero,
        &root);
    TEST_CHECK(
        authored_state_apply_document(&state, &document, &error) ==
        Game_Result_Invalid_Document);
    TEST_CHECK(state.document_count == 1);
}

int
main(void)
{
    test_valid_scene_apply_read_preview_remove();
    test_failed_apply_preserves_last_valid_state();
    test_post_process_and_snapshot_validation();
    test_json_unicode_and_number_rules();

    if (test_failure_count != 0)
    {
        printf("%u authored-state test check(s) failed\n", test_failure_count);
        return 1;
    }

    printf("all authored-state tests passed\n");
    return 0;
}
