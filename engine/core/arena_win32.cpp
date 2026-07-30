#include "arena.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string.h>

static void
arena_zero_state(arena* value)
{
    memset(value, 0, sizeof(*value));
}

static arena_result
arena_report(arena* value, arena_result result)
{
    if (value != 0)
    {
        value->last_result = result;
    }
    return result;
}

static b32
arena_address_at(const arena* value, usize offset, u8** out_address)
{
    usize base_address = 0;
    usize address = 0;

    if (value == 0 || value->base == 0 || out_address == 0 || offset > value->reserve_size)
    {
        return CORE_FALSE;
    }

    base_address = (usize)value->base;
    if (!core_checked_add_usize(base_address, offset, &address))
    {
        return CORE_FALSE;
    }

    *out_address = (u8*)address;
    return CORE_TRUE;
}

static arena_result
arena_commit_to(arena* value, usize required_position)
{
    usize commit_target = 0;
    usize commit_amount = 0;
    u8* commit_base = 0;
    void* committed = 0;

    if (required_position <= value->committed_size)
    {
        return Arena_Result_Ok;
    }

    if (required_position > value->reserve_size)
    {
        return Arena_Result_Out_Of_Memory;
    }

    if (!core_checked_align_up_usize(
            required_position,
            value->commit_granularity,
            &commit_target))
    {
        return Arena_Result_Arithmetic_Overflow;
    }

    if (commit_target > value->reserve_size)
    {
        commit_target = value->reserve_size;
    }

    commit_amount = commit_target - value->committed_size;
    if (commit_amount == 0)
    {
        return Arena_Result_Out_Of_Memory;
    }

    if (!arena_address_at(value, value->committed_size, &commit_base))
    {
        return Arena_Result_Arithmetic_Overflow;
    }

    committed = VirtualAlloc(
        commit_base,
        (SIZE_T)commit_amount,
        MEM_COMMIT,
        PAGE_READWRITE);

    if (committed != commit_base)
    {
        return Arena_Result_Commit_Failed;
    }

    value->committed_size = commit_target;
    return Arena_Result_Ok;
}

static arena_result
arena_decommit_tail(
    arena* value,
    usize retained_position,
    arena_decommit_mode decommit_mode)
{
    usize retained_commit = 0;
    usize decommit_size = 0;
    u8* decommit_base = 0;

    if (decommit_mode == Arena_Decommit_Keep)
    {
        return Arena_Result_Ok;
    }

    if (decommit_mode != Arena_Decommit_If_Large &&
        decommit_mode != Arena_Decommit_Always)
    {
        return Arena_Result_Invalid_Argument;
    }

    retained_position = core_max_usize(retained_position, value->minimum_commit_size);
    if (!core_checked_align_up_usize(
            retained_position,
            value->page_size,
            &retained_commit))
    {
        return Arena_Result_Arithmetic_Overflow;
    }

    retained_commit = core_min_usize(retained_commit, value->committed_size);
    decommit_size = value->committed_size - retained_commit;

    if (decommit_size == 0)
    {
        return Arena_Result_Ok;
    }

    if (decommit_mode == Arena_Decommit_If_Large &&
        decommit_size < value->decommit_threshold)
    {
        return Arena_Result_Ok;
    }

    if (!arena_address_at(value, retained_commit, &decommit_base))
    {
        return Arena_Result_Arithmetic_Overflow;
    }

    if (!VirtualFree(decommit_base, (SIZE_T)decommit_size, MEM_DECOMMIT))
    {
        return Arena_Result_Decommit_Failed;
    }

    value->committed_size = retained_commit;
    return Arena_Result_Ok;
}

