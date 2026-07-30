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
