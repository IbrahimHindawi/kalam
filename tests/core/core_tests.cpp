#include "arena.h"
#include "core_templates.h"
#include "core_types.h"
#include "string_view.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>
#include <string.h>

static u32 test_failure_count;

static void
test_fail(const char* expression, const char* file, int line)
{
    printf("%s(%d): check failed: %s\n", file, line, expression);
    test_failure_count += 1;
}

#define TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            test_fail(#expression, __FILE__, __LINE__); \
        } \
    } while (0)

#define TEST_REQUIRE(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            test_fail(#expression, __FILE__, __LINE__); \
            return; \
        } \
    } while (0)

static arena_desc
test_arena_desc(
    usize reserve_size,
    usize initial_commit_size,
    usize decommit_threshold)
{
    arena_desc result;
    result.reserve_size = reserve_size;
    result.initial_commit_size = initial_commit_size;
    result.commit_granularity = 64u * 1024u;
    result.decommit_threshold = decommit_threshold;
    return result;
}

static void
test_core_types_and_string_view(void)
{
    usize result = 0;
    string_view whole = string_view_from_c_string("kalam");
    string_view expected = string_view_make("ala", 3);
    string_view subview = string_view_empty();
    string_view invalid = string_view_make(0, 1);

    TEST_CHECK(core_checked_add_usize(CORE_USIZE_MAX - 1, 1, &result));
    TEST_CHECK(result == CORE_USIZE_MAX);
    TEST_CHECK(!core_checked_add_usize(CORE_USIZE_MAX, 1, &result));

    TEST_CHECK(core_checked_mul_usize(0, CORE_USIZE_MAX, &result));
    TEST_CHECK(result == 0);
    TEST_CHECK(core_checked_mul_usize(13, 7, &result));
    TEST_CHECK(result == 91);
    TEST_CHECK(!core_checked_mul_usize(CORE_USIZE_MAX, 2, &result));

    TEST_CHECK(core_checked_align_up_usize(13, 8, &result));
    TEST_CHECK(result == 16);
    TEST_CHECK(core_checked_align_up_usize(16, 8, &result));
    TEST_CHECK(result == 16);
    TEST_CHECK(!core_checked_align_up_usize(CORE_USIZE_MAX, 8, &result));
    TEST_CHECK(!core_checked_align_up_usize(12, 3, &result));

    TEST_CHECK(whole.size == 5);
    TEST_CHECK(string_view_subview(whole, 1, 3, &subview));
    TEST_CHECK(string_view_equal(subview, expected));
    TEST_CHECK(!string_view_subview(whole, 4, 2, &subview));
    TEST_CHECK(!string_view_is_valid(invalid));
    TEST_CHECK(string_view_equal(string_view_empty(), string_view_make(0, 0)));
}

static void
test_arena_alignment_and_commit_growth(void)
{
    arena value = {};
    arena_desc desc = test_arena_desc(2u * 1024u * 1024u, 0, 64u * 1024u);
    void* first = 0;
    void* aligned = 0;
    void* large = 0;
    usize first_commit = 0;

    TEST_REQUIRE(arena_init(&value, &desc) == Arena_Result_Ok);
    TEST_CHECK(value.committed_size == 0);

    TEST_REQUIRE(arena_push(&value, 1, 1, &first) == Arena_Result_Ok);
    TEST_CHECK(first != 0);
    TEST_CHECK(value.committed_size >= 1);
    first_commit = value.committed_size;

    TEST_REQUIRE(arena_push(&value, 37, 256, &aligned) == Arena_Result_Ok);
    TEST_CHECK(((usize)aligned % 256) == 0);
    TEST_CHECK(value.position >= 37);

    TEST_REQUIRE(arena_push(&value, 192u * 1024u, 4096, &large) == Arena_Result_Ok);
    TEST_CHECK(((usize)large % 4096) == 0);
    TEST_CHECK(value.committed_size > first_commit);
    TEST_CHECK(value.committed_size <= value.reserve_size);
    TEST_CHECK(arena_remaining(&value) == value.reserve_size - value.position);

    ((u8*)first)[0] = 0x11;
    ((u8*)aligned)[0] = 0x22;
    ((u8*)large)[0] = 0x33;
    ((u8*)large)[192u * 1024u - 1] = 0x44;

    TEST_CHECK(arena_release(&value) == Arena_Result_Ok);
    TEST_CHECK(!arena_is_initialized(&value));
}

