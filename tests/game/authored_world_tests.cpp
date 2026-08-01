/*
 * Proves the ECS world is the storage of record for authored documents
 * (docs/entity_documents.md section 7). The bridge-level behaviour is
 * covered by the existing game/bridge/reload suites; this file checks the
 * storage itself: entity lifetime, revision gating, and that reads come
 * back from ECS component memory.
 */

#include "authored_world.h"

#include <stdio.h>
#include <string.h>

static uint32_t world_test_failures = 0;

static void
world_check(uint32_t condition, const char* message)
{
    if (!condition)
    {
        fprintf(stderr, "check failed: %s\n", message);
        world_test_failures += 1;
    }
}

static String_View
world_view(const char* text)
{
    String_View view;
    view.data = text;
    view.size = text != 0 ? (uint64_t)strlen(text) : 0;
    return view;
}

static const char scene_json[] =
    "{"
    "\"$schema\":\"nveditor.scene/v1\","
    "\"cube\":{"
    "\"color\":[0.25,0.50,0.75,1.0],"
    "\"transform\":{"
    "\"translation\":[1.0,2.0,3.0],"
    "\"rotation_degrees\":[0.0,45.0,0.0],"
    "\"scale\":[1.0,1.0,1.0]"
    "}}}";

static const char post_json[] =
    "{"
    "\"$schema\":\"nveditor.post_process/v1\","
    "\"exposure\":1.5,"
    "\"bloom_threshold\":0.8,"
    "\"vignette\":0.25,"
    "\"tint\":[1.0,0.9,0.8,1.0]"
    "}";

static Author_Document_State
world_document(
    Author_Document_Id document_id,
    uint64_t revision,
    const char* schema_id,
    const char* json,
    Author_Object_Source* object_storage)
{
    Author_Document_State document;
    memset(&document, 0, sizeof(document));
    memset(object_storage, 0, sizeof(*object_storage));

    object_storage->object_id = document_id + 1;
    object_storage->json_path = world_view("$");

    document.struct_size = (uint32_t)sizeof(document);
    document.document_id = document_id;
    document.root_object_id = object_storage->object_id;
    document.revision = revision;
    document.schema_id = world_view(schema_id);
    document.project_relative_path = world_view("tests/authored_world.json");
    document.json_utf8 = world_view(json);
    document.objects = object_storage;
    document.object_count = 1;
    return document;
}

