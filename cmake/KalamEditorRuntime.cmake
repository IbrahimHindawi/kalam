# Stages the Kalam-owned Neovim runtime (Lua plugin and modules) beside the
# editor executable. The editor speaks Neovim's UI protocol directly
# (docs/spec_2.md); there is no third-party GUI runtime to stage.

function(kalam_stage_editor_runtime application_target runtime_source_dir)
    if(NOT TARGET "${application_target}")
        message(
            FATAL_ERROR
            "Cannot stage the editor runtime for missing target: "
            "${application_target}"
        )
    endif()
    if(NOT IS_DIRECTORY "${runtime_source_dir}")
        message(
            FATAL_ERROR
            "The editor runtime directory does not exist: "
            "${runtime_source_dir}"
        )
    endif()

    file(
        GLOB_RECURSE
        runtime_dependencies
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        "${runtime_source_dir}/*"
    )
    set_property(
        TARGET "${application_target}"
        APPEND
        PROPERTY LINK_DEPENDS "${runtime_dependencies}"
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
            "${runtime_source_dir}"
            "${runtime_output}"
        COMMENT "Staging the Kalam editor Neovim runtime"
        VERBATIM
    )
endfunction()
