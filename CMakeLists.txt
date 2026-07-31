cmake_minimum_required(VERSION 3.28)

project(
    kalam
    VERSION 0.1.0
    LANGUAGES C CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "Kalam Revision 1 targets Windows and D3D11.")
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(
        FATAL_ERROR
        "In-source builds are not supported. Use a CMake preset so generated "
        "files remain under build/."
    )
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KALAM_BUILD_TESTS "Build first-party CTest targets" ON)
option(KALAM_BUILD_EDITOR "Build Qt/Neovim editor targets" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(KalamTargetPolicy)

# ABI-only headers. Consumers receive only bridge/, never a repository-wide
# include path.
add_library(module_bridge INTERFACE)
target_include_directories(
    module_bridge
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/bridge"
)

add_library(
    engine_core
    STATIC
        engine/core/arena_win32.cpp
)
target_include_directories(
    engine_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/core"
)
kalam_apply_engine_policy(engine_core)

# Declare the acyclic engine seams up front. Concrete sources replace these
# interface targets milestone by milestone without changing dependency
# direction.
add_library(render_api INTERFACE)
target_link_libraries(render_api INTERFACE engine_core)

add_library(engine_runtime INTERFACE)
target_link_libraries(
    engine_runtime
    INTERFACE
        render_api
        engine_core
)

add_library(
    game_logic
    STATIC
        game/authored_state.cpp
)
target_include_directories(
    game_logic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/game"
)
target_link_libraries(
    game_logic
    PUBLIC
        engine_runtime
        engine_core
        module_bridge
)
kalam_apply_engine_policy(game_logic)

add_library(
    reflection_core
    STATIC
        reflection/reflection_tool.cpp
)
target_include_directories(
    reflection_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/reflection"
)
target_link_libraries(reflection_core PRIVATE engine_core)
kalam_apply_engine_policy(reflection_core)

add_executable(
    reflection_tool
    reflection/main.cpp
)
target_link_libraries(reflection_tool PRIVATE reflection_core)
kalam_apply_engine_policy(reflection_tool)

set(KALAM_GENERATED_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(
    KALAM_GENERATED_AUTHOR_SCHEMAS
    "${KALAM_GENERATED_DIRECTORY}/author_schemas.generated.h"
)
file(MAKE_DIRECTORY "${KALAM_GENERATED_DIRECTORY}")

add_custom_command(
    OUTPUT
        "${KALAM_GENERATED_AUTHOR_SCHEMAS}"
    COMMAND
        "$<TARGET_FILE:reflection_tool>"
        --output
        "${KALAM_GENERATED_AUTHOR_SCHEMAS}"
        "${CMAKE_CURRENT_SOURCE_DIR}/game/author_types.h"
    DEPENDS
        reflection_tool
        game/author_types.h
    COMMENT
        "Generating deterministic authored-field descriptors"
    VERBATIM
)
add_custom_target(
    author_schema_generation
    DEPENDS
        "${KALAM_GENERATED_AUTHOR_SCHEMAS}"
)
add_dependencies(game_logic author_schema_generation)
target_include_directories(
    game_logic
    PUBLIC
        "${KALAM_GENERATED_DIRECTORY}"
)

add_library(
    host_common
    STATIC
        editor/host/build_coordinator.cpp
        editor/host/frame_scheduler.cpp
        editor/host/input_latch.cpp
        editor/host/module_loader.cpp
)
target_include_directories(
    host_common
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/editor/host"
)
target_link_libraries(
    host_common
    PUBLIC
        module_bridge
    PRIVATE
        kernel32
)
kalam_apply_engine_policy(host_common)

add_library(
    d3d11_viewport_host
    STATIC
        editor/win32/d3d11_viewport_host.cpp
)
target_include_directories(
    d3d11_viewport_host
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/editor/win32"
)
target_link_libraries(
    d3d11_viewport_host
    PUBLIC
        module_bridge
    PRIVATE
        d3d11
        dxgi
)
kalam_apply_engine_policy(d3d11_viewport_host)

add_library(
    render_d3d11
    STATIC
        engine/render/d3d11/module_renderer.cpp
)
target_include_directories(
    render_d3d11
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/engine/render/d3d11
)
target_link_libraries(
    render_d3d11
    PUBLIC
        render_api
        engine_core
    PRIVATE
        d3d11
        d3dcompiler
)
kalam_apply_engine_policy(render_d3d11)

add_library(
    game_module
    SHARED
        game/game_module.cpp
)
add_dependencies(game_module author_schema_generation)
target_include_directories(
    game_module
    PRIVATE
        ${KALAM_GENERATED_DIRECTORY}
)
target_compile_definitions(
    game_module
    PRIVATE
        KALAM_GAME_MODULE_EXPORTS
)
target_link_libraries(
    game_module
    PRIVATE
        game_logic
        render_d3d11
        module_bridge
)
set_target_properties(
    game_module
    PROPERTIES
        OUTPUT_NAME game_module
)
kalam_apply_engine_policy(game_module)

add_subdirectory(standalone)

if(KALAM_BUILD_TESTS)
    include(CTest)
    enable_testing()

    function(kalam_add_public_header_smoke target_name header_name)
        cmake_parse_arguments(
            PARSE_ARGV 2
            header_smoke
            ""
            ""
            "INCLUDE_DIRECTORIES"
        )
        set(
            header_smoke_source
            "${KALAM_GENERATED_DIRECTORY}/header_smokes/${target_name}.cpp"
        )
        file(
            GENERATE
            OUTPUT "${header_smoke_source}"
            CONTENT "#include <${header_name}>\n"
        )
        add_library(${target_name} OBJECT "${header_smoke_source}")
        target_include_directories(
            ${target_name}
            PRIVATE
                ${header_smoke_INCLUDE_DIRECTORIES}
        )
        kalam_apply_engine_policy(${target_name})
        list(APPEND KALAM_PUBLIC_HEADER_SMOKE_TARGETS ${target_name})
        set(
            KALAM_PUBLIC_HEADER_SMOKE_TARGETS
            "${KALAM_PUBLIC_HEADER_SMOKE_TARGETS}"
            PARENT_SCOPE
        )
    endfunction()

    kalam_add_public_header_smoke(
        header_smoke_core_types
        core_types.h
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/engine/core"
    )
    kalam_add_public_header_smoke(
        header_smoke_string_view
        string_view.h
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/engine/core"
    )
    kalam_add_public_header_smoke(
        header_smoke_arena
        arena.h
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/engine/core"
    )
    kalam_add_public_header_smoke(
        header_smoke_core_templates
        core_templates.h
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/engine/core"
    )
    kalam_add_public_header_smoke(
        header_smoke_author_types
        author_types.h
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/game"
    )
    kalam_add_public_header_smoke(
        header_smoke_authored_state
        authored_state.h
        INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_SOURCE_DIR}/game"
            "${CMAKE_CURRENT_SOURCE_DIR}/bridge"
    )
    kalam_add_public_header_smoke(
        header_smoke_module_renderer
        module_renderer.h
        INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/render/d3d11"
    )
    kalam_add_public_header_smoke(
        header_smoke_build_coordinator
        build_coordinator.h
        INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_SOURCE_DIR}/editor/host"
            "${CMAKE_CURRENT_SOURCE_DIR}/bridge"
    )
    kalam_add_public_header_smoke(
        header_smoke_frame_scheduler
        frame_scheduler.h
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/editor/host"
    )
    kalam_add_public_header_smoke(
        header_smoke_input_latch
        input_latch.h
        INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_SOURCE_DIR}/editor/host"
            "${CMAKE_CURRENT_SOURCE_DIR}/bridge"
    )
    kalam_add_public_header_smoke(
        header_smoke_module_loader
        module_loader.h
        INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_SOURCE_DIR}/editor/host"
            "${CMAKE_CURRENT_SOURCE_DIR}/bridge"
    )
    kalam_add_public_header_smoke(
        header_smoke_d3d11_viewport_host
        d3d11_viewport_host.h
        INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_SOURCE_DIR}/editor/win32"
            "${CMAKE_CURRENT_SOURCE_DIR}/bridge"
    )
    kalam_add_public_header_smoke(
        header_smoke_reflection_tool
        reflection_tool.h
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/reflection"
    )
    kalam_add_public_header_smoke(
        header_smoke_standalone_host
        standalone_host.h
        INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_SOURCE_DIR}/standalone"
            "${CMAKE_CURRENT_SOURCE_DIR}/bridge"
    )
    add_custom_target(
        public_header_smokes
        ALL
        DEPENDS ${KALAM_PUBLIC_HEADER_SMOKE_TARGETS}
    )

    add_executable(
        core_tests
        tests/core/core_tests.cpp
    )
    target_link_libraries(core_tests PRIVATE engine_core)
    kalam_apply_engine_policy(core_tests)
    add_test(NAME core.memory_and_containers COMMAND core_tests)

    add_executable(
        bridge_author_schema_c_smoke
        tests/bridge/author_schema_c_smoke.c
    )
    target_link_libraries(
        bridge_author_schema_c_smoke
        PRIVATE
            module_bridge
    )
    kalam_apply_c_abi_policy(bridge_author_schema_c_smoke)
    add_test(
        NAME bridge.author_schema_c_layout
        COMMAND bridge_author_schema_c_smoke
    )

    add_executable(
        bridge_author_schema_cpp_smoke
        tests/bridge/author_schema_cpp_smoke.cpp
    )
    target_link_libraries(
        bridge_author_schema_cpp_smoke
        PRIVATE
            module_bridge
    )
    kalam_apply_engine_policy(bridge_author_schema_cpp_smoke)
    add_test(
        NAME bridge.author_schema_cpp_layout
        COMMAND bridge_author_schema_cpp_smoke
    )

    add_executable(
        bridge_game_api_c_smoke
        tests/bridge/game_api_c_smoke.c
    )
    target_link_libraries(
        bridge_game_api_c_smoke
        PRIVATE
            module_bridge
    )
    kalam_apply_c_abi_policy(bridge_game_api_c_smoke)
    add_test(NAME bridge.game_api_c_layout COMMAND bridge_game_api_c_smoke)

    add_executable(
        bridge_game_api_cpp_smoke
        tests/bridge/game_api_cpp_smoke.cpp
    )
    target_link_libraries(
        bridge_game_api_cpp_smoke
        PRIVATE
            module_bridge
    )
    kalam_apply_engine_policy(bridge_game_api_cpp_smoke)
    add_test(NAME bridge.game_api_cpp_layout COMMAND bridge_game_api_cpp_smoke)

    add_executable(
        frame_input_tests
        tests/host/frame_input_tests.cpp
    )
    target_link_libraries(frame_input_tests PRIVATE host_common)
    kalam_apply_engine_policy(frame_input_tests)
    add_test(NAME host.frame_scheduler_and_input COMMAND frame_input_tests)

    add_executable(
        build_coordinator_tests
        tests/host/build_coordinator_tests.cpp
    )
    target_link_libraries(build_coordinator_tests PRIVATE host_common)
    kalam_apply_engine_policy(build_coordinator_tests)
    add_test(
        NAME host.build_coordinator_and_diagnostics
        COMMAND build_coordinator_tests
    )

    add_executable(
        authored_state_tests
        tests/game/authored_state_tests.cpp
    )
    target_link_libraries(authored_state_tests PRIVATE game_logic)
    kalam_apply_engine_policy(authored_state_tests)
    add_test(NAME game.authored_state COMMAND authored_state_tests)

    add_executable(
        d3d11_viewport_host_smoke
        tests/render/d3d11_viewport_host_smoke.cpp
    )
    target_link_libraries(
        d3d11_viewport_host_smoke
        PRIVATE
            d3d11_viewport_host
            d3d11
            dxgi
            user32
    )
    kalam_apply_engine_policy(d3d11_viewport_host_smoke)
    add_test(
        NAME render.d3d11_warp_viewport
        COMMAND d3d11_viewport_host_smoke
    )

    add_executable(
        game_module_warp_smoke
        tests/render/game_module_warp_smoke.cpp
    )
    add_dependencies(game_module_warp_smoke game_module)
    target_link_libraries(
        game_module_warp_smoke
        PRIVATE
            d3d11_viewport_host
            module_bridge
            d3d11
            dxgi
            user32
    )
    kalam_apply_engine_policy(game_module_warp_smoke)
    add_test(
        NAME render.game_module_warp
        COMMAND
            game_module_warp_smoke
            $<TARGET_FILE:game_module>
    )

    add_subdirectory(tests/render)

    add_executable(
        reflection_tool_tests
        tests/reflection/reflection_tool_tests.cpp
    )
    target_link_libraries(reflection_tool_tests PRIVATE reflection_core)
    kalam_apply_engine_policy(reflection_tool_tests)
    add_test(NAME reflection.generator_unit COMMAND reflection_tool_tests)

    set(
        KALAM_REFLECTION_FIXTURE_OUTPUT
        "${KALAM_GENERATED_DIRECTORY}/tests/author_schemas_fixture.generated.h"
    )
    file(MAKE_DIRECTORY "${KALAM_GENERATED_DIRECTORY}/tests")
    add_custom_command(
        OUTPUT
            "${KALAM_REFLECTION_FIXTURE_OUTPUT}"
        COMMAND
            "$<TARGET_FILE:reflection_tool>"
            --output
            "${KALAM_REFLECTION_FIXTURE_OUTPUT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/reflection/fixtures/authored_fixture.h"
        DEPENDS
            reflection_tool
            tests/reflection/fixtures/authored_fixture.h
        COMMENT
            "Generating reflection fixture descriptors"
        VERBATIM
    )

    add_executable(
        reflection_generated_header_c_smoke
        tests/reflection/generated_header_c_smoke.c
        "${KALAM_REFLECTION_FIXTURE_OUTPUT}"
    )
    target_include_directories(
        reflection_generated_header_c_smoke
        PRIVATE
            "${KALAM_GENERATED_DIRECTORY}/tests"
    )
    target_link_libraries(
        reflection_generated_header_c_smoke
        PRIVATE
            module_bridge
    )
    kalam_apply_c_abi_policy(reflection_generated_header_c_smoke)
    add_test(
        NAME reflection.generated_header_c
        COMMAND reflection_generated_header_c_smoke
    )

    add_executable(
        reflection_generated_header_cpp_smoke
        tests/reflection/generated_header_cpp_smoke.cpp
        "${KALAM_REFLECTION_FIXTURE_OUTPUT}"
    )
    target_include_directories(
        reflection_generated_header_cpp_smoke
        PRIVATE
            "${KALAM_GENERATED_DIRECTORY}/tests"
    )
    target_link_libraries(
        reflection_generated_header_cpp_smoke
        PRIVATE
            module_bridge
    )
    kalam_apply_engine_policy(reflection_generated_header_cpp_smoke)
    add_test(
        NAME reflection.generated_header_cpp
        COMMAND reflection_generated_header_cpp_smoke
    )

    find_program(KALAM_NEOVIM_EXECUTABLE NAMES nvim)
    if(KALAM_NEOVIM_EXECUTABLE)
        add_test(
            NAME neovim.runtime_document_bridge
            COMMAND
                "${KALAM_NEOVIM_EXECUTABLE}"
                --headless
                -u
                NONE
                -l
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/neovim/runtime_tests.lua"
        )
        set_tests_properties(
            neovim.runtime_document_bridge
            PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                TIMEOUT 30
        )
    else()
        message(
            STATUS
            "Neovim was not found; the headless runtime bridge test is skipped."
        )
    endif()

    add_subdirectory(tests/reload)
    add_subdirectory(tests/standalone)
    add_subdirectory(tests/firewall)

    set(
        KALAM_SOURCE_TREE_BASELINE
        "${KALAM_GENERATED_DIRECTORY}/tests/source_tree_at_configure.sha256"
    )
    set(
        KALAM_SOURCE_TREE_AFTER_BUILD
        "${KALAM_GENERATED_DIRECTORY}/tests/source_tree_after_build.sha256"
    )
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            "-DKALAM_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DKALAM_SNAPSHOT_OUTPUT=${KALAM_SOURCE_TREE_BASELINE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/KalamSourceTreeSnapshot.cmake"
        RESULT_VARIABLE KALAM_SOURCE_TREE_BASELINE_RESULT
    )
    if(NOT KALAM_SOURCE_TREE_BASELINE_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not capture the configured source-tree state.")
    endif()

    add_test(
        NAME build.source_tree_unchanged
        COMMAND
            "${CMAKE_COMMAND}"
            "-DKALAM_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DKALAM_SNAPSHOT_OUTPUT=${KALAM_SOURCE_TREE_AFTER_BUILD}"
            "-DKALAM_EXPECTED_SNAPSHOT=${KALAM_SOURCE_TREE_BASELINE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/KalamSourceTreeSnapshot.cmake"
    )
    add_test(
        NAME build.ninja_noop
        COMMAND
            "${CMAKE_COMMAND}"
            "-DKALAM_BINARY_DIR=${CMAKE_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/KalamNoOpBuild.cmake"
    )
    set_tests_properties(
        build.source_tree_unchanged
        build.ninja_noop
        PROPERTIES
            LABELS "build"
            RUN_SERIAL TRUE
            TIMEOUT 150
    )
endif()

if(KALAM_BUILD_EDITOR)
    include(KalamEditorRuntime)
    add_subdirectory(editor/qt)
endif()
