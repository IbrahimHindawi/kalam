#include "build_coordinator.h"

#include <limits.h>
#include <string.h>

typedef struct Build_Parsed_Location
{
    uint64_t file_end;
    uint64_t diagnostic_start;
    uint32_t line;
    uint32_t column;
} Build_Parsed_Location;

static uint32_t
build_is_digit(char value)
{
    return value >= '0' && value <= '9';
}

static uint64_t
build_skip_spaces(String_View text, uint64_t at)
{
    while (at < text.size &&
           (text.data[at] == ' ' || text.data[at] == '\t'))
    {
        at += 1;
    }
    return at;
}

static uint64_t
build_trim_end(String_View text, uint64_t end)
{
    while (end > 0 &&
           (text.data[end - 1] == ' ' ||
            text.data[end - 1] == '\t' ||
            text.data[end - 1] == '\r' ||
            text.data[end - 1] == '\n'))
    {
        end -= 1;
    }
    return end;
}

static uint32_t
build_parse_u32(
    String_View text,
    uint64_t* in_out_at,
    uint32_t* out_value)
{
    uint64_t at = 0;
    uint64_t value = 0;

    if (in_out_at == 0 || out_value == 0)
    {
        return 0;
    }
    at = *in_out_at;
    if (at >= text.size || !build_is_digit(text.data[at]))
    {
        return 0;
    }
    while (at < text.size && build_is_digit(text.data[at]))
    {
        value = value * 10 + (uint64_t)(text.data[at] - '0');
        if (value > UINT32_MAX)
        {
            return 0;
        }
        at += 1;
    }
    *in_out_at = at;
    *out_value = (uint32_t)value;
    return 1;
}

static uint32_t
build_text_equals_at(
    String_View text,
    uint64_t at,
    const char* expected,
    uint64_t expected_size)
{
    return expected != 0 &&
        at <= text.size &&
        expected_size <= text.size - at &&
        memcmp(text.data + at, expected, (size_t)expected_size) == 0;
}

static uint32_t
build_parse_msvc_location(
    String_View text,
    Build_Parsed_Location* out_location)
{
    uint64_t open = 0;

    for (open = 1; open < text.size; open += 1)
    {
        uint64_t at = open + 1;
        uint32_t line = 0;
        uint32_t column = 0;

        if (text.data[open] != '(' ||
            at >= text.size ||
            !build_parse_u32(text, &at, &line))
        {
            continue;
        }
        if (at < text.size && text.data[at] == ',')
        {
            at += 1;
            if (!build_parse_u32(text, &at, &column))
            {
                continue;
            }
        }
        if (at + 1 >= text.size ||
            text.data[at] != ')' ||
            text.data[at + 1] != ':')
        {
            continue;
        }
        out_location->file_end = build_trim_end(text, open);
        out_location->diagnostic_start = at + 2;
        out_location->line = line;
        out_location->column = column;
        return out_location->file_end != 0 && line != 0;
    }
    return 0;
}

static uint32_t
build_parse_clang_location(
    String_View text,
    Build_Parsed_Location* out_location)
{
    uint64_t colon = 0;

    for (colon = 1; colon + 2 < text.size; colon += 1)
    {
        uint64_t at = colon + 1;
        uint32_t line = 0;
        uint32_t column = 0;

        if (text.data[colon] != ':' ||
            !build_parse_u32(text, &at, &line) ||
            at >= text.size ||
            text.data[at] != ':')
        {
            continue;
        }
        at += 1;
        if (at < text.size && build_is_digit(text.data[at]))
        {
            if (!build_parse_u32(text, &at, &column) ||
                at >= text.size ||
                text.data[at] != ':')
            {
                continue;
            }
            at += 1;
        }
        out_location->file_end = build_trim_end(text, colon);
        out_location->diagnostic_start = at;
        out_location->line = line;
        out_location->column = column;
        return out_location->file_end != 0 && line != 0;
    }
    return 0;
}

static uint32_t
build_parse_severity(
    String_View text,
    uint64_t* in_out_at,
    Build_Diagnostic_Severity* out_severity)
{
    uint64_t at = 0;

    if (in_out_at == 0 || out_severity == 0)
    {
        return 0;
    }
    at = build_skip_spaces(text, *in_out_at);
    if (build_text_equals_at(text, at, "fatal error", 11))
    {
        *out_severity = Build_Diagnostic_Severity_Fatal_Error;
        at += 11;
    }
    else if (build_text_equals_at(text, at, "error", 5))
    {
        *out_severity = Build_Diagnostic_Severity_Error;
        at += 5;
    }
    else if (build_text_equals_at(text, at, "warning", 7))
    {
        *out_severity = Build_Diagnostic_Severity_Warning;
        at += 7;
    }
    else if (build_text_equals_at(text, at, "note", 4))
    {
        *out_severity = Build_Diagnostic_Severity_Note;
        at += 4;
    }
    else
    {
        return 0;
    }
    *in_out_at = at;
    return 1;
}

void
build_coordinator_init(Build_Coordinator* coordinator)
{
    if (coordinator != 0)
    {
        memset(coordinator, 0, sizeof(*coordinator));
    }
}

