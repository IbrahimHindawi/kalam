#ifndef KALAM_ENGINE_CORE_CORE_TEMPLATES_H
#define KALAM_ENGINE_CORE_CORE_TEMPLATES_H

#if !defined(__cplusplus)
#error core_templates.h requires C++ because it provides engine-owned templates
#endif

#include "arena.h"

#include <string.h>

// T must be plain data that may be relocated byte-for-byte. These helpers do
// not run constructors or destructors and never own the backing arena.

template <typename T>
struct array
{
    T* data;
    usize count;
};

template <typename T>
static inline b32
array_is_valid(const array<T>* value)
{
    return value != 0 &&
        ((value->count == 0 && value->data == 0) ||
         (value->count != 0 && value->data != 0));
}

template <typename T>
static inline arena_result
array_reserve(arena* storage, usize count, array<T>* out_array)
{
    usize size = 0;
    void* memory = 0;
    arena_result result = Arena_Result_Ok;

    if (!arena_is_initialized(storage) || out_array == 0)
    {
        return Arena_Result_Invalid_Argument;
    }

    if (count == 0)
    {
        out_array->data = 0;
        out_array->count = 0;
        return Arena_Result_Ok;
    }

    if (!core_checked_mul_usize(count, (usize)sizeof(T), &size))
    {
        return Arena_Result_Arithmetic_Overflow;
    }

    result = arena_push(storage, size, (usize)alignof(T), &memory);
    if (result != Arena_Result_Ok)
    {
        return result;
    }

    out_array->data = (T*)memory;
    out_array->count = count;
    return Arena_Result_Ok;
}

template <typename T>
static inline arena_result
array_reserve_zero(arena* storage, usize count, array<T>* out_array)
{
    usize size = 0;
    void* memory = 0;
    arena_result result = Arena_Result_Ok;

    if (!arena_is_initialized(storage) || out_array == 0)
    {
        return Arena_Result_Invalid_Argument;
    }

    if (count == 0)
    {
        out_array->data = 0;
        out_array->count = 0;
        return Arena_Result_Ok;
    }

    if (!core_checked_mul_usize(count, (usize)sizeof(T), &size))
    {
        return Arena_Result_Arithmetic_Overflow;
    }

    result = arena_push_zero(storage, size, (usize)alignof(T), &memory);
    if (result != Arena_Result_Ok)
    {
        return result;
    }

    out_array->data = (T*)memory;
    out_array->count = count;
    return Arena_Result_Ok;
}

template <typename T>
static inline T*
array_at(array<T>* value, usize index)
{
    if (!array_is_valid(value) || index >= value->count)
    {
        return 0;
    }

    return value->data + index;
}

template <typename T>
static inline const T*
array_at(const array<T>* value, usize index)
{
    if (!array_is_valid(value) || index >= value->count)
    {
        return 0;
    }

    return value->data + index;
}

template <typename T>
struct vec
{
    T* data;
    usize count;
    usize capacity;
    arena* owner;
};

template <typename T>
static inline void
vec_init(vec<T>* value)
{
    if (value != 0)
    {
        value->data = 0;
        value->count = 0;
        value->capacity = 0;
        value->owner = 0;
    }
}

template <typename T>
static inline b32
vec_is_valid(const vec<T>* value)
{
    if (value == 0 || value->count > value->capacity)
    {
        return CORE_FALSE;
    }

    if (value->capacity == 0)
    {
        return value->data == 0;
    }

    return value->data != 0 && value->owner != 0;
}

static inline arena_result
vec_growth_capacity(
    usize current_capacity,
    usize minimum_capacity,
    usize* out_capacity)
{
    usize candidate = current_capacity;

    if (out_capacity == 0)
    {
        return Arena_Result_Invalid_Argument;
    }

    if (candidate >= minimum_capacity)
    {
        *out_capacity = candidate;
        return Arena_Result_Ok;
    }

    if (candidate < 8)
    {
        candidate = 8;
    }

    while (candidate < minimum_capacity)
    {
        if (candidate > CORE_USIZE_MAX / 2)
        {
            candidate = minimum_capacity;
            break;
        }

        candidate *= 2;
    }

    *out_capacity = candidate;
    return Arena_Result_Ok;
}

