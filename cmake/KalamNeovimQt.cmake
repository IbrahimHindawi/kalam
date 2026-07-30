include_guard(GLOBAL)

include(CMakeParseArguments)
include(FetchContent)

set(
    KALAM_NEOVIM_QT_PINNED_COMMIT
    "6c506094f5d0e9663a3a907622d5aef8e19de85f"
)
set(KALAM_NEOVIM_QT_PINNED_VERSION "0.2.19")
set(KALAM_QT_PINNED_VERSION "6.8.3")
set(
    KALAM_MSGPACK_PINNED_COMMIT
    "29b9505cb76710802e130d5b8403ec1b40de1c8d"
)
set(KALAM_MSGPACK_PINNED_VERSION "3.2.0")
set(
    KALAM_MSGPACK_PINNED_SHA256
    "ff865a36bad5c72b8e7ebc4b7cf5f27a820fce4faff9c571c1791e3728355a39"
)

set(
    KALAM_NEOVIM_QT_SOURCE_DIR
    ""
    CACHE PATH
    "Existing clean equalsraf/neovim-qt checkout at Kalam's pinned commit"
)
set(
    KALAM_MSGPACK_SOURCE_DIR
    ""
    CACHE PATH
    "Existing msgpack-c cpp-3.2.0 checkout or extracted source directory"
)

set(
    _KALAM_NEOVIM_QT_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/../editor/neovim/upstream_patch/process_only.patch"
)
set(
    _KALAM_NEOVIM_QT_PROVENANCE_TEMPLATE
    "${CMAKE_CURRENT_LIST_DIR}/../editor/neovim/upstream_patch/provenance.h.in"
)

function(_kalam_dependency_git_revision source_dir expected_revision label)
    find_package(Git 2.20 REQUIRED)

    if(NOT EXISTS "${source_dir}/.git")
        message(
            FATAL_ERROR
            "${label} source override must be a git checkout so its exact "
            "revision can be proven. Got: ${source_dir}"
        )
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" rev-parse HEAD
        RESULT_VARIABLE revision_result
        OUTPUT_VARIABLE actual_revision
        ERROR_VARIABLE revision_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT revision_result EQUAL 0)
        message(
            FATAL_ERROR
            "Unable to inspect ${label} at ${source_dir}: ${revision_error}"
        )
    endif()
    if(NOT actual_revision STREQUAL expected_revision)
        message(
            FATAL_ERROR
            "${label} revision mismatch. Expected ${expected_revision}, got "
            "${actual_revision} at ${source_dir}."
        )
    endif()

    execute_process(
        COMMAND
            "${GIT_EXECUTABLE}"
            -C
            "${source_dir}"
            status
            --porcelain=v1
            --untracked-files=no
        RESULT_VARIABLE dirty_result
        OUTPUT_VARIABLE dirty_state
        ERROR_VARIABLE dirty_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT dirty_result EQUAL 0 OR dirty_state)
        message(
            FATAL_ERROR
            "${label} checkout has tracked modifications. Kalam applies its "
            "small fork patch to a generated copy; keep the supplied checkout "
            "clean. ${dirty_error}"
        )
    endif()
endfunction()

function(_kalam_get_neovim_qt_source output_var)
    if(KALAM_NEOVIM_QT_SOURCE_DIR)
        get_filename_component(
            source_dir
            "${KALAM_NEOVIM_QT_SOURCE_DIR}"
            ABSOLUTE
            BASE_DIR "${CMAKE_SOURCE_DIR}"
        )
    else()
        FetchContent_Declare(
            kalam_neovim_qt_upstream
            GIT_REPOSITORY https://github.com/equalsraf/neovim-qt.git
            GIT_TAG "${KALAM_NEOVIM_QT_PINNED_COMMIT}"
            GIT_SHALLOW FALSE
            GIT_SUBMODULES ""
            GIT_PROGRESS TRUE
            SOURCE_SUBDIR _kalam_do_not_add_upstream_project
        )
        FetchContent_MakeAvailable(kalam_neovim_qt_upstream)
        FetchContent_GetProperties(
            kalam_neovim_qt_upstream
            SOURCE_DIR source_dir
        )
    endif()

    if(NOT EXISTS "${source_dir}/src/gui/shell.cpp")
        message(
            FATAL_ERROR
            "Neovim-Qt source directory is incomplete: ${source_dir}"
        )
    endif()
    _kalam_dependency_git_revision(
        "${source_dir}"
        "${KALAM_NEOVIM_QT_PINNED_COMMIT}"
        "Neovim-Qt ${KALAM_NEOVIM_QT_PINNED_VERSION}"
    )
    set(${output_var} "${source_dir}" PARENT_SCOPE)
