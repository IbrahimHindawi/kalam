#ifndef KALAM_BRIDGE_GAME_API_H
#define KALAM_BRIDGE_GAME_API_H

#include <stdint.h>

#include "author_schema.h"

typedef uint32_t Game_Result;

enum
{
    Host_API_Version = 1,
    Game_API_Version = 1
};

enum
{
    Game_Result_Ok = 0,
    Game_Result_Invalid_Argument = 1,
    Game_Result_Unsupported_Schema = 2,
    Game_Result_Invalid_Document = 3,
    Game_Result_Create_Failed = 4,
    Game_Result_Device_Failed = 5,
    Game_Result_Internal_Error = 6
};

typedef struct Game_Error
{
    String_View message;
    String_View json_path;
} Game_Error;

/*
 * The render target and both opaque D3D11 interface pointers are borrowed for
 * one render call. The module must not AddRef, release, resize, or present
 * them. A generation change invalidates target-sized module resources.
 */
typedef struct Host_Render_Target
{
    void* d3d11_render_target_view;
    void* d3d11_depth_stencil_view;
    uint32_t width;
    uint32_t height;
    uint64_t generation;
} Host_Render_Target;

typedef uint32_t Game_Key;

/*
 * These values are engine ABI indices, not Qt keys or Win32 virtual-key
 * ordinals. Platform adapters must translate explicitly into this space.
 * Unassigned values below Game_Key_Count remain reserved.
 */
enum
{
    Game_Key_Unknown = 0,

    Game_Key_A = 1,
    Game_Key_B = 2,
    Game_Key_C = 3,
    Game_Key_D = 4,
    Game_Key_E = 5,
    Game_Key_F = 6,
    Game_Key_G = 7,
    Game_Key_H = 8,
    Game_Key_I = 9,
    Game_Key_J = 10,
    Game_Key_K = 11,
    Game_Key_L = 12,
    Game_Key_M = 13,
    Game_Key_N = 14,
    Game_Key_O = 15,
    Game_Key_P = 16,
    Game_Key_Q = 17,
    Game_Key_R = 18,
    Game_Key_S = 19,
    Game_Key_T = 20,
    Game_Key_U = 21,
    Game_Key_V = 22,
    Game_Key_W = 23,
    Game_Key_X = 24,
    Game_Key_Y = 25,
    Game_Key_Z = 26,

    Game_Key_Digit_0 = 27,
    Game_Key_Digit_1 = 28,
    Game_Key_Digit_2 = 29,
    Game_Key_Digit_3 = 30,
    Game_Key_Digit_4 = 31,
    Game_Key_Digit_5 = 32,
    Game_Key_Digit_6 = 33,
    Game_Key_Digit_7 = 34,
    Game_Key_Digit_8 = 35,
    Game_Key_Digit_9 = 36,

    Game_Key_Escape = 37,
    Game_Key_Enter = 38,
    Game_Key_Tab = 39,
    Game_Key_Backspace = 40,
    Game_Key_Space = 41,
    Game_Key_Left = 42,
    Game_Key_Right = 43,
    Game_Key_Up = 44,
    Game_Key_Down = 45,
    Game_Key_Insert = 46,
    Game_Key_Delete = 47,
    Game_Key_Home = 48,
    Game_Key_End = 49,
    Game_Key_Page_Up = 50,
    Game_Key_Page_Down = 51,

    Game_Key_Left_Shift = 52,
    Game_Key_Right_Shift = 53,
    Game_Key_Left_Control = 54,
    Game_Key_Right_Control = 55,
    Game_Key_Left_Alt = 56,
    Game_Key_Right_Alt = 57,
    Game_Key_Left_Super = 58,
    Game_Key_Right_Super = 59,

    Game_Key_Caps_Lock = 60,
    Game_Key_Num_Lock = 61,
    Game_Key_Scroll_Lock = 62,
    Game_Key_Print_Screen = 63,
    Game_Key_Pause = 64,

