add_executable(
    standalone_host_tests
    standalone_host_tests.cpp
)
target_link_libraries(
    standalone_host_tests
    PRIVATE
        standalone_host
        user32
)
kalam_apply_engine_policy(standalone_host_tests)
add_test(
    NAME standalone.win32_key_translation
    COMMAND standalone_host_tests
)

set(
    KALAM_STANDALONE_HOT_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/hot"
)
file(MAKE_DIRECTORY "${KALAM_STANDALONE_HOT_DIRECTORY}")

add_test(
    NAME standalone.hidden_warp_game_module
    COMMAND
        game
        --smoke
        --frames
        4
        --module
        "$<TARGET_FILE:game_module>"
        --hot-dir
        "${KALAM_STANDALONE_HOT_DIRECTORY}"
        --scene
        "${CMAKE_SOURCE_DIR}/examples/cube.scene.json"
        --post
        "${CMAKE_SOURCE_DIR}/examples/post_process.json"
)
set_tests_properties(
    standalone.hidden_warp_game_module
    PROPERTIES
        TIMEOUT 30
)