endfunction()

function(_kalam_verify_msgpack_archive_source source_dir)
    set(
        sentinel_files
        include/msgpack/version_master.h
        include/msgpack.h
        src/objectc.c
        src/unpack.c
        src/version.c
        src/vrefbuffer.c
        src/zone.c
    )
    set(
        sentinel_hashes
        a4dc80dd68171b1375262f5cf9880a6b30c283d7bec17f1c87ede08e3b72f2cf
        58316d8acd57cb66bd9919e9eca1e360e2a4e80b55fe9d5f22b624822e4a4bef
        f8fa061be35b46e2e8a302f90cc4523792115dd07d5ef20be522ea9ad035e324
        269696be8c74e174dad966a66add2d7dca8e0e32dff3edf872426b3322c90332
        8b7dc68893b0b677b1574d25ce1629ca38a5fd053817447a78f513875caaed2e
        7a5b851bb07d6e3922d401c73a6473e490f170d339c0af1f42743b27075b8ae5
        7be9746c0bba8233faee8050bc8136238c9368483cf4549fe1b1cc608e40b227
    )
    list(LENGTH sentinel_files sentinel_count)
    math(EXPR sentinel_last "${sentinel_count} - 1")
    foreach(index RANGE 0 ${sentinel_last})
        list(GET sentinel_files ${index} relative_path)
        list(GET sentinel_hashes ${index} expected_hash)
        set(absolute_path "${source_dir}/${relative_path}")
        if(NOT EXISTS "${absolute_path}")
            message(
                FATAL_ERROR
                "msgpack-c source is missing ${relative_path}: ${source_dir}"
            )
        endif()
        file(SHA256 "${absolute_path}" actual_hash)
        if(NOT actual_hash STREQUAL expected_hash)
            message(
                FATAL_ERROR
                "msgpack-c cpp-3.2.0 source hash mismatch for "
                "${relative_path}. Expected ${expected_hash}, got "
                "${actual_hash}."
            )
        endif()
    endforeach()
endfunction()

function(_kalam_get_msgpack_source output_var)
    if(KALAM_MSGPACK_SOURCE_DIR)
        get_filename_component(
            source_dir
            "${KALAM_MSGPACK_SOURCE_DIR}"
            ABSOLUTE
            BASE_DIR "${CMAKE_SOURCE_DIR}"
        )
        if(EXISTS "${source_dir}/.git")
            _kalam_dependency_git_revision(
                "${source_dir}"
                "${KALAM_MSGPACK_PINNED_COMMIT}"
                "msgpack-c cpp-${KALAM_MSGPACK_PINNED_VERSION}"
            )
        else()
            _kalam_verify_msgpack_archive_source("${source_dir}")
        endif()
    else()
        FetchContent_Declare(
            kalam_msgpack_upstream
            URL
                "https://github.com/msgpack/msgpack-c/archive/cpp-${KALAM_MSGPACK_PINNED_VERSION}.tar.gz"
            URL_HASH "SHA256=${KALAM_MSGPACK_PINNED_SHA256}"
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
            SOURCE_SUBDIR _kalam_do_not_add_upstream_project
        )
        FetchContent_MakeAvailable(kalam_msgpack_upstream)
        FetchContent_GetProperties(
            kalam_msgpack_upstream
            SOURCE_DIR source_dir
        )
        _kalam_verify_msgpack_archive_source("${source_dir}")
    endif()

    set(${output_var} "${source_dir}" PARENT_SCOPE)
endfunction()

function(_kalam_assert_process_only_sources source_root source_list)
    set(
        forbidden_basenames
        app.cpp
        contextmenu.cpp
        errorwidget.cpp
        main.cpp
        mainwindow.cpp
        scrollbar.cpp
        tabline.cpp
        treeview.cpp
    )
    set(
        forbidden_tokens
        QAbstractSocket
        QLocalSocket
        QSocketNotifier
        QTcpSocket
        QtNetwork
        QtSvg
        QSvgWidget
        QSvgRenderer
    )

    foreach(relative_path IN LISTS source_list)
        get_filename_component(basename "${relative_path}" NAME)
        if(basename IN_LIST forbidden_basenames)
            message(
                FATAL_ERROR
                "Forbidden upstream window-chrome source selected: "
                "${relative_path}"
            )
        endif()
        file(READ "${source_root}/${relative_path}" source_text)
        foreach(token IN LISTS forbidden_tokens)
            string(FIND "${source_text}" "${token}" token_offset)
            if(NOT token_offset EQUAL -1)
                message(
                    FATAL_ERROR
                    "Selected Neovim-Qt source ${relative_path} still refers "
                    "to forbidden Qt Network/SVG token ${token}."
                )
            endif()
        endforeach()
    endforeach()
