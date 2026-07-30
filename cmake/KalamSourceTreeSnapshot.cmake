cmake_minimum_required(VERSION 3.28)

foreach(_kalam_required IN ITEMS KALAM_SOURCE_ROOT KALAM_SNAPSHOT_OUTPUT)
    if(NOT DEFINED ${_kalam_required} OR "${${_kalam_required}}" STREQUAL "")
        message(FATAL_ERROR "${_kalam_required} is required.")
    endif()
endforeach()

cmake_path(
    ABSOLUTE_PATH KALAM_SOURCE_ROOT
    NORMALIZE
    OUTPUT_VARIABLE _kalam_source_root
)
cmake_path(
    ABSOLUTE_PATH KALAM_SNAPSHOT_OUTPUT
    NORMALIZE
    OUTPUT_VARIABLE _kalam_snapshot_output
)

set(
    _kalam_source_roots
    bridge
    cmake
    docs
    editor
    engine
    examples
    game
    reflection
    standalone
    tests
)
set(
    _kalam_root_files
    .gitignore
    CMakeLists.txt
    CMakePresets.json
    README.md
)

set(_kalam_files)
foreach(_kalam_root_file IN LISTS _kalam_root_files)
    set(_kalam_path "${_kalam_source_root}/${_kalam_root_file}")
    if(EXISTS "${_kalam_path}")
        list(APPEND _kalam_files "${_kalam_path}")
    endif()
endforeach()
foreach(_kalam_source_subdirectory IN LISTS _kalam_source_roots)
    set(
        _kalam_source_subdirectory_absolute
        "${_kalam_source_root}/${_kalam_source_subdirectory}"
    )
    if(IS_DIRECTORY "${_kalam_source_subdirectory_absolute}")
        file(
            GLOB_RECURSE _kalam_subdirectory_files
            LIST_DIRECTORIES FALSE
            "${_kalam_source_subdirectory_absolute}/*"
        )
        list(APPEND _kalam_files ${_kalam_subdirectory_files})
    endif()
endforeach()

list(REMOVE_DUPLICATES _kalam_files)
list(SORT _kalam_files)
set(_kalam_manifest "")
foreach(_kalam_file IN LISTS _kalam_files)
    file(RELATIVE_PATH _kalam_relative "${_kalam_source_root}" "${_kalam_file}")
    cmake_path(NORMAL_PATH _kalam_relative)
    file(SHA256 "${_kalam_file}" _kalam_sha256)
    string(APPEND _kalam_manifest "${_kalam_sha256}  ${_kalam_relative}\n")
endforeach()

get_filename_component(
    _kalam_snapshot_directory
    "${_kalam_snapshot_output}"
    DIRECTORY
)
file(MAKE_DIRECTORY "${_kalam_snapshot_directory}")
file(WRITE "${_kalam_snapshot_output}" "${_kalam_manifest}")

if(DEFINED KALAM_EXPECTED_SNAPSHOT AND
   NOT KALAM_EXPECTED_SNAPSHOT STREQUAL "")
    cmake_path(
        ABSOLUTE_PATH KALAM_EXPECTED_SNAPSHOT
        NORMALIZE
        OUTPUT_VARIABLE _kalam_expected_snapshot
    )
    if(NOT EXISTS "${_kalam_expected_snapshot}")
        message(
            FATAL_ERROR
            "Source-tree baseline is missing: ${_kalam_expected_snapshot}"
        )
    endif()
    file(READ "${_kalam_expected_snapshot}" _kalam_expected_manifest)
    if(NOT _kalam_manifest STREQUAL _kalam_expected_manifest)
        message(
            FATAL_ERROR
            "The Kalam source tree changed after configure. Generated, test, "
            "and runtime output must remain under the build/runtime roots.\n"
            "Expected: ${_kalam_expected_snapshot}\n"
            "Actual:   ${_kalam_snapshot_output}"
        )
    endif()
endif()

list(LENGTH _kalam_files _kalam_file_count)
message(STATUS "Kalam source-tree snapshot passed (${_kalam_file_count} files).")
