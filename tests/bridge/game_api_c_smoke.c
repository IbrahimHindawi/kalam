#include "game_api.h"

#include <stddef.h>
#include <stdint.h>

_Static_assert(sizeof(Game_Result) == 4, "Game_Result size");
_Static_assert(Host_API_Version == 1, "host API version is ABI");
_Static_assert(Game_API_Version == 2, "game API version is ABI");
_Static_assert(Game_Result_Ok == 0, "result values are ABI");
_Static_assert(Game_Result_Internal_Error == 6, "result values are ABI");
_Static_assert(Game_Key_Count == 256, "key count is ABI");
_Static_assert(Game_Mouse_Button_Count == 8, "mouse button count is ABI");

_Static_assert(offsetof(Game_Error, message) == 0, "error message offset");
_Static_assert(offsetof(Game_Error, json_path) == 16, "error path offset");
_Static_assert(sizeof(Game_Error) == 32, "error size");
_Static_assert(_Alignof(Game_Error) == 8, "error alignment");

#if UINTPTR_MAX == UINT64_MAX
_Static_assert(offsetof(Host_Render_Target, d3d11_render_target_view) == 0, "render view offset");
_Static_assert(offsetof(Host_Render_Target, d3d11_depth_stencil_view) == 8, "depth view offset");
_Static_assert(offsetof(Host_Render_Target, width) == 16, "render width offset");
_Static_assert(offsetof(Host_Render_Target, height) == 20, "render height offset");
_Static_assert(offsetof(Host_Render_Target, generation) == 24, "render generation offset");
_Static_assert(sizeof(Host_Render_Target) == 32, "render target size");
_Static_assert(_Alignof(Host_Render_Target) == 8, "render target alignment");
#elif UINTPTR_MAX == UINT32_MAX
_Static_assert(offsetof(Host_Render_Target, d3d11_render_target_view) == 0, "render view offset");
_Static_assert(offsetof(Host_Render_Target, d3d11_depth_stencil_view) == 4, "depth view offset");
_Static_assert(offsetof(Host_Render_Target, width) == 8, "render width offset");
_Static_assert(offsetof(Host_Render_Target, height) == 12, "render height offset");
_Static_assert(offsetof(Host_Render_Target, generation) == 16, "render generation offset");
_Static_assert(sizeof(Host_Render_Target) == 24, "render target size");
_Static_assert(_Alignof(Host_Render_Target) == 8, "render target alignment");
#else
#error Unsupported pointer size
#endif

_Static_assert(offsetof(Game_Button, is_down) == 0, "button down offset");
_Static_assert(offsetof(Game_Button, pressed_count) == 4, "button press offset");
_Static_assert(offsetof(Game_Button, released_count) == 8, "button release offset");
_Static_assert(sizeof(Game_Button) == 12, "button size");
_Static_assert(_Alignof(Game_Button) == 4, "button alignment");

_Static_assert(offsetof(Game_Input, struct_size) == 0, "input size offset");
_Static_assert(offsetof(Game_Input, flags) == 4, "input flags offset");
_Static_assert(offsetof(Game_Input, fixed_tick_index) == 8, "input tick offset");
_Static_assert(offsetof(Game_Input, keys) == 16, "input keys offset");
_Static_assert(offsetof(Game_Input, mouse_buttons) == 3088, "input mouse offset");
_Static_assert(offsetof(Game_Input, pointer_x_pixels) == 3184, "input pointer x offset");
_Static_assert(offsetof(Game_Input, pointer_y_pixels) == 3188, "input pointer y offset");
_Static_assert(offsetof(Game_Input, pointer_delta_x_pixels) == 3192, "input pointer dx offset");
_Static_assert(offsetof(Game_Input, pointer_delta_y_pixels) == 3196, "input pointer dy offset");
_Static_assert(offsetof(Game_Input, wheel_delta) == 3200, "input wheel offset");
_Static_assert(sizeof(Game_Input) == 3208, "input size");
_Static_assert(_Alignof(Game_Input) == 8, "input alignment");

_Static_assert(sizeof(Game_Viewport_Interaction) == 56, "viewport interaction size");
_Static_assert(_Alignof(Game_Viewport_Interaction) == 8, "viewport interaction alignment");
_Static_assert(offsetof(Game_Viewport_Interaction_Result, value) == 32, "viewport result value offset");
_Static_assert(sizeof(Game_Viewport_Interaction_Result) == 80, "viewport result size");
_Static_assert(_Alignof(Game_Viewport_Interaction_Result) == 8, "viewport result alignment");

