if(NOT DEFINED KALAM_EDITOR_EXECUTABLE OR
   NOT DEFINED KALAM_QT_CORE_RUNTIME_NAME OR
   NOT DEFINED KALAM_QT_GUI_RUNTIME_NAME OR
   NOT DEFINED KALAM_QT_WIDGETS_RUNTIME_NAME OR
   NOT DEFINED KALAM_SYSTEM_ROOT)
    message(FATAL_ERROR "Qt runtime deployment test arguments are incomplete.")
endif()

get_filename_component(
    editor_directory
    "${KALAM_EDITOR_EXECUTABLE}"
    DIRECTORY
)

foreach(runtime_name IN ITEMS
        "${KALAM_QT_CORE_RUNTIME_NAME}"
        "${KALAM_QT_GUI_RUNTIME_NAME}"
        "${KALAM_QT_WIDGETS_RUNTIME_NAME}")
    if(NOT EXISTS "${editor_directory}/${runtime_name}")
        message(FATAL_ERROR
            "Missing deployed Qt runtime: ${editor_directory}/${runtime_name}")
    endif()
endforeach()

file(
    GLOB
    windows_platform_plugins
    "${editor_directory}/platforms/qwindows*.dll"
)
if(NOT windows_platform_plugins)
    message(FATAL_ERROR
        "Missing deployed Qt Windows platform plugin under "
        "${editor_directory}/platforms")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -E
        env
        --unset=QT_PLUGIN_PATH
        "PATH=${KALAM_SYSTEM_ROOT}/System32"
        --
        "${KALAM_EDITOR_EXECUTABLE}"
        --version
    RESULT_VARIABLE editor_result
    OUTPUT_VARIABLE editor_stdout
    ERROR_VARIABLE editor_stderr
    TIMEOUT 10
)
if(NOT editor_result EQUAL 0)
    message(FATAL_ERROR
        "The deployed editor did not start with Qt removed from PATH "
        "(exit ${editor_result}).\n"
        "stdout:\n${editor_stdout}\n"
        "stderr:\n${editor_stderr}")
endif()

message(STATUS
    "Verified self-contained Qt runtime deployment for ${KALAM_EDITOR_EXECUTABLE}")
