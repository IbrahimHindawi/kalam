#include "input_latch.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

static uint32_t
input_latch_saturating_increment(uint32_t value)
{
    return value == UINT32_MAX ? UINT32_MAX : value + 1;
}

static int32_t
input_latch_saturating_add_i32(int32_t left, int32_t right)
{
    if (right > 0 && left > INT32_MAX - right)
    {
        return INT32_MAX;
    }

    if (right < 0 && left < INT32_MIN - right)
    {
        return INT32_MIN;
    }

    return left + right;
}

static void
input_latch_set_button(Game_Button* button, uint32_t is_down)
{
    uint32_t normalized_is_down = is_down != 0 ? 1u : 0u;

    if (button->is_down == normalized_is_down)
    {
        return;
    }

    button->is_down = normalized_is_down;
    if (normalized_is_down != 0)
    {
        button->pressed_count =
            input_latch_saturating_increment(button->pressed_count);
    }
    else
    {
        button->released_count =
            input_latch_saturating_increment(button->released_count);
    }
}

static void
input_latch_clear_edges(Game_Button* buttons, uint32_t button_count)
{
    uint32_t index = 0;

    for (index = 0; index < button_count; index += 1)
    {
        buttons[index].pressed_count = 0;
        buttons[index].released_count = 0;
    }
}

void
input_latch_init(Input_Latch* latch)
{
    if (latch != 0)
    {
        memset(latch, 0, sizeof(*latch));
    }
}

uint32_t
input_latch_set_key(
    Input_Latch* latch,
    Game_Key key,
    uint32_t is_down)
{
    if (latch == 0 || key >= Game_Key_Count)
    {
        return 0;
    }

    input_latch_set_button(&latch->keys[key], is_down);
    return 1;
}

uint32_t
input_latch_set_mouse_button(
    Input_Latch* latch,
    Game_Mouse_Button button,
    uint32_t is_down)
{
    if (latch == 0 || button >= Game_Mouse_Button_Count)
    {
        return 0;
    }

    input_latch_set_button(&latch->mouse_buttons[button], is_down);
    return 1;
}

void
input_latch_set_pointer_position(
    Input_Latch* latch,
    int32_t x_pixels,
    int32_t y_pixels)
{
    if (latch != 0)
    {
        latch->pointer_x_pixels = x_pixels;
        latch->pointer_y_pixels = y_pixels;
    }
}

void
input_latch_add_pointer_delta(
    Input_Latch* latch,
    int32_t delta_x_pixels,
    int32_t delta_y_pixels)
{
    if (latch != 0)
    {
        latch->pending_pointer_delta_x_pixels =
            input_latch_saturating_add_i32(
                latch->pending_pointer_delta_x_pixels,
                delta_x_pixels);
        latch->pending_pointer_delta_y_pixels =
            input_latch_saturating_add_i32(
                latch->pending_pointer_delta_y_pixels,
                delta_y_pixels);
    }
}

void
input_latch_add_wheel_delta(Input_Latch* latch, float delta)
{
    float value = 0.0f;

    if (latch == 0 || !isfinite(delta))
    {
        return;
    }

    value = latch->pending_wheel_delta + delta;
    if (!isfinite(value))
    {
        value = delta > 0.0f ? FLT_MAX : -FLT_MAX;
    }

    latch->pending_wheel_delta = value;
}

void
input_latch_focus_gained(Input_Latch* latch)
{
    if (latch != 0)
    {
        latch->focused = 1;
    }
}

void
input_latch_focus_lost(Input_Latch* latch)
{
    uint32_t index = 0;

    if (latch == 0)
    {
        return;
    }

    for (index = 0; index < Game_Key_Count; index += 1)
    {
        if (latch->keys[index].is_down != 0)
        {
            input_latch_set_button(&latch->keys[index], 0);
        }
    }

    for (index = 0; index < Game_Mouse_Button_Count; index += 1)
    {
        if (latch->mouse_buttons[index].is_down != 0)
        {
            input_latch_set_button(&latch->mouse_buttons[index], 0);
        }
    }

    latch->focused = 0;
    latch->pending_pointer_delta_x_pixels = 0;
    latch->pending_pointer_delta_y_pixels = 0;
}

void
input_latch_consume(
    Input_Latch* latch,
    uint64_t fixed_tick_index,
    Game_Input* out_input)
{
    if (out_input == 0)
    {
        return;
    }

    memset(out_input, 0, sizeof(*out_input));
    out_input->struct_size = (uint32_t)sizeof(*out_input);
    out_input->fixed_tick_index = fixed_tick_index;

    if (latch == 0)
    {
        return;
    }

    out_input->flags = latch->flags;
    memcpy(out_input->keys, latch->keys, sizeof(latch->keys));
    memcpy(
        out_input->mouse_buttons,
        latch->mouse_buttons,
        sizeof(latch->mouse_buttons));
    out_input->pointer_x_pixels = latch->pointer_x_pixels;
    out_input->pointer_y_pixels = latch->pointer_y_pixels;
    out_input->pointer_delta_x_pixels =
        latch->pending_pointer_delta_x_pixels;
    out_input->pointer_delta_y_pixels =
        latch->pending_pointer_delta_y_pixels;
    out_input->wheel_delta = latch->pending_wheel_delta;

    input_latch_clear_edges(latch->keys, Game_Key_Count);
    input_latch_clear_edges(
        latch->mouse_buttons,
        Game_Mouse_Button_Count);
    latch->pending_pointer_delta_x_pixels = 0;
    latch->pending_pointer_delta_y_pixels = 0;
    latch->pending_wheel_delta = 0.0f;
}
