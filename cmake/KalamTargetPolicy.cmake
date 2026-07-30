include_guard(GLOBAL)

function(kalam_apply_engine_policy target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_20)

    if(MSVC)
        target_compile_options(
            ${target_name}
            PRIVATE
                /W4
                /WX
                /permissive-
                /EHs-c-
                /GR-
                /utf-8
        )
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target_name} PRIVATE /Zc:preprocessor)
        endif()
        set_property(
            TARGET ${target_name}
            PROPERTY
                MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        )
    else()
        target_compile_options(
            ${target_name}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Werror
                -fno-exceptions
                -fno-rtti
        )
    endif()
endfunction()

function(kalam_apply_qt_policy target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_20)

    if(MSVC)
        target_compile_options(
            ${target_name}
            PRIVATE
                /W4
                /WX
                /permissive-
                /EHs-c-
                /GR
                /utf-8
        )
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target_name} PRIVATE /Zc:preprocessor)
        endif()
        set_property(
            TARGET ${target_name}
            PROPERTY
                MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        )
    else()
        target_compile_options(
            ${target_name}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Werror
                -fno-exceptions
                -frtti
        )
    endif()
endfunction()

function(kalam_apply_c_abi_policy target_name)
    if(MSVC)
        target_compile_options(
            ${target_name}
            PRIVATE
                /W4
                /WX
                /utf-8
        )
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target_name} PRIVATE /Zc:preprocessor)
        endif()
        set_property(
            TARGET ${target_name}
            PROPERTY
                MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        )
    else()
        target_compile_options(
            ${target_name}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Werror
        )
    endif()
endfunction()
