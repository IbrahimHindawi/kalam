#ifndef KALAM_ENGINE_CORE_CORE_TYPES_H
#define KALAM_ENGINE_CORE_CORE_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef float f32;
typedef double f64;

typedef uintptr_t usize;
typedef intptr_t isize;
typedef u32 b32;

enum
{
    CORE_FALSE = 0,
    CORE_TRUE = 1
};

#if defined(__cplusplus)
#define CORE_STATIC_ASSERT(condition, message) static_assert(condition, message)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define CORE_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#else
#define CORE_STATIC_ASSERT_JOIN_INNER(a, b) a##b
#define CORE_STATIC_ASSERT_JOIN(a, b) CORE_STATIC_ASSERT_JOIN_INNER(a, b)
#define CORE_STATIC_ASSERT(condition, message) \
    typedef char CORE_STATIC_ASSERT_JOIN(core_static_assert_, __LINE__)[(condition) ? 1 : -1]
#endif

CORE_STATIC_ASSERT(sizeof(s8) == 1, "s8 must be 8 bits");
CORE_STATIC_ASSERT(sizeof(s16) == 2, "s16 must be 16 bits");
CORE_STATIC_ASSERT(sizeof(s32) == 4, "s32 must be 32 bits");
CORE_STATIC_ASSERT(sizeof(s64) == 8, "s64 must be 64 bits");
CORE_STATIC_ASSERT(sizeof(u8) == 1, "u8 must be 8 bits");
CORE_STATIC_ASSERT(sizeof(u16) == 2, "u16 must be 16 bits");
CORE_STATIC_ASSERT(sizeof(u32) == 4, "u32 must be 32 bits");
CORE_STATIC_ASSERT(sizeof(u64) == 8, "u64 must be 64 bits");
CORE_STATIC_ASSERT(sizeof(f32) == 4, "f32 must be 32 bits");
CORE_STATIC_ASSERT(sizeof(f64) == 8, "f64 must be 64 bits");
CORE_STATIC_ASSERT(sizeof(usize) == sizeof(void*), "usize must hold a pointer");
CORE_STATIC_ASSERT(sizeof(isize) == sizeof(void*), "isize must hold a pointer difference");

#define CORE_USIZE_MAX ((usize)UINTPTR_MAX)

static inline b32
core_is_power_of_two_usize(usize value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static inline usize
core_min_usize(usize a, usize b)
{
    return a < b ? a : b;
}

static inline usize
core_max_usize(usize a, usize b)
{
    return a > b ? a : b;
}

static inline b32
core_checked_add_usize(usize a, usize b, usize* out_value)
{
    if (out_value == 0 || a > CORE_USIZE_MAX - b)
    {
        return CORE_FALSE;
    }

    *out_value = a + b;
    return CORE_TRUE;
}

static inline b32
core_checked_mul_usize(usize a, usize b, usize* out_value)
{
    if (out_value == 0 || (a != 0 && b > CORE_USIZE_MAX / a))
    {
        return CORE_FALSE;
    }

    *out_value = a * b;
    return CORE_TRUE;
}

static inline b32
core_checked_align_up_usize(usize value, usize alignment, usize* out_value)
{
    usize with_mask = 0;

    if (out_value == 0 || !core_is_power_of_two_usize(alignment))
    {
        return CORE_FALSE;
    }

    if (!core_checked_add_usize(value, alignment - 1, &with_mask))
    {
        return CORE_FALSE;
    }

    *out_value = with_mask & ~(alignment - 1);
    return CORE_TRUE;
}

#endif
