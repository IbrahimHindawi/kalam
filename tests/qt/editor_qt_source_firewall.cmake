cmake_minimum_required(VERSION 3.25)

cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH _tests_directory)
cmake_path(GET _tests_directory PARENT_PATH _repository_root)
set(_editor_source_directory "${_repository_root}/editor/qt")

if(NOT IS_DIRECTORY "${_editor_source_directory}")
    message(FATAL_ERROR
        "Qt editor source-policy firewall could not find: "
        "${_editor_source_directory}")
endif()

file(GLOB_RECURSE _editor_sources
    LIST_DIRECTORIES FALSE
    "${_editor_source_directory}/*.cpp"
    "${_editor_source_directory}/*.h")
list(SORT _editor_sources)

if(NOT _editor_sources)
    message(FATAL_ERROR
        "Qt editor source-policy firewall found no first-party sources in: "
        "${_editor_source_directory}")
endif()

set(_violations)

macro(kalam_check_editor_policy _description _pattern)
    if("${_line}" MATCHES "${_pattern}")
        string(STRIP "${_line}" _excerpt)
        if(_excerpt STREQUAL "")
            set(_excerpt "<empty line>")
        endif()
        list(APPEND _violations
            "${_relative_path}:${_line_number}: ${_description}: ${_excerpt}")
    endif()
endmacro()

foreach(_source IN LISTS _editor_sources)
    cmake_path(
        RELATIVE_PATH _source
        BASE_DIRECTORY "${_repository_root}"
        OUTPUT_VARIABLE _relative_path)
    file(STRINGS "${_source}" _lines ENCODING UTF-8)
    set(_line_number 0)

    foreach(_line IN LISTS _lines)
        math(EXPR _line_number "${_line_number} + 1")

        kalam_check_editor_policy(
            "C++ standard-library namespace is forbidden"
            "std::")
        kalam_check_editor_policy(
            "C++ standard-library header is forbidden"
            "#[ \t]*include[ \t]*[<\"][ \t]*(algorithm|array|atomic|bitset|chrono|deque|exception|filesystem|forward_list|fstream|functional|future|initializer_list|iomanip|ios|iosfwd|iostream|istream|iterator|limits|list|map|memory|mutex|new|optional|ostream|queue|random|ranges|regex|set|span|sstream|stack|stdexcept|streambuf|string|string_view|system_error|thread|tuple|type_traits|unordered_map|unordered_set|utility|variant|vector)[ \t]*[>\"]")
        kalam_check_editor_policy(
            "C++ standard-library type is forbidden"
            "(^|[^A-Za-z0-9_])(array|basic_string|deque|forward_list|function|list|map|multimap|multiset|optional|priority_queue|queue|set|shared_ptr|span|stack|tuple|unique_ptr|unordered_map|unordered_set|variant|vector|weak_ptr)[ \t]*<")
        kalam_check_editor_policy(
            "C++ standard-library namespace directive is forbidden"
            "(^|[^A-Za-z0-9_])namespace[ \t]+std([^A-Za-z0-9_]|$)")
        kalam_check_editor_policy(
            "C++ exception syntax is forbidden"
            "(^|[^A-Za-z0-9_])(throw|try|catch)([^A-Za-z0-9_]|$)")
        kalam_check_editor_policy(
            "forbidden Qt module is referenced"
            "(Qt|Qt5|Qt6)(Network|Svg|Test|Qml|Quick|::Network|::Svg|::Test|::Qml|::Quick)")
        kalam_check_editor_policy(
            "forbidden Qt API is referenced"
            "(^|[^A-Za-z0-9_])(QAbstractItemModelTester|QAbstractSocket|QJSEngine|QJSValue|QLocalSocket|QNetwork[A-Za-z0-9_]*|QQml[A-Za-z0-9_]*|QQuick[A-Za-z0-9_]*|QSignalSpy|QSsl[A-Za-z0-9_]*|QSvg[A-Za-z0-9_]*|QTcpSocket|QTest[A-Za-z0-9_]*|QUdpSocket)([^A-Za-z0-9_]|$)")
        kalam_check_editor_policy(
            "permanent dock UI is forbidden"
            "(^|[^A-Za-z0-9_])QDockWidget([^A-Za-z0-9_]|$)")
    endforeach()
endforeach()

if(_violations)
    list(JOIN _violations "\n  " _formatted_violations)
    message(FATAL_ERROR
        "Qt editor source-policy firewall failed:\n"
        "  ${_formatted_violations}")
endif()

list(LENGTH _editor_sources _source_count)
message(STATUS
    "Qt editor source-policy firewall passed (${_source_count} files checked)")