static void
test_arena_overflow_exhaustion_and_oom(void)
{
    arena value = {};
    arena_desc desc = test_arena_desc(128u * 1024u, 0, 64u * 1024u);
    void* memory = 0;
    usize old_position = 0;
    usize too_large = 0;
    array<u64> overflowing_array = {};
    vec<u64> overflowing_vec = {};
    u64* vec_data_before = 0;
    usize vec_count_before = 0;
    usize vec_capacity_before = 0;

    TEST_REQUIRE(arena_init(&value, &desc) == Arena_Result_Ok);
    TEST_REQUIRE(arena_push(&value, 32, 16, &memory) == Arena_Result_Ok);
    old_position = value.position;

    memory = (void*)1;
    TEST_CHECK(
        arena_push(&value, CORE_USIZE_MAX, 1, &memory) ==
        Arena_Result_Arithmetic_Overflow);
    TEST_CHECK(memory == 0);
    TEST_CHECK(value.position == old_position);

    memory = (void*)1;
    TEST_CHECK(
        arena_push(&value, 1, 3, &memory) ==
        Arena_Result_Invalid_Argument);
    TEST_CHECK(memory == 0);
    TEST_CHECK(value.position == old_position);

    TEST_REQUIRE(core_checked_add_usize(arena_remaining(&value), 1, &too_large));
    memory = (void*)1;
    TEST_CHECK(
        arena_push(&value, too_large, 1, &memory) ==
        Arena_Result_Out_Of_Memory);
    TEST_CHECK(memory == 0);
    TEST_CHECK(value.last_result == Arena_Result_Out_Of_Memory);
    TEST_CHECK(value.position == old_position);
    TEST_CHECK(strcmp(arena_result_string(value.last_result), "out of memory") == 0);

    TEST_CHECK(
        array_reserve<u64>(
            &value,
            CORE_USIZE_MAX / (usize)sizeof(u64) + 1,
            &overflowing_array) ==
        Arena_Result_Arithmetic_Overflow);
    TEST_CHECK(overflowing_array.data == 0);
    TEST_CHECK(overflowing_array.count == 0);

    vec_init(&overflowing_vec);
    TEST_REQUIRE(vec_append<u64>(&value, &overflowing_vec, (const u64*)&old_position) == Arena_Result_Ok);
    vec_data_before = overflowing_vec.data;
    vec_count_before = overflowing_vec.count;
    vec_capacity_before = overflowing_vec.capacity;

    TEST_CHECK(
        vec_set_capacity<u64>(
            &value,
            &overflowing_vec,
            CORE_USIZE_MAX / (usize)sizeof(u64) + 1) ==
        Arena_Result_Arithmetic_Overflow);
    TEST_CHECK(overflowing_vec.data == vec_data_before);
    TEST_CHECK(overflowing_vec.count == vec_count_before);
    TEST_CHECK(overflowing_vec.capacity == vec_capacity_before);

    TEST_CHECK(arena_release(&value) == Arena_Result_Ok);
}