int
main(void)
{
    Authored_World* world = authored_world_create();
    Author_Object_Source scene_object;
    Author_Object_Source post_object;
    Game_Error error;
    Author_Value value;

    world_check(world != 0, "the ECS world is created");
    if (world == 0)
    {
        return 1;
    }
    world_check(
        authored_world_document_count(world) == 0,
        "a fresh world holds no authored entities");

    Author_Document_State scene = world_document(
        1000,
        1,
        "nveditor.scene/v1",
        scene_json,
        &scene_object);
    world_check(
        authored_world_apply_document(world, &scene, &error) ==
            Game_Result_Ok,
        "a scene document applies");
    world_check(
        authored_world_document_count(world) == 1,
        "applying creates exactly one entity");

    /* Reads are served from ECS component storage. */
    memset(&value, 0, sizeof(value));
    world_check(
        authored_world_read_field(
            world,
            1000,
            scene_object.object_id,
            Author_Field_Id_Scene_Cube_Color,
            &value,
            &error) == Game_Result_Ok &&
            value.kind == Author_Field_Color4 &&
            value.data.f32[0] > 0.249f && value.data.f32[0] < 0.251f &&
            value.data.f32[2] > 0.749f && value.data.f32[2] < 0.751f,
        "the stored color reads back from the entity");

    world_check(
        authored_world_latest_scene(world) != 0 &&
            authored_world_latest_scene(world)->cube_transform
                    .translation[1] > 1.99f,
        "the latest scene query resolves through the world");

    /* Revision gating is enforced against the stored component. */
    Author_Document_State stale = world_document(
        1000,
        1,
        "nveditor.scene/v1",
        scene_json,
        &scene_object);
    world_check(
        authored_world_apply_document(world, &stale, &error) ==
            Game_Result_Invalid_Document,
        "a non-advancing revision is rejected");
    world_check(
        authored_world_document_count(world) == 1,
        "a rejected apply creates no entity");

    /* Re-applying with a higher revision updates in place. */
    Author_Document_State advanced = world_document(
        1000,
        2,
        "nveditor.scene/v1",
        scene_json,
        &scene_object);
    world_check(
        authored_world_apply_document(world, &advanced, &error) ==
            Game_Result_Ok &&
            authored_world_document_count(world) == 1,
        "an advancing revision updates the same entity");

    /* A second document is a second entity. */
    Author_Document_State post = world_document(
        2000,
        1,
        "nveditor.post_process/v1",
        post_json,
        &post_object);
    world_check(
        authored_world_apply_document(world, &post, &error) ==
            Game_Result_Ok &&
            authored_world_document_count(world) == 2,
        "a second document becomes a second entity");
    world_check(
        authored_world_latest_post_process(world) != 0 &&
            authored_world_latest_post_process(world)->exposure > 1.49f,
        "the latest post-process query resolves through the world");

    /* Previews mutate the component in place. */
    memset(&value, 0, sizeof(value));
    value.kind = Author_Field_Color4;
    value.data.f32[0] = 0.9f;
    value.data.f32[1] = 0.1f;
    value.data.f32[2] = 0.2f;
    value.data.f32[3] = 1.0f;
    world_check(
        authored_world_preview_field(
            world,
            1000,
            scene_object.object_id,
            Author_Field_Id_Scene_Cube_Color,
            &value,
            &error) == Game_Result_Ok,
        "a preview applies to the entity");

    memset(&value, 0, sizeof(value));
    world_check(
        authored_world_read_field(
            world,
            1000,
            scene_object.object_id,
            Author_Field_Id_Scene_Cube_Color,
            &value,
            &error) == Game_Result_Ok &&
            value.data.f32[0] > 0.899f && value.data.f32[0] < 0.901f,
        "the previewed value reads back from the entity");

    /* Out-of-range previews are refused by the shared validation. */
    memset(&value, 0, sizeof(value));
    value.kind = Author_Field_Color4;
    value.data.f32[0] = 5.0f;
    world_check(
        authored_world_preview_field(
            world,
            1000,
            scene_object.object_id,
            Author_Field_Id_Scene_Cube_Color,
            &value,
            &error) != Game_Result_Ok,
        "an out-of-range preview is refused");

    /* Unknown documents and objects fail, they never crash. */
    memset(&value, 0, sizeof(value));
    world_check(
        authored_world_read_field(
            world,
            4242,
            scene_object.object_id,
            Author_Field_Id_Scene_Cube_Color,
            &value,
            &error) == Game_Result_Invalid_Document,
        "an unknown document id fails cleanly");
    world_check(
        authored_world_read_field(
            world,
            1000,
            999999,
            Author_Field_Id_Scene_Cube_Color,
            &value,
            &error) == Game_Result_Invalid_Document,
        "an unknown object id fails cleanly");

    /* Removal deletes the entity. */
    world_check(
        authored_world_remove_document(world, 1000, &error) ==
            Game_Result_Ok &&
            authored_world_document_count(world) == 1,
        "removing a document deletes its entity");
    world_check(
        authored_world_latest_scene(world) == 0,
        "the removed scene no longer resolves");
    world_check(
        authored_world_remove_document(world, 1000, &error) !=
            Game_Result_Ok,
        "removing twice fails cleanly");

    authored_world_destroy(world);

    if (world_test_failures == 0)
    {
        printf("all authored world tests passed\n");
        return 0;
    }
    fprintf(
        stderr,
        "%u authored world checks failed\n",
        world_test_failures);
    return 1;
}
