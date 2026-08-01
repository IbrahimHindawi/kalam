#ifndef KALAM_GAME_AUTHORED_STATE_H
#define KALAM_GAME_AUTHORED_STATE_H

#include "author_types.h"
#include "game_api.h"

enum
{
    Authored_Document_Capacity = 32
};

typedef uint32_t Authored_Document_Kind;

enum
{
    Authored_Document_None = 0,
    Authored_Document_Scene = 1,
    Authored_Document_Post_Process = 2
};

typedef struct Authored_Document
{
    Author_Document_Id document_id;
    Author_Object_Id root_object_id;
    Author_Object_Id primary_object_id;
    uint64_t revision;
    Authored_Document_Kind kind;
    uint32_t active;

    union
    {
        Authored_Scene scene;
        Authored_Post_Process post_process;
    } value;
} Authored_Document;

typedef struct Authored_State
{
    Authored_Document documents[Authored_Document_Capacity];
    uint32_t document_count;
} Authored_State;

#if defined(__cplusplus)
extern "C" {
#endif

void authored_state_init(Authored_State* state);

Game_Result authored_state_apply_document(
    Authored_State* state,
    const Author_Document_State* document,
    Game_Error* out_error);

Game_Result authored_state_remove_document(
    Authored_State* state,
    Author_Document_Id document_id,
    Game_Error* out_error);

Game_Result authored_state_read_field(
    const Authored_State* state,
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    Author_Field_Id field_id,
    Author_Value* out_value,
    Game_Error* out_error);

Game_Result authored_state_preview_field(
    Authored_State* state,
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    Author_Field_Id field_id,
    const Author_Value* value,
    Game_Error* out_error);

/*
 * Document-level operations. The array store above and the ECS store in
 * authored_world.h share these, so parsing, validation, range checks, and
 * field mapping exist exactly once.
 */
Game_Result authored_document_parse(
    const Author_Document_State* document,
    Authored_Document* out_document,
    Game_Error* out_error);

Game_Result authored_document_read_field(
    const Authored_Document* document,
    Author_Field_Id field_id,
    Author_Value* out_value,
    Game_Error* out_error);

Game_Result authored_document_preview_field(
    Authored_Document* document,
    Author_Field_Id field_id,
    const Author_Value* value,
    Game_Error* out_error);

const Authored_Scene* authored_state_latest_scene(const Authored_State* state);
const Authored_Post_Process* authored_state_latest_post_process(
    const Authored_State* state);

#if defined(__cplusplus)
}
#endif

#endif
