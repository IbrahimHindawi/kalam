#ifndef KALAM_EDITOR_HOST_FRAME_SCHEDULER_H
#define KALAM_EDITOR_HOST_FRAME_SCHEDULER_H

#include <stdint.h>

enum
{
    Frame_Scheduler_Max_Updates_Per_Frame = 5
};

typedef struct Frame_Scheduler
{
    double accumulator_seconds;
    uint64_t next_fixed_tick_index;
} Frame_Scheduler;

typedef struct Frame_Schedule
{
    uint32_t update_count;
    uint32_t discarded_tick_count;
    uint64_t first_fixed_tick_index;

    float fixed_delta_time;
    float interpolation_alpha;
    float measured_delta_seconds;
    float accepted_delta_seconds;
} Frame_Schedule;

#if defined(__cplusplus)
extern "C" {
#endif

double frame_scheduler_fixed_delta_seconds(void);
double frame_scheduler_max_measured_delta_seconds(void);

void frame_scheduler_init(Frame_Scheduler* scheduler);
void frame_scheduler_reset_backlog(Frame_Scheduler* scheduler);

Frame_Schedule frame_scheduler_advance(
    Frame_Scheduler* scheduler,
    double measured_delta_seconds);

#if defined(__cplusplus)
}
#endif

#endif
