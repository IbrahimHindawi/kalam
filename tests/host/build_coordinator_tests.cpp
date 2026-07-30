#include "build_coordinator.h"

#include <stdio.h>
#include <string.h>

static uint32_t test_failure_count = 0;

#define TEST_CHECK(expression)                                                \
    do                                                                        \
    {                                                                         \
        if (!(expression))                                                    \
        {                                                                     \
            fprintf(                                                          \
                stderr,                                                       \
                "%s(%d): check failed: %s\n",                               \
                __FILE__,                                                     \
                __LINE__,                                                     \
                #expression);                                                 \
            test_failure_count += 1;                                          \
        }                                                                     \
    } while (0)

static String_View
test_view(const char* text)
{
    String_View result;
    result.data = text;
    result.size = text != 0 ? (uint64_t)strlen(text) : 0;
    return result;
}

static uint32_t
test_view_equals(String_View value, const char* expected)
{
    String_View other = test_view(expected);
    return value.size == other.size &&
        (value.size == 0 ||
         memcmp(value.data, other.data, (size_t)value.size) == 0);
}

static void
test_single_success(void)
{
    Build_Coordinator coordinator;
    Build_Request_Decision request;
    Build_Finish_Decision finish;

    build_coordinator_init(&coordinator);
    request = build_coordinator_request(&coordinator);
    TEST_CHECK(request.result == Build_Coordinator_Result_Ok);
    TEST_CHECK(request.start_build_now == 1);
    TEST_CHECK(request.generation == 1);

    finish = build_coordinator_finish(&coordinator, 1, 1);
    TEST_CHECK(finish.result == Build_Coordinator_Result_Ok);
    TEST_CHECK(finish.publish_reload == 1);
    TEST_CHECK(finish.start_next_build == 0);
    TEST_CHECK(
        build_coordinator_mark_published(&coordinator, 1) ==
        Build_Coordinator_Result_Ok);
    TEST_CHECK(coordinator.latest_published_generation == 1);
}

static void
test_rapid_requests_coalesce_and_stale_success_cannot_publish(void)
{
    Build_Coordinator coordinator;
    Build_Request_Decision request;
    Build_Finish_Decision finish;

    build_coordinator_init(&coordinator);
    request = build_coordinator_request(&coordinator);
    TEST_CHECK(request.generation == 1 && request.start_build_now == 1);
    request = build_coordinator_request(&coordinator);
    TEST_CHECK(request.generation == 2 && request.start_build_now == 0);
    request = build_coordinator_request(&coordinator);
    TEST_CHECK(request.generation == 3 && request.start_build_now == 0);

    finish = build_coordinator_finish(&coordinator, 1, 1);
    TEST_CHECK(finish.publish_reload == 0);
    TEST_CHECK(finish.start_next_build == 1);
    TEST_CHECK(finish.next_generation == 3);
    TEST_CHECK(
        build_coordinator_mark_published(&coordinator, 1) ==
        Build_Coordinator_Result_Invalid_State);

    finish = build_coordinator_finish(&coordinator, 3, 1);
    TEST_CHECK(finish.publish_reload == 1);
    TEST_CHECK(finish.start_next_build == 0);
    TEST_CHECK(
        build_coordinator_mark_published(&coordinator, 3) ==
        Build_Coordinator_Result_Ok);
}

static void
test_failure_starts_latest_queued_request(void)
{
    Build_Coordinator coordinator;
    Build_Finish_Decision finish;

    build_coordinator_init(&coordinator);
    (void)build_coordinator_request(&coordinator);
    (void)build_coordinator_request(&coordinator);
    finish = build_coordinator_finish(&coordinator, 1, 0);
    TEST_CHECK(finish.publish_reload == 0);
    TEST_CHECK(finish.start_next_build == 1);
    TEST_CHECK(finish.next_generation == 2);
    finish = build_coordinator_finish(&coordinator, 2, 0);
    TEST_CHECK(finish.publish_reload == 0);
    TEST_CHECK(coordinator.latest_published_generation == 0);
}

static void
test_invalid_finish_is_non_mutating(void)
{
    Build_Coordinator coordinator;
    Build_Finish_Decision finish;

    build_coordinator_init(&coordinator);
    (void)build_coordinator_request(&coordinator);
    finish = build_coordinator_finish(&coordinator, 9, 1);
    TEST_CHECK(finish.result == Build_Coordinator_Result_Invalid_State);
    TEST_CHECK(coordinator.build_running == 1);
    TEST_CHECK(coordinator.running_generation == 1);
}

static void
test_diagnostics(void)
{
    Build_Diagnostic diagnostic;

    TEST_CHECK(build_diagnostic_parse_line(
        test_view("C:\\work\\cube.cpp(42,7): error C2065: missing_name\r\n"),
        &diagnostic));
    TEST_CHECK(test_view_equals(diagnostic.file, "C:\\work\\cube.cpp"));
    TEST_CHECK(diagnostic.line == 42);
    TEST_CHECK(diagnostic.column == 7);
    TEST_CHECK(diagnostic.severity == Build_Diagnostic_Severity_Error);
    TEST_CHECK(test_view_equals(diagnostic.code, "C2065"));
    TEST_CHECK(test_view_equals(diagnostic.message, "missing_name"));

    TEST_CHECK(build_diagnostic_parse_line(
        test_view("game/render.cpp:18:3: warning: unused value"),
        &diagnostic));
    TEST_CHECK(test_view_equals(diagnostic.file, "game/render.cpp"));
    TEST_CHECK(diagnostic.line == 18);
    TEST_CHECK(diagnostic.column == 3);
    TEST_CHECK(diagnostic.severity == Build_Diagnostic_Severity_Warning);
    TEST_CHECK(diagnostic.code.size == 0);
    TEST_CHECK(test_view_equals(diagnostic.message, "unused value"));

    TEST_CHECK(build_diagnostic_parse_line(
        test_view("game/main.cpp:9: fatal error: header not found"),
        &diagnostic));
    TEST_CHECK(diagnostic.line == 9);
    TEST_CHECK(diagnostic.column == 0);
    TEST_CHECK(
        diagnostic.severity == Build_Diagnostic_Severity_Fatal_Error);
    TEST_CHECK(test_view_equals(diagnostic.message, "header not found"));

    TEST_CHECK(!build_diagnostic_parse_line(
        test_view("FAILED: game_module.dll"),
        &diagnostic));
    TEST_CHECK(!build_diagnostic_parse_line(
        test_view("C:\\work\\cube.cpp(0): status: not a diagnostic"),
        &diagnostic));
}

int
main(void)
{
    test_single_success();
    test_rapid_requests_coalesce_and_stale_success_cannot_publish();
    test_failure_starts_latest_queued_request();
    test_invalid_finish_is_non_mutating();
    test_diagnostics();

    if (test_failure_count != 0)
    {
        fprintf(stderr, "%u build coordinator test(s) failed\n", test_failure_count);
        return 1;
    }
    printf("build coordinator tests passed\n");
    return 0;
}
