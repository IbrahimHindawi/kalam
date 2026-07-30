cmake_minimum_required(VERSION 3.28)

if(NOT KALAM_NEOVIM_QT_SOURCE_DIR)
    message(
        FATAL_ERROR
        "Pass -DKALAM_NEOVIM_QT_SOURCE_DIR=<clean v0.2.19 checkout>."
    )
endif()
if(NOT KALAM_NEOVIM_QT_VERIFY_OUTPUT_DIR)
    set(
        KALAM_NEOVIM_QT_VERIFY_OUTPUT_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/build/neovim-qt-patch-verification"
    )
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/KalamNeovimQt.cmake")
kalam_prepare_pinned_neovim_qt(
    patched_source
    SOURCE_DIR "${KALAM_NEOVIM_QT_SOURCE_DIR}"
    OUTPUT_DIR "${KALAM_NEOVIM_QT_VERIFY_OUTPUT_DIR}"
)
message(
    STATUS
    "Verified pinned process-only Neovim-Qt source at ${patched_source}"
)