static void
test_nested_temporary_scope_lifo(void)
{
    arena value = {};
    arena_desc desc = test_arena_desc(512u * 1024u, 0, 64u * 1024u);
    arena_temp outer = {};
    arena_temp inner = {};
    void* memory = 0;
    usize base_position = 0;
    usize deepest_position = 0;

    TEST_REQUIRE(arena_init(&value, &desc) == Arena_Result_Ok);
    TEST_REQUIRE(arena_push(&value, 32, 8, &memory) == Arena_Result_Ok);
    base_position = value.position;

    TEST_REQUIRE(arena_temp_begin(&value, &outer) == Arena_Result_Ok);
    TEST_REQUIRE(arena_push(&value, 80, 16, &memory) == Arena_Result_Ok);
    TEST_REQUIRE(arena_temp_begin(&value, &inner) == Arena_Result_Ok);
    TEST_REQUIRE(arena_push(&value, 160, 32, &memory) == Arena_Result_Ok);
    deepest_position = value.position;

    TEST_CHECK(
        arena_temp_end(&outer, Arena_Decommit_Keep) ==
        Arena_Result_Temp_Order_Violation);
    TEST_CHECK(value.position == deepest_position);
    TEST_CHECK(value.temp_depth == 2);
    TEST_CHECK(outer.active);

    TEST_CHECK(
        arena_reset(&value, Arena_Decommit_Keep) ==
        Arena_Result_Temp_Order_Violation);
    TEST_CHECK(
        arena_release(&value) ==
        Arena_Result_Temp_Order_Violation);
    TEST_CHECK(arena_is_initialized(&value));

    TEST_REQUIRE(arena_temp_end(&inner, Arena_Decommit_Keep) == Arena_Result_Ok);
    TEST_CHECK(value.position == inner.position);
    TEST_CHECK(value.temp_depth == 1);
    TEST_CHECK(
        arena_temp_end(&inner, Arena_Decommit_Keep) ==
        Arena_Result_Invalid_Argument);

    TEST_REQUIRE(arena_temp_end(&outer, Arena_Decommit_Keep) == Arena_Result_Ok);
    TEST_CHECK(value.position == base_position);
    TEST_CHECK(value.temp_depth == 0);
    TEST_CHECK(value.active_temp_token == 0);

    TEST_CHECK(
        arena_pop_to(&value, value.position + 1, Arena_Decommit_Keep) ==
        Arena_Result_Invalid_Argument);
    TEST_CHECK(arena_release(&value) == Arena_Result_Ok);
}

static void
test_frame_scene_reset_and_decommit(void)
{
    arena frame = {};
    arena scene = {};
    arena_desc desc = test_arena_desc(
        4u * 1024u * 1024u,
        64u * 1024u,
        64u * 1024u);
    void* frame_memory = 0;
    void* scene_memory = 0;
    usize frame_peak_commit = 0;
    usize scene_peak_commit = 0;

    TEST_REQUIRE(arena_init(&frame, &desc) == Arena_Result_Ok);
    TEST_REQUIRE(arena_init(&scene, &desc) == Arena_Result_Ok);

    TEST_REQUIRE(
        arena_push(&frame, 2u * 1024u * 1024u, 64, &frame_memory) ==
        Arena_Result_Ok);
    TEST_REQUIRE(
        arena_push(&scene, 2u * 1024u * 1024u, 64, &scene_memory) ==
        Arena_Result_Ok);

    ((u8*)frame_memory)[2u * 1024u * 1024u - 1] = 0x5a;
    ((u8*)scene_memory)[2u * 1024u * 1024u - 1] = 0xa5;
    frame_peak_commit = frame.committed_size;
    scene_peak_commit = scene.committed_size;

    TEST_REQUIRE(arena_reset(&frame, Arena_Decommit_If_Large) == Arena_Result_Ok);
    TEST_CHECK(frame.position == 0);
    TEST_CHECK(frame.committed_size == frame.minimum_commit_size);
    TEST_CHECK(frame.committed_size < frame_peak_commit);

    TEST_REQUIRE(arena_reset(&scene, Arena_Decommit_Keep) == Arena_Result_Ok);
    TEST_CHECK(scene.position == 0);
    TEST_CHECK(scene.committed_size == scene_peak_commit);

    TEST_REQUIRE(arena_trim(&scene) == Arena_Result_Ok);
    TEST_CHECK(scene.committed_size == scene.minimum_commit_size);

    TEST_REQUIRE(arena_push(&frame, 256u * 1024u, 32, &frame_memory) == Arena_Result_Ok);
    TEST_REQUIRE(arena_reset(&frame, Arena_Decommit_Always) == Arena_Result_Ok);
    TEST_CHECK(frame.committed_size == frame.minimum_commit_size);

    TEST_CHECK(arena_release(&frame) == Arena_Result_Ok);
    TEST_CHECK(arena_release(&scene) == Arena_Result_Ok);
}

