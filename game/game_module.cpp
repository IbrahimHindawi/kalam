#include "game_api.h"

#include "authored_state.h"
#include "author_schemas.generated.h"
#include "module_renderer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <math.h>
#include <string.h>

typedef struct Game_Instance
{
    Authored_State authored_state;
    Module_Renderer renderer;
    uint64_t fixed_tick_count;

    Author_Document_Id selected_document_id;
    Author_Object_Id selected_object_id;
    Author_Field_Id selected_field_id;
    Author_Value gizmo_original_value;
    Author_Value gizmo_current_value;
    uint32_t selection_active;
    uint32_t gizmo_active;
    uint32_t gizmo_axis;
} Game_Instance;

static void
game_clear_error(Game_Error* out_error)
{
    if (out_error != 0)
    {
        memset(out_error, 0, sizeof(*out_error));
    }
}

static Game_Result
game_fail(
    Game_Result result,
    const char* message,
    const char* json_path,
    Game_Error* out_error)
{
    if (out_error != 0)
    {
        out_error->message.data = message;
        out_error->message.size =
            message != 0 ? (uint64_t)strlen(message) : 0;
        out_error->json_path.data = json_path;
        out_error->json_path.size =
            json_path != 0 ? (uint64_t)strlen(json_path) : 0;
    }
    return result;
}

static Game_Result
game_renderer_fail(
    Module_Renderer_Result renderer_result,
    Game_Error* out_error)
{
    Game_Result result = Game_Result_Device_Failed;

    if (renderer_result == Module_Renderer_Result_Invalid_Argument)
    {
        result = Game_Result_Invalid_Argument;
    }
    return game_fail(
        result,
        module_renderer_result_string(renderer_result),
        "",
        out_error);
}

static const Authored_Document*
game_latest_scene_document(const Authored_State* state)
{
    const Authored_Document* result = 0;
    uint64_t latest_revision = 0;
    uint32_t index = 0;

    if (state == 0)
    {
        return 0;
    }
    for (index = 0; index < Authored_Document_Capacity; index += 1)
    {
        const Authored_Document* document = &state->documents[index];
        if (document->active != 0 &&
            document->kind == Authored_Document_Scene &&
            document->revision >= latest_revision)
        {
            latest_revision = document->revision;
            result = document;
        }
    }
    return result;
}

static void
game_clear_selection(Game_Instance* game)
{
    if (game == 0)
    {
        return;
    }
    game->selected_document_id = 0;
    game->selected_object_id = 0;
    game->selected_field_id = 0;
    memset(&game->gizmo_original_value, 0, sizeof(game->gizmo_original_value));
    memset(&game->gizmo_current_value, 0, sizeof(game->gizmo_current_value));
    game->selection_active = 0;
    game->gizmo_active = 0;
    game->gizmo_axis = 0;
}

static void
game_fill_viewport_result(
    const Game_Instance* game,
    Game_Viewport_Interaction_Result* out_result)
{
    memset(out_result, 0, sizeof(*out_result));
    out_result->struct_size = (uint32_t)sizeof(*out_result);
    if (game != 0 && game->selection_active != 0)
    {
        out_result->flags |= Game_Viewport_Result_Has_Selection;
        out_result->document_id = game->selected_document_id;
        out_result->object_id = game->selected_object_id;
        out_result->field_id = game->selected_field_id;
    }
    if (game != 0 && game->gizmo_active != 0)
    {
        out_result->flags |= Game_Viewport_Result_Gizmo_Active;
    }
}

static void
game_scene_center_pixels(
    const Authored_Scene* scene,
    uint32_t viewport_width,
    uint32_t viewport_height,
    float* out_x,
    float* out_y,
    float* out_radius)
{
    const float vertical_scale = 1.7320508075688772f;
    float depth = scene->cube_transform.translation[2] + 5.0f;
    float aspect = (float)viewport_width / (float)viewport_height;
    float horizontal_scale = vertical_scale / aspect;
    float maximum_scale = scene->cube_transform.scale[0];

    if (depth < 0.25f)
    {
        depth = 0.25f;
    }
    if (scene->cube_transform.scale[1] > maximum_scale)
    {
        maximum_scale = scene->cube_transform.scale[1];
    }
    if (scene->cube_transform.scale[2] > maximum_scale)
    {
        maximum_scale = scene->cube_transform.scale[2];
    }

    *out_x =
        ((scene->cube_transform.translation[0] * horizontal_scale / depth) *
             0.5f +
         0.5f) *
        (float)viewport_width;
    *out_y =
        (0.5f -
         (scene->cube_transform.translation[1] * vertical_scale / depth) *
             0.5f) *
        (float)viewport_height;
    *out_radius =
        maximum_scale * vertical_scale / depth *
        (float)viewport_height * 0.5f;
    if (*out_radius < 16.0f)
    {
        *out_radius = 16.0f;
    }
}

