#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "reflection_tool.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <process.h>
#define KALAM_TEST_GET_PROCESS_ID() ((uint32_t)_getpid())
typedef struct _stat64 Kalam_Test_File_Stat;
#define KALAM_TEST_STAT _stat64
#else
#include <unistd.h>
#define KALAM_TEST_GET_PROCESS_ID() ((uint32_t)getpid())
typedef struct stat Kalam_Test_File_Stat;
#define KALAM_TEST_STAT stat
#endif

typedef struct Reflection_Test_Context
{
    uint32_t checks;
    uint32_t failures;
} Reflection_Test_Context;

#define KALAM_TEST_CHECK(context, expression)                                      \
    do                                                                              \
    {                                                                               \
        (context)->checks += 1;                                                      \
        if (!(expression))                                                          \
        {                                                                           \
            (context)->failures += 1;                                                \
            fprintf(                                                                \
                stderr,                                                             \
                "%s(%d): check failed: %s\n",                                      \
                __FILE__,                                                           \
                __LINE__,                                                           \
                #expression                                                         \
            );                                                                      \
        }                                                                           \
    } while (0)

static const char g_scene_source_order_a[] =
    "// ordinary C/C++ remains opaque to the reflection parser\n"
    "//@kalam schema symbol=Scene id=\"nveditor.scene/v1\" version=1 flags=0\n"
    "//@kalam field key=\"scene.cube.color\" name=\"color\" path=\"$.cube.color\" group=\"cube\" kind=Color3 semantic=Color min=[0,0,0,0] max=[1,1,1,0] step=[0.01,0.01,0.01,0] flags=0 id=20\n"
    "float color[3];\n"
    "//@kalam field key=\"scene.cube.translation\" name=\"translation\" path=\"$.cube.translation\" group=\"cube\" kind=Vec3 semantic=Position min=[-100,-100,-100,0] max=[100,100,100,0] step=[0.1,0.1,0.1,0] flags=2 id=10\n"
    "float translation[3];\n"
    "//@kalam end\n";

static const char g_scene_source_order_b[] =
    "//@kalam schema symbol=Scene id=\"nveditor.scene/v1\" version=1 flags=0\n"
    "//@kalam field key=\"scene.cube.translation\" name=\"translation\" path=\"$.cube.translation\" group=\"cube\" kind=Vec3 semantic=Position min=[-100,-100,-100,0] max=[100,100,100,0] step=[0.1,0.1,0.1,0] flags=2 id=10\n"
    "//@kalam field key=\"scene.cube.color\" name=\"color\" path=\"$.cube.color\" group=\"cube\" kind=Color3 semantic=Color min=[0,0,0,0] max=[1,1,1,0] step=[0.01,0.01,0.01,0] flags=0 id=20\n"
    "//@kalam end\n";

static const char g_post_process_source[] =
    "//@kalam schema symbol=Post_Process id=\"nveditor.post_process/v1\" version=1 flags=4\n"
    "//@kalam field key=\"post.exposure\" name=\"exposure\" path=\"$.exposure\" group=\"tone_mapping\" kind=F32 semantic=None min=[0,0,0,0] max=[16,0,0,0] step=[0.05,0,0,0] flags=1 id=auto\n"
    "//@kalam end\n";

static Reflection_Result reflection_test_generate_one(
    const char* path,
    const char* text,
    const Reflection_Generate_Options* options,
    Reflection_Output* output,
    Reflection_Diagnostic* diagnostic
)
{
    Reflection_Source source = {};
    source.path = path;
    source.text = text;
    source.text_size = (uint64_t)strlen(text);
    return reflection_generate(&source, 1, options, output, diagnostic);
}

static uint64_t reflection_test_constant_hash(
    const char* data,
    uint64_t size,
    void* user
)
{
    (void)data;
    (void)size;
    (void)user;
    return UINT64_C(0x123456789abcdef0);
}

static void reflection_test_fnv1a64(Reflection_Test_Context* context)
{
    KALAM_TEST_CHECK(
        context,
        reflection_fnv1a64("", 0, 0) == UINT64_C(0xcbf29ce484222325)
    );
    KALAM_TEST_CHECK(
        context,
        reflection_fnv1a64("hello", 5, 0) == UINT64_C(0xa430d84680aabd0b)
    );
}

