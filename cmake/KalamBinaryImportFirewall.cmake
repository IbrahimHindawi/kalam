cmake_minimum_required(VERSION 3.28)

# This script deliberately parses PE/COFF itself. Depending on dumpbin, link,
# llvm-readobj, or a particular Visual Studio environment would make the test
# behave differently depending on which developer shell launched CTest.

set(_kalam_binary_paths)
if(DEFINED KALAM_BINARY_PATHS AND NOT KALAM_BINARY_PATHS STREQUAL "")
    list(APPEND _kalam_binary_paths ${KALAM_BINARY_PATHS})
endif()
foreach(_kalam_binary_variable IN ITEMS KALAM_GAME_EXE KALAM_GAME_MODULE)
    if(
        DEFINED ${_kalam_binary_variable}
        AND NOT "${${_kalam_binary_variable}}" STREQUAL ""
    )
        list(APPEND _kalam_binary_paths "${${_kalam_binary_variable}}")
    endif()
endforeach()

list(REMOVE_DUPLICATES _kalam_binary_paths)
list(SORT _kalam_binary_paths)

if(NOT _kalam_binary_paths)
    message(
        STATUS
        "Kalam binary import firewall passed (no shipping binaries exist yet)."
    )
    return()
endif()

function(_kalam_pe_fail _kalam_reason)
    message(
        FATAL_ERROR
        "Kalam binary import firewall could not inspect "
        "'${_kalam_current_binary}': ${_kalam_reason}"
    )
endfunction()

function(
    _kalam_pe_require_range
    _kalam_offset
    _kalam_byte_count
    _kalam_context
)
    if(_kalam_offset LESS 0 OR _kalam_byte_count LESS 0)
        _kalam_pe_fail("${_kalam_context} has a negative file range")
    endif()
    math(
        EXPR _kalam_range_end
        "${_kalam_offset} + ${_kalam_byte_count}"
    )
    if(_kalam_range_end GREATER _kalam_binary_size)
        _kalam_pe_fail(
            "${_kalam_context} extends past the end of the file"
        )
    endif()
endfunction()

function(_kalam_pe_read_hex _kalam_offset _kalam_byte_count _kalam_output)
    _kalam_pe_require_range(
        "${_kalam_offset}"
        "${_kalam_byte_count}"
        "PE field"
    )
    math(EXPR _kalam_hex_offset "${_kalam_offset} * 2")
    math(EXPR _kalam_hex_count "${_kalam_byte_count} * 2")
    string(
        SUBSTRING "${_kalam_binary_hex}"
        "${_kalam_hex_offset}"
        "${_kalam_hex_count}"
        _kalam_hex_value
    )
    string(TOLOWER "${_kalam_hex_value}" _kalam_hex_value)
    set(${_kalam_output} "${_kalam_hex_value}" PARENT_SCOPE)
endfunction()

function(_kalam_pe_read_u8 _kalam_offset _kalam_output)
    _kalam_pe_read_hex("${_kalam_offset}" 1 _kalam_u8_hex)
    math(EXPR _kalam_u8_value "0x${_kalam_u8_hex}")
    set(${_kalam_output} "${_kalam_u8_value}" PARENT_SCOPE)
endfunction()

function(_kalam_pe_read_u16 _kalam_offset _kalam_output)
    _kalam_pe_read_hex("${_kalam_offset}" 2 _kalam_u16_hex)
    string(SUBSTRING "${_kalam_u16_hex}" 0 2 _kalam_u16_b0)
    string(SUBSTRING "${_kalam_u16_hex}" 2 2 _kalam_u16_b1)
    math(EXPR _kalam_u16_value "0x${_kalam_u16_b1}${_kalam_u16_b0}")
    set(${_kalam_output} "${_kalam_u16_value}" PARENT_SCOPE)
endfunction()

