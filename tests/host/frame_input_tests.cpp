#include "frame_scheduler.h"
#include "input_latch.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t test_failure_count;

static void
test_fail(const char* expression, int line)
{
    printf("frame_input_tests.cpp(%d): check failed: %s\n", line, expression);
    test_failure_count += 1;
}

#define TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            test_fail(#expression, __LINE__); \
        } \
    } while (0)

static uint32_t
test_near(double left, double right, double tolerance)
{
    return fabs(left - right) <= tolerance ? 1u : 0u;
}

static void
test_scheduler_zero_tick_and_interpolation(void)
{
    Frame_Scheduler scheduler;
    Frame_Schedule schedule;
    double fixed_delta = frame_scheduler_fixed_delta_seconds();

    frame_scheduler_init(&scheduler);
    schedule = frame_scheduler_advance(&scheduler, fixed_delta * 0.25);
    TEST_CHECK(schedule.update_count == 0);
    TEST_CHECK(schedule.discarded_tick_count == 0);
    TEST_CHECK(test_near(schedule.interpolation_alpha, 0.25, 0.00001));

    schedule = frame_scheduler_advance(&scheduler, fixed_delta * 0.75);
    TEST_CHECK(schedule.update_count == 1);
    TEST_CHECK(schedule.first_fixed_tick_index == 0);
    TEST_CHECK(scheduler.next_fixed_tick_index == 1);
    TEST_CHECK(schedule.interpolation_alpha >= 0.0f);
    TEST_CHECK(schedule.interpolation_alpha < 1.0f);
}

static void
test_scheduler_clamp_cap_discard_and_reset(void)
{
    Frame_Scheduler scheduler;
    Frame_Schedule schedule;

    frame_scheduler_init(&scheduler);
    schedule = frame_scheduler_advance(&scheduler, 9.0);

    TEST_CHECK(test_near(schedule.accepted_delta_seconds, 0.25, 0.000001));
    TEST_CHECK(schedule.update_count == 5);
    TEST_CHECK(schedule.discarded_tick_count == 10);
    TEST_CHECK(schedule.interpolation_alpha >= 0.0f);
    TEST_CHECK(schedule.interpolation_alpha < 1.0f);
    TEST_CHECK(scheduler.next_fixed_tick_index == 5);

    (void)frame_scheduler_advance(
        &scheduler,
        frame_scheduler_fixed_delta_seconds() * 0.5);
    TEST_CHECK(scheduler.accumulator_seconds > 0.0);
    frame_scheduler_reset_backlog(&scheduler);
    TEST_CHECK(scheduler.accumulator_seconds == 0.0);
    TEST_CHECK(scheduler.next_fixed_tick_index == 5);

    schedule = frame_scheduler_advance(&scheduler, -1.0);
    TEST_CHECK(schedule.update_count == 0);
    TEST_CHECK(schedule.accepted_delta_seconds == 0.0f);

    schedule = frame_scheduler_advance(&scheduler, HUGE_VAL);
    TEST_CHECK(schedule.update_count == 0);
    TEST_CHECK(schedule.accepted_delta_seconds == 0.0f);
}

static void
test_scheduler_host_parity(void)
{
    Frame_Scheduler editor_scheduler;
    Frame_Scheduler standalone_scheduler;
    const double deltas[] = {
        0.001,
        0.010,
        0.030,
        0.250,
        0.0,
        0.017,
        0.100
    };
    uint32_t index = 0;

    frame_scheduler_init(&editor_scheduler);
    frame_scheduler_init(&standalone_scheduler);

    for (index = 0; index < sizeof(deltas) / sizeof(deltas[0]); index += 1)
    {
        Frame_Schedule editor =
            frame_scheduler_advance(&editor_scheduler, deltas[index]);
        Frame_Schedule standalone =
            frame_scheduler_advance(&standalone_scheduler, deltas[index]);

        TEST_CHECK(
            memcmp(&editor, &standalone, sizeof(editor)) == 0);
        TEST_CHECK(
            memcmp(
                &editor_scheduler,
                &standalone_scheduler,
                sizeof(editor_scheduler)) == 0);
    }
}

