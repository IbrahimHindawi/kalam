cmake_minimum_required(VERSION 3.28)

foreach(_kalam_required IN ITEMS KALAM_FIREWALL_SCRIPT KALAM_TEST_ROOT)
    if(NOT DEFINED ${_kalam_required} OR "${${_kalam_required}}" STREQUAL "")
        message(FATAL_ERROR "${_kalam_required} is required.")
    endif()
endforeach()

cmake_path(
    ABSOLUTE_PATH KALAM_FIREWALL_SCRIPT
    NORMALIZE
    OUTPUT_VARIABLE _kalam_firewall_script
)
cmake_path(
    ABSOLUTE_PATH KALAM_TEST_ROOT
    NORMALIZE
    OUTPUT_VARIABLE _kalam_test_root
)

if(NOT EXISTS "${_kalam_firewall_script}")
    message(FATAL_ERROR "Firewall script does not exist: ${_kalam_firewall_script}")
endif()

# The caller supplies a dedicated build-tree directory. Refuse suspiciously
# broad paths before clearing stale fixture files from an earlier test run.
string(LENGTH "${_kalam_test_root}" _kalam_test_root_length)
if(_kalam_test_root_length LESS 12)
    message(FATAL_ERROR "KALAM_TEST_ROOT is too broad: ${_kalam_test_root}")
endif()

file(REMOVE_RECURSE "${_kalam_test_root}")
file(
    MAKE_DIRECTORY
    "${_kalam_test_root}/bridge"
    "${_kalam_test_root}/editor/qt"
    "${_kalam_test_root}/engine/render/d3d11"
)
file(WRITE "${_kalam_test_root}/bridge/clean.h" "#include <stdint.h>\n")
file(
    WRITE
    "${_kalam_test_root}/editor/qt/allowed.cpp"
    "#include <QWidget>\n"
)
file(
    WRITE
    "${_kalam_test_root}/engine/render/d3d11/allowed.cpp"
    "#include <d3d11.h>\n"
)

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        "-DKALAM_SOURCE_ROOT=${_kalam_test_root}"
        -P "${_kalam_firewall_script}"
    RESULT_VARIABLE _kalam_clean_result
    OUTPUT_VARIABLE _kalam_clean_stdout
    ERROR_VARIABLE _kalam_clean_stderr
)
if(NOT _kalam_clean_result EQUAL 0)
    message(
        FATAL_ERROR
        "The source firewall rejected its clean fixture.\n"
        "stdout:\n${_kalam_clean_stdout}\n"
        "stderr:\n${_kalam_clean_stderr}"
    )
endif()

file(
    MAKE_DIRECTORY
    "${_kalam_test_root}/editor/host"
    "${_kalam_test_root}/engine/runtime"
    "${_kalam_test_root}/engine/simulation"
    "${_kalam_test_root}/engine/public"
)
file(
    WRITE
    "${_kalam_test_root}/editor/qt/policy_bad.cpp"
    "#include <string>\n"
    "#include <QtNetwork/QTcpSocket>\n"
    "#include <d3d11.h>\n"
    "#include <ui_generated.h>\n"
    "std::string bad;\n"
    "QDockWidget* dock = 0;\n"
    "void fail() { throw 1; }\n"
)
file(
    WRITE
    "${_kalam_test_root}/engine/runtime/stl_bad.cpp"
    "#include <vector>\n"
    "std::vector<int> bad;\n"
)
file(WRITE "${_kalam_test_root}/bridge/qt_bad.h" "#include <QtCore/QObject>\n")
file(WRITE "${_kalam_test_root}/bridge/d3d_bad.h" "#include <dxgi1_6.h>\n")
file(
    WRITE
    "${_kalam_test_root}/editor/host/qt_bad.cpp"
    "# include \"qwidget.h\"\n"
)
file(
    WRITE
    "${_kalam_test_root}/engine/runtime/d3d_bad.h"
    "#include <d3d11.h>\n"
)
file(
    WRITE
    "${_kalam_test_root}/engine/simulation/d3d_bad.cpp"
    "#include <d3dcommon.h>\n"
)
file(
    WRITE
    "${_kalam_test_root}/engine/public/upward_bad.h"
    "#include \"../../game/game.h\"\n"
    "#include \"../../editor/host/editor_host.h\"\n"
)

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        "-DKALAM_SOURCE_ROOT=${_kalam_test_root}"
        -P "${_kalam_firewall_script}"
    RESULT_VARIABLE _kalam_bad_result
    OUTPUT_VARIABLE _kalam_bad_stdout
    ERROR_VARIABLE _kalam_bad_stderr
)

if(_kalam_bad_result EQUAL 0)
    message(FATAL_ERROR "The source firewall accepted its forbidden fixture.")
endif()

set(_kalam_bad_output "${_kalam_bad_stdout}\n${_kalam_bad_stderr}")
set(
    _kalam_expected_diagnostics
    "bridge/d3d_bad.h:1: D3D/DXGI include is forbidden here: dxgi1_6.h"
    "bridge/qt_bad.h:1: Qt include is forbidden here: QtCore/QObject"
    "editor/host/qt_bad.cpp:1: Qt include is forbidden here: qwidget.h"
    "editor/qt/policy_bad.cpp:1: STL header is forbidden in first-party code: string"
    "editor/qt/policy_bad.cpp:2: Qt module or UI framework is forbidden here: QtNetwork/QTcpSocket"
    "editor/qt/policy_bad.cpp:3: D3D/DXGI include is forbidden here: d3d11.h"
    "editor/qt/policy_bad.cpp:4: Qt module or UI framework is forbidden here: ui_generated.h"
    "editor/qt/policy_bad.cpp:5: STL namespace use is forbidden in first-party code"
    "editor/qt/policy_bad.cpp:6: dock/plugin framework use is forbidden in the Qt adapter"
    "editor/qt/policy_bad.cpp:7: exceptions or RTTI are forbidden in first-party code"
    "engine/public/upward_bad.h:1: engine dependency points upward: ../../game/game.h"
    "engine/public/upward_bad.h:2: engine dependency points upward: ../../editor/host/editor_host.h"
    "engine/runtime/d3d_bad.h:1: D3D/DXGI include is forbidden here: d3d11.h"
    "engine/runtime/stl_bad.cpp:1: STL header is forbidden in first-party code: vector"
    "engine/runtime/stl_bad.cpp:2: STL namespace use is forbidden in first-party code"
    "engine/simulation/d3d_bad.cpp:1: D3D/DXGI include is forbidden here: d3dcommon.h"
)
foreach(_kalam_expected IN LISTS _kalam_expected_diagnostics)
    string(FIND "${_kalam_bad_output}" "${_kalam_expected}" _kalam_position)
    if(_kalam_position EQUAL -1)
        message(
            FATAL_ERROR
            "Missing expected firewall diagnostic:\n"
            "  ${_kalam_expected}\n"
            "Actual output:\n${_kalam_bad_output}"
        )
    endif()
endforeach()

message(STATUS "Kalam source firewall self-test passed.")
