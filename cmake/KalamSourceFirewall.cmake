cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED KALAM_SOURCE_ROOT OR KALAM_SOURCE_ROOT STREQUAL "")
    message(
        FATAL_ERROR
        "KALAM_SOURCE_ROOT must name the Kalam repository root."
    )
endif()

cmake_path(
    ABSOLUTE_PATH KALAM_SOURCE_ROOT
    NORMALIZE
    OUTPUT_VARIABLE _kalam_source_root
)

if(NOT IS_DIRECTORY "${_kalam_source_root}")
    message(
        FATAL_ERROR
        "KALAM_SOURCE_ROOT is not a directory: ${_kalam_source_root}"
    )
endif()

set(
    _kalam_source_extensions
    .c
    .cc
    .cpp
    .cppm
    .cxx
    .h
    .hh
    .hpp
    .hxx
    .inc
    .inl
    .ipp
    .ixx
    .tpp
)
set(_kalam_header_extensions .h .hh .hpp .hxx .inc .inl .ipp .tpp)

# Enumerate only first-party roots. In particular, this intentionally avoids
# build trees and the Neovim-Qt vendor/fork.
set(
    _kalam_scan_roots
    bridge
    engine
    game
    standalone
    reflection
    editor/host
    editor/qt
    editor/win32
)

set(_kalam_source_files)
foreach(_kalam_scan_root IN LISTS _kalam_scan_roots)
    set(_kalam_absolute_scan_root "${_kalam_source_root}/${_kalam_scan_root}")
    if(NOT IS_DIRECTORY "${_kalam_absolute_scan_root}")
        continue()
    endif()

    file(
        GLOB_RECURSE _kalam_root_files
        LIST_DIRECTORIES FALSE
        "${_kalam_absolute_scan_root}/*"
    )
    foreach(_kalam_file IN LISTS _kalam_root_files)
        get_filename_component(_kalam_extension "${_kalam_file}" EXT)
        string(TOLOWER "${_kalam_extension}" _kalam_extension)
        if(_kalam_extension IN_LIST _kalam_source_extensions)
            list(APPEND _kalam_source_files "${_kalam_file}")
        endif()
    endforeach()
endforeach()

list(REMOVE_DUPLICATES _kalam_source_files)
list(SORT _kalam_source_files)

set(_kalam_violations)

