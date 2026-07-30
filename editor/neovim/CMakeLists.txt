if(NOT COMMAND kalam_add_neovim_qt)
    message(
        FATAL_ERROR
        "Include KalamNeovimQt before adding editor/neovim."
    )
endif()

kalam_add_neovim_qt(
    TARGET kalam_neovim_qt
    PROJECT_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
)
add_library(Kalam::NeovimQt ALIAS kalam_neovim_qt)
