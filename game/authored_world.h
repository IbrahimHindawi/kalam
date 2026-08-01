#ifndef KALAM_GAME_AUTHORED_WORLD_H
#define KALAM_GAME_AUTHORED_WORLD_H

#include "author_types.h"
#include "authored_state.h"
#include "game_api.h"

/*
 * Flecs-backed storage for authored documents
 * (docs/entity_documents.md section 7).
 *
 * Every authored document becomes one entity in an ECS world, carrying the
 * component that matches its schema. The world is an implementation detail
 * of the game module: it lives entirely behind the unchanged bridge, and
 * neither engine/, editor/, nor bridge/ ever sees a flecs type. The
 * existing bridge, game, and reload suites are the conformance tests for
 * this storage, which is why the interface below mirrors the previous
 * plain-array API exactly.
 */

typedef struct Authored_World Authored_World;

#if defined(__cplusplus)
extern "C" {
#endif

/* Returns 0 when the ECS world could not be created. */
Authored_World* authored_world_create(void);
void authored_world_destroy(Authored_World* world);

Game_Result authored_world_apply_document(
    Authored_World* world,
    const Author_Document_State* document,
    Game_Error* out_error);

Game_Result authored_world_remove_document(
    Authored_World* world,
    Author_Document_Id document_id,
    Game_Error* out_error);

Game_Result authored_world_read_field(
    const Authored_World* world,
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    Author_Field_Id field_id,
    Author_Value* out_value,
    Game_Error* out_error);

Game_Result authored_world_preview_field(
    Authored_World* world,
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    Author_Field_Id field_id,
    const Author_Value* value,
    Game_Error* out_error);

/*
 * The newest active scene document, for selection bookkeeping. Returns a
 * pointer into ECS component storage that is valid until the next apply,
 * preview, or remove.
 */
const Authored_Document* authored_world_latest_scene_document(
    const Authored_World* world);

const Authored_Scene* authored_world_latest_scene(
    const Authored_World* world);
const Authored_Post_Process* authored_world_latest_post_process(
    const Authored_World* world);

/* Diagnostics: how many authored entities the world currently holds. */
uint32_t authored_world_document_count(const Authored_World* world);

#if defined(__cplusplus)
}
#endif

#endif