    Game_Key_F1 = 65,
    Game_Key_F2 = 66,
    Game_Key_F3 = 67,
    Game_Key_F4 = 68,
    Game_Key_F5 = 69,
    Game_Key_F6 = 70,
    Game_Key_F7 = 71,
    Game_Key_F8 = 72,
    Game_Key_F9 = 73,
    Game_Key_F10 = 74,
    Game_Key_F11 = 75,
    Game_Key_F12 = 76,
    Game_Key_F13 = 77,
    Game_Key_F14 = 78,
    Game_Key_F15 = 79,
    Game_Key_F16 = 80,
    Game_Key_F17 = 81,
    Game_Key_F18 = 82,
    Game_Key_F19 = 83,
    Game_Key_F20 = 84,
    Game_Key_F21 = 85,
    Game_Key_F22 = 86,
    Game_Key_F23 = 87,
    Game_Key_F24 = 88,

    Game_Key_Apostrophe = 89,
    Game_Key_Comma = 90,
    Game_Key_Minus = 91,
    Game_Key_Period = 92,
    Game_Key_Slash = 93,
    Game_Key_Semicolon = 94,
    Game_Key_Equal = 95,
    Game_Key_Left_Bracket = 96,
    Game_Key_Backslash = 97,
    Game_Key_Right_Bracket = 98,
    Game_Key_Grave_Accent = 99,

    Game_Key_Keypad_0 = 100,
    Game_Key_Keypad_1 = 101,
    Game_Key_Keypad_2 = 102,
    Game_Key_Keypad_3 = 103,
    Game_Key_Keypad_4 = 104,
    Game_Key_Keypad_5 = 105,
    Game_Key_Keypad_6 = 106,
    Game_Key_Keypad_7 = 107,
    Game_Key_Keypad_8 = 108,
    Game_Key_Keypad_9 = 109,
    Game_Key_Keypad_Decimal = 110,
    Game_Key_Keypad_Divide = 111,
    Game_Key_Keypad_Multiply = 112,
    Game_Key_Keypad_Subtract = 113,
    Game_Key_Keypad_Add = 114,
    Game_Key_Keypad_Enter = 115,
    Game_Key_Keypad_Equal = 116,

    Game_Key_Count = 256
};

typedef uint32_t Game_Mouse_Button;

enum
{
    Game_Mouse_Button_Left = 0,
    Game_Mouse_Button_Right = 1,
    Game_Mouse_Button_Middle = 2,
    Game_Mouse_Button_Auxiliary_1 = 3,
    Game_Mouse_Button_Auxiliary_2 = 4,
    Game_Mouse_Button_Auxiliary_3 = 5,
    Game_Mouse_Button_Auxiliary_4 = 6,
    Game_Mouse_Button_Auxiliary_5 = 7,
    Game_Mouse_Button_Count = 8
};

typedef struct Game_Button
{
    uint32_t is_down;
    uint32_t pressed_count;
    uint32_t released_count;
} Game_Button;

/*
 * One immutable, borrowed fixed-update snapshot. Edge counts and pointer/wheel
 * deltas accumulate until the first fixed update that consumes them.
 */
typedef struct Game_Input
{
    uint32_t struct_size;
    uint32_t flags;
    uint64_t fixed_tick_index;

    Game_Button keys[Game_Key_Count];
    Game_Button mouse_buttons[Game_Mouse_Button_Count];

    int32_t pointer_x_pixels;
    int32_t pointer_y_pixels;
    int32_t pointer_delta_x_pixels;
    int32_t pointer_delta_y_pixels;
    float wheel_delta;
} Game_Input;

typedef struct Game_Create_Desc
{
    uint32_t struct_size;
    uint32_t host_api_version;

    void* d3d11_device;
    void* d3d11_immediate_context;

    void* host_user;
    void (*host_log)(void* host_user, uint32_t level, String_View message);

    /*
     * Project root for manifest resources (docs/resource_manifest.md).
     * The module resolves generated resource-table paths against it and
     * borrows the bytes for the create call only.
     */
    String_View project_root;
} Game_Create_Desc;

typedef uint32_t Game_Viewport_Interaction_Kind;

enum
{
    Game_Viewport_Interaction_Select = 1,
    Game_Viewport_Interaction_Gizmo_Begin = 2,
    Game_Viewport_Interaction_Gizmo_Update = 3,
    Game_Viewport_Interaction_Gizmo_Commit = 4,
    Game_Viewport_Interaction_Gizmo_Cancel = 5,
    Game_Viewport_Interaction_Clear_Selection = 6
};

