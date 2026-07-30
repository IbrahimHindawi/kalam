#ifndef KALAM_EDITOR_HOST_INPUT_LATCH_H
#define KALAM_EDITOR_HOST_INPUT_LATCH_H

#include "game_api.h"

typedef struct Input_Latch
{
    Game_Button keys[Game_Key_Count];
    Game_Button mouse_buttons[Game_Mouse_Button_Count];

    int32_t pointer_x_pixels;
    int32_t pointer_y_pixels;
    int32_t pending_pointer_delta_x_pixels;
    int32_t pending_pointer_delta_y_pixels;
    float pending_wheel_delta;

    uint32_t flags;
    uint32_t focused;
} Input_Latch;

#if defined(__cplusplus)
extern "C" {
#endif

void input_latch_init(Input_Latch* latch);

uint32_t input_latch_set_key(
    Input_Latch* latch,
    Game_Key key,
    uint32_t is_down);

uint32_t input_latch_set_mouse_button(
    Input_Latch* latch,
    Game_Mouse_Button button,
    uint32_t is_down);

void input_latch_set_pointer_position(
    Input_Latch* latch,
    int32_t x_pixels,
    int32_t y_pixels);

void input_latch_add_pointer_delta(
    Input_Latch* latch,
    int32_t delta_x_pixels,
    int32_t delta_y_pixels);

void input_latch_add_wheel_delta(Input_Latch* latch, float delta);

void input_latch_focus_gained(Input_Latch* latch);
void input_latch_focus_lost(Input_Latch* latch);

/*
 * Produces the immutable snapshot for one fixed update and consumes pending
 * edges/deltas. Held and absolute pointer state remain latched.
 */
void input_latch_consume(
    Input_Latch* latch,
    uint64_t fixed_tick_index,
    Game_Input* out_input);

#if defined(__cplusplus)
}
#endif

#endif