static float
game_point_segment_distance_squared(
    float point_x,
    float point_y,
    float start_x,
    float start_y,
    float end_x,
    float end_y)
{
    float segment_x = end_x - start_x;
    float segment_y = end_y - start_y;
    float length_squared = segment_x * segment_x + segment_y * segment_y;
    float amount = 0.0f;
    float nearest_x = start_x;
    float nearest_y = start_y;
    float delta_x = 0.0f;
    float delta_y = 0.0f;

    if (length_squared > 0.0f)
    {
        amount =
            ((point_x - start_x) * segment_x +
             (point_y - start_y) * segment_y) /
            length_squared;
        if (amount < 0.0f)
        {
            amount = 0.0f;
        }
        else if (amount > 1.0f)
        {
            amount = 1.0f;
        }
        nearest_x += segment_x * amount;
        nearest_y += segment_y * amount;
    }
    delta_x = point_x - nearest_x;
    delta_y = point_y - nearest_y;
    return delta_x * delta_x + delta_y * delta_y;
}

static Game_Result
game_create(
    const Game_Create_Desc* desc,
    void** out_game,
    Game_Error* out_error)
{
    Game_Instance* game = 0;
    Module_Renderer_Result renderer_result = Module_Renderer_Result_Ok;

    game_clear_error(out_error);
    if (out_game != 0)
    {
        *out_game = 0;
    }

    if (desc == 0 ||
        out_game == 0 ||
        desc->struct_size < (uint32_t)sizeof(*desc) ||
        desc->host_api_version != Host_API_Version ||
        desc->d3d11_device == 0 ||
        desc->d3d11_immediate_context == 0)
    {
        return game_fail(
            Game_Result_Invalid_Argument,
            "invalid game creation descriptor",
            "",
            out_error);
    }

    game = (Game_Instance*)HeapAlloc(
        GetProcessHeap(),
        HEAP_ZERO_MEMORY,
        sizeof(*game));
    if (game == 0)
    {
        return game_fail(
            Game_Result_Create_Failed,
            "could not allocate the game instance",
            "",
            out_error);
    }

    authored_state_init(&game->authored_state);
    renderer_result = module_renderer_create(
        &game->renderer,
        desc->d3d11_device,
        desc->d3d11_immediate_context);
    if (renderer_result != Module_Renderer_Result_Ok)
    {
        module_renderer_destroy(&game->renderer);
        HeapFree(GetProcessHeap(), 0, game);
        return game_renderer_fail(renderer_result, out_error);
    }

    *out_game = game;
    if (desc->host_log != 0)
    {
        static const char message[] = "game module instance created";
        String_View view;
        view.data = message;
        view.size = (uint64_t)(sizeof(message) - 1);
        desc->host_log(desc->host_user, 0, view);
    }
    return Game_Result_Ok;
}

static void
game_destroy(void* game_pointer)
{
    Game_Instance* game = (Game_Instance*)game_pointer;

    if (game == 0)
    {
        return;
    }

    module_renderer_destroy(&game->renderer);
    memset(&game->authored_state, 0, sizeof(game->authored_state));
    game->fixed_tick_count = 0;
    HeapFree(GetProcessHeap(), 0, game);
}

static void
game_fixed_update(
    void* game_pointer,
    const Game_Input* input,
    float fixed_delta_time)
{
    Game_Instance* game = (Game_Instance*)game_pointer;

    if (game == 0 ||
        input == 0 ||
        input->struct_size < (uint32_t)sizeof(*input) ||
        fixed_delta_time <= 0.0f)
    {
        return;
    }

    /*
     * Revision 1 has no autonomous cube animation. The fixed step advances
     * only authoritative simulation bookkeeping; renderer state and authored
     * transforms are untouched.
     */
    if (game->fixed_tick_count != UINT64_MAX)
    {
        game->fixed_tick_count += 1;
    }
}