template <typename T>
static inline arena_result
vec_set_capacity(arena* storage, vec<T>* value, usize new_capacity)
{
    usize allocation_size = 0;
    usize copy_count = 0;
    usize copy_size = 0;
    void* allocation = 0;
    arena_result result = Arena_Result_Ok;

    if (!arena_is_initialized(storage) || !vec_is_valid(value))
    {
        return Arena_Result_Invalid_State;
    }

    if (value->owner != 0 && value->owner != storage)
    {
        return Arena_Result_Invalid_State;
    }

    if (new_capacity == value->capacity)
    {
        return Arena_Result_Ok;
    }

    if (new_capacity == 0)
    {
        value->data = 0;
        value->count = 0;
        value->capacity = 0;
        if (value->owner == 0)
        {
            value->owner = storage;
        }
        return Arena_Result_Ok;
    }

    if (!core_checked_mul_usize(
            new_capacity,
            (usize)sizeof(T),
            &allocation_size))
    {
        return Arena_Result_Arithmetic_Overflow;
    }

    copy_count = core_min_usize(value->count, new_capacity);
    if (!core_checked_mul_usize(copy_count, (usize)sizeof(T), &copy_size))
    {
        return Arena_Result_Arithmetic_Overflow;
    }

    result = arena_push(storage, allocation_size, (usize)alignof(T), &allocation);
    if (result != Arena_Result_Ok)
    {
        return result;
    }

    if (copy_size != 0)
    {
        memmove(allocation, value->data, copy_size);
    }

    value->data = (T*)allocation;
    value->count = copy_count;
    value->capacity = new_capacity;
    value->owner = storage;
    return Arena_Result_Ok;
}

template <typename T>
static inline arena_result
vec_reserve(arena* storage, vec<T>* value, usize minimum_capacity)
{
    usize new_capacity = 0;
    arena_result result = Arena_Result_Ok;

    if (!vec_is_valid(value))
    {
        return Arena_Result_Invalid_State;
    }

    if (minimum_capacity <= value->capacity)
    {
        return Arena_Result_Ok;
    }

    result = vec_growth_capacity(value->capacity, minimum_capacity, &new_capacity);
    if (result != Arena_Result_Ok)
    {
        return result;
    }

    return vec_set_capacity(storage, value, new_capacity);
}

template <typename T>
static inline arena_result
vec_append(arena* storage, vec<T>* value, const T* item)
{
    usize new_count = 0;
    arena_result result = Arena_Result_Ok;

    if (!vec_is_valid(value) || item == 0)
    {
        return Arena_Result_Invalid_Argument;
    }

    if (!core_checked_add_usize(value->count, 1, &new_count))
    {
        return Arena_Result_Arithmetic_Overflow;
    }

    result = vec_reserve(storage, value, new_count);
    if (result != Arena_Result_Ok)
    {
        return result;
    }

    memmove(value->data + value->count, item, sizeof(T));
    value->count = new_count;
    return Arena_Result_Ok;
}

template <typename T>
static inline arena_result
vec_append_many(
    arena* storage,
    vec<T>* value,
    const T* items,
    usize item_count)
{
    usize new_count = 0;
    usize copy_size = 0;
    arena_result result = Arena_Result_Ok;

    if (!vec_is_valid(value) || (item_count != 0 && items == 0))
    {
        return Arena_Result_Invalid_Argument;
    }

    if (!core_checked_add_usize(value->count, item_count, &new_count) ||
        !core_checked_mul_usize(item_count, (usize)sizeof(T), &copy_size))
    {
        return Arena_Result_Arithmetic_Overflow;
    }

    result = vec_reserve(storage, value, new_count);
    if (result != Arena_Result_Ok)
    {
        return result;
    }

    if (copy_size != 0)
    {
        memmove(value->data + value->count, items, copy_size);
    }

    value->count = new_count;
    return Arena_Result_Ok;
}

template <typename T>
static inline arena_result
vec_resize_zero(arena* storage, vec<T>* value, usize new_count)
{
    usize old_count = 0;
    usize added_count = 0;
    usize clear_size = 0;
    arena_result result = Arena_Result_Ok;

    if (!vec_is_valid(value))
    {
        return Arena_Result_Invalid_State;
    }

    old_count = value->count;
    if (new_count > value->capacity)
    {
        result = vec_reserve(storage, value, new_count);
        if (result != Arena_Result_Ok)
        {
            return result;
        }
    }

    if (new_count > old_count)
    {
        added_count = new_count - old_count;
        if (!core_checked_mul_usize(added_count, (usize)sizeof(T), &clear_size))
        {
            return Arena_Result_Arithmetic_Overflow;
        }

        memset(value->data + old_count, 0, clear_size);
    }

    value->count = new_count;
    return Arena_Result_Ok;
}

template <typename T>
static inline void
vec_clear(vec<T>* value)
{
    if (vec_is_valid(value))
    {
        value->count = 0;
    }
}

template <typename T>
static inline T*
vec_at(vec<T>* value, usize index)
{
    if (!vec_is_valid(value) || index >= value->count)
    {
        return 0;
    }

    return value->data + index;
}

template <typename T>
static inline const T*
vec_at(const vec<T>* value, usize index)
{
    if (!vec_is_valid(value) || index >= value->count)
    {
        return 0;
    }

    return value->data + index;
}

#endif