static void reflection_test_deterministic_output(Reflection_Test_Context* context)
{
    Reflection_Source first_order[2] = {};
    Reflection_Source second_order[2] = {};
    Reflection_Output first_output = {};
    Reflection_Output second_output = {};
    Reflection_Diagnostic diagnostic = {};
    Reflection_Result result = Reflection_Result_Ok;
    const char* scene_schema = "nveditor.scene/v1";
    const char* post_schema = "nveditor.post_process/v1";
    const char* first_scene = 0;
    const char* first_post = 0;
    const char* field_10 = 0;
    const char* field_20 = 0;

    first_order[0].path = "scene_a.h";
    first_order[0].text = g_scene_source_order_a;
    first_order[0].text_size = sizeof(g_scene_source_order_a) - 1;
    first_order[1].path = "post.h";
    first_order[1].text = g_post_process_source;
    first_order[1].text_size = sizeof(g_post_process_source) - 1;

    second_order[0].path = "post.h";
    second_order[0].text = g_post_process_source;
    second_order[0].text_size = sizeof(g_post_process_source) - 1;
    second_order[1].path = "scene_b.h";
    second_order[1].text = g_scene_source_order_b;
    second_order[1].text_size = sizeof(g_scene_source_order_b) - 1;

    reflection_output_init(&first_output);
    reflection_output_init(&second_output);

    result = reflection_generate(
        first_order,
        2,
        0,
        &first_output,
        &diagnostic
    );
    KALAM_TEST_CHECK(context, result == Reflection_Result_Ok);

    result = reflection_generate(
        second_order,
        2,
        0,
        &second_output,
        &diagnostic
    );
    KALAM_TEST_CHECK(context, result == Reflection_Result_Ok);
    KALAM_TEST_CHECK(context, first_output.size == second_output.size);
    KALAM_TEST_CHECK(
        context,
        (first_output.size == second_output.size)
            && (memcmp(first_output.data, second_output.data, first_output.size) == 0)
    );

    KALAM_TEST_CHECK(
        context,
        strstr(first_output.data, "#include \"author_schema.h\"") != 0
    );
    KALAM_TEST_CHECK(
        context,
        strstr(first_output.data, "bridge/author_schema.h") == 0
    );

    first_scene = strstr(first_output.data, scene_schema);
    first_post = strstr(first_output.data, post_schema);
    KALAM_TEST_CHECK(context, first_scene != 0);
    KALAM_TEST_CHECK(context, first_post != 0);
    KALAM_TEST_CHECK(context, first_post < first_scene);

    field_10 = strstr(
        first_output.data,
        "UINT64_C(10),\n        { \"translation\""
    );
    field_20 = strstr(
        first_output.data,
        "UINT64_C(20),\n        { \"color\""
    );
    KALAM_TEST_CHECK(context, field_10 != 0);
    KALAM_TEST_CHECK(context, field_20 != 0);
    KALAM_TEST_CHECK(context, field_10 < field_20);

    reflection_output_release(&second_output);
    reflection_output_release(&first_output);
}

static void reflection_test_malformed_annotation(Reflection_Test_Context* context)
{
    const char source[] =
        "//@kalam schema symbol=Broken id=\"broken/v1\" version=1 flags=0\n"
        "//@kalam field key=\"broken.value\" name=\"value\" path=\"$.value\" group=\"\" kind=F32 semantic=None min=[0,0,0,0 max=[1,0,0,0] step=[1,0,0,0] flags=0 id=auto\n"
        "//@kalam end\n";
    Reflection_Output output = {};
    Reflection_Diagnostic diagnostic = {};
    Reflection_Result result = Reflection_Result_Ok;

    reflection_output_init(&output);
    result = reflection_test_generate_one(
        "malformed.h",
        source,
        0,
        &output,
        &diagnostic
    );
    KALAM_TEST_CHECK(context, result == Reflection_Result_Parse_Error);
    KALAM_TEST_CHECK(context, diagnostic.line == 2);
    KALAM_TEST_CHECK(context, diagnostic.message[0] != 0);
    KALAM_TEST_CHECK(context, output.size == 0);
    reflection_output_release(&output);
}