function(_kalam_pe_read_u32 _kalam_offset _kalam_output)
    _kalam_pe_read_hex("${_kalam_offset}" 4 _kalam_u32_hex)
    string(SUBSTRING "${_kalam_u32_hex}" 0 2 _kalam_u32_b0)
    string(SUBSTRING "${_kalam_u32_hex}" 2 2 _kalam_u32_b1)
    string(SUBSTRING "${_kalam_u32_hex}" 4 2 _kalam_u32_b2)
    string(SUBSTRING "${_kalam_u32_hex}" 6 2 _kalam_u32_b3)
    math(
        EXPR _kalam_u32_value
        "0x${_kalam_u32_b3}${_kalam_u32_b2}${_kalam_u32_b1}${_kalam_u32_b0}"
    )
    set(${_kalam_output} "${_kalam_u32_value}" PARENT_SCOPE)
endfunction()

function(_kalam_pe_read_u64 _kalam_offset _kalam_output)
    _kalam_pe_read_hex("${_kalam_offset}" 8 _kalam_u64_hex)
    set(_kalam_u64_reversed)
    foreach(_kalam_byte_index RANGE 0 7)
        math(EXPR _kalam_byte_offset "${_kalam_byte_index} * 2")
        string(
            SUBSTRING "${_kalam_u64_hex}"
            "${_kalam_byte_offset}"
            2
            _kalam_byte
        )
        set(_kalam_u64_reversed "${_kalam_byte}${_kalam_u64_reversed}")
    endforeach()
    math(EXPR _kalam_u64_value "0x${_kalam_u64_reversed}")
    set(${_kalam_output} "${_kalam_u64_value}" PARENT_SCOPE)
endfunction()

function(_kalam_pe_rva_to_file_offset _kalam_rva _kalam_output)
    if(
        _kalam_rva GREATER_EQUAL 0
        AND _kalam_rva LESS _kalam_size_of_headers
        AND _kalam_rva LESS _kalam_binary_size
    )
        set(${_kalam_output} "${_kalam_rva}" PARENT_SCOPE)
        return()
    endif()

    list(LENGTH _kalam_section_virtual_addresses _kalam_section_count)
    if(_kalam_section_count GREATER 0)
        math(EXPR _kalam_last_section "${_kalam_section_count} - 1")
        foreach(_kalam_section_index RANGE 0 ${_kalam_last_section})
            list(
                GET _kalam_section_virtual_addresses
                ${_kalam_section_index}
                _kalam_section_va
            )
            list(
                GET _kalam_section_virtual_sizes
                ${_kalam_section_index}
                _kalam_section_virtual_size
            )
            list(
                GET _kalam_section_raw_sizes
                ${_kalam_section_index}
                _kalam_section_raw_size
            )
            list(
                GET _kalam_section_raw_offsets
                ${_kalam_section_index}
                _kalam_section_raw_offset
            )

            set(_kalam_section_span "${_kalam_section_virtual_size}")
            if(_kalam_section_raw_size GREATER _kalam_section_span)
                set(_kalam_section_span "${_kalam_section_raw_size}")
            endif()
            math(
                EXPR _kalam_section_end
                "${_kalam_section_va} + ${_kalam_section_span}"
            )

            if(
                _kalam_rva GREATER_EQUAL _kalam_section_va
                AND _kalam_rva LESS _kalam_section_end
            )
                math(
                    EXPR _kalam_section_delta
                    "${_kalam_rva} - ${_kalam_section_va}"
                )
                if(_kalam_section_delta GREATER_EQUAL _kalam_section_raw_size)
                    _kalam_pe_fail(
                        "RVA ${_kalam_rva} points into virtual-only section data"
                    )
                endif()
                math(
                    EXPR _kalam_file_offset
                    "${_kalam_section_raw_offset} + ${_kalam_section_delta}"
                )
                _kalam_pe_require_range(
                    "${_kalam_file_offset}"
                    1
                    "RVA ${_kalam_rva}"
                )
                set(
                    ${_kalam_output}
                    "${_kalam_file_offset}"
                    PARENT_SCOPE
                )
                return()
            endif()
        endforeach()
    endif()

    _kalam_pe_fail("RVA ${_kalam_rva} is not mapped by a PE section")
endfunction()