typedef struct Game_Viewport_Interaction
{
    uint32_t struct_size;
    Game_Viewport_Interaction_Kind kind;

    int32_t pointer_x_pixels;
    int32_t pointer_y_pixels;
    int32_t pointer_delta_x_pixels;
    int32_t pointer_delta_y_pixels;

    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t flags;
    uint32_t reserved;
} Game_Viewport_Interaction;

enum
{
    Game_Viewport_Result_Selection_Changed = 1u << 0,
    Game_Viewport_Result_Has_Selection = 1u << 1,
    Game_Viewport_Result_Gizmo_Active = 1u << 2,
    Game_Viewport_Result_Value_Changed = 1u << 3,
    Game_Viewport_Result_Commit_Requested = 1u << 4,
    Game_Viewport_Result_Cancelled = 1u << 5
};

typedef struct Game_Viewport_Interaction_Result
{
    uint32_t struct_size;
    uint32_t flags;

    Author_Document_Id document_id;
    Author_Object_Id object_id;
    Author_Field_Id field_id;
    Author_Value value;
} Game_Viewport_Interaction_Result;

typedef struct Game_API
{
    uint32_t api_version;
    uint32_t struct_size;

    Game_Result (*create)(
        const Game_Create_Desc* desc,
        void** out_game,
        Game_Error* out_error
    );

    void (*destroy)(
        void* game
    );

    void (*fixed_update)(
        void* game,
        const Game_Input* input,
        float fixed_delta_time
    );

    Game_Result (*render)(
        void* game,
        const Host_Render_Target* target,
        float interpolation_alpha,
        Game_Error* out_error
    );

    Game_Result (*apply_document)(
        void* game,
        const Author_Document_State* state,
        Game_Error* out_error
    );

    Game_Result (*remove_document)(
        void* game,
        Author_Document_Id document_id,
        Game_Error* out_error
    );

    uint32_t (*get_author_schema_count)(void);

    Game_Result (*get_author_schema)(
        uint32_t schema_index,
        Author_Schema_View* out_schema,
        Game_Error* out_error
    );

    Game_Result (*read_author_field)(
        void* game,
        Author_Document_Id document_id,
        Author_Object_Id object_id,
        Author_Field_Id field_id,
        Author_Value* out_value,
        Game_Error* out_error
    );

    Game_Result (*preview_author_field)(
        void* game,
        Author_Document_Id document_id,
        Author_Object_Id object_id,
        Author_Field_Id field_id,
        const Author_Value* value,
        Game_Error* out_error
    );

    /*
     * Optional editor capability. A null pointer means the module does not
     * expose engine-owned viewport picking/gizmos. Qt forwards only pixel
     * coordinates and input deltas; selection and transform validation stay
     * in the module.
     */
    Game_Result (*viewport_interaction)(
        void* game,
        const Game_Viewport_Interaction* interaction,
        Game_Viewport_Interaction_Result* out_result,
        Game_Error* out_error
    );
} Game_API;

typedef Game_Result (*Game_Get_API_Function)(
    uint32_t host_api_version,
    uint32_t host_game_api_struct_size,
    Game_API* out_api,
    Game_Error* out_error
);

/*
 * The module target defines KALAM_GAME_MODULE_EXPORTS. Hosts resolve this
 * function dynamically and therefore do not import-link against the module.
 */
#if defined(_WIN32) && defined(KALAM_GAME_MODULE_EXPORTS)
#define KALAM_GAME_API_EXPORT __declspec(dllexport)
#else
#define KALAM_GAME_API_EXPORT
#endif

#if defined(__cplusplus)
extern "C" {
#endif

KALAM_GAME_API_EXPORT
Game_Result game_get_api(
    uint32_t host_api_version,
    uint32_t host_game_api_struct_size,
    Game_API* out_api,
    Game_Error* out_error
);

#if defined(__cplusplus)
}
#endif

#endif /* KALAM_BRIDGE_GAME_API_H */
