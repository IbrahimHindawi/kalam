if(NOT BUILD_TESTING)
    return()
endif()

set(_kalam_source_firewall "${PROJECT_SOURCE_DIR}/cmake/KalamSourceFirewall.cmake")
set(
    _kalam_binary_firewall
    "${PROJECT_SOURCE_DIR}/cmake/KalamBinaryImportFirewall.cmake"
)

add_test(
    NAME firewall.source_includes
    COMMAND
        "${CMAKE_COMMAND}"
        "-DKALAM_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        -P "${_kalam_source_firewall}"
)

add_test(
    NAME firewall.source_scanner_self_test
    COMMAND
        "${CMAKE_COMMAND}"
        "-DKALAM_FIREWALL_SCRIPT=${_kalam_source_firewall}"
        "-DKALAM_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/source_scanner_fixture"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/source_firewall_self_test.cmake"
)

set(_kalam_binary_firewall_arguments)
if(TARGET game)
    list(
        APPEND _kalam_binary_firewall_arguments
        "-DKALAM_GAME_EXE=$<TARGET_FILE:game>"
    )
endif()
if(TARGET game_module)
    list(
        APPEND _kalam_binary_firewall_arguments
        "-DKALAM_GAME_MODULE=$<TARGET_FILE:game_module>"
    )
endif()

add_test(
    NAME firewall.shipping_binary_imports
    COMMAND
        "${CMAKE_COMMAND}"
        ${_kalam_binary_firewall_arguments}
        -P "${_kalam_binary_firewall}"
)

add_library(kalam_firewall_fake_qt_core SHARED fake_qt_core.c)
set_target_properties(
    kalam_firewall_fake_qt_core
    PROPERTIES
        DEBUG_POSTFIX ""
        OUTPUT_NAME "Qt6Core"
        PREFIX ""
)

add_executable(kalam_firewall_fake_qt_importer fake_qt_importer.c)
target_link_libraries(
    kalam_firewall_fake_qt_importer
    PRIVATE
        kalam_firewall_fake_qt_core
)
if(COMMAND kalam_apply_c_abi_policy)
    kalam_apply_c_abi_policy(kalam_firewall_fake_qt_core)
    kalam_apply_c_abi_policy(kalam_firewall_fake_qt_importer)
endif()

add_test(
    NAME firewall.binary_import_scanner_self_test
    COMMAND
        "${CMAKE_COMMAND}"
        "-DKALAM_FIREWALL_SCRIPT=${_kalam_binary_firewall}"
        "-DKALAM_QT_IMPORT_FIXTURE=$<TARGET_FILE:kalam_firewall_fake_qt_importer>"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/binary_import_firewall_self_test.cmake"
)

set_tests_properties(
    firewall.source_includes
    firewall.source_scanner_self_test
    firewall.shipping_binary_imports
    firewall.binary_import_scanner_self_test
    PROPERTIES
        LABELS "firewall"
)