static Game_Result
game_render(
    void* game_pointer,
    const Host_Render_Target* target,
    float interpolation_alpha,
    Game_Error* out_error)
{
    Game_Instance* game = (Game_Instance*)game_pointer;
    const Authored_Scene* scene = 0;
    const Authored_Post_Process* post_process = 0;
    Module_Render_Cube cube;
    Module_Render_Post_Process render_post_process;
    const Module_Render_Post_Process* render_post_process_pointer = 0;
    Module_Render_Target render_target;
    Module_Renderer_Result renderer_result = Module_Renderer_Result_Ok;

    (void)interpolation_alpha;
    game_clear_error(out_error);
    if (game == 0 || target == 0)
    {
        return game_fail(
            Game_Result_Invalid_Argument,
            "invalid game render call",
            "",
            out_error);
    }

    /*
     * An unavailable/minimized target is an expected host state, not a module
     * error. Do not touch either borrowed view in this case.
     */
    if (target->d3d11_render_target_view == 0 ||
        target->width == 0 ||
        target->height == 0)
    {
        return Game_Result_Ok;
    }

    memset(&render_target, 0, sizeof(render_target));
    render_target.d3d11_render_target_view =
        target->d3d11_render_target_view;
    render_target.d3d11_depth_stencil_view =
        target->d3d11_depth_stencil_view;
    render_target.width = target->width;
    render_target.height = target->height;
    render_target.generation = target->generation;

    scene = authored_state_latest_scene(&game->authored_state);
    post_process =
        authored_state_latest_post_process(&game->authored_state);
    if (post_process != 0)
    {
        memset(&render_post_process, 0, sizeof(render_post_process));
        render_post_process.exposure = post_process->exposure;
        render_post_process.bloom_threshold =
            post_process->bloom_threshold;
        render_post_process.vignette = post_process->vignette;
        memcpy(
            render_post_process.tint,
            post_process->tint,
            sizeof(render_post_process.tint));
        render_post_process_pointer = &render_post_process;
    }

    if (scene == 0)
    {
        renderer_result = module_renderer_render_cube(
            &game->renderer,
            &render_target,
            0,
            render_post_process_pointer);
    }
    else
    {
        memset(&cube, 0, sizeof(cube));
        memcpy(
            cube.translation,
            scene->cube_transform.translation,
            sizeof(cube.translation));
        memcpy(
            cube.rotation_degrees,
            scene->cube_transform.rotation_degrees,
            sizeof(cube.rotation_degrees));
        memcpy(
            cube.scale,
            scene->cube_transform.scale,
            sizeof(cube.scale));
        memcpy(cube.color, scene->cube_color, sizeof(cube.color));
        cube.draw_transform_gizmo = game->selection_active;

        renderer_result = module_renderer_render_cube(
            &game->renderer,
            &render_target,
            &cube,
            render_post_process_pointer);
    }

    if (renderer_result != Module_Renderer_Result_Ok)
    {
        return game_renderer_fail(renderer_result, out_error);
    }
    return Game_Result_Ok;
}

