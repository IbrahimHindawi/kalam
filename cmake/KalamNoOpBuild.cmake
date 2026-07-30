cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED KALAM_BINARY_DIR OR KALAM_BINARY_DIR STREQUAL "")
    message(FATAL_ERROR "KALAM_BINARY_DIR is required.")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${KALAM_BINARY_DIR}"
    RESULT_VARIABLE _kalam_build_result
    OUTPUT_VARIABLE _kalam_build_stdout
    ERROR_VARIABLE _kalam_build_stderr
    TIMEOUT 120
)
if(NOT _kalam_build_result EQUAL 0)
    message(
        FATAL_ERROR
        "The no-op build failed (${_kalam_build_result}).\n"
        "stdout:\n${_kalam_build_stdout}\n"
        "stderr:\n${_kalam_build_stderr}"
    )
endif()

set(_kalam_build_output "${_kalam_build_stdout}\n${_kalam_build_stderr}")
if(NOT _kalam_build_output MATCHES "ninja: no work to do[.]")
    message(
        FATAL_ERROR
        "The post-build Ninja invocation performed work or gave an "
        "unexpected result.\n${_kalam_build_output}"
    )
endif()

message(STATUS "Kalam no-op Ninja build performed no work.")