#if UINTPTR_MAX == UINT64_MAX
_Static_assert(offsetof(Game_Create_Desc, struct_size) == 0, "create size offset");
_Static_assert(offsetof(Game_Create_Desc, host_api_version) == 4, "create version offset");
_Static_assert(offsetof(Game_Create_Desc, d3d11_device) == 8, "create device offset");
_Static_assert(offsetof(Game_Create_Desc, d3d11_immediate_context) == 16, "create context offset");
_Static_assert(offsetof(Game_Create_Desc, host_user) == 24, "create host user offset");
_Static_assert(offsetof(Game_Create_Desc, host_log) == 32, "create host log offset");
_Static_assert(offsetof(Game_Create_Desc, project_root) == 40, "create project root offset");
_Static_assert(sizeof(Game_Create_Desc) == 56, "create descriptor size");
_Static_assert(_Alignof(Game_Create_Desc) == 8, "create descriptor alignment");

_Static_assert(offsetof(Game_API, api_version) == 0, "API version offset");
_Static_assert(offsetof(Game_API, struct_size) == 4, "API size offset");
_Static_assert(offsetof(Game_API, create) == 8, "API create offset");
_Static_assert(offsetof(Game_API, destroy) == 16, "API destroy offset");
_Static_assert(offsetof(Game_API, fixed_update) == 24, "API update offset");
_Static_assert(offsetof(Game_API, render) == 32, "API render offset");
_Static_assert(offsetof(Game_API, apply_document) == 40, "API apply offset");
_Static_assert(offsetof(Game_API, remove_document) == 48, "API remove offset");
_Static_assert(offsetof(Game_API, get_author_schema_count) == 56, "API schema count offset");
_Static_assert(offsetof(Game_API, get_author_schema) == 64, "API schema offset");
_Static_assert(offsetof(Game_API, read_author_field) == 72, "API field read offset");
_Static_assert(offsetof(Game_API, preview_author_field) == 80, "API field preview offset");
_Static_assert(offsetof(Game_API, viewport_interaction) == 88, "API viewport interaction offset");
_Static_assert(sizeof(Game_API) == 96, "API table size");
_Static_assert(_Alignof(Game_API) == 8, "API table alignment");
#elif UINTPTR_MAX == UINT32_MAX
_Static_assert(offsetof(Game_Create_Desc, struct_size) == 0, "create size offset");
_Static_assert(offsetof(Game_Create_Desc, host_api_version) == 4, "create version offset");
_Static_assert(offsetof(Game_Create_Desc, d3d11_device) == 8, "create device offset");
_Static_assert(offsetof(Game_Create_Desc, d3d11_immediate_context) == 12, "create context offset");
_Static_assert(offsetof(Game_Create_Desc, host_user) == 16, "create host user offset");
_Static_assert(offsetof(Game_Create_Desc, host_log) == 20, "create host log offset");
_Static_assert(offsetof(Game_Create_Desc, project_root) == 24, "create project root offset");
_Static_assert(sizeof(Game_Create_Desc) == 40, "create descriptor size");
_Static_assert(_Alignof(Game_Create_Desc) == 8, "create descriptor alignment");

_Static_assert(offsetof(Game_API, api_version) == 0, "API version offset");
_Static_assert(offsetof(Game_API, struct_size) == 4, "API size offset");
_Static_assert(offsetof(Game_API, create) == 8, "API create offset");
_Static_assert(offsetof(Game_API, destroy) == 12, "API destroy offset");
_Static_assert(offsetof(Game_API, fixed_update) == 16, "API update offset");
_Static_assert(offsetof(Game_API, render) == 20, "API render offset");
_Static_assert(offsetof(Game_API, apply_document) == 24, "API apply offset");
_Static_assert(offsetof(Game_API, remove_document) == 28, "API remove offset");
_Static_assert(offsetof(Game_API, get_author_schema_count) == 32, "API schema count offset");
_Static_assert(offsetof(Game_API, get_author_schema) == 36, "API schema offset");
_Static_assert(offsetof(Game_API, read_author_field) == 40, "API field read offset");
_Static_assert(offsetof(Game_API, preview_author_field) == 44, "API field preview offset");
_Static_assert(offsetof(Game_API, viewport_interaction) == 48, "API viewport interaction offset");
_Static_assert(sizeof(Game_API) == 52, "API table size");
_Static_assert(_Alignof(Game_API) == 4, "API table alignment");
#else
#error Unsupported pointer size
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(
    _Generic(
        &game_get_api,
        Game_Get_API_Function: 1,
        default: 0
    ),
    "game_get_api signature"
);
#endif

int main(void)
{
    return 0;
}