static Game_Result
game_viewport_interaction(
    void* game_pointer,
    const Game_Viewport_Interaction* interaction,
    Game_Viewport_Interaction_Result* out_result,
    Game_Error* out_error)
{
    Game_Instance* game = (Game_Instance*)game_pointer;
    const Authored_Document* document = 0;
    const Authored_Scene* scene = 0;
    Game_Result result = Game_Result_Ok;
    float center_x = 0.0f;
    float center_y = 0.0f;
    float radius = 0.0f;

    game_clear_error(out_error);
    if (out_result != 0)
    {
        memset(out_result, 0, sizeof(*out_result));
    }
    if (game == 0 || interaction == 0 || out_result == 0 ||
        interaction->struct_size < (uint32_t)sizeof(*interaction) ||
        interaction->viewport_width == 0 ||
        interaction->viewport_height == 0)
    {
        return game_fail(
            Game_Result_Invalid_Argument,
            "invalid viewport interaction",
            "",
            out_error);
    }

    document = game_latest_scene_document(&game->authored_state);
    scene = document != 0 ? &document->value.scene : 0;
    if (scene != 0)
    {
        game_scene_center_pixels(
            scene,
            interaction->viewport_width,
            interaction->viewport_height,
            &center_x,
            &center_y,
            &radius);
    }

    if (interaction->kind == Game_Viewport_Interaction_Select)
    {
        float delta_x = (float)interaction->pointer_x_pixels - center_x;
        float delta_y = (float)interaction->pointer_y_pixels - center_y;
        uint32_t hit = scene != 0 &&
            fabsf(delta_x) <= radius &&
            fabsf(delta_y) <= radius;

        if (game->gizmo_active != 0)
        {
            (void)authored_state_preview_field(
                &game->authored_state,
                game->selected_document_id,
                game->selected_object_id,
                game->selected_field_id,
                &game->gizmo_original_value,
                0);
        }
        game_clear_selection(game);
        if (hit != 0)
        {
            game->selected_document_id = document->document_id;
            game->selected_object_id = document->primary_object_id;
            game->selected_field_id = Author_Field_Id_Scene_Transform;
            game->selection_active = 1;
        }
        game_fill_viewport_result(game, out_result);
        out_result->flags |= Game_Viewport_Result_Selection_Changed;
        return Game_Result_Ok;
    }

    if (interaction->kind == Game_Viewport_Interaction_Clear_Selection)
    {
        if (game->gizmo_active != 0)
        {
            (void)authored_state_preview_field(
                &game->authored_state,
                game->selected_document_id,
                game->selected_object_id,
                game->selected_field_id,
                &game->gizmo_original_value,
                0);
        }
        game_clear_selection(game);
        game_fill_viewport_result(game, out_result);
        out_result->flags |= Game_Viewport_Result_Selection_Changed;
        return Game_Result_Ok;
    }

    if (game->selection_active == 0 || scene == 0 ||
        document->document_id != game->selected_document_id)
    {
        game_fill_viewport_result(game, out_result);
        return Game_Result_Ok;
    }

    if (interaction->kind == Game_Viewport_Interaction_Gizmo_Begin)
    {
        float point_x = (float)interaction->pointer_x_pixels;
        float point_y = (float)interaction->pointer_y_pixels;
        float distance_x = game_point_segment_distance_squared(
            point_x,
            point_y,
            center_x + radius * 0.35f,
            center_y,
            center_x + radius + 64.0f,
            center_y);
        float distance_y = game_point_segment_distance_squared(
            point_x,
            point_y,
            center_x,
            center_y - radius * 0.35f,
            center_x,
            center_y - radius - 64.0f);
        float distance_z = game_point_segment_distance_squared(
            point_x,
            point_y,
            center_x + radius * 0.25f,
            center_y + radius * 0.25f,
            center_x + radius + 45.0f,
            center_y + radius + 45.0f);
        float best_distance = distance_x;
        uint32_t axis = 1;

        if (distance_y < best_distance)
        {
            best_distance = distance_y;
            axis = 2;
        }
        if (distance_z < best_distance)
        {
            best_distance = distance_z;
            axis = 3;
        }
        if (best_distance <= 144.0f)
        {
            result = authored_state_read_field(
                &game->authored_state,
                game->selected_document_id,
                game->selected_object_id,
                game->selected_field_id,
                &game->gizmo_original_value,
                out_error);
            if (result != Game_Result_Ok)
            {
                return result;
            }
            game->gizmo_current_value = game->gizmo_original_value;
            game->gizmo_axis = axis;
            game->gizmo_active = 1;
        }
        game_fill_viewport_result(game, out_result);
        return Game_Result_Ok;
    }

    if (interaction->kind == Game_Viewport_Interaction_Gizmo_Update)
    {
        if (game->gizmo_active == 0 ||
            game->gizmo_current_value.kind != Author_Field_Transform)
        {
            game_fill_viewport_result(game, out_result);
            return Game_Result_Ok;
        }
        if (game->gizmo_axis == 1)
        {
            game->gizmo_current_value.data.transform.translation[0] +=
                (float)interaction->pointer_delta_x_pixels * 0.01f;
        }
        else if (game->gizmo_axis == 2)
        {
            game->gizmo_current_value.data.transform.translation[1] -=
                (float)interaction->pointer_delta_y_pixels * 0.01f;
        }
        else
        {
            game->gizmo_current_value.data.transform.translation[2] +=
                (float)(interaction->pointer_delta_x_pixels +
                        interaction->pointer_delta_y_pixels) *
                0.007f;
        }

        result = authored_state_preview_field(
            &game->authored_state,
            game->selected_document_id,
            game->selected_object_id,
            game->selected_field_id,
            &game->gizmo_current_value,
            out_error);
        if (result != Game_Result_Ok)
        {
            return result;
        }
        game_fill_viewport_result(game, out_result);
        out_result->flags |= Game_Viewport_Result_Value_Changed;
        out_result->value = game->gizmo_current_value;
        return Game_Result_Ok;
    }

    if (interaction->kind == Game_Viewport_Interaction_Gizmo_Commit)
    {
        if (game->gizmo_active != 0)
        {
            game->gizmo_active = 0;
            game->gizmo_axis = 0;
            game_fill_viewport_result(game, out_result);
            out_result->flags |=
                Game_Viewport_Result_Value_Changed |
                Game_Viewport_Result_Commit_Requested;
            out_result->value = game->gizmo_current_value;
            return Game_Result_Ok;
        }
        game_fill_viewport_result(game, out_result);
        return Game_Result_Ok;
    }

    if (interaction->kind == Game_Viewport_Interaction_Gizmo_Cancel)
    {
        if (game->gizmo_active != 0)
        {
            result = authored_state_preview_field(
                &game->authored_state,
                game->selected_document_id,
                game->selected_object_id,
                game->selected_field_id,
                &game->gizmo_original_value,
                out_error);
            if (result != Game_Result_Ok)
            {
                return result;
            }
            game->gizmo_current_value = game->gizmo_original_value;
            game->gizmo_active = 0;
            game->gizmo_axis = 0;
            game_fill_viewport_result(game, out_result);
            out_result->flags |=
                Game_Viewport_Result_Value_Changed |
                Game_Viewport_Result_Cancelled;
            out_result->value = game->gizmo_current_value;
            return Game_Result_Ok;
        }
        game_fill_viewport_result(game, out_result);
        return Game_Result_Ok;
    }

    return game_fail(
        Game_Result_Invalid_Argument,
        "unsupported viewport interaction kind",
        "",
        out_error);
}

