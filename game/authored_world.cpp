#include "authored_world.h"

#include "authored_state.h"

#include "flecs.h"

#include <string.h>

/*
 * One entity per authored document, carrying an Authored_Document
 * component. Reads, previews, and the renderer's "latest" queries are all
 * served from the ECS world; the plain-array store is no longer the
 * storage of record for the module (docs/entity_documents.md section 7).
 *
 * Parsing, validation, and field mapping live in authored_state.cpp and
 * are shared, so this file only owns storage and lookup.
 */

struct Authored_World
{
    ecs_world_t* world;
    ecs_entity_t document_component;
};

namespace
{

Game_Result
world_fail(
    Game_Result result,
    const char* message,
    const char* json_path,
    Game_Error* out_error)
{
    if (out_error != 0)
    {
        out_error->message.data = message;
        out_error->message.size = (uint64_t)strlen(message);
        out_error->json_path.data = json_path;
        out_error->json_path.size = (uint64_t)strlen(json_path);
    }
    return result;
}

void
world_clear_error(Game_Error* out_error)
{
    if (out_error != 0)
    {
        memset(out_error, 0, sizeof(*out_error));
    }
}

/*
 * Document ids are stable 64-bit hashes and would collide with flecs'
 * own entity id encoding, so the lookup is an explicit scan over the
 * component's table. Revision 1 holds a handful of documents; a lookup
 * index is a later optimization, not a correctness matter.
 */
ecs_entity_t
world_find_entity(
    const Authored_World* world,
    Author_Document_Id document_id)
{
    if (world == 0 || world->world == 0 || document_id == 0)
    {
        return 0;
    }

    ecs_query_desc_t query_desc;
    memset(&query_desc, 0, sizeof(query_desc));
    query_desc.terms[0].id = world->document_component;
    ecs_query_t* query = ecs_query_init(world->world, &query_desc);
    if (query == 0)
    {
        return 0;
    }

    ecs_entity_t found = 0;
    ecs_iter_t iterator = ecs_query_iter(world->world, query);
    while (ecs_query_next(&iterator))
    {
        const Authored_Document* documents =
            (const Authored_Document*)ecs_field_w_size(
                &iterator,
                sizeof(Authored_Document),
                0);
        if (documents == 0)
        {
            continue;
        }
        for (int32_t index = 0; index < iterator.count; ++index)
        {
            if (documents[index].document_id == document_id)
            {
                found = iterator.entities[index];
                ecs_iter_fini(&iterator);
                ecs_query_fini(query);
                return found;
            }
        }
    }
    ecs_query_fini(query);
    return 0;
}

const Authored_Document*
world_find_document(
    const Authored_World* world,
    Author_Document_Id document_id)
{
    const ecs_entity_t entity = world_find_entity(world, document_id);
    if (entity == 0)
    {
        return 0;
    }
    return (const Authored_Document*)ecs_get_id(
        world->world,
        entity,
        world->document_component);
}

const Authored_Document*
world_latest_of_kind(
    const Authored_World* world,
    Authored_Document_Kind kind)
{
    if (world == 0 || world->world == 0)
    {
        return 0;
    }

    ecs_query_desc_t query_desc;
    memset(&query_desc, 0, sizeof(query_desc));
    query_desc.terms[0].id = world->document_component;
    ecs_query_t* query = ecs_query_init(world->world, &query_desc);
    if (query == 0)
    {
        return 0;
    }

    const Authored_Document* latest = 0;
    ecs_iter_t iterator = ecs_query_iter(world->world, query);
    while (ecs_query_next(&iterator))
    {
        const Authored_Document* documents =
            (const Authored_Document*)ecs_field_w_size(
                &iterator,
                sizeof(Authored_Document),
                0);
        if (documents == 0)
        {
            continue;
        }
        for (int32_t index = 0; index < iterator.count; ++index)
        {
            const Authored_Document* candidate = &documents[index];
            if (!candidate->active || candidate->kind != kind)
            {
                continue;
            }
            if (latest == 0 || candidate->revision >= latest->revision)
            {
                latest = candidate;
            }
        }
    }
    ecs_query_fini(query);
    return latest;
}

} // namespace

extern "C" Authored_World*
authored_world_create(void)
{
    ecs_world_t* world = ecs_init();
    if (world == 0)
    {
        return 0;
    }

    Authored_World* result =
        (Authored_World*)ecs_os_calloc(sizeof(Authored_World));
    if (result == 0)
    {
        ecs_fini(world);
        return 0;
    }
    result->world = world;

    /* One opaque POD component; the reflection-driven member registration
       is a later milestone and changes nothing about storage. */
    ecs_component_desc_t component_desc;
    memset(&component_desc, 0, sizeof(component_desc));
    ecs_entity_desc_t entity_desc;
    memset(&entity_desc, 0, sizeof(entity_desc));
    entity_desc.name = "Authored_Document";
    component_desc.entity = ecs_entity_init(world, &entity_desc);
    component_desc.type.size = (ecs_size_t)sizeof(Authored_Document);
    component_desc.type.alignment = (ecs_size_t)alignof(Authored_Document);
    result->document_component = ecs_component_init(world, &component_desc);
    if (result->document_component == 0)
    {
        ecs_os_free(result);
        ecs_fini(world);
        return 0;
    }
    return result;
}

extern "C" void
authored_world_destroy(Authored_World* world)
{
    if (world == 0)
    {
        return;
    }
    if (world->world != 0)
    {
        ecs_fini(world->world);
        world->world = 0;
    }
    ecs_os_free(world);
}