endfunction()

function(kalam_prepare_pinned_neovim_qt output_var)
    set(options)
    set(one_value_args SOURCE_DIR OUTPUT_DIR)
    cmake_parse_arguments(
        KALAM_PREPARE
        "${options}"
        "${one_value_args}"
        ""
        ${ARGN}
    )

    if(KALAM_PREPARE_SOURCE_DIR)
        get_filename_component(
            source_dir
            "${KALAM_PREPARE_SOURCE_DIR}"
            ABSOLUTE
            BASE_DIR "${CMAKE_SOURCE_DIR}"
        )
        _kalam_dependency_git_revision(
            "${source_dir}"
            "${KALAM_NEOVIM_QT_PINNED_COMMIT}"
            "Neovim-Qt ${KALAM_NEOVIM_QT_PINNED_VERSION}"
        )
    else()
        _kalam_get_neovim_qt_source(source_dir)
    endif()

    if(KALAM_PREPARE_OUTPUT_DIR)
        get_filename_component(
            patched_dir
            "${KALAM_PREPARE_OUTPUT_DIR}"
            ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}"
        )
    else()
        set(
            patched_dir
            "${CMAKE_CURRENT_BINARY_DIR}/_deps/kalam-neovim-qt-patched"
        )
    endif()

    get_filename_component(binary_root "${CMAKE_BINARY_DIR}" ABSOLUTE)
    cmake_path(IS_PREFIX binary_root "${patched_dir}" NORMALIZE is_in_build_tree)
    if(NOT is_in_build_tree)
        message(
            FATAL_ERROR
            "Patched Neovim-Qt output must stay inside the build tree. Got: "
            "${patched_dir}"
        )
    endif()
    if(patched_dir STREQUAL source_dir)
        message(FATAL_ERROR "Refusing to patch Neovim-Qt in place.")
    endif()
    if(NOT EXISTS "${_KALAM_NEOVIM_QT_PATCH}")
        message(
            FATAL_ERROR
            "Kalam Neovim-Qt patch is missing: ${_KALAM_NEOVIM_QT_PATCH}"
        )
    endif()

    find_package(Git 2.20 REQUIRED)
    file(REMOVE_RECURSE "${patched_dir}")
    file(MAKE_DIRECTORY "${patched_dir}")
    file(COPY "${source_dir}/src" DESTINATION "${patched_dir}")
    file(COPY "${source_dir}/LICENSE" DESTINATION "${patched_dir}")
    get_filename_component(patched_parent "${patched_dir}" DIRECTORY)

    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            -E
            env
            "GIT_CEILING_DIRECTORIES=${patched_parent}"
            "${GIT_EXECUTABLE}"
            apply
            --no-index
            --check
            --whitespace=nowarn
            "${_KALAM_NEOVIM_QT_PATCH}"
        WORKING_DIRECTORY "${patched_dir}"
        RESULT_VARIABLE patch_check_result
        ERROR_VARIABLE patch_check_error
    )
    if(NOT patch_check_result EQUAL 0)
        message(
            FATAL_ERROR
            "Kalam's minimal Neovim-Qt patch does not apply cleanly to "
            "${KALAM_NEOVIM_QT_PINNED_COMMIT}: ${patch_check_error}"
        )
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            -E
            env
            "GIT_CEILING_DIRECTORIES=${patched_parent}"
            "${GIT_EXECUTABLE}"
            apply
            --no-index
            --whitespace=nowarn
            "${_KALAM_NEOVIM_QT_PATCH}"
        WORKING_DIRECTORY "${patched_dir}"
        RESULT_VARIABLE patch_result
        ERROR_VARIABLE patch_error
    )
    if(NOT patch_result EQUAL 0)
        message(FATAL_ERROR "Failed to apply Neovim-Qt patch: ${patch_error}")
    endif()

    set(
        selected_sources
        auto/neovimapi0.cpp
        auto/neovimapi1.cpp
        auto/neovimapi2.cpp
        auto/neovimapi3.cpp
        auto/neovimapi4.cpp
        auto/neovimapi5.cpp
        auto/neovimapi6.cpp
        compat/compat_qt6.cpp
        function.cpp
        msgpackiodevice.cpp
        msgpackrequest.cpp
        neovimconnector.cpp
        neovimconnectorhelper.cpp
        util.cpp
        gui/compat_gui_qt6.cpp
        gui/input.cpp
        gui/input_win32.cpp
        gui/popupmenu.cpp
        gui/popupmenumodel.cpp
        gui/shell.cpp
        gui/shellwidget/cell.cpp
        gui/shellwidget/compat_shellwidget_qt6.cpp
        gui/shellwidget/cursor.cpp
        gui/shellwidget/helpers.cpp
        gui/shellwidget/highlight.cpp
        gui/shellwidget/konsole_wcwidth.cpp
        gui/shellwidget/shellcontents.cpp
        gui/shellwidget/shellwidget.cpp
    )
    _kalam_assert_process_only_sources(
        "${patched_dir}/src"
        "${selected_sources}"
    )

    file(SHA256 "${_KALAM_NEOVIM_QT_PATCH}" patch_sha256)
    file(
        WRITE
        "${patched_dir}/KALAM_PROVENANCE.txt"
        "upstream=https://github.com/equalsraf/neovim-qt.git\n"
        "version=${KALAM_NEOVIM_QT_PINNED_VERSION}\n"
        "commit=${KALAM_NEOVIM_QT_PINNED_COMMIT}\n"
        "patch_sha256=${patch_sha256}\n"
        "profile=process-rpc-shell-shellwidget;qt=Core,Gui,Widgets\n"
    )

    set(${output_var} "${patched_dir}" PARENT_SCOPE)