static Game_Result
game_apply_document(
    void* game_pointer,
    const Author_Document_State* state,
    Game_Error* out_error)
{
    Game_Instance* game = (Game_Instance*)game_pointer;
    Game_Result result = Game_Result_Ok;

    game_clear_error(out_error);
    if (game == 0)
    {
        return game_fail(
            Game_Result_Invalid_Argument,
            "game instance is required to apply a document",
            "",
            out_error);
    }
    result = authored_state_apply_document(
        &game->authored_state,
        state,
        out_error);
    if (result == Game_Result_Ok &&
        game->selection_active != 0 &&
        game->selected_document_id == state->document_id)
    {
        const Authored_Document* scene_document =
            game_latest_scene_document(&game->authored_state);
        if (scene_document != 0 &&
            scene_document->document_id == state->document_id)
        {
            game->selected_object_id = scene_document->primary_object_id;
            game->gizmo_active = 0;
            game->gizmo_axis = 0;
        }
        else
        {
            game_clear_selection(game);
        }
    }
    return result;
}

static Game_Result
game_remove_document(
    void* game_pointer,
    Author_Document_Id document_id,
    Game_Error* out_error)
{
    Game_Instance* game = (Game_Instance*)game_pointer;

    game_clear_error(out_error);
    if (game == 0)
    {
        return game_fail(
            Game_Result_Invalid_Argument,
            "game instance is required to remove a document",
            "",
            out_error);
    }
    if (game->selected_document_id == document_id)
    {
        game_clear_selection(game);
    }
    return authored_state_remove_document(
        &game->authored_state,
        document_id,
        out_error);
}

static uint32_t
game_get_author_schema_count(void)
{
    return kalam_generated_author_schema_count;
}

static Game_Result
game_get_author_schema(
    uint32_t schema_index,
    Author_Schema_View* out_schema,
    Game_Error* out_error)
{
    game_clear_error(out_error);
    if (out_schema != 0)
    {
        memset(out_schema, 0, sizeof(*out_schema));
    }

    if (out_schema == 0 ||
        schema_index >= kalam_generated_author_schema_count)
    {
        return game_fail(
            Game_Result_Invalid_Argument,
            "author schema index is out of range",
            "",
            out_error);
    }

    *out_schema = kalam_generated_author_schemas[schema_index];
    return Game_Result_Ok;
}