Build_Request_Decision
build_coordinator_request(Build_Coordinator* coordinator)
{
    Build_Request_Decision decision;

    memset(&decision, 0, sizeof(decision));
    if (coordinator == 0)
    {
        decision.result = Build_Coordinator_Result_Invalid_Argument;
        return decision;
    }
    if (coordinator->latest_requested_generation == UINT64_MAX)
    {
        decision.result =
            Build_Coordinator_Result_Generation_Exhausted;
        return decision;
    }

    coordinator->latest_requested_generation += 1;
    decision.result = Build_Coordinator_Result_Ok;
    decision.generation = coordinator->latest_requested_generation;
    if (coordinator->build_running == 0)
    {
        coordinator->build_running = 1;
        coordinator->running_generation = decision.generation;
        decision.start_build_now = 1;
    }
    else
    {
        coordinator->request_queued = 1;
    }
    return decision;
}

Build_Finish_Decision
build_coordinator_finish(
    Build_Coordinator* coordinator,
    uint64_t generation,
    uint32_t build_succeeded)
{
    Build_Finish_Decision decision;

    memset(&decision, 0, sizeof(decision));
    decision.completed_generation = generation;
    if (coordinator == 0 || generation == 0)
    {
        decision.result = Build_Coordinator_Result_Invalid_Argument;
        return decision;
    }
    if (coordinator->build_running == 0 ||
        coordinator->running_generation != generation)
    {
        decision.result = Build_Coordinator_Result_Invalid_State;
        return decision;
    }

    coordinator->build_running = 0;
    coordinator->running_generation = 0;
    decision.result = Build_Coordinator_Result_Ok;

    if (build_succeeded != 0)
    {
        coordinator->latest_successful_generation = generation;
        if (generation == coordinator->latest_requested_generation &&
            generation > coordinator->latest_published_generation)
        {
            decision.publish_reload = 1;
        }
    }

    if (coordinator->request_queued != 0 &&
        coordinator->latest_requested_generation > generation)
    {
        coordinator->request_queued = 0;
        coordinator->build_running = 1;
        coordinator->running_generation =
            coordinator->latest_requested_generation;
        decision.start_next_build = 1;
        decision.next_generation = coordinator->running_generation;
    }
    else
    {
        coordinator->request_queued = 0;
    }
    return decision;
}

Build_Coordinator_Result
build_coordinator_mark_published(
    Build_Coordinator* coordinator,
    uint64_t generation)
{
    if (coordinator == 0 || generation == 0)
    {
        return Build_Coordinator_Result_Invalid_Argument;
    }
    if (generation != coordinator->latest_successful_generation ||
        generation != coordinator->latest_requested_generation ||
        generation <= coordinator->latest_published_generation)
    {
        return Build_Coordinator_Result_Invalid_State;
    }
    coordinator->latest_published_generation = generation;
    return Build_Coordinator_Result_Ok;
}

uint32_t
build_diagnostic_parse_line(
    String_View line,
    Build_Diagnostic* out_diagnostic)
{
    Build_Parsed_Location location;
    Build_Diagnostic diagnostic;
    uint64_t at = 0;
    uint64_t separator = 0;
    uint64_t message_start = 0;
    uint64_t end = 0;

    if (out_diagnostic != 0)
    {
        memset(out_diagnostic, 0, sizeof(*out_diagnostic));
    }
    if (line.data == 0 || line.size == 0 || out_diagnostic == 0)
    {
        return 0;
    }

    memset(&location, 0, sizeof(location));
    memset(&diagnostic, 0, sizeof(diagnostic));
    if (!build_parse_msvc_location(line, &location) &&
        !build_parse_clang_location(line, &location))
    {
        return 0;
    }

    at = location.diagnostic_start;
    if (!build_parse_severity(line, &at, &diagnostic.severity))
    {
        return 0;
    }
    at = build_skip_spaces(line, at);
    separator = at;
    while (separator < line.size && line.data[separator] != ':')
    {
        separator += 1;
    }
    if (separator >= line.size)
    {
        return 0;
    }

    if (separator > at)
    {
        uint64_t code_end = build_trim_end(line, separator);
        diagnostic.code.data = line.data + at;
        diagnostic.code.size = code_end - at;
    }
    message_start = build_skip_spaces(line, separator + 1);
    end = build_trim_end(line, line.size);
    if (message_start >= end)
    {
        return 0;
    }

    diagnostic.file.data = line.data;
    diagnostic.file.size = location.file_end;
    diagnostic.message.data = line.data + message_start;
    diagnostic.message.size = end - message_start;
    diagnostic.line = location.line;
    diagnostic.column = location.column;
    *out_diagnostic = diagnostic;
    return 1;
}

const char*
build_coordinator_result_string(Build_Coordinator_Result result)
{
    switch (result)
    {
        case Build_Coordinator_Result_Ok:
            return "ok";
        case Build_Coordinator_Result_Invalid_Argument:
            return "invalid build coordinator argument";
        case Build_Coordinator_Result_Invalid_State:
            return "invalid build coordinator state";
        case Build_Coordinator_Result_Generation_Exhausted:
            return "build generation exhausted";
        default:
            return "unknown build coordinator result";
    }
}