foreach(_kalam_file IN LISTS _kalam_source_files)
    file(RELATIVE_PATH _kalam_relative_file "${_kalam_source_root}" "${_kalam_file}")
    cmake_path(NORMAL_PATH _kalam_relative_file)
    string(REPLACE "\\" "/" _kalam_relative_file "${_kalam_relative_file}")
    string(TOLOWER "${_kalam_relative_file}" _kalam_relative_file_lower)

    get_filename_component(_kalam_extension "${_kalam_file}" EXT)
    string(TOLOWER "${_kalam_extension}" _kalam_extension)
    set(_kalam_is_header FALSE)
    if(_kalam_extension IN_LIST _kalam_header_extensions)
        set(_kalam_is_header TRUE)
    endif()

    set(_kalam_qt_forbidden FALSE)
    if(
        _kalam_relative_file_lower MATCHES
        "^(bridge|engine|game|reflection)/"
        OR _kalam_relative_file_lower MATCHES "^editor/(host|win32)/"
        OR _kalam_relative_file_lower MATCHES "^standalone/"
    )
        set(_kalam_qt_forbidden TRUE)
    endif()

    set(_kalam_is_editor_qt FALSE)
    if(_kalam_relative_file_lower MATCHES "^editor/qt/")
        set(_kalam_is_editor_qt TRUE)
    endif()

    # D3D/DXGI is an implementation detail of the concrete renderer and stable
    # hosts. It is forbidden in the ABI bridge, simulation/runtime layers, and
    # explicitly public engine/game headers.
    set(_kalam_d3d_forbidden FALSE)
    if(_kalam_relative_file_lower MATCHES "^bridge/")
        set(_kalam_d3d_forbidden TRUE)
    elseif(_kalam_is_editor_qt)
        set(_kalam_d3d_forbidden TRUE)
    elseif(
        _kalam_relative_file_lower MATCHES
        "^(engine|game)/([^/]+/)*(simulation|sim|logic)(/|$)"
    )
        set(_kalam_d3d_forbidden TRUE)
    elseif(_kalam_relative_file_lower MATCHES "^engine/runtime/")
        set(_kalam_d3d_forbidden TRUE)
    elseif(
        _kalam_is_header
        AND _kalam_relative_file_lower MATCHES
            "^(engine|game)/([^/]+/)*public/"
    )
        set(_kalam_d3d_forbidden TRUE)
    endif()

    file(READ "${_kalam_file}" _kalam_contents)
    string(REPLACE "\r\n" "\n" _kalam_contents "${_kalam_contents}")
    string(REPLACE "\r" "\n" _kalam_contents "${_kalam_contents}")

    # CMake lists use semicolons as separators. Includes cannot meaningfully
    # contain this sentinel, so protect source semicolons before splitting into
    # lines and restore them for diagnostics afterward.
    string(
        REPLACE ";"
        "__KALAM_SOURCE_FIREWALL_SEMICOLON__"
        _kalam_contents
        "${_kalam_contents}"
    )
    string(REPLACE "\n" ";" _kalam_lines "${_kalam_contents}")

    set(_kalam_line_number 0)
    foreach(_kalam_line IN LISTS _kalam_lines)
        math(EXPR _kalam_line_number "${_kalam_line_number} + 1")
        string(
            REPLACE "__KALAM_SOURCE_FIREWALL_SEMICOLON__"
            ";"
            _kalam_line
            "${_kalam_line}"
        )

        set(_kalam_code_line "${_kalam_line}")
        if(_kalam_code_line MATCHES "^[ \t]*//")
            set(_kalam_code_line "")
        else()
            string(REGEX REPLACE "//.*$" "" _kalam_code_line "${_kalam_code_line}")
        endif()

        if(_kalam_code_line MATCHES "(^|[^A-Za-z0-9_])std::")
            string(
                CONCAT
                _kalam_diagnostic
                "${_kalam_relative_file}:${_kalam_line_number}: "
                "STL namespace use is forbidden in first-party code"
            )
            list(APPEND _kalam_violations "${_kalam_diagnostic}")
        endif()

        if(
            _kalam_code_line MATCHES
                "(^|[^A-Za-z0-9_])(throw|try|catch|dynamic_cast|typeid)([ \t({]|$)"
        )
            string(
                CONCAT
                _kalam_diagnostic
                "${_kalam_relative_file}:${_kalam_line_number}: "
                "exceptions or RTTI are forbidden in first-party code"
            )
            list(APPEND _kalam_violations "${_kalam_diagnostic}")
        endif()

        if(
            _kalam_is_editor_qt
            AND _kalam_code_line MATCHES
                "Q_(PLUGIN_METADATA|DECLARE_INTERFACE)|QPluginLoader|QDockWidget"
        )
            string(
                CONCAT
                _kalam_diagnostic
                "${_kalam_relative_file}:${_kalam_line_number}: "
                "dock/plugin framework use is forbidden in the Qt adapter"
            )
            list(APPEND _kalam_violations "${_kalam_diagnostic}")
        endif()

        if(
            NOT _kalam_line MATCHES
                "^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"]+)[>\"]"
        )
            continue()
        endif()

        set(_kalam_header "${CMAKE_MATCH_1}")
        string(REPLACE "\\" "/" _kalam_header_normalized "${_kalam_header}")
        string(TOLOWER "${_kalam_header_normalized}" _kalam_header_lower)
        get_filename_component(
            _kalam_header_basename
            "${_kalam_header_normalized}"
            NAME
        )
        string(
            TOLOWER
            "${_kalam_header_basename}"
            _kalam_header_basename_lower
        )

        if(
            _kalam_header_lower MATCHES
                "^(algorithm|any|array|atomic|bit|chrono|concepts|coroutine|deque|exception|filesystem|format|forward_list|functional|future|initializer_list|iostream|istream|iterator|limits|list|map|memory|mutex|optional|ostream|queue|ranges|regex|set|span|sstream|stack|stdexcept|streambuf|string|string_view|system_error|thread|tuple|type_traits|typeinfo|unordered_map|unordered_set|utility|valarray|variant|vector)$"
        )
            string(
                CONCAT
                _kalam_diagnostic
                "${_kalam_relative_file}:${_kalam_line_number}: "
                "STL header is forbidden in first-party code: ${_kalam_header}"
            )
            list(APPEND _kalam_violations "${_kalam_diagnostic}")
        endif()

        set(_kalam_is_qt_header FALSE)
        if(_kalam_header_lower MATCHES "^qt[^/]*(/|$)")
            set(_kalam_is_qt_header TRUE)
        elseif(
            _kalam_header_basename MATCHES
                "^Q[A-Z][A-Za-z0-9_]*([.]h)?$"
        )
            set(_kalam_is_qt_header TRUE)
        elseif(
            _kalam_header_basename_lower MATCHES
                "^q[a-z0-9_]+[.]h$"
        )
            set(_kalam_is_qt_header TRUE)
        elseif(
            _kalam_header_basename_lower MATCHES
                "^(moc_|ui_|qrc_).*[.](h|hh|hpp|cpp|cxx)$"
        )
            set(_kalam_is_qt_header TRUE)
        endif()

        if(_kalam_qt_forbidden AND _kalam_is_qt_header)
            string(
                CONCAT
                _kalam_diagnostic
                "${_kalam_relative_file}:${_kalam_line_number}: "
                "Qt include is forbidden here: ${_kalam_header}"
            )
            list(
                APPEND _kalam_violations
                "${_kalam_diagnostic}"
            )
        endif()

        if(
            _kalam_is_editor_qt
            AND (
                _kalam_header_lower MATCHES
                    "^qt(network|svg|quick|qml|webengine|multimedia|designer|uitools|test)(/|$)"
                OR _kalam_header_basename MATCHES
                    "^(QNetwork|QTcp|QUdp|QLocalSocket|QSvg|QQuick|QQml|QWebEngine|QMedia|QAudio|QDesigner|QUiLoader|QPluginLoader|QDockWidget)"
                OR _kalam_header_basename_lower MATCHES "^ui_.*[.]h$"
            )
        )
            string(
                CONCAT
                _kalam_diagnostic
                "${_kalam_relative_file}:${_kalam_line_number}: "
                "Qt module or UI framework is forbidden here: ${_kalam_header}"
            )
            list(APPEND _kalam_violations "${_kalam_diagnostic}")
        endif()

        set(_kalam_is_d3d_header FALSE)
        if(
            _kalam_header_basename_lower MATCHES
                "^((d3d(9|10|11|12)(_[0-9]+)?)|d3dcommon|d3dcompiler|d3dcsx|dxgi([0-9]+)?(_[0-9]+)?|dxgiformat|dxgitype)[.]h$"
        )
            set(_kalam_is_d3d_header TRUE)
        endif()

        if(_kalam_d3d_forbidden AND _kalam_is_d3d_header)
            string(
                CONCAT
                _kalam_diagnostic
                "${_kalam_relative_file}:${_kalam_line_number}: "
                "D3D/DXGI include is forbidden here: ${_kalam_header}"
            )
            list(
                APPEND _kalam_violations
                "${_kalam_diagnostic}"
            )
        endif()

        if(_kalam_relative_file_lower MATCHES "^engine/")
            if(
                _kalam_header_lower MATCHES "(^|/)game(/|$)"
                OR _kalam_header_lower MATCHES "(^|/)editor(/|$)"
            )
                string(
                    CONCAT
                    _kalam_diagnostic
                    "${_kalam_relative_file}:${_kalam_line_number}: "
                    "engine dependency points upward: ${_kalam_header}"
                )
                list(
                    APPEND _kalam_violations
                    "${_kalam_diagnostic}"
                )
            endif()
        endif()
    endforeach()
endforeach()

if(_kalam_violations)
    list(SORT _kalam_violations)
    list(JOIN _kalam_violations "\n  " _kalam_violation_report)
    message(
        FATAL_ERROR
        "Kalam source firewall rejected forbidden source boundaries:\n"
        "  ${_kalam_violation_report}"
    )
endif()

list(LENGTH _kalam_source_files _kalam_source_file_count)
message(
    STATUS
    "Kalam source firewall passed (${_kalam_source_file_count} first-party files)."
)
