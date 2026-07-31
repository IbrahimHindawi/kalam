find_package(
    Qt6
    6.8.3
    EXACT
    REQUIRED
    COMPONENTS Core Gui Widgets
)

set(
    KALAM_EDITOR_QT_SOURCES
    author_transaction_controller.cpp
    author_transaction_controller.h
    build_controller.cpp
    build_controller.h
    context_popup.cpp
    context_popup.h
    editor_main_window.cpp
    editor_main_window.h
    editor_neovim_adapter.cpp
    editor_neovim_adapter.h
    editor_neovim_shell.cpp
    editor_neovim_shell.h
    editor_runtime_host.cpp
    editor_runtime_host.h
    editor_types.h
    engine_viewport_widget.cpp
    engine_viewport_widget.h
    generated_form.cpp
    generated_form.h
    neovim_grid_model.cpp
    neovim_grid_model.h
    neovim_grid_widget.cpp
    neovim_grid_widget.h
    neovim_input_translation.cpp
    neovim_input_translation.h
    neovim_msgpack.cpp
    neovim_msgpack.h
    neovim_rpc_client.cpp
    neovim_rpc_client.h
    neovim_theme_bridge.cpp
    neovim_theme_bridge.h
    qt_value_widgets.cpp
    qt_value_widgets.h
)

add_library(kalam_editor_qt STATIC ${KALAM_EDITOR_QT_SOURCES})
add_library(Kalam::EditorQt ALIAS kalam_editor_qt)
target_include_directories(
    kalam_editor_qt
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(
    kalam_editor_qt
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)
target_link_libraries(
    kalam_editor_qt
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        module_bridge
    PRIVATE
        host_common
        d3d11_viewport_host
)
set_target_properties(
    kalam_editor_qt
    PROPERTIES
        AUTOMOC ON
        AUTORCC OFF
        AUTOUIC OFF
        FOLDER "editor"
)
kalam_apply_qt_policy(kalam_editor_qt)

qt_add_executable(nveditor WIN32 main.cpp)
target_compile_definitions(
    nveditor
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        KALAM_SOURCE_ROOT_DEFAULT="${CMAKE_SOURCE_DIR}"
        KALAM_BUILD_DIRECTORY_DEFAULT="${CMAKE_BINARY_DIR}"
        KALAM_GAME_MODULE_DEFAULT="$<TARGET_FILE:game_module>"
        KALAM_HOT_COPY_DIRECTORY_DEFAULT="${CMAKE_BINARY_DIR}/hot-editor-modules"
        KALAM_NEOVIM_RUNTIME_RELATIVE_PATH="nvim-runtime"
)
target_link_libraries(
    nveditor
    PRIVATE
        Kalam::EditorQt
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)
add_dependencies(nveditor game_module)
set_target_properties(
    nveditor
    PROPERTIES
        AUTOMOC ON
        AUTORCC OFF
        AUTOUIC OFF
        FOLDER "editor"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
kalam_apply_qt_policy(nveditor)
kalam_stage_editor_runtime(
    nveditor
    "${CMAKE_SOURCE_DIR}/editor/neovim/runtime"
)

if(WIN32)
    if(NOT TARGET Qt6::windeployqt)
        message(FATAL_ERROR
            "The Qt editor build requires the Qt6::windeployqt tool target.")
    endif()

    add_custom_command(
        TARGET nveditor
        POST_BUILD
        COMMAND
            $<TARGET_FILE:Qt6::windeployqt>
            --dir $<TARGET_FILE_DIR:nveditor>
            --no-translations
            --no-system-d3d-compiler
            --no-compiler-runtime
            $<TARGET_FILE:nveditor>
        COMMENT "Deploying the Qt runtime beside nveditor"
        VERBATIM
    )
endif()

if(KALAM_BUILD_TESTS)
    add_subdirectory(
        "${CMAKE_SOURCE_DIR}/tests/qt"
        "${CMAKE_BINARY_DIR}/tests/qt"
    )

    add_test(
        NAME editor.qt_runtime_deployment
        COMMAND
            "${CMAKE_COMMAND}"
            -DKALAM_EDITOR_EXECUTABLE=$<TARGET_FILE:nveditor>
            -DKALAM_QT_CORE_RUNTIME_NAME=$<TARGET_FILE_NAME:Qt6::Core>
            -DKALAM_QT_GUI_RUNTIME_NAME=$<TARGET_FILE_NAME:Qt6::Gui>
            -DKALAM_QT_WIDGETS_RUNTIME_NAME=$<TARGET_FILE_NAME:Qt6::Widgets>
            -DKALAM_PROJECT_NVIM_RUNTIME_SOURCE=${CMAKE_SOURCE_DIR}/editor/neovim/runtime
            -DKALAM_SYSTEM_ROOT=$ENV{SystemRoot}
            -P
            "${CMAKE_SOURCE_DIR}/tests/qt/qt_runtime_deployment_test.cmake"
    )
    set_tests_properties(
        editor.qt_runtime_deployment
        PROPERTIES
            LABELS "editor"
            TIMEOUT 15
    )

    find_program(KALAM_EDITOR_NEOVIM_EXECUTABLE NAMES nvim)
    if(KALAM_EDITOR_NEOVIM_EXECUTABLE)
        add_test(
            NAME editor.integrated_warp_smoke
            COMMAND
                $<TARGET_FILE:nveditor>
                --acceptance-smoke
                --source-root "${CMAKE_SOURCE_DIR}"
                --build-directory "${CMAKE_BINARY_DIR}"
                --module $<TARGET_FILE:game_module>
                --hot-directory "${CMAKE_BINARY_DIR}/hot-editor-smoke"
                --neovim "${KALAM_EDITOR_NEOVIM_EXECUTABLE}"
                --smoke-timeout 25000
                --smoke-screenshot
                "${CMAKE_BINARY_DIR}/context-interaction-smoke.png"
        )
        set_tests_properties(
            editor.integrated_warp_smoke
            PROPERTIES
                LABELS "editor;render"
                TIMEOUT 35
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        message(STATUS
            "Neovim was not found; integrated editor smoke is skipped.")
    endif()
endif()
