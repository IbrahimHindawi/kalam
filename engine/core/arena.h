#ifndef KALAM_ENGINE_CORE_ARENA_H
#define KALAM_ENGINE_CORE_ARENA_H

#include "core_types.h"

typedef u32 arena_result;

enum
{
    Arena_Result_Ok = 0,
    Arena_Result_Invalid_Argument,
    Arena_Result_Invalid_State,
    Arena_Result_Arithmetic_Overflow,
    Arena_Result_Out_Of_Memory,
    Arena_Result_Reserve_Failed,
    Arena_Result_Commit_Failed,
    Arena_Result_Decommit_Failed,
    Arena_Result_Release_Failed,
    Arena_Result_Temp_Order_Violation
};

typedef u32 arena_decommit_mode;

enum
{
    Arena_Decommit_Keep = 0,
    Arena_Decommit_If_Large,
    Arena_Decommit_Always
};

typedef struct arena arena;
typedef struct arena_temp arena_temp;

typedef struct arena_desc
{
    usize reserve_size;
    usize initial_commit_size;
    usize commit_granularity;
    usize decommit_threshold;
} arena_desc;

struct arena
{
    u8* base;

    usize reserve_size;
    usize committed_size;
    usize position;

    usize page_size;
    usize commit_granularity;
    usize minimum_commit_size;
    usize decommit_threshold;

    u64 next_temp_token;
    u64 active_temp_token;
    u32 temp_depth;
    arena_result last_result;
};

struct arena_temp
{
    arena* owner;
    usize position;
    u64 token;
    u64 parent_token;
    u32 depth;
    b32 active;
};

#if defined(__cplusplus)
extern "C" {
#endif

arena_result arena_init(arena* out_arena, const arena_desc* desc);
arena_result arena_release(arena* value);

b32 arena_is_initialized(const arena* value);
usize arena_remaining(const arena* value);
const char* arena_result_string(arena_result result);

arena_result arena_push(
    arena* value,
    usize size,
    usize alignment,
    void** out_memory);

arena_result arena_push_zero(
    arena* value,
    usize size,
    usize alignment,
    void** out_memory);

arena_result arena_pop_to(
    arena* value,
    usize position,
    arena_decommit_mode decommit_mode);

arena_result arena_reset(
    arena* value,
    arena_decommit_mode decommit_mode);

arena_result arena_trim(arena* value);

arena_result arena_temp_begin(arena* value, arena_temp* out_temp);
arena_result arena_temp_end(arena_temp* temp, arena_decommit_mode decommit_mode);

#if defined(__cplusplus)
}
#endif

#endif