static void
test_zero_memory_and_temporary_decommit(void)
{
    arena value = {};
    arena_desc desc = test_arena_desc(
        4u * 1024u * 1024u,
        0,
        64u * 1024u);
    arena_temp temp = {};
    void* zero_memory = 0;
    void* large_memory = 0;
    usize index = 0;
    usize committed_inside_temp = 0;

    TEST_REQUIRE(arena_init(&value, &desc) == Arena_Result_Ok);
    TEST_REQUIRE(arena_push_zero(&value, 1024, 64, &zero_memory) == Arena_Result_Ok);
    for (index = 0; index < 1024; index += 1)
    {
        TEST_CHECK(((u8*)zero_memory)[index] == 0);
    }

    TEST_REQUIRE(arena_temp_begin(&value, &temp) == Arena_Result_Ok);
    TEST_REQUIRE(
        arena_push(&value, 2u * 1024u * 1024u, 16, &large_memory) ==
        Arena_Result_Ok);
    committed_inside_temp = value.committed_size;

    TEST_REQUIRE(
        arena_temp_end(&temp, Arena_Decommit_If_Large) ==
        Arena_Result_Ok);
    TEST_CHECK(value.position == 1024);
    TEST_CHECK(value.committed_size < committed_inside_temp);
    TEST_CHECK(value.committed_size >= value.position);
    TEST_CHECK(arena_release(&value) == Arena_Result_Ok);
}

struct alignas(64) test_aligned_value
{
    u8 bytes[64];
};

static void
test_arrays_and_vectors(void)
{
    arena storage = {};
    arena_desc desc = test_arena_desc(4u * 1024u * 1024u, 64u * 1024u, 64u * 1024u);
    array<test_aligned_value> aligned_array = {};
    array<u32> zero_array = {};
    vec<s32> values = {};
    usize index = 0;
    s32 item = 0;
    const s32* aliased_item = 0;
    usize position_before_probe = 0;
    usize predicted_allocation = 0;
    void* probe = 0;
    u8* sentinel = 0;
    usize sentinel_size = 64;

    TEST_REQUIRE(arena_init(&storage, &desc) == Arena_Result_Ok);

    TEST_REQUIRE(array_reserve<test_aligned_value>(&storage, 3, &aligned_array) == Arena_Result_Ok);
    TEST_CHECK(aligned_array.count == 3);
    TEST_CHECK(((usize)aligned_array.data % 64) == 0);
    TEST_CHECK(array_at(&aligned_array, 2) == aligned_array.data + 2);
    TEST_CHECK(array_at(&aligned_array, 3) == 0);

    TEST_REQUIRE(array_reserve_zero<u32>(&storage, 16, &zero_array) == Arena_Result_Ok);
    for (index = 0; index < zero_array.count; index += 1)
    {
        TEST_CHECK(zero_array.data[index] == 0);
    }

    vec_init(&values);
    TEST_CHECK(vec_is_valid(&values));

    for (index = 0; index < 32; index += 1)
    {
        item = (s32)(index * 3 + 1);
        TEST_REQUIRE(vec_append<s32>(&storage, &values, &item) == Arena_Result_Ok);
    }

    TEST_CHECK(values.count == 32);
    TEST_CHECK(values.capacity == 32);
    for (index = 0; index < values.count; index += 1)
    {
        TEST_CHECK(values.data[index] == (s32)(index * 3 + 1));
    }

    // Force growth while the appended value aliases the old vector storage.
    aliased_item = vec_at(&values, 0);
    TEST_REQUIRE(aliased_item != 0);
    TEST_REQUIRE(vec_append<s32>(&storage, &values, aliased_item) == Arena_Result_Ok);
    TEST_CHECK(values.count == 33);
    TEST_CHECK(values.data[32] == 1);

    TEST_REQUIRE(vec_resize_zero<s32>(&storage, &values, 40) == Arena_Result_Ok);
    for (index = 33; index < 40; index += 1)
    {
        TEST_CHECK(values.data[index] == 0);
    }

    TEST_REQUIRE(vec_set_capacity<s32>(&storage, &values, 40) == Arena_Result_Ok);
    TEST_CHECK(values.count == 40);
    TEST_CHECK(values.capacity == 40);

    // Commit and prefill the next allocation area. Bytes immediately after a
    // shrunken vector must remain untouched, proving copy_count is bounded by
    // the new capacity rather than the old element count.
    position_before_probe = storage.position;
    TEST_REQUIRE(arena_push(&storage, 128, 1, &probe) == Arena_Result_Ok);
    TEST_REQUIRE(
        arena_pop_to(&storage, position_before_probe, Arena_Decommit_Keep) ==
        Arena_Result_Ok);
    TEST_REQUIRE(
        core_checked_align_up_usize(
            storage.position,
            (usize)alignof(s32),
            &predicted_allocation));
    TEST_REQUIRE(predicted_allocation + sentinel_size <= storage.committed_size);
    sentinel = storage.base + predicted_allocation;
    memset(sentinel, 0xa7, sentinel_size);

    TEST_REQUIRE(vec_set_capacity<s32>(&storage, &values, 3) == Arena_Result_Ok);
    TEST_CHECK(values.count == 3);
    TEST_CHECK(values.capacity == 3);
    TEST_CHECK(values.data[0] == 1);
    TEST_CHECK(values.data[1] == 4);
    TEST_CHECK(values.data[2] == 7);

    for (index = 3u * sizeof(s32); index < sentinel_size; index += 1)
    {
        TEST_CHECK(sentinel[index] == 0xa7);
    }

    vec_clear(&values);
    TEST_CHECK(values.count == 0);
    TEST_CHECK(values.capacity == 3);
    TEST_REQUIRE(vec_set_capacity<s32>(&storage, &values, 0) == Arena_Result_Ok);
    TEST_CHECK(values.data == 0);
    TEST_CHECK(values.count == 0);
    TEST_CHECK(values.capacity == 0);

    TEST_CHECK(arena_release(&storage) == Arena_Result_Ok);
}

