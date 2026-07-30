#include "game_api.h"

#include <cstddef>
#include <cstdint>

template<typename Left, typename Right>
struct Types_Are_Equal
{
    enum { value = 0 };
};

template<typename Value>
struct Types_Are_Equal<Value, Value>
{
    enum { value = 1 };
};

static_assert(sizeof(Game_Result) == 4, "Game_Result size");
static_assert(Host_API_Version == 1, "host API version is ABI");
static_assert(Game_API_Version == 1, "game API version is ABI");
static_assert(Game_Result_Ok == 0, "result values are ABI");
static_assert(Game_Result_Internal_Error == 6, "result values are ABI");
static_assert(Game_Key_Count == 256, "key count is ABI");
static_assert(Game_Mouse_Button_Count == 8, "mouse button count is ABI");

static_assert(offsetof(Game_Error, message) == 0, "error message offset");
static_assert(offsetof(Game_Error, json_path) == 16, "error path offset");
static_assert(sizeof(Game_Error) == 32, "error size");
static_assert(alignof(Game_Error) == 8, "error alignment");

#if UINTPTR_MAX == UINT64_MAX
static_assert(offsetof(Host_Render_Target, d3d11_render_target_view) == 0, "render view offset");
static_assert(offsetof(Host_Render_Target, d3d11_depth_stencil_view) == 8, "depth view offset");
static_assert(offsetof(Host_Render_Target, width) == 16, "render width offset");
static_assert(offsetof(Host_Render_Target, height) == 20, "render height offset");
static_assert(offsetof(Host_Render_Target, generation) == 24, "render generation offset");
static_assert(sizeof(Host_Render_Target) == 32, "render target size");
static_assert(alignof(Host_Render_Target) == 8, "render target alignment");
#elif UINTPTR_MAX == UINT32_MAX
static_assert(offsetof(Host_Render_Target, d3d11_render_target_view) == 0, "render view offset");
static_assert(offsetof(Host_Render_Target, d3d11_depth_stencil_view) == 4, "depth view offset");
static_assert(offsetof(Host_Render_Target, width) == 8, "render width offset");
static_assert(offsetof(Host_Render_Target, height) == 12, "render height offset");
static_assert(offsetof(Host_Render_Target, generation) == 16, "render generation offset");
static_assert(sizeof(Host_Render_Target) == 24, "render target size");
static_assert(alignof(Host_Render_Target) == 8, "render target alignment");
#else
#error Unsupported pointer size
#endif

static_assert(offsetof(Game_Button, is_down) == 0, "button down offset");
static_assert(offsetof(Game_Button, pressed_count) == 4, "button press offset");
static_assert(offsetof(Game_Button, released_count) == 8, "button release offset");
static_assert(sizeof(Game_Button) == 12, "button size");
static_assert(alignof(Game_Button) == 4, "button alignment");

static_assert(offsetof(Game_Input, struct_size) == 0, "input size offset");
static_assert(offsetof(Game_Input, flags) == 4, "input flags offset");
static_assert(offsetof(Game_Input, fixed_tick_index) == 8, "input tick offset");
static_assert(offsetof(Game_Input, keys) == 16, "input keys offset");
static_assert(offsetof(Game_Input, mouse_buttons) == 3088, "input mouse offset");
static_assert(offsetof(Game_Input, pointer_x_pixels) == 3184, "input pointer x offset");
static_assert(offsetof(Game_Input, pointer_y_pixels) == 3188, "input pointer y offset");
static_assert(offsetof(Game_Input, pointer_delta_x_pixels) == 3192, "input pointer dx offset");
static_assert(offsetof(Game_Input, pointer_delta_y_pixels) == 3196, "input pointer dy offset");
static_assert(offsetof(Game_Input, wheel_delta) == 3200, "input wheel offset");
static_assert(sizeof(Game_Input) == 3208, "input size");
static_assert(alignof(Game_Input) == 8, "input alignment");

static_assert(sizeof(Game_Viewport_Interaction) == 40, "viewport interaction size");
static_assert(alignof(Game_Viewport_Interaction) == 4, "viewport interaction alignment");
static_assert(offsetof(Game_Viewport_Interaction_Result, value) == 32, "viewport result value offset");
static_assert(sizeof(Game_Viewport_Interaction_Result) == 80, "viewport result size");
static_assert(alignof(Game_Viewport_Interaction_Result) == 8, "viewport result alignment");

