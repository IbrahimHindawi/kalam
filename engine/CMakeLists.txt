cmake_minimum_required(VERSION 3.24)
project(kalam_frontend NONE)
# Keep the previous Windows CMake build entry point. Cargo xtask also packages assets.
find_program(CARGO cargo REQUIRED)
set(engine_build ${CMAKE_CURRENT_SOURCE_DIR}/build/windows)
add_custom_target(kalam ALL
    COMMAND ${CMAKE_COMMAND} -E env "SDL3_DIR=${SDL3_DIR}"
            ${CARGO} xtask build --target windows
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${engine_build}/kalam.exe ${CMAKE_CURRENT_BINARY_DIR}/kalam.exe
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${engine_build}/assets ${CMAKE_CURRENT_BINARY_DIR}/assets
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    VERBATIM USES_TERMINAL)