endfunction()

function(_kalam_add_msgpack_target source_dir)
    if(TARGET kalam_neovim_qt_msgpack)
        return()
    endif()

    add_library(
        kalam_neovim_qt_msgpack
        STATIC
            "${source_dir}/src/objectc.c"
            "${source_dir}/src/unpack.c"
            "${source_dir}/src/version.c"
            "${source_dir}/src/vrefbuffer.c"
            "${source_dir}/src/zone.c"
    )
    target_include_directories(
        kalam_neovim_qt_msgpack
        PUBLIC
            "${source_dir}/include"
        PRIVATE
            "${source_dir}"
    )
    target_compile_features(kalam_neovim_qt_msgpack PRIVATE c_std_99)
    set_target_properties(
        kalam_neovim_qt_msgpack
        PROPERTIES
            FOLDER "third_party/neovim_qt"
            POSITION_INDEPENDENT_CODE ON
    )
endfunction()

function(kalam_add_neovim_qt)
    set(options)
    set(one_value_args TARGET PROJECT_RUNTIME_DIR)
    cmake_parse_arguments(
        KALAM_NVIM
        "${options}"
        "${one_value_args}"
        ""
        ${ARGN}
    )
    if(NOT KALAM_NVIM_TARGET)
        message(FATAL_ERROR "kalam_add_neovim_qt requires TARGET <name>.")
    endif()
    if(TARGET "${KALAM_NVIM_TARGET}")
        message(
            FATAL_ERROR
            "Neovim-Qt target already exists: ${KALAM_NVIM_TARGET}"
        )
    endif()
    if(NOT WIN32)
        message(
            FATAL_ERROR
            "Kalam's pinned Neovim-Qt profile currently selects Win32 input."
        )
    endif()

    # Keep a missing Qt installation from triggering network downloads first.
    find_package(
        Qt6
        ${KALAM_QT_PINNED_VERSION}
        EXACT
        REQUIRED
        COMPONENTS Core Gui Widgets
    )

    kalam_prepare_pinned_neovim_qt(patched_dir)
    _kalam_get_msgpack_source(msgpack_source_dir)
    _kalam_add_msgpack_target("${msgpack_source_dir}")

    set(source_root "${patched_dir}/src")
    set(
        source_files
        auto/neovimapi0.cpp
        auto/neovimapi1.cpp
        auto/neovimapi2.cpp
        auto/neovimapi3.cpp
        auto/neovimapi4.cpp
        auto/neovimapi5.cpp
        auto/neovimapi6.cpp
        compat/compat_qt6.cpp
        function.cpp
        msgpackiodevice.cpp
        msgpackrequest.cpp
        neovimconnector.cpp
        neovimconnectorhelper.cpp
        util.cpp
        gui/compat_gui_qt6.cpp
        gui/input.cpp
        gui/input_win32.cpp
        gui/popupmenu.cpp
        gui/popupmenumodel.cpp
        gui/shell.cpp
        gui/shellwidget/cell.cpp
        gui/shellwidget/compat_shellwidget_qt6.cpp
        gui/shellwidget/cursor.cpp
        gui/shellwidget/helpers.cpp
        gui/shellwidget/highlight.cpp
        gui/shellwidget/konsole_wcwidth.cpp
        gui/shellwidget/shellcontents.cpp
        gui/shellwidget/shellwidget.cpp
    )
    set(
        moc_headers
        auto/neovimapi0.h
        auto/neovimapi1.h
        auto/neovimapi2.h
        auto/neovimapi3.h
        auto/neovimapi4.h
        auto/neovimapi5.h
        auto/neovimapi6.h
        msgpackiodevice.h
        msgpackrequest.h
        neovimconnector.h
        neovimconnectorhelper.h
        gui/shell.h
        gui/shellwidget/cursor.h
        gui/shellwidget/shellwidget.h
    )
    _kalam_assert_process_only_sources("${source_root}" "${source_files}")
    list(TRANSFORM source_files PREPEND "${source_root}/")
    list(TRANSFORM moc_headers PREPEND "${source_root}/")

    set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/neovim_qt")
    file(MAKE_DIRECTORY "${generated_dir}")
    set(PROJECT "neovim-qt")
    set(PROJECT_NAME "neovim-qt")
    set(PROJECT_VERSION "0.2.19.0")
    set(PROJECT_VERSION_MAJOR 0)
    set(PROJECT_VERSION_MINOR 2)
    set(PROJECT_VERSION_PATCH 19)
    set(PROJECT_VERSION_TWEAK 0)
    set(REPRODUCIBLE_BUILD ON)
    configure_file(
        "${source_root}/version.h.in"
        "${generated_dir}/version.h"
        @ONLY
    )
    configure_file(
        "${_KALAM_NEOVIM_QT_PROVENANCE_TEMPLATE}"
        "${generated_dir}/kalam_neovim_qt_provenance.h"
        @ONLY
    )

    add_library(
        "${KALAM_NVIM_TARGET}"
        STATIC
            ${source_files}
            ${moc_headers}
    )
    target_compile_features("${KALAM_NVIM_TARGET}" PUBLIC cxx_std_17)
    target_compile_definitions(
        "${KALAM_NVIM_TARGET}"
        PRIVATE
            KALAM_NEOVIM_QT_PROCESS_ONLY=1
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            _CRT_NONSTDC_NO_DEPRECATE
    )
    target_include_directories(
        "${KALAM_NVIM_TARGET}"
        PUBLIC
            "${generated_dir}"
            "${source_root}"
            "${source_root}/auto"
            "${source_root}/compat"
            "${source_root}/gui"
            "${source_root}/gui/shellwidget"
    )
    target_link_libraries(
        "${KALAM_NVIM_TARGET}"
        PUBLIC
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
            kalam_neovim_qt_msgpack
    )
    set_target_properties(
        "${KALAM_NVIM_TARGET}"
        PROPERTIES
            AUTOMOC ON
            AUTORCC OFF
            AUTOUIC OFF
            FOLDER "third_party/neovim_qt"
            POSITION_INDEPENDENT_CODE ON
            KALAM_NEOVIM_QT_PINNED_COMMIT
                "${KALAM_NEOVIM_QT_PINNED_COMMIT}"
            KALAM_NEOVIM_QT_PATCHED_SOURCE_DIR "${patched_dir}"
            KALAM_NEOVIM_QT_UPSTREAM_RUNTIME_DIR
                "${source_root}/gui/runtime"
            KALAM_NEOVIM_QT_PROJECT_RUNTIME_DIR
                "${KALAM_NVIM_PROJECT_RUNTIME_DIR}"
    )