static void
test_repeated_arena_lifecycle_and_vm_release(void)
{
    usize iteration = 0;

    for (iteration = 0; iteration < 64; iteration += 1)
    {
        arena value = {};
        arena_desc desc = test_arena_desc(256u * 1024u, 0, 64u * 1024u);
        void* memory = 0;
        void* released_base = 0;
        MEMORY_BASIC_INFORMATION memory_info;
        SIZE_T query_size = 0;

        TEST_REQUIRE(arena_init(&value, &desc) == Arena_Result_Ok);
        TEST_REQUIRE(
            arena_push(
                &value,
                1024 + iteration * 13,
                64,
                &memory) ==
            Arena_Result_Ok);

        memset(memory, (int)(iteration & 0xff), 1024 + iteration * 13);
        released_base = value.base;

        TEST_REQUIRE(arena_release(&value) == Arena_Result_Ok);
        TEST_CHECK(value.base == 0);
        TEST_CHECK(value.reserve_size == 0);
        TEST_CHECK(value.committed_size == 0);
        TEST_CHECK(value.position == 0);

        memset(&memory_info, 0, sizeof(memory_info));
        query_size = VirtualQuery(released_base, &memory_info, sizeof(memory_info));
        TEST_CHECK(query_size == sizeof(memory_info));
        TEST_CHECK(memory_info.State == MEM_FREE);

        // Destruction is idempotent for an already released, zeroed arena.
        TEST_CHECK(arena_release(&value) == Arena_Result_Ok);
    }
}

typedef void (*test_function)(void);

static void
run_test(const char* name, test_function function)
{
    u32 failures_before = test_failure_count;
    printf("[ RUN      ] %s\n", name);
    function();
    if (test_failure_count == failures_before)
    {
        printf("[       OK ] %s\n", name);
    }
    else
    {
        printf("[  FAILED  ] %s\n", name);
    }
}

int
main(void)
{
    run_test("core_types_and_string_view", test_core_types_and_string_view);
    run_test("arena_alignment_and_commit_growth", test_arena_alignment_and_commit_growth);
    run_test("arena_overflow_exhaustion_and_oom", test_arena_overflow_exhaustion_and_oom);
    run_test("nested_temporary_scope_lifo", test_nested_temporary_scope_lifo);
    run_test("frame_scene_reset_and_decommit", test_frame_scene_reset_and_decommit);
    run_test("zero_memory_and_temporary_decommit", test_zero_memory_and_temporary_decommit);
    run_test("arrays_and_vectors", test_arrays_and_vectors);
    run_test("repeated_arena_lifecycle_and_vm_release", test_repeated_arena_lifecycle_and_vm_release);

    if (test_failure_count != 0)
    {
        printf("%u core test check(s) failed\n", test_failure_count);
        return 1;
    }

    printf("all core tests passed\n");
    return 0;
}
