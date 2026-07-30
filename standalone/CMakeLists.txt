add_library(
    standalone_host
    STATIC
        standalone_host.cpp
)
target_include_directories(
    standalone_host
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}"
)
target_link_libraries(
    standalone_host
    PUBLIC
        module_bridge
        host_common
        d3d11_viewport_host
    PRIVATE
        kernel32
        user32
)
kalam_apply_engine_policy(standalone_host)

add_executable(
    game
    game_main.cpp
)
target_link_libraries(
    game
    PRIVATE
        standalone_host
)
set_target_properties(
    game
    PROPERTIES
        OUTPUT_NAME game
)
kalam_apply_engine_policy(game)

# This is a build-order edge only. game.exe resolves game_get_api dynamically
# and never import-links against game_module.dll.
if(TARGET game_module)
    add_dependencies(game game_module)
endif()