function(_kalam_pe_read_ascii_z _kalam_offset _kalam_output)
    set(_kalam_text)
    foreach(_kalam_character_index RANGE 0 511)
        math(
            EXPR _kalam_character_offset
            "${_kalam_offset} + ${_kalam_character_index}"
        )
        _kalam_pe_read_u8("${_kalam_character_offset}" _kalam_character)
        if(_kalam_character EQUAL 0)
            set(${_kalam_output} "${_kalam_text}" PARENT_SCOPE)
            return()
        endif()
        if(_kalam_character LESS 32 OR _kalam_character GREATER 126)
            _kalam_pe_fail("import library name is not printable ASCII")
        endif()
        string(ASCII "${_kalam_character}" _kalam_character_text)
        string(APPEND _kalam_text "${_kalam_character_text}")
    endforeach()

    _kalam_pe_fail("import library name exceeds 511 bytes")
endfunction()

function(
    _kalam_pe_scan_import_directory
    _kalam_directory_rva
    _kalam_directory_size
    _kalam_output
)
    set(_kalam_imports)
    if(_kalam_directory_rva EQUAL 0)
        set(${_kalam_output} "" PARENT_SCOPE)
        return()
    endif()

    _kalam_pe_rva_to_file_offset(
        "${_kalam_directory_rva}"
        _kalam_descriptor_offset
    )

    set(_kalam_descriptor_limit 4096)
    if(_kalam_directory_size GREATER 0)
        math(EXPR _kalam_sized_limit "${_kalam_directory_size} / 20")
        if(_kalam_sized_limit LESS 1)
            _kalam_pe_fail("import directory is smaller than one descriptor")
        endif()
        if(_kalam_sized_limit LESS _kalam_descriptor_limit)
            set(_kalam_descriptor_limit "${_kalam_sized_limit}")
        endif()
    endif()

    math(EXPR _kalam_last_descriptor "${_kalam_descriptor_limit} - 1")
    set(_kalam_found_terminator FALSE)
    foreach(_kalam_descriptor_index RANGE 0 ${_kalam_last_descriptor})
        math(
            EXPR _kalam_descriptor
            "${_kalam_descriptor_offset} + ${_kalam_descriptor_index} * 20"
        )
        _kalam_pe_read_u32("${_kalam_descriptor}" _kalam_original_thunk)
        math(EXPR _kalam_field "${_kalam_descriptor} + 4")
        _kalam_pe_read_u32("${_kalam_field}" _kalam_timestamp)
        math(EXPR _kalam_field "${_kalam_descriptor} + 8")
        _kalam_pe_read_u32("${_kalam_field}" _kalam_forwarder_chain)
        math(EXPR _kalam_field "${_kalam_descriptor} + 12")
        _kalam_pe_read_u32("${_kalam_field}" _kalam_name_rva)
        math(EXPR _kalam_field "${_kalam_descriptor} + 16")
        _kalam_pe_read_u32("${_kalam_field}" _kalam_first_thunk)

        if(
            _kalam_original_thunk EQUAL 0
            AND _kalam_timestamp EQUAL 0
            AND _kalam_forwarder_chain EQUAL 0
            AND _kalam_name_rva EQUAL 0
            AND _kalam_first_thunk EQUAL 0
        )
            set(_kalam_found_terminator TRUE)
            break()
        endif()
        if(_kalam_name_rva EQUAL 0)
            _kalam_pe_fail("import descriptor has no library name")
        endif()

        _kalam_pe_rva_to_file_offset(
            "${_kalam_name_rva}"
            _kalam_name_offset
        )
        _kalam_pe_read_ascii_z("${_kalam_name_offset}" _kalam_import_name)
        list(APPEND _kalam_imports "${_kalam_import_name}")
    endforeach()

    if(NOT _kalam_found_terminator)
        _kalam_pe_fail("import descriptor table has no terminator")
    endif()
    set(${_kalam_output} "${_kalam_imports}" PARENT_SCOPE)
endfunction()