static void reflection_test_duplicate_schema_id(Reflection_Test_Context* context)
{
    const char source[] =
        "//@kalam schema symbol=First id=\"duplicate/v1\" version=1 flags=0\n"
        "//@kalam end\n"
        "//@kalam schema symbol=Second id=\"duplicate/v1\" version=1 flags=0\n"
        "//@kalam end\n";
    Reflection_Output output = {};
    Reflection_Diagnostic diagnostic = {};
    Reflection_Result result = Reflection_Result_Ok;

    reflection_output_init(&output);
    result = reflection_test_generate_one(
        "duplicate_schema.h",
        source,
        0,
        &output,
        &diagnostic
    );
    KALAM_TEST_CHECK(context, result == Reflection_Result_Duplicate_Schema_Id);
    KALAM_TEST_CHECK(context, diagnostic.line == 3);
    reflection_output_release(&output);
}

static void reflection_test_duplicate_field_id(Reflection_Test_Context* context)
{
    const char source[] =
        "//@kalam schema symbol=Duplicate_Field id=\"duplicate_field/v1\" version=1 flags=0\n"
        "//@kalam field key=\"first\" name=\"first\" path=\"$.first\" group=\"\" kind=F32 semantic=None min=[0,0,0,0] max=[1,0,0,0] step=[1,0,0,0] flags=0 id=7\n"
        "//@kalam field key=\"second\" name=\"second\" path=\"$.second\" group=\"\" kind=F32 semantic=None min=[0,0,0,0] max=[1,0,0,0] step=[1,0,0,0] flags=0 id=7\n"
        "//@kalam end\n";
    Reflection_Output output = {};
    Reflection_Diagnostic diagnostic = {};
    Reflection_Result result = Reflection_Result_Ok;

    reflection_output_init(&output);
    result = reflection_test_generate_one(
        "duplicate_field.h",
        source,
        0,
        &output,
        &diagnostic
    );
    KALAM_TEST_CHECK(context, result == Reflection_Result_Duplicate_Field_Id);
    KALAM_TEST_CHECK(context, diagnostic.line == 3);
    reflection_output_release(&output);
}

static void reflection_test_hash_collision(Reflection_Test_Context* context)
{
    const char source[] =
        "//@kalam schema symbol=Collision id=\"collision/v1\" version=1 flags=0\n"
        "//@kalam field key=\"first.auto\" name=\"first\" path=\"$.first\" group=\"\" kind=F32 semantic=None min=[0,0,0,0] max=[1,0,0,0] step=[1,0,0,0] flags=0 id=auto\n"
        "//@kalam field key=\"second.auto\" name=\"second\" path=\"$.second\" group=\"\" kind=F32 semantic=None min=[0,0,0,0] max=[1,0,0,0] step=[1,0,0,0] flags=0 id=auto\n"
        "//@kalam end\n";
    Reflection_Generate_Options options = {};
    Reflection_Output output = {};
    Reflection_Diagnostic diagnostic = {};
    Reflection_Result result = Reflection_Result_Ok;

    options.hash_function = reflection_test_constant_hash;
    reflection_output_init(&output);
    result = reflection_test_generate_one(
        "collision.h",
        source,
        &options,
        &output,
        &diagnostic
    );
    KALAM_TEST_CHECK(context, result == Reflection_Result_Fnv64_Collision);
    KALAM_TEST_CHECK(context, diagnostic.line == 3);
    KALAM_TEST_CHECK(context, strstr(diagnostic.message, "collision") != 0);
    reflection_output_release(&output);
}

static void reflection_test_zero_and_version_ids(Reflection_Test_Context* context)
{
    const char zero_id_source[] =
        "//@kalam schema symbol=Zero id=\"zero/v1\" version=1 flags=0\n"
        "//@kalam field key=\"zero\" name=\"zero\" path=\"$.zero\" group=\"\" kind=I32 semantic=None min=[0,0,0,0] max=[1,0,0,0] step=[1,0,0,0] flags=0 id=0\n"
        "//@kalam end\n";
    const char version_source[] =
        "//@kalam schema symbol=Wrong_Version id=\"wrong/v2\" version=1 flags=0\n"
        "//@kalam end\n";
    Reflection_Output output = {};
    Reflection_Diagnostic diagnostic = {};
    Reflection_Result result = Reflection_Result_Ok;

    reflection_output_init(&output);
    result = reflection_test_generate_one(
        "zero.h",
        zero_id_source,
        0,
        &output,
        &diagnostic
    );
    KALAM_TEST_CHECK(context, result == Reflection_Result_Invalid_Field_Id);

    result = reflection_test_generate_one(
        "version.h",
        version_source,
        0,
        &output,
        &diagnostic
    );
    KALAM_TEST_CHECK(context, result == Reflection_Result_Parse_Error);
    KALAM_TEST_CHECK(context, strstr(diagnostic.message, "matching") != 0);
    reflection_output_release(&output);
}