arena_result
arena_init(arena* out_arena, const arena_desc* desc)
{
    SYSTEM_INFO system_info;
    usize reserve_size = 0;
    usize initial_commit_size = 0;
    usize commit_granularity = 0;
    usize decommit_threshold = 0;
    void* reserved = 0;
    void* committed = 0;

    if (out_arena == 0 || desc == 0)
    {
        return Arena_Result_Invalid_Argument;
    }

    if (out_arena->base != 0)
    {
        return arena_report(out_arena, Arena_Result_Invalid_State);
    }

    arena_zero_state(out_arena);

    GetSystemInfo(&system_info);
    out_arena->page_size = (usize)system_info.dwPageSize;

    if (desc->reserve_size == 0 ||
        desc->initial_commit_size > desc->reserve_size ||
        desc->commit_granularity == 0 ||
        !core_is_power_of_two_usize(desc->commit_granularity) ||
        desc->commit_granularity < out_arena->page_size ||
        (desc->commit_granularity % out_arena->page_size) != 0)
    {
        return arena_report(out_arena, Arena_Result_Invalid_Argument);
    }

    if (!core_checked_align_up_usize(
            desc->reserve_size,
            out_arena->page_size,
            &reserve_size) ||
        !core_checked_align_up_usize(
            desc->initial_commit_size,
            out_arena->page_size,
            &initial_commit_size))
    {
        return arena_report(out_arena, Arena_Result_Arithmetic_Overflow);
    }

    commit_granularity = desc->commit_granularity;
    decommit_threshold = desc->decommit_threshold;
    if (decommit_threshold == 0)
    {
        decommit_threshold = commit_granularity;
    }

    reserved = VirtualAlloc(
        0,
        (SIZE_T)reserve_size,
        MEM_RESERVE,
        PAGE_READWRITE);

    if (reserved == 0)
    {
        return arena_report(out_arena, Arena_Result_Reserve_Failed);
    }

    if (initial_commit_size != 0)
    {
        committed = VirtualAlloc(
            reserved,
            (SIZE_T)initial_commit_size,
            MEM_COMMIT,
            PAGE_READWRITE);

        if (committed != reserved)
        {
            VirtualFree(reserved, 0, MEM_RELEASE);
            arena_zero_state(out_arena);
            return arena_report(out_arena, Arena_Result_Commit_Failed);
        }
    }

    out_arena->base = (u8*)reserved;
    out_arena->reserve_size = reserve_size;
    out_arena->committed_size = initial_commit_size;
    out_arena->position = 0;
    out_arena->commit_granularity = commit_granularity;
    out_arena->minimum_commit_size = initial_commit_size;
    out_arena->decommit_threshold = decommit_threshold;
    out_arena->last_result = Arena_Result_Ok;
    return Arena_Result_Ok;
}

arena_result
arena_release(arena* value)
{
    if (value == 0)
    {
        return Arena_Result_Invalid_Argument;
    }

    if (value->temp_depth != 0)
    {
        return arena_report(value, Arena_Result_Temp_Order_Violation);
    }

    if (value->base == 0)
    {
        arena_zero_state(value);
        return Arena_Result_Ok;
    }

    if (!VirtualFree(value->base, 0, MEM_RELEASE))
    {
        return arena_report(value, Arena_Result_Release_Failed);
    }

    arena_zero_state(value);
    return Arena_Result_Ok;
}

b32
arena_is_initialized(const arena* value)
{
    return value != 0 && value->base != 0;
}

usize
arena_remaining(const arena* value)
{
    if (!arena_is_initialized(value) || value->position > value->reserve_size)
    {
        return 0;
    }

    return value->reserve_size - value->position;
}

const char*
arena_result_string(arena_result result)
{
    switch (result)
    {
        case Arena_Result_Ok: return "ok";
        case Arena_Result_Invalid_Argument: return "invalid argument";
        case Arena_Result_Invalid_State: return "invalid state";
        case Arena_Result_Arithmetic_Overflow: return "arithmetic overflow";
        case Arena_Result_Out_Of_Memory: return "out of memory";
        case Arena_Result_Reserve_Failed: return "reserve failed";
        case Arena_Result_Commit_Failed: return "commit failed";
        case Arena_Result_Decommit_Failed: return "decommit failed";
        case Arena_Result_Release_Failed: return "release failed";
        case Arena_Result_Temp_Order_Violation: return "temporary scope order violation";
        default: return "unknown arena result";
    }
}