endfunction()

function(kalam_stage_neovim_runtime application_target)
    if(NOT TARGET "${application_target}")
        message(
            FATAL_ERROR
            "Cannot stage Neovim runtime for missing target: "
            "${application_target}"
        )
    endif()
    if(NOT TARGET kalam_neovim_qt)
        message(
            FATAL_ERROR
            "kalam_stage_neovim_runtime expects target kalam_neovim_qt."
        )
    endif()

    get_target_property(
        upstream_runtime
        kalam_neovim_qt
        KALAM_NEOVIM_QT_UPSTREAM_RUNTIME_DIR
    )
    get_target_property(
        project_runtime
        kalam_neovim_qt
        KALAM_NEOVIM_QT_PROJECT_RUNTIME_DIR
    )
    set(runtime_output "$<TARGET_FILE_DIR:${application_target}>/nvim-runtime")
    add_custom_command(
        TARGET "${application_target}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${runtime_output}"
        COMMAND
            "${CMAKE_COMMAND}"
            -E
            copy_directory
            "${upstream_runtime}"
            "${runtime_output}"
        COMMAND
            "${CMAKE_COMMAND}"
            -E
            copy_directory
            "${project_runtime}"
            "${runtime_output}"
        COMMENT "Staging pinned Neovim-Qt and Kalam editor runtime"
        VERBATIM
    )
endfunction()
