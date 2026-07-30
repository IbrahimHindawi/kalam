cmake_minimum_required(VERSION 3.28)

foreach(
    _kalam_required
    IN ITEMS KALAM_FIREWALL_SCRIPT KALAM_QT_IMPORT_FIXTURE
)
    if(NOT DEFINED ${_kalam_required} OR "${${_kalam_required}}" STREQUAL "")
        message(FATAL_ERROR "${_kalam_required} is required.")
    endif()
endforeach()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        "-DKALAM_BINARY_PATHS=${KALAM_QT_IMPORT_FIXTURE}"
        -P "${KALAM_FIREWALL_SCRIPT}"
    RESULT_VARIABLE _kalam_result
    OUTPUT_VARIABLE _kalam_stdout
    ERROR_VARIABLE _kalam_stderr
)

if(_kalam_result EQUAL 0)
    message(
        FATAL_ERROR
        "The binary import firewall accepted a Qt-importing PE fixture."
    )
endif()

set(_kalam_output "${_kalam_stdout}\n${_kalam_stderr}")
string(
    FIND "${_kalam_output}"
    "imports Qt library: Qt6Core.dll"
    _kalam_diagnostic_position
)
if(_kalam_diagnostic_position EQUAL -1)
    message(
        FATAL_ERROR
        "The binary import firewall failed without the expected diagnostic.\n"
        "Actual output:\n${_kalam_output}"
    )
endif()

message(STATUS "Kalam binary import firewall self-test passed.")
