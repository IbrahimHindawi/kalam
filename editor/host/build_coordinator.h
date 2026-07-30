#ifndef KALAM_EDITOR_HOST_BUILD_COORDINATOR_H
#define KALAM_EDITOR_HOST_BUILD_COORDINATOR_H

#include <stdint.h>

#include "author_schema.h"

typedef uint32_t Build_Coordinator_Result;

enum
{
    Build_Coordinator_Result_Ok = 0,
    Build_Coordinator_Result_Invalid_Argument,
    Build_Coordinator_Result_Invalid_State,
    Build_Coordinator_Result_Generation_Exhausted
};

typedef struct Build_Coordinator
{
    uint64_t latest_requested_generation;
    uint64_t running_generation;
    uint64_t latest_successful_generation;
    uint64_t latest_published_generation;
    uint32_t build_running;
    uint32_t request_queued;
} Build_Coordinator;

typedef struct Build_Request_Decision
{
    Build_Coordinator_Result result;
    uint32_t start_build_now;
    uint64_t generation;
} Build_Request_Decision;

typedef struct Build_Finish_Decision
{
    Build_Coordinator_Result result;
    uint32_t publish_reload;
    uint32_t start_next_build;
    uint64_t completed_generation;
    uint64_t next_generation;
} Build_Finish_Decision;

typedef uint32_t Build_Diagnostic_Severity;

enum
{
    Build_Diagnostic_Severity_Unknown = 0,
    Build_Diagnostic_Severity_Note,
    Build_Diagnostic_Severity_Warning,
    Build_Diagnostic_Severity_Error,
    Build_Diagnostic_Severity_Fatal_Error
};

typedef struct Build_Diagnostic
{
    String_View file;
    String_View code;
    String_View message;
    uint32_t line;
    uint32_t column;
    Build_Diagnostic_Severity severity;
} Build_Diagnostic;

#if defined(__cplusplus)
extern "C" {
#endif

void build_coordinator_init(Build_Coordinator* coordinator);

Build_Request_Decision build_coordinator_request(
    Build_Coordinator* coordinator);

Build_Finish_Decision build_coordinator_finish(
    Build_Coordinator* coordinator,
    uint64_t generation,
    uint32_t build_succeeded);

Build_Coordinator_Result build_coordinator_mark_published(
    Build_Coordinator* coordinator,
    uint64_t generation);

/*
 * Parses one borrowed UTF-8 compiler-output line. Supported forms include:
 *   path(line,column): error C1234: message
 *   path:line:column: warning: message
 *   path:line: error: message
 * Returned String_View values point into line and remain valid only as long
 * as the caller keeps that line alive.
 */
uint32_t build_diagnostic_parse_line(
    String_View line,
    Build_Diagnostic* out_diagnostic);

const char* build_coordinator_result_string(
    Build_Coordinator_Result result);

#if defined(__cplusplus)
}
#endif

#endif /* KALAM_EDITOR_HOST_BUILD_COORDINATOR_H */
