cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(
        kalam_reload_tests
        LANGUAGES CXX
    )

    if(NOT WIN32)
        message(FATAL_ERROR "The module reload tests require Win32.")
    endif()

    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
        message(FATAL_ERROR "Use an out-of-source build for reload tests.")
    endif()

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

set(KALAM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
include("${KALAM_ROOT}/cmake/KalamTargetPolicy.cmake")

add_library(
    reload_module_loader
    STATIC
        "${KALAM_ROOT}/editor/host/module_loader.cpp"
)
target_include_directories(
    reload_module_loader
    PUBLIC
        "${KALAM_ROOT}/editor/host"
        "${KALAM_ROOT}/bridge"
)
target_link_libraries(
    reload_module_loader
    PRIVATE
        kernel32
)
kalam_apply_engine_policy(reload_module_loader)

function(kalam_add_reload_fake target_name source_name)
    add_library(
        ${target_name}
        SHARED
            "${CMAKE_CURRENT_LIST_DIR}/${source_name}"
    )
    target_include_directories(
        ${target_name}
        PRIVATE
            "${KALAM_ROOT}/bridge"
    )
    target_compile_definitions(
        ${target_name}
        PRIVATE
            KALAM_GAME_MODULE_EXPORTS=1
            ${ARGN}
    )
    kalam_apply_engine_policy(${target_name})
endfunction()

kalam_add_reload_fake(reload_fake_valid_a fake_module_a.cpp)
kalam_add_reload_fake(reload_fake_valid_b fake_module_b.cpp)
kalam_add_reload_fake(
    reload_fake_missing_entry
    fake_module_missing_entry.cpp
)
kalam_add_reload_fake(
    reload_fake_invalid_version
    fake_module_invalid.cpp
    KALAM_RELOAD_FAKE_FAILURE=1
)
kalam_add_reload_fake(
    reload_fake_older_version
    fake_module_invalid.cpp
    KALAM_RELOAD_FAKE_FAILURE=10
)
kalam_add_reload_fake(
    reload_fake_invalid_size
    fake_module_invalid.cpp
    KALAM_RELOAD_FAKE_FAILURE=2
)
kalam_add_reload_fake(
    reload_fake_invalid_pointer
    fake_module_invalid.cpp
    KALAM_RELOAD_FAKE_FAILURE=3
)
kalam_add_reload_fake(
    reload_fake_invalid_create
    fake_module_invalid.cpp
    KALAM_RELOAD_FAKE_FAILURE=4
)
kalam_add_reload_fake(
    reload_fake_invalid_schema_query
    fake_module_invalid.cpp
    KALAM_RELOAD_FAKE_FAILURE=5
)
kalam_add_reload_fake(
    reload_fake_invalid_schema_duplicate
    fake_module_invalid.cpp
    KALAM_RELOAD_FAKE_FAILURE=6
)
kalam_add_reload_fake(
    reload_fake_invalid_apply
    fake_module_invalid.cpp
    KALAM_RELOAD_FAKE_FAILURE=7
)
kalam_add_reload_fake(
    reload_fake_invalid_schema_field
    fake_module_invalid.cpp
    KALAM_RELOAD_FAKE_FAILURE=9
)

add_executable(
    reload_module_loader_tests
    module_loader_tests.cpp
)
target_link_libraries(
    reload_module_loader_tests
    PRIVATE
        reload_module_loader
        kernel32
)
kalam_apply_engine_policy(reload_module_loader_tests)
target_compile_definitions(
    reload_module_loader_tests
    PRIVATE
        KALAM_TEST_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)

add_dependencies(
    reload_module_loader_tests
    reload_fake_valid_a
    reload_fake_valid_b
    reload_fake_missing_entry
    reload_fake_invalid_version
    reload_fake_invalid_size
    reload_fake_invalid_pointer
    reload_fake_invalid_create
    reload_fake_invalid_schema_query
    reload_fake_invalid_schema_duplicate
    reload_fake_invalid_schema_field
    reload_fake_invalid_apply
    reload_fake_older_version
)

set(KALAM_RELOAD_HOT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/hot")
file(MAKE_DIRECTORY "${KALAM_RELOAD_HOT_DIRECTORY}")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
    enable_testing()
endif()
add_test(
    NAME reload.transactional_candidate_swap
    COMMAND
        reload_module_loader_tests
        "${KALAM_RELOAD_HOT_DIRECTORY}"
        "$<TARGET_FILE:reload_fake_valid_a>"
        "$<TARGET_FILE:reload_fake_valid_b>"
        "$<TARGET_FILE:reload_fake_missing_entry>"
        "$<TARGET_FILE:reload_fake_invalid_version>"
        "$<TARGET_FILE:reload_fake_invalid_size>"
        "$<TARGET_FILE:reload_fake_invalid_pointer>"
        "$<TARGET_FILE:reload_fake_invalid_create>"
        "$<TARGET_FILE:reload_fake_invalid_schema_query>"
        "$<TARGET_FILE:reload_fake_invalid_schema_duplicate>"
        "$<TARGET_FILE:reload_fake_invalid_schema_field>"
        "$<TARGET_FILE:reload_fake_invalid_apply>"
)

if(TARGET game_module AND TARGET d3d11_viewport_host)
    add_executable(
        reload_real_module_rejection_render
        real_module_rejection_render.cpp
    )
    target_link_libraries(
        reload_real_module_rejection_render
        PRIVATE
            reload_module_loader
            d3d11_viewport_host
            module_bridge
            d3d11
            dxgi
            user32
            kernel32
    )
    kalam_apply_engine_policy(reload_real_module_rejection_render)
target_compile_definitions(
    reload_real_module_rejection_render
    PRIVATE
        KALAM_TEST_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)
    add_dependencies(
        reload_real_module_rejection_render
        game_module
        reload_fake_invalid_version
        reload_fake_invalid_create
        reload_fake_invalid_schema_duplicate
        reload_fake_invalid_apply
    )
    add_test(
        NAME reload.real_module_rejection_preserves_render
        COMMAND
            reload_real_module_rejection_render
            "${KALAM_RELOAD_HOT_DIRECTORY}"
            "$<TARGET_FILE:game_module>"
            "${KALAM_ROOT}/examples/cube.scene.json"
            "$<TARGET_FILE:reload_fake_invalid_version>"
            "$<TARGET_FILE:reload_fake_invalid_create>"
            "$<TARGET_FILE:reload_fake_invalid_schema_duplicate>"
            "$<TARGET_FILE:reload_fake_invalid_apply>"
    )
    set_tests_properties(
        reload.real_module_rejection_preserves_render
        PROPERTIES TIMEOUT 120
    )
endif()

add_executable(
    reload_api_version_compat_tests
    api_version_compat_tests.cpp
)
target_link_libraries(
    reload_api_version_compat_tests
    PRIVATE
        reload_module_loader
        kernel32
)
kalam_apply_engine_policy(reload_api_version_compat_tests)
target_compile_definitions(
    reload_api_version_compat_tests
    PRIVATE
        KALAM_TEST_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)
add_dependencies(
    reload_api_version_compat_tests
    reload_fake_older_version
    reload_fake_invalid_version
)
add_test(
    NAME reload.api_version_compatibility
    COMMAND
        reload_api_version_compat_tests
        "${KALAM_RELOAD_HOT_DIRECTORY}"
        "$<TARGET_FILE:reload_fake_older_version>"
        "$<TARGET_FILE:reload_fake_invalid_version>"
)
set_tests_properties(
    reload.api_version_compatibility
    PROPERTIES
        LABELS "reload"
        TIMEOUT 30
)