extern "C" Game_Result
authored_world_apply_document(
    Authored_World* world,
    const Author_Document_State* document,
    Game_Error* out_error)
{
    Authored_Document candidate;
    Game_Result result = Game_Result_Ok;

    world_clear_error(out_error);
    if (world == 0 || world->world == 0)
    {
        return world_fail(
            Game_Result_Invalid_Argument,
            "authored world is null",
            "",
            out_error);
    }

    memset(&candidate, 0, sizeof(candidate));
    result = authored_document_parse(document, &candidate, out_error);
    if (result != Game_Result_Ok)
    {
        return result;
    }

    ecs_entity_t entity = world_find_entity(world, document->document_id);
    if (entity != 0)
    {
        const Authored_Document* existing =
            (const Authored_Document*)ecs_get_id(
                world->world,
                entity,
                world->document_component);
        if (existing != 0 && document->revision <= existing->revision)
        {
            return world_fail(
                Game_Result_Invalid_Document,
                "document revision did not advance",
                "$",
                out_error);
        }
    }
    else
    {
        entity = ecs_new(world->world);
        if (entity == 0)
        {
            return world_fail(
                Game_Result_Internal_Error,
                "could not create an authored entity",
                "",
                out_error);
        }
    }

    ecs_set_id(
        world->world,
        entity,
        world->document_component,
        sizeof(Authored_Document),
        &candidate);
    return Game_Result_Ok;
}

extern "C" Game_Result
authored_world_remove_document(
    Authored_World* world,
    Author_Document_Id document_id,
    Game_Error* out_error)
{
    world_clear_error(out_error);
    if (world == 0 || world->world == 0 || document_id == 0)
    {
        return world_fail(
            Game_Result_Invalid_Argument,
            "invalid document removal request",
            "",
            out_error);
    }

    const ecs_entity_t entity = world_find_entity(world, document_id);
    if (entity == 0)
    {
        return world_fail(
            Game_Result_Invalid_Document,
            "authored document was not found",
            "$",
            out_error);
    }
    ecs_delete(world->world, entity);
    return Game_Result_Ok;
}

extern "C" Game_Result
authored_world_read_field(
    const Authored_World* world,
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    Author_Field_Id field_id,
    Author_Value* out_value,
    Game_Error* out_error)
{
    world_clear_error(out_error);
    if (world == 0 || world->world == 0 || document_id == 0 ||
        object_id == 0 || field_id == 0 || out_value == 0)
    {
        return world_fail(
            Game_Result_Invalid_Argument,
            "invalid authored field read",
            "",
            out_error);
    }

    const Authored_Document* document =
        world_find_document(world, document_id);
    if (document == 0 ||
        (document->root_object_id != object_id &&
         document->primary_object_id != object_id))
    {
        memset(out_value, 0, sizeof(*out_value));
        return world_fail(
            Game_Result_Invalid_Document,
            "authored document or object was not found",
            "$",
            out_error);
    }
    return authored_document_read_field(
        document,
        field_id,
        out_value,
        out_error);
}

extern "C" Game_Result
authored_world_preview_field(
    Authored_World* world,
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    Author_Field_Id field_id,
    const Author_Value* value,
    Game_Error* out_error)
{
    world_clear_error(out_error);
    if (world == 0 || world->world == 0 || value == 0)
    {
        return world_fail(
            Game_Result_Invalid_Argument,
            "invalid authored field preview",
            "",
            out_error);
    }

    const ecs_entity_t entity = world_find_entity(world, document_id);
    Authored_Document* document = entity == 0
        ? 0
        : (Authored_Document*)ecs_get_mut_id(
              world->world,
              entity,
              world->document_component);
    if (document == 0 ||
        (document->root_object_id != object_id &&
         document->primary_object_id != object_id))
    {
        return world_fail(
            Game_Result_Invalid_Document,
            "authored document or object was not found",
            "$",
            out_error);
    }

    const Game_Result result = authored_document_preview_field(
        document,
        field_id,
        value,
        out_error);
    if (result == Game_Result_Ok)
    {
        ecs_modified_id(world->world, entity, world->document_component);
    }
    return result;
}

extern "C" const Authored_Document*
authored_world_latest_scene_document(const Authored_World* world)
{
    return world_latest_of_kind(world, Authored_Document_Scene);
}

extern "C" const Authored_Scene*
authored_world_latest_scene(const Authored_World* world)
{
    const Authored_Document* document =
        world_latest_of_kind(world, Authored_Document_Scene);
    return document == 0 ? 0 : &document->value.scene;
}

extern "C" const Authored_Post_Process*
authored_world_latest_post_process(const Authored_World* world)
{
    const Authored_Document* document =
        world_latest_of_kind(world, Authored_Document_Post_Process);
    return document == 0 ? 0 : &document->value.post_process;
}

extern "C" uint32_t
authored_world_document_count(const Authored_World* world)
{
    if (world == 0 || world->world == 0)
    {
        return 0;
    }

    ecs_query_desc_t query_desc;
    memset(&query_desc, 0, sizeof(query_desc));
    query_desc.terms[0].id = world->document_component;
    ecs_query_t* query = ecs_query_init(
        (ecs_world_t*)world->world,
        &query_desc);
    if (query == 0)
    {
        return 0;
    }

    uint32_t count = 0;
    ecs_iter_t iterator = ecs_query_iter((ecs_world_t*)world->world, query);
    while (ecs_query_next(&iterator))
    {
        count += (uint32_t)iterator.count;
    }
    ecs_query_fini(query);
    return count;
}