static Game_Result
game_read_author_field(
    void* game_pointer,
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    Author_Field_Id field_id,
    Author_Value* out_value,
    Game_Error* out_error)
{
    Game_Instance* game = (Game_Instance*)game_pointer;

    game_clear_error(out_error);
    if (game == 0)
    {
        if (out_value != 0)
        {
            memset(out_value, 0, sizeof(*out_value));
        }
        return game_fail(
            Game_Result_Invalid_Argument,
            "game instance is required to read an authored field",
            "",
            out_error);
    }
    return authored_state_read_field(
        &game->authored_state,
        document_id,
        object_id,
        field_id,
        out_value,
        out_error);
}

static Game_Result
game_preview_author_field(
    void* game_pointer,
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    Author_Field_Id field_id,
    const Author_Value* value,
    Game_Error* out_error)
{
    Game_Instance* game = (Game_Instance*)game_pointer;

    game_clear_error(out_error);
    if (game == 0)
    {
        return game_fail(
            Game_Result_Invalid_Argument,
            "game instance is required to preview an authored field",
            "",
            out_error);
    }
    return authored_state_preview_field(
        &game->authored_state,
        document_id,
        object_id,
        field_id,
        value,
        out_error);
}

static uint32_t
game_api_has_all_required_functions(const Game_API* api)
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
        api->preview_author_field != 0 &&
        api->viewport_interaction != 0;
}

static uint32_t
game_generated_schemas_are_valid(void)
{
    uint32_t schema_index = 0;
    uint32_t schema_count = kalam_generated_author_schema_count;

    if (schema_count == 0)
    {
        return 0;
    }

    for (schema_index = 0;
         schema_index < schema_count;
         schema_index += 1)
    {
        const Author_Schema_View* schema =
            &kalam_generated_author_schemas[schema_index];
        uint32_t field_index = 0;

        if (schema->schema_id.data == 0 ||
            schema->schema_id.size == 0 ||
            schema->schema_version == 0 ||
            schema->fields == 0 ||
            schema->field_count == 0)
        {
            return 0;
        }

        for (field_index = 0;
             field_index < schema->field_count;
             field_index += 1)
        {
            const Author_Field_Desc* field =
                &schema->fields[field_index];
            if (field->id == 0 ||
                field->name.data == 0 ||
                field->name.size == 0 ||
                field->json_path.data == 0 ||
                field->json_path.size == 0)
            {
                return 0;
            }
        }
    }
    return 1;
}

extern "C" KALAM_GAME_API_EXPORT Game_Result
game_get_api(
    uint32_t host_api_version,
    uint32_t host_game_api_struct_size,
    Game_API* out_api,
    Game_Error* out_error)
{
    Game_API api;
    uint32_t clear_size = host_game_api_struct_size;

    game_clear_error(out_error);
    if (out_api == 0)
    {
        return game_fail(
            Game_Result_Invalid_Argument,
            "game API output table is required",
            "",
            out_error);
    }

    if (clear_size > (uint32_t)sizeof(*out_api))
    {
        clear_size = (uint32_t)sizeof(*out_api);
    }
    if (clear_size != 0)
    {
        memset(out_api, 0, clear_size);
    }

    if (host_api_version != Host_API_Version)
    {
        return game_fail(
            Game_Result_Invalid_Argument,
            "host API version is not supported",
            "",
            out_error);
    }
    if (host_game_api_struct_size < (uint32_t)sizeof(*out_api))
    {
        return game_fail(
            Game_Result_Invalid_Argument,
            "host Game_API table is too small",
            "",
            out_error);
    }

    memset(&api, 0, sizeof(api));
    api.api_version = Game_API_Version;
    api.struct_size = (uint32_t)sizeof(api);
    api.create = game_create;
    api.destroy = game_destroy;
    api.fixed_update = game_fixed_update;
    api.render = game_render;
    api.apply_document = game_apply_document;
    api.remove_document = game_remove_document;
    api.get_author_schema_count = game_get_author_schema_count;
    api.get_author_schema = game_get_author_schema;
    api.read_author_field = game_read_author_field;
    api.preview_author_field = game_preview_author_field;
    api.viewport_interaction = game_viewport_interaction;

    if (!game_api_has_all_required_functions(&api) ||
        !game_generated_schemas_are_valid())
    {
        return game_fail(
            Game_Result_Internal_Error,
            "game module API or generated schemas are incomplete",
            "",
            out_error);
    }

    memcpy(out_api, &api, sizeof(api));
    return Game_Result_Ok;
}