static void reflection_test_write_if_changed(Reflection_Test_Context* context)
{
    Reflection_Output output = {};
    Reflection_Diagnostic diagnostic = {};
    Reflection_Result result = Reflection_Result_Ok;
    uint32_t changed = 0;
    char output_path[128] = {};
    Kalam_Test_File_Stat first_stat = {};
    Kalam_Test_File_Stat second_stat = {};
    FILE* file = 0;
    char* read_back = 0;
    long file_size = 0;

    snprintf(
        output_path,
        sizeof(output_path),
        "kalam_reflection_write_%u.generated.h",
        KALAM_TEST_GET_PROCESS_ID()
    );
    remove(output_path);

    reflection_output_init(&output);
    result = reflection_test_generate_one(
        "post.h",
        g_post_process_source,
        0,
        &output,
        &diagnostic
    );
    KALAM_TEST_CHECK(context, result == Reflection_Result_Ok);

    result = reflection_write_if_changed_atomic(
        output_path,
        output.data,
        output.size,
        &changed,
        &diagnostic
    );
    KALAM_TEST_CHECK(context, result == Reflection_Result_Ok);
    KALAM_TEST_CHECK(context, changed == 1);
    KALAM_TEST_CHECK(context, KALAM_TEST_STAT(output_path, &first_stat) == 0);

    changed = 99;
    result = reflection_write_if_changed_atomic(
        output_path,
        output.data,
        output.size,
        &changed,
        &diagnostic
    );
    KALAM_TEST_CHECK(context, result == Reflection_Result_Ok);
    KALAM_TEST_CHECK(context, changed == 0);
    KALAM_TEST_CHECK(context, KALAM_TEST_STAT(output_path, &second_stat) == 0);
#if defined(_WIN32)
    KALAM_TEST_CHECK(context, first_stat.st_mtime == second_stat.st_mtime);
    KALAM_TEST_CHECK(context, first_stat.st_size == second_stat.st_size);
#else
    KALAM_TEST_CHECK(context, first_stat.st_mtime == second_stat.st_mtime);
    KALAM_TEST_CHECK(context, first_stat.st_size == second_stat.st_size);
#endif

    file = fopen(output_path, "rb");
    KALAM_TEST_CHECK(context, file != 0);
    if (file != 0)
    {
        KALAM_TEST_CHECK(context, fseek(file, 0, SEEK_END) == 0);
        file_size = ftell(file);
        KALAM_TEST_CHECK(context, file_size >= 0);
        KALAM_TEST_CHECK(context, fseek(file, 0, SEEK_SET) == 0);
        if ((file_size >= 0) && ((uint64_t)file_size < (uint64_t)SIZE_MAX))
        {
            read_back = (char*)malloc((size_t)file_size + 1);
        }
        KALAM_TEST_CHECK(context, read_back != 0);
        if (read_back != 0)
        {
            size_t read_size = fread(read_back, 1, (size_t)file_size, file);
            KALAM_TEST_CHECK(context, read_size == (size_t)file_size);
            read_back[file_size] = 0;
            KALAM_TEST_CHECK(context, (uint64_t)file_size == output.size);
            KALAM_TEST_CHECK(
                context,
                ((uint64_t)file_size == output.size)
                    && (memcmp(read_back, output.data, output.size) == 0)
            );
        }
        fclose(file);
    }

    free(read_back);
    KALAM_TEST_CHECK(context, remove(output_path) == 0);
    reflection_output_release(&output);
}

int main()
{
    Reflection_Test_Context context = {};

    reflection_test_fnv1a64(&context);
    reflection_test_deterministic_output(&context);
    reflection_test_malformed_annotation(&context);
    reflection_test_duplicate_schema_id(&context);
    reflection_test_duplicate_field_id(&context);
    reflection_test_hash_collision(&context);
    reflection_test_zero_and_version_ids(&context);
    reflection_test_write_if_changed(&context);

    if (context.failures != 0)
    {
        fprintf(
            stderr,
            "reflection_tool_tests: %u of %u checks failed\n",
            context.failures,
            context.checks
        );
        return 1;
    }

    printf("reflection_tool_tests: %u checks passed\n", context.checks);
    return 0;
}
