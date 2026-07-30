#include "frame_scheduler.h"

#include <float.h>
#include <math.h>
#include <string.h>

static const double Frame_Scheduler_Fixed_Delta_Seconds = 1.0 / 60.0;
static const double Frame_Scheduler_Max_Delta_Seconds = 0.25;

double
frame_scheduler_fixed_delta_seconds(void)
{
    return Frame_Scheduler_Fixed_Delta_Seconds;
}

double
frame_scheduler_max_measured_delta_seconds(void)
{
    return Frame_Scheduler_Max_Delta_Seconds;
}

void
frame_scheduler_init(Frame_Scheduler* scheduler)
{
    if (scheduler != 0)
    {
        memset(scheduler, 0, sizeof(*scheduler));
    }
}

void
frame_scheduler_reset_backlog(Frame_Scheduler* scheduler)
{
    if (scheduler != 0)
    {
        scheduler->accumulator_seconds = 0.0;
    }
}

Frame_Schedule
frame_scheduler_advance(
    Frame_Scheduler* scheduler,
    double measured_delta_seconds)
{
    Frame_Schedule result;
    double accepted_delta_seconds = measured_delta_seconds;
    double available_tick_value = 0.0;
    uint64_t available_tick_count = 0;
    uint64_t discarded_tick_count = 0;

    memset(&result, 0, sizeof(result));
    result.fixed_delta_time = (float)Frame_Scheduler_Fixed_Delta_Seconds;
    result.measured_delta_seconds = (float)measured_delta_seconds;

    if (scheduler == 0)
    {
        return result;
    }

    if (!isfinite(accepted_delta_seconds) || accepted_delta_seconds < 0.0)
    {
        accepted_delta_seconds = 0.0;
    }
    else if (accepted_delta_seconds > Frame_Scheduler_Max_Delta_Seconds)
    {
        accepted_delta_seconds = Frame_Scheduler_Max_Delta_Seconds;
    }

    result.accepted_delta_seconds = (float)accepted_delta_seconds;
    result.first_fixed_tick_index = scheduler->next_fixed_tick_index;
    scheduler->accumulator_seconds += accepted_delta_seconds;

    /*
     * A tiny relative epsilon prevents a mathematically exact fixed interval
     * represented just below its boundary from spuriously producing no tick.
     */
    available_tick_value =
        (scheduler->accumulator_seconds +
         Frame_Scheduler_Fixed_Delta_Seconds * DBL_EPSILON * 8.0) /
        Frame_Scheduler_Fixed_Delta_Seconds;

    if (available_tick_value >= 1.0)
    {
        if (available_tick_value > (double)UINT64_MAX)
        {
            available_tick_count = UINT64_MAX;
        }
        else
        {
            available_tick_count = (uint64_t)available_tick_value;
        }
    }

    result.update_count =
        available_tick_count > Frame_Scheduler_Max_Updates_Per_Frame
            ? Frame_Scheduler_Max_Updates_Per_Frame
            : (uint32_t)available_tick_count;

    discarded_tick_count =
        available_tick_count - (uint64_t)result.update_count;
    result.discarded_tick_count =
        discarded_tick_count > UINT32_MAX
            ? UINT32_MAX
            : (uint32_t)discarded_tick_count;

    if (available_tick_count != 0)
    {
        scheduler->accumulator_seconds -=
            (double)available_tick_count *
            Frame_Scheduler_Fixed_Delta_Seconds;

        if (scheduler->accumulator_seconds < 0.0)
        {
            scheduler->accumulator_seconds = 0.0;
        }
    }

    scheduler->next_fixed_tick_index += result.update_count;

    result.interpolation_alpha =
        (float)(
            scheduler->accumulator_seconds /
            Frame_Scheduler_Fixed_Delta_Seconds);

    if (result.interpolation_alpha < 0.0f)
    {
        result.interpolation_alpha = 0.0f;
    }
    else if (result.interpolation_alpha >= 1.0f)
    {
        /*
         * Keep the ABI promise even under a hostile floating-point
         * environment. The exact value is not gameplay-authoritative.
         */
        result.interpolation_alpha = 0.99999994f;
    }

    return result;
}
