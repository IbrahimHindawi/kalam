add_executable(
    qt_build_fake_worker
    fake_build_worker.cpp
)
target_link_libraries(qt_build_fake_worker PRIVATE Qt6::Core)
set_target_properties(
    qt_build_fake_worker
    PROPERTIES
        FOLDER "tests/editor"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
kalam_apply_qt_policy(qt_build_fake_worker)

add_executable(
    editor_qt_controller_tests
    editor_qt_controller_tests.cpp
)
target_compile_definitions(
    editor_qt_controller_tests
    PRIVATE
        KALAM_QT_FAKE_BUILD_WORKER="$<TARGET_FILE:qt_build_fake_worker>"
)
target_link_libraries(
    editor_qt_controller_tests
    PRIVATE
        Kalam::EditorQt
        Qt6::Core
)
add_dependencies(editor_qt_controller_tests qt_build_fake_worker)
set_target_properties(
    editor_qt_controller_tests
    PROPERTIES
        AUTOMOC ON
        FOLDER "tests/editor"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
kalam_apply_qt_policy(editor_qt_controller_tests)
add_test(NAME editor.qt_controllers COMMAND editor_qt_controller_tests)
set_tests_properties(
    editor.qt_controllers
    PROPERTIES
        LABELS "editor"
        TIMEOUT 20
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

add_executable(
    editor_neovim_adapter_tests
    editor_neovim_adapter_tests.cpp
)
target_link_libraries(
    editor_neovim_adapter_tests
    PRIVATE
        Kalam::EditorQt
        Kalam::NeovimQt
        Qt6::Core
        Qt6::Widgets
)
set_target_properties(
    editor_neovim_adapter_tests
    PROPERTIES
        AUTOMOC ON
        FOLDER "tests/editor"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
kalam_apply_qt_policy(editor_neovim_adapter_tests)
add_test(
    NAME editor.neovim_adapter
    COMMAND editor_neovim_adapter_tests
)
set_tests_properties(
    editor.neovim_adapter
    PROPERTIES
        LABELS "editor"
        TIMEOUT 10
        ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

add_executable(
    editor_neovim_theme_bridge_tests
    neovim_theme_bridge_tests.cpp
)
target_link_libraries(
    editor_neovim_theme_bridge_tests
    PRIVATE
        Kalam::EditorQt
        Kalam::NeovimQt
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)
set_target_properties(
    editor_neovim_theme_bridge_tests
    PROPERTIES
        AUTOMOC ON
        FOLDER "tests/editor"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
kalam_apply_qt_policy(editor_neovim_theme_bridge_tests)
add_test(
    NAME editor.neovim_theme_bridge
    COMMAND editor_neovim_theme_bridge_tests
)
set_tests_properties(
    editor.neovim_theme_bridge
    PROPERTIES
        LABELS "editor"
        TIMEOUT 10
        ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

add_executable(
    editor_qt_author_transaction_tests
    author_transaction_regression_tests.cpp
    "${CMAKE_SOURCE_DIR}/editor/qt/author_transaction_controller.h"
)
target_include_directories(
    editor_qt_author_transaction_tests
    PRIVATE
        "${CMAKE_SOURCE_DIR}/editor/qt"
)
target_link_libraries(
    editor_qt_author_transaction_tests
    PRIVATE
        module_bridge
        Qt6::Core
)
set_target_properties(
    editor_qt_author_transaction_tests
    PROPERTIES
        AUTOMOC ON
        FOLDER "tests/editor"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
kalam_apply_qt_policy(editor_qt_author_transaction_tests)
add_test(
    NAME editor.qt_author_transactions
    COMMAND editor_qt_author_transaction_tests
)
set_tests_properties(
    editor.qt_author_transactions
    PROPERTIES
        LABELS "editor"
        TIMEOUT 10
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

add_test(
    NAME editor.qt_source_firewall
    COMMAND
        "${CMAKE_COMMAND}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/editor_qt_source_firewall.cmake"
)
set_tests_properties(
    editor.qt_source_firewall
    PROPERTIES
        LABELS "editor;firewall"
        TIMEOUT 15
)
