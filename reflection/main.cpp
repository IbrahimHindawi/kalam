#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "reflection_tool.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Reflection_Loaded_Source
{
    Reflection_Source source;
    char* owned_text;
} Reflection_Loaded_Source;

static void reflection_cli_print_usage(void)
{
    fprintf(
        stderr,
        "usage: reflection_tool --output <generated-header> <input>...\n"
    );
}

static uint32_t reflection_cli_read_file(
    const char* path,
    Reflection_Loaded_Source* loaded
)
{
    FILE* file = 0;
    long file_size = 0;
    char* text = 0;
    size_t read_size = 0;

    errno = 0;
    file = fopen(path, "rb");
    if (file == 0)
    {
        fprintf(stderr, "%s: error: could not open input (errno %d)\n", path, errno);
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0)
    {
        fprintf(stderr, "%s: error: could not seek input\n", path);
        fclose(file);
        return 0;
    }

    file_size = ftell(file);
    if (file_size < 0)
    {
        fprintf(stderr, "%s: error: could not determine input size\n", path);
        fclose(file);
        return 0;
    }
    if (fseek(file, 0, SEEK_SET) != 0)
    {
        fprintf(stderr, "%s: error: could not rewind input\n", path);
        fclose(file);
        return 0;
    }

    if ((uint64_t)file_size >= (uint64_t)SIZE_MAX)
    {
        fprintf(stderr, "%s: error: input is too large\n", path);
        fclose(file);
        return 0;
    }

    text = (char*)malloc((size_t)file_size + 1);
    if (text == 0)
    {
        fprintf(stderr, "%s: error: out of memory while reading input\n", path);
        fclose(file);
        return 0;
    }

    read_size = fread(text, 1, (size_t)file_size, file);
    if (read_size != (size_t)file_size)
    {
        fprintf(stderr, "%s: error: short read while loading input\n", path);
        free(text);
        fclose(file);
        return 0;
    }
    if (fclose(file) != 0)
    {
        fprintf(stderr, "%s: error: could not close input\n", path);
        free(text);
        return 0;
    }

    text[file_size] = 0;
    loaded->source.path = path;
    loaded->source.text = text;
    loaded->source.text_size = (uint64_t)file_size;
    loaded->owned_text = text;
    return 1;
}

static void reflection_cli_print_diagnostic(
    const Reflection_Diagnostic* diagnostic
)
{
    if ((diagnostic->path[0] != 0) && (diagnostic->line != 0))
    {
        fprintf(
            stderr,
            "%s(%u,%u): error: %s\n",
            diagnostic->path,
            diagnostic->line,
            diagnostic->column,
            diagnostic->message
        );
    }
    else if (diagnostic->path[0] != 0)
    {
        fprintf(stderr, "%s: error: %s\n", diagnostic->path, diagnostic->message);
    }
    else
    {
        fprintf(stderr, "reflection_tool: error: %s\n", diagnostic->message);
    }
}

int main(int argument_count, char** arguments)
{
    const char* output_path = 0;
    int first_input_index = 0;
    uint32_t source_count = 0;
    Reflection_Loaded_Source* loaded_sources = 0;
    Reflection_Source* sources = 0;
    Reflection_Output output = {};
    Reflection_Diagnostic diagnostic = {};
    Reflection_Result result = Reflection_Result_Ok;
    uint32_t changed = 0;
    uint32_t source_index = 0;
    int exit_code = 1;

    if ((argument_count == 2)
        && ((strcmp(arguments[1], "--help") == 0)
            || (strcmp(arguments[1], "-h") == 0)))
    {
        reflection_cli_print_usage();
        return 0;
    }
    if ((argument_count < 4) || (strcmp(arguments[1], "--output") != 0))
    {
        reflection_cli_print_usage();
        return 2;
    }

    output_path = arguments[2];
    first_input_index = 3;
    source_count = (uint32_t)(argument_count - first_input_index);

    if ((uint64_t)source_count > ((uint64_t)SIZE_MAX / sizeof(*loaded_sources))
        || (uint64_t)source_count > ((uint64_t)SIZE_MAX / sizeof(*sources)))
    {
        fprintf(stderr, "reflection_tool: error: too many input files\n");
        return 1;
    }

    loaded_sources = (Reflection_Loaded_Source*)calloc(
        source_count,
        sizeof(*loaded_sources)
    );
    sources = (Reflection_Source*)calloc(source_count, sizeof(*sources));
    if ((loaded_sources == 0) || (sources == 0))
    {
        fprintf(stderr, "reflection_tool: error: out of memory\n");
        goto cleanup;
    }

    for (source_index = 0; source_index < source_count; ++source_index)
    {
        if (!reflection_cli_read_file(
            arguments[first_input_index + (int)source_index],
            &loaded_sources[source_index]
        ))
        {
            goto cleanup;
        }
        sources[source_index] = loaded_sources[source_index].source;
    }

    reflection_output_init(&output);
    result = reflection_generate(
        sources,
        source_count,
        0,
        &output,
        &diagnostic
    );
    if (result != Reflection_Result_Ok)
    {
        reflection_cli_print_diagnostic(&diagnostic);
        goto cleanup;
    }

    result = reflection_write_if_changed_atomic(
        output_path,
        output.data,
        output.size,
        &changed,
        &diagnostic
    );
    if (result != Reflection_Result_Ok)
    {
        reflection_cli_print_diagnostic(&diagnostic);
        goto cleanup;
    }

    fprintf(
        stdout,
        "reflection_tool: %s %s\n",
        changed ? "generated" : "unchanged",
        output_path
    );
    exit_code = 0;

cleanup:
    reflection_output_release(&output);
    if (loaded_sources != 0)
    {
        for (source_index = 0; source_index < source_count; ++source_index)
        {
            free(loaded_sources[source_index].owned_text);
        }
    }
    free(sources);
    free(loaded_sources);
    return exit_code;
}