static void
test_input_zero_tick_retention_and_dual_edges(void)
{
    Input_Latch latch;
    Game_Input input;

    input_latch_init(&latch);
    input_latch_focus_gained(&latch);
    TEST_CHECK(input_latch_set_key(&latch, Game_Key_F6, 1));
    TEST_CHECK(input_latch_set_key(&latch, Game_Key_F6, 0));
    input_latch_add_pointer_delta(&latch, 4, -3);
    input_latch_add_wheel_delta(&latch, 1.25f);

    /* Several presentation frames may pass without calling consume. */
    TEST_CHECK(latch.keys[Game_Key_F6].pressed_count == 1);
    TEST_CHECK(latch.keys[Game_Key_F6].released_count == 1);
    TEST_CHECK(latch.pending_pointer_delta_x_pixels == 4);

    input_latch_consume(&latch, 17, &input);
    TEST_CHECK(input.fixed_tick_index == 17);
    TEST_CHECK(input.keys[Game_Key_F6].is_down == 0);
    TEST_CHECK(input.keys[Game_Key_F6].pressed_count == 1);
    TEST_CHECK(input.keys[Game_Key_F6].released_count == 1);
    TEST_CHECK(input.pointer_delta_x_pixels == 4);
    TEST_CHECK(input.pointer_delta_y_pixels == -3);
    TEST_CHECK(input.wheel_delta == 1.25f);
}

static void
test_input_catch_up_consumes_edges_once(void)
{
    Input_Latch latch;
    Game_Input first_tick;
    Game_Input second_tick;

    input_latch_init(&latch);
    TEST_CHECK(input_latch_set_key(&latch, Game_Key_W, 1));
    TEST_CHECK(
        input_latch_set_mouse_button(
            &latch,
            Game_Mouse_Button_Left,
            1));
    input_latch_set_pointer_position(&latch, 100, 200);
    input_latch_add_pointer_delta(&latch, 8, 9);

    input_latch_consume(&latch, 3, &first_tick);
    input_latch_consume(&latch, 4, &second_tick);

    TEST_CHECK(first_tick.keys[Game_Key_W].is_down == 1);
    TEST_CHECK(first_tick.keys[Game_Key_W].pressed_count == 1);
    TEST_CHECK(first_tick.mouse_buttons[0].pressed_count == 1);
    TEST_CHECK(first_tick.pointer_delta_x_pixels == 8);

    TEST_CHECK(second_tick.keys[Game_Key_W].is_down == 1);
    TEST_CHECK(second_tick.keys[Game_Key_W].pressed_count == 0);
    TEST_CHECK(second_tick.keys[Game_Key_W].released_count == 0);
    TEST_CHECK(second_tick.mouse_buttons[0].is_down == 1);
    TEST_CHECK(second_tick.mouse_buttons[0].pressed_count == 0);
    TEST_CHECK(second_tick.pointer_x_pixels == 100);
    TEST_CHECK(second_tick.pointer_y_pixels == 200);
    TEST_CHECK(second_tick.pointer_delta_x_pixels == 0);
    TEST_CHECK(second_tick.pointer_delta_y_pixels == 0);
}

static void
test_input_focus_loss_synthesizes_release(void)
{
    Input_Latch latch;
    Game_Input input;

    input_latch_init(&latch);
    input_latch_focus_gained(&latch);
    TEST_CHECK(input_latch_set_key(&latch, Game_Key_A, 1));
    TEST_CHECK(
        input_latch_set_mouse_button(
            &latch,
            Game_Mouse_Button_Right,
            1));
    input_latch_add_pointer_delta(&latch, 99, 71);

    /* Consume initial presses, leaving only held state. */
    input_latch_consume(&latch, 0, &input);
    input_latch_add_pointer_delta(&latch, 3, 4);
    input_latch_focus_lost(&latch);
    input_latch_consume(&latch, 1, &input);

    TEST_CHECK(input.keys[Game_Key_A].is_down == 0);
    TEST_CHECK(input.keys[Game_Key_A].pressed_count == 0);
    TEST_CHECK(input.keys[Game_Key_A].released_count == 1);
    TEST_CHECK(input.mouse_buttons[Game_Mouse_Button_Right].is_down == 0);
    TEST_CHECK(
        input.mouse_buttons[Game_Mouse_Button_Right].released_count == 1);
    TEST_CHECK(input.pointer_delta_x_pixels == 0);
    TEST_CHECK(input.pointer_delta_y_pixels == 0);
}

int
main(void)
{
    test_scheduler_zero_tick_and_interpolation();
    test_scheduler_clamp_cap_discard_and_reset();
    test_scheduler_host_parity();
    test_input_zero_tick_retention_and_dual_edges();
    test_input_catch_up_consumes_edges_once();
    test_input_focus_loss_synthesizes_release();

    if (test_failure_count != 0)
    {
        printf("%u frame/input test check(s) failed\n", test_failure_count);
        return 1;
    }

    printf("all frame scheduler and input tests passed\n");
    return 0;
}