function(
    _kalam_pe_scan_delay_import_directory
    _kalam_directory_rva
    _kalam_directory_size
    _kalam_output
)
    set(_kalam_imports)
    if(_kalam_directory_rva EQUAL 0)
        set(${_kalam_output} "" PARENT_SCOPE)
        return()
    endif()

    _kalam_pe_rva_to_file_offset(
        "${_kalam_directory_rva}"
        _kalam_descriptor_offset
    )

    set(_kalam_descriptor_limit 4096)
    if(_kalam_directory_size GREATER 0)
        math(EXPR _kalam_sized_limit "${_kalam_directory_size} / 32")
        if(_kalam_sized_limit LESS 1)
            _kalam_pe_fail(
                "delay-import directory is smaller than one descriptor"
            )
        endif()
        if(_kalam_sized_limit LESS _kalam_descriptor_limit)
            set(_kalam_descriptor_limit "${_kalam_sized_limit}")
        endif()
    endif()

    math(EXPR _kalam_last_descriptor "${_kalam_descriptor_limit} - 1")
    set(_kalam_found_terminator FALSE)
    foreach(_kalam_descriptor_index RANGE 0 ${_kalam_last_descriptor})
        math(
            EXPR _kalam_descriptor
            "${_kalam_descriptor_offset} + ${_kalam_descriptor_index} * 32"
        )
        set(_kalam_delay_fields)
        foreach(_kalam_field_index RANGE 0 7)
            math(
                EXPR _kalam_field
                "${_kalam_descriptor} + ${_kalam_field_index} * 4"
            )
            _kalam_pe_read_u32("${_kalam_field}" _kalam_field_value)
            list(APPEND _kalam_delay_fields "${_kalam_field_value}")
        endforeach()

        set(_kalam_all_zero TRUE)
        foreach(_kalam_field_value IN LISTS _kalam_delay_fields)
            if(NOT _kalam_field_value EQUAL 0)
                set(_kalam_all_zero FALSE)
                break()
            endif()
        endforeach()
        if(_kalam_all_zero)
            set(_kalam_found_terminator TRUE)
            break()
        endif()

        list(GET _kalam_delay_fields 0 _kalam_attributes)
        list(GET _kalam_delay_fields 1 _kalam_name_value)
        if(_kalam_name_value EQUAL 0)
            _kalam_pe_fail("delay-import descriptor has no library name")
        endif()

        math(EXPR _kalam_fields_are_rvas "${_kalam_attributes} & 1")
        if(_kalam_fields_are_rvas)
            set(_kalam_name_rva "${_kalam_name_value}")
        else()
            if(_kalam_name_value LESS _kalam_image_base)
                _kalam_pe_fail(
                    "delay-import library name address precedes image base"
                )
            endif()
            math(
                EXPR _kalam_name_rva
                "${_kalam_name_value} - ${_kalam_image_base}"
            )
        endif()

        _kalam_pe_rva_to_file_offset(
            "${_kalam_name_rva}"
            _kalam_name_offset
        )
        _kalam_pe_read_ascii_z("${_kalam_name_offset}" _kalam_import_name)
        list(APPEND _kalam_imports "${_kalam_import_name}")
    endforeach()

    if(NOT _kalam_found_terminator)
        _kalam_pe_fail("delay-import descriptor table has no terminator")
    endif()
    set(${_kalam_output} "${_kalam_imports}" PARENT_SCOPE)
endfunction()

set(_kalam_violations)
set(_kalam_inspected_binary_count 0)

