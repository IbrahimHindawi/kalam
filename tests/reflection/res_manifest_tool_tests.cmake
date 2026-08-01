# Exercises res_manifest_tool over fixture manifests
# (docs/resource_manifest.md section 8).

if(NOT DEFINED KALAM_RES_MANIFEST_TOOL OR NOT DEFINED KALAM_FIXTURE_DIR OR
   NOT DEFINED KALAM_WORK_DIR)
    message(FATAL_ERROR "res_manifest_tool test arguments are incomplete.")
endif()

file(MAKE_DIRECTORY "${KALAM_WORK_DIR}")

function(kalam_expect_manifest_result manifest expected_ok label)
    execute_process(
        COMMAND
            "${KALAM_RES_MANIFEST_TOOL}"
            --manifest "${KALAM_FIXTURE_DIR}/${manifest}"
            --output "${KALAM_WORK_DIR}/${label}.h"
        RESULT_VARIABLE tool_result
        ERROR_VARIABLE tool_stderr
    )
    if(expected_ok AND NOT tool_result EQUAL 0)
        message(FATAL_ERROR
            "${label}: expected success, got ${tool_result}:\n${tool_stderr}")
    endif()
    if(NOT expected_ok AND tool_result EQUAL 0)
        message(FATAL_ERROR
            "${label}: expected rejection, but the tool succeeded")
    endif()
endfunction()

# A valid manifest generates, twice, byte-identically.
kalam_expect_manifest_result("good/res.manifest.json" TRUE "good_first")
kalam_expect_manifest_result("good/res.manifest.json" TRUE "good_second")
file(SHA256 "${KALAM_WORK_DIR}/good_first.h" first_hash)
file(SHA256 "${KALAM_WORK_DIR}/good_second.h" second_hash)
if(NOT first_hash STREQUAL second_hash)
    message(FATAL_ERROR "regeneration is not deterministic")
endif()

# The generated header carries the inferred handles and tables.
file(READ "${KALAM_WORK_DIR}/good_first.h" generated)
foreach(expected_token
        "res_shader_fixture_basic"
        "res_tex_checker"
        "res_shader_xmacro"
        "res_tex_xmacro"
        "Res_Shader_Count = 1"
        "Res_Tex_Count = 1"
        "res/textures/checker.dds")
    if(NOT generated MATCHES "${expected_token}")
        message(FATAL_ERROR
            "the generated header is missing: ${expected_token}")
    endif()
endforeach()

# Rewriting an unchanged output must not touch the file timestamp content.
file(SHA256 "${KALAM_WORK_DIR}/good_first.h" before_hash)
kalam_expect_manifest_result("good/res.manifest.json" TRUE "good_first")
file(SHA256 "${KALAM_WORK_DIR}/good_first.h" after_hash)
if(NOT before_hash STREQUAL after_hash)
    message(FATAL_ERROR "an unchanged manifest rewrote its output")
endif()

# Rejections, each with the offending entry diagnosed.
kalam_expect_manifest_result("authored_id/res.manifest.json" FALSE
    "authored_id")
kalam_expect_manifest_result("duplicate/res.manifest.json" FALSE "duplicate")
kalam_expect_manifest_result("missing_file/res.manifest.json" FALSE
    "missing_file")
kalam_expect_manifest_result("wrong_schema/res.manifest.json" FALSE
    "wrong_schema")
kalam_expect_manifest_result("unknown_key/res.manifest.json" FALSE
    "unknown_key")

message(STATUS "res_manifest_tool fixture checks passed")
