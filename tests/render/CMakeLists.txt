add_executable(
    game_module_d3d11_debug_stress
    game_module_d3d11_debug_stress.cpp
)
add_dependencies(game_module_d3d11_debug_stress game_module)
target_link_libraries(
    game_module_d3d11_debug_stress
    PRIVATE
        d3d11_viewport_host
        module_bridge
        d3d11
        dxgi
        user32
)
kalam_apply_engine_policy(game_module_d3d11_debug_stress)
target_compile_definitions(
    game_module_d3d11_debug_stress
    PRIVATE
        KALAM_TEST_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)

add_test(
    NAME render.game_module_reload_resize_debug
    COMMAND
        game_module_d3d11_debug_stress
        $<TARGET_FILE:game_module>
)
set_tests_properties(
    render.game_module_reload_resize_debug
    PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 180
)

add_executable(
    dds_parse_tests
    dds_parse_tests.cpp
)
target_link_libraries(
    dds_parse_tests
    PRIVATE
        render_d3d11
)
kalam_apply_engine_policy(dds_parse_tests)
add_test(NAME render.dds_parse COMMAND dds_parse_tests)
set_tests_properties(
    render.dds_parse
    PROPERTIES
        LABELS "render"
        TIMEOUT 20
)