foreach(_kalam_binary IN LISTS _kalam_binary_paths)
    cmake_path(
        ABSOLUTE_PATH _kalam_binary
        NORMALIZE
        OUTPUT_VARIABLE _kalam_current_binary
    )
    if(NOT EXISTS "${_kalam_current_binary}")
        _kalam_pe_fail("configured target binary does not exist")
    endif()
    if(IS_DIRECTORY "${_kalam_current_binary}")
        _kalam_pe_fail("target path is a directory")
    endif()

    math(
        EXPR _kalam_inspected_binary_count
        "${_kalam_inspected_binary_count} + 1"
    )
    file(SIZE "${_kalam_current_binary}" _kalam_binary_size)
    file(READ "${_kalam_current_binary}" _kalam_binary_hex HEX)

    _kalam_pe_read_hex(0 2 _kalam_dos_signature)
    if(NOT _kalam_dos_signature STREQUAL "4d5a")
        _kalam_pe_fail("file does not have an MZ signature")
    endif()
    _kalam_pe_read_u32(60 _kalam_pe_offset)
    _kalam_pe_read_hex("${_kalam_pe_offset}" 4 _kalam_pe_signature)
    if(NOT _kalam_pe_signature STREQUAL "50450000")
        _kalam_pe_fail("file does not have a PE signature")
    endif()

    math(EXPR _kalam_coff_header "${_kalam_pe_offset} + 4")
    math(EXPR _kalam_field "${_kalam_coff_header} + 2")
    _kalam_pe_read_u16("${_kalam_field}" _kalam_number_of_sections)
    math(EXPR _kalam_field "${_kalam_coff_header} + 16")
    _kalam_pe_read_u16("${_kalam_field}" _kalam_optional_header_size)
    math(EXPR _kalam_optional_header "${_kalam_coff_header} + 20")
    _kalam_pe_require_range(
        "${_kalam_optional_header}"
        "${_kalam_optional_header_size}"
        "optional header"
    )
    _kalam_pe_read_u16("${_kalam_optional_header}" _kalam_optional_magic)

    if(_kalam_optional_magic EQUAL 267)
        set(_kalam_data_directories_relative 96)
        set(_kalam_directory_count_relative 92)
        math(EXPR _kalam_field "${_kalam_optional_header} + 28")
        _kalam_pe_read_u32("${_kalam_field}" _kalam_image_base)
    elseif(_kalam_optional_magic EQUAL 523)
        set(_kalam_data_directories_relative 112)
        set(_kalam_directory_count_relative 108)
        math(EXPR _kalam_field "${_kalam_optional_header} + 24")
        _kalam_pe_read_u64("${_kalam_field}" _kalam_image_base)
    else()
        _kalam_pe_fail(
            "unsupported optional-header magic ${_kalam_optional_magic}"
        )
    endif()

    math(EXPR _kalam_field "${_kalam_optional_header} + 60")
    _kalam_pe_read_u32("${_kalam_field}" _kalam_size_of_headers)
    math(
        EXPR _kalam_directory_count_offset
        "${_kalam_optional_header} + ${_kalam_directory_count_relative}"
    )
    _kalam_pe_read_u32(
        "${_kalam_directory_count_offset}"
        _kalam_directory_count
    )

    set(_kalam_import_rva 0)
    set(_kalam_import_size 0)
    if(_kalam_directory_count GREATER 1)
        math(
            EXPR _kalam_import_directory
            "${_kalam_optional_header} + ${_kalam_data_directories_relative} + 8"
        )
        math(
            EXPR _kalam_optional_end
            "${_kalam_optional_header} + ${_kalam_optional_header_size}"
        )
        math(EXPR _kalam_import_directory_end "${_kalam_import_directory} + 8")
        if(_kalam_import_directory_end GREATER _kalam_optional_end)
            _kalam_pe_fail("import data directory is outside optional header")
        endif()
        _kalam_pe_read_u32("${_kalam_import_directory}" _kalam_import_rva)
        math(EXPR _kalam_field "${_kalam_import_directory} + 4")
        _kalam_pe_read_u32("${_kalam_field}" _kalam_import_size)
    endif()

    set(_kalam_delay_import_rva 0)
    set(_kalam_delay_import_size 0)
    if(_kalam_directory_count GREATER 13)
        math(
            EXPR _kalam_delay_import_directory
            "${_kalam_optional_header} + ${_kalam_data_directories_relative} + 104"
        )
        math(
            EXPR _kalam_optional_end
            "${_kalam_optional_header} + ${_kalam_optional_header_size}"
        )
        math(
            EXPR _kalam_delay_import_directory_end
            "${_kalam_delay_import_directory} + 8"
        )
        if(_kalam_delay_import_directory_end GREATER _kalam_optional_end)
            _kalam_pe_fail(
                "delay-import data directory is outside optional header"
            )
        endif()
        _kalam_pe_read_u32(
            "${_kalam_delay_import_directory}"
            _kalam_delay_import_rva
        )
        math(EXPR _kalam_field "${_kalam_delay_import_directory} + 4")
        _kalam_pe_read_u32(
            "${_kalam_field}"
            _kalam_delay_import_size
        )
    endif()

    math(
        EXPR _kalam_section_table
        "${_kalam_optional_header} + ${_kalam_optional_header_size}"
    )
    math(EXPR _kalam_section_table_size "${_kalam_number_of_sections} * 40")
    _kalam_pe_require_range(
        "${_kalam_section_table}"
        "${_kalam_section_table_size}"
        "section table"
    )

    set(_kalam_section_virtual_addresses)
    set(_kalam_section_virtual_sizes)
    set(_kalam_section_raw_sizes)
    set(_kalam_section_raw_offsets)
    if(_kalam_number_of_sections GREATER 0)
        math(EXPR _kalam_last_section "${_kalam_number_of_sections} - 1")
        foreach(_kalam_section_index RANGE 0 ${_kalam_last_section})
            math(
                EXPR _kalam_section
                "${_kalam_section_table} + ${_kalam_section_index} * 40"
            )
            math(EXPR _kalam_field "${_kalam_section} + 8")
            _kalam_pe_read_u32("${_kalam_field}" _kalam_virtual_size)
            math(EXPR _kalam_field "${_kalam_section} + 12")
            _kalam_pe_read_u32("${_kalam_field}" _kalam_virtual_address)
            math(EXPR _kalam_field "${_kalam_section} + 16")
            _kalam_pe_read_u32("${_kalam_field}" _kalam_raw_size)
            math(EXPR _kalam_field "${_kalam_section} + 20")
            _kalam_pe_read_u32("${_kalam_field}" _kalam_raw_offset)
            list(
                APPEND _kalam_section_virtual_addresses
                "${_kalam_virtual_address}"
            )
            list(APPEND _kalam_section_virtual_sizes "${_kalam_virtual_size}")
            list(APPEND _kalam_section_raw_sizes "${_kalam_raw_size}")
            list(APPEND _kalam_section_raw_offsets "${_kalam_raw_offset}")
        endforeach()
    endif()

    _kalam_pe_scan_import_directory(
        "${_kalam_import_rva}"
        "${_kalam_import_size}"
        _kalam_imports
    )
    _kalam_pe_scan_delay_import_directory(
        "${_kalam_delay_import_rva}"
        "${_kalam_delay_import_size}"
        _kalam_delay_imports
    )
    list(APPEND _kalam_imports ${_kalam_delay_imports})
    list(REMOVE_DUPLICATES _kalam_imports)
    list(SORT _kalam_imports)

    foreach(_kalam_import IN LISTS _kalam_imports)
        string(REPLACE "\\" "/" _kalam_import_normalized "${_kalam_import}")
        get_filename_component(
            _kalam_import_basename
            "${_kalam_import_normalized}"
            NAME
        )
        string(TOLOWER "${_kalam_import_basename}" _kalam_import_lower)
        if(_kalam_import_lower MATCHES "^qt[^/]*[.]dll$")
            string(
                CONCAT _kalam_diagnostic
                "${_kalam_current_binary} imports Qt library: "
                "${_kalam_import}"
            )
            list(APPEND _kalam_violations "${_kalam_diagnostic}")
        endif()
    endforeach()

    list(LENGTH _kalam_imports _kalam_import_count)
    message(
        STATUS
        "Inspected ${_kalam_current_binary} "
        "(${_kalam_import_count} direct/delay-load imports)."
    )
endforeach()

if(_kalam_violations)
    list(SORT _kalam_violations)
    list(JOIN _kalam_violations "\n  " _kalam_violation_report)
    message(
        FATAL_ERROR
        "Kalam binary import firewall rejected Qt dependencies:\n"
        "  ${_kalam_violation_report}"
    )
endif()

message(
    STATUS
    "Kalam binary import firewall passed "
    "(${_kalam_inspected_binary_count} binaries inspected)."
)