arena_result
arena_push(
    arena* value,
    usize size,
    usize alignment,
    void** out_memory)
{
    usize aligned_position = 0;
    usize end_position = 0;
    u8* memory = 0;
    arena_result result = Arena_Result_Ok;

    if (out_memory == 0)
    {
        return arena_report(value, Arena_Result_Invalid_Argument);
    }

    *out_memory = 0;

    if (!arena_is_initialized(value) ||
        !core_is_power_of_two_usize(alignment))
    {
        return arena_report(value, Arena_Result_Invalid_Argument);
    }

    if (!core_checked_align_up_usize(value->position, alignment, &aligned_position) ||
        !core_checked_add_usize(aligned_position, size, &end_position))
    {
        return arena_report(value, Arena_Result_Arithmetic_Overflow);
    }

    if (end_position > value->reserve_size)
    {
        return arena_report(value, Arena_Result_Out_Of_Memory);
    }

    result = arena_commit_to(value, end_position);
    if (result != Arena_Result_Ok)
    {
        return arena_report(value, result);
    }

    if (!arena_address_at(value, aligned_position, &memory))
    {
        return arena_report(value, Arena_Result_Arithmetic_Overflow);
    }

    value->position = end_position;
    *out_memory = memory;
    return arena_report(value, Arena_Result_Ok);
}

arena_result
arena_push_zero(
    arena* value,
    usize size,
    usize alignment,
    void** out_memory)
{
    arena_result result = arena_push(value, size, alignment, out_memory);

    if (result == Arena_Result_Ok && size != 0)
    {
        memset(*out_memory, 0, size);
    }

    return result;
}

arena_result
arena_pop_to(
    arena* value,
    usize position,
    arena_decommit_mode decommit_mode)
{
    arena_result result = Arena_Result_Ok;

    if (!arena_is_initialized(value) || position > value->position)
    {
        return arena_report(value, Arena_Result_Invalid_Argument);
    }

    if (value->temp_depth != 0)
    {
        return arena_report(value, Arena_Result_Temp_Order_Violation);
    }

    value->position = position;
    result = arena_decommit_tail(value, position, decommit_mode);
    return arena_report(value, result);
}

arena_result
arena_reset(arena* value, arena_decommit_mode decommit_mode)
{
    return arena_pop_to(value, 0, decommit_mode);
}

arena_result
arena_trim(arena* value)
{
    arena_result result = Arena_Result_Ok;

    if (!arena_is_initialized(value))
    {
        return arena_report(value, Arena_Result_Invalid_Argument);
    }

    if (value->temp_depth != 0)
    {
        return arena_report(value, Arena_Result_Temp_Order_Violation);
    }

    result = arena_decommit_tail(value, value->position, Arena_Decommit_Always);
    return arena_report(value, result);
}

arena_result
arena_temp_begin(arena* value, arena_temp* out_temp)
{
    u64 token = 0;

    if (!arena_is_initialized(value) || out_temp == 0)
    {
        return arena_report(value, Arena_Result_Invalid_Argument);
    }

    if (value->next_temp_token == UINT64_MAX || value->temp_depth == UINT32_MAX)
    {
        return arena_report(value, Arena_Result_Arithmetic_Overflow);
    }

    token = value->next_temp_token + 1;

    out_temp->owner = value;
    out_temp->position = value->position;
    out_temp->token = token;
    out_temp->parent_token = value->active_temp_token;
    out_temp->depth = value->temp_depth + 1;
    out_temp->active = CORE_TRUE;

    value->next_temp_token = token;
    value->active_temp_token = token;
    value->temp_depth += 1;
    return arena_report(value, Arena_Result_Ok);
}

arena_result
arena_temp_end(arena_temp* temp, arena_decommit_mode decommit_mode)
{
    arena* value = 0;
    arena_result result = Arena_Result_Ok;

    if (temp == 0 || !temp->active || temp->owner == 0)
    {
        return Arena_Result_Invalid_Argument;
    }

    value = temp->owner;

    if (!arena_is_initialized(value) ||
        value->temp_depth != temp->depth ||
        value->active_temp_token != temp->token ||
        temp->position > value->position)
    {
        return arena_report(value, Arena_Result_Temp_Order_Violation);
    }

    value->position = temp->position;
    value->active_temp_token = temp->parent_token;
    value->temp_depth -= 1;
    temp->active = CORE_FALSE;
    temp->owner = 0;

    result = arena_decommit_tail(value, value->position, decommit_mode);
    return arena_report(value, result);
}