#if UINTPTR_MAX == UINT64_MAX
static_assert(offsetof(Game_Create_Desc, struct_size) == 0, "create size offset");
static_assert(offsetof(Game_Create_Desc, host_api_version) == 4, "create version offset");
static_assert(offsetof(Game_Create_Desc, d3d11_device) == 8, "create device offset");
static_assert(offsetof(Game_Create_Desc, d3d11_immediate_context) == 16, "create context offset");
static_assert(offsetof(Game_Create_Desc, host_user) == 24, "create host user offset");
static_assert(offsetof(Game_Create_Desc, host_log) == 32, "create host log offset");
static_assert(sizeof(Game_Create_Desc) == 40, "create descriptor size");
static_assert(alignof(Game_Create_Desc) == 8, "create descriptor alignment");

static_assert(offsetof(Game_API, api_version) == 0, "API version offset");
static_assert(offsetof(Game_API, struct_size) == 4, "API size offset");
static_assert(offsetof(Game_API, create) == 8, "API create offset");
static_assert(offsetof(Game_API, destroy) == 16, "API destroy offset");
static_assert(offsetof(Game_API, fixed_update) == 24, "API update offset");
static_assert(offsetof(Game_API, render) == 32, "API render offset");
static_assert(offsetof(Game_API, apply_document) == 40, "API apply offset");
static_assert(offsetof(Game_API, remove_document) == 48, "API remove offset");
static_assert(offsetof(Game_API, get_author_schema_count) == 56, "API schema count offset");
static_assert(offsetof(Game_API, get_author_schema) == 64, "API schema offset");
static_assert(offsetof(Game_API, read_author_field) == 72, "API field read offset");
static_assert(offsetof(Game_API, preview_author_field) == 80, "API field preview offset");
static_assert(offsetof(Game_API, viewport_interaction) == 88, "API viewport interaction offset");
static_assert(sizeof(Game_API) == 96, "API table size");
static_assert(alignof(Game_API) == 8, "API table alignment");
#elif UINTPTR_MAX == UINT32_MAX
static_assert(offsetof(Game_Create_Desc, struct_size) == 0, "create size offset");
static_assert(offsetof(Game_Create_Desc, host_api_version) == 4, "create version offset");
static_assert(offsetof(Game_Create_Desc, d3d11_device) == 8, "create device offset");
static_assert(offsetof(Game_Create_Desc, d3d11_immediate_context) == 12, "create context offset");
static_assert(offsetof(Game_Create_Desc, host_user) == 16, "create host user offset");
static_assert(offsetof(Game_Create_Desc, host_log) == 20, "create host log offset");
static_assert(sizeof(Game_Create_Desc) == 24, "create descriptor size");
static_assert(alignof(Game_Create_Desc) == 4, "create descriptor alignment");

static_assert(offsetof(Game_API, api_version) == 0, "API version offset");
static_assert(offsetof(Game_API, struct_size) == 4, "API size offset");
static_assert(offsetof(Game_API, create) == 8, "API create offset");
static_assert(offsetof(Game_API, destroy) == 12, "API destroy offset");
static_assert(offsetof(Game_API, fixed_update) == 16, "API update offset");
static_assert(offsetof(Game_API, render) == 20, "API render offset");
static_assert(offsetof(Game_API, apply_document) == 24, "API apply offset");
static_assert(offsetof(Game_API, remove_document) == 28, "API remove offset");
static_assert(offsetof(Game_API, get_author_schema_count) == 32, "API schema count offset");
static_assert(offsetof(Game_API, get_author_schema) == 36, "API schema offset");
static_assert(offsetof(Game_API, read_author_field) == 40, "API field read offset");
static_assert(offsetof(Game_API, preview_author_field) == 44, "API field preview offset");
static_assert(offsetof(Game_API, viewport_interaction) == 48, "API viewport interaction offset");
static_assert(sizeof(Game_API) == 52, "API table size");
static_assert(alignof(Game_API) == 4, "API table alignment");
#else
#error Unsupported pointer size
#endif

static_assert(
    Types_Are_Equal<decltype(&game_get_api), Game_Get_API_Function>::value,
    "game_get_api must retain C ABI signature"
);

int main()
{
    return 0;
}
