/*
 * res_manifest_tool: generates compile-time resource tables from
 * res/res.manifest.json (docs/resource_manifest.md sections 2-4).
 *
 * Identity is inferred from paths; authored ids are rejected outright.
 * Output is deterministic (manifest order), written only when the content
 * changes, and C-compatible plain data.
 */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    Res_Max_Entries = 512,
    Res_Max_Path = 512,
    Res_Max_Name = 256
};

typedef struct Res_Shader
{
    char program[Res_Max_Name];
    char source[Res_Max_Path];
    char handle[Res_Max_Name];
    uint64_t id;
} Res_Shader;

typedef struct Res_Texture
{
    char path[Res_Max_Path];
    char handle[Res_Max_Name];
    int srgb;
    uint64_t id;
} Res_Texture;

typedef struct Res_Manifest
{
    Res_Shader shaders[Res_Max_Entries];
    uint32_t shader_count;
    Res_Texture textures[Res_Max_Entries];
    uint32_t texture_count;
} Res_Manifest;

/* ------------------------------------------------------------- hashing */

static uint64_t res_hash64(const char* prefix, const char* text)
{
    /* FNV-1a, the same family as authored field ids. */
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    const char* cursor;
    for (cursor = prefix; *cursor != 0; ++cursor)
    {
        hash ^= (uint64_t)(unsigned char)*cursor;
        hash *= UINT64_C(0x100000001b3);
    }
    for (cursor = text; *cursor != 0; ++cursor)
    {
        hash ^= (uint64_t)(unsigned char)*cursor;
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}

/* ------------------------------------------------------- tiny JSON */

typedef struct Json_Reader
{
    const char* text;
    size_t size;
    size_t position;
    const char* error;
} Json_Reader;

static void json_skip_space(Json_Reader* reader)
{
    while (reader->position < reader->size)
    {
        const char c = reader->text[reader->position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        {
            break;
        }
        ++reader->position;
    }
}

static int json_peek(Json_Reader* reader)
{
    json_skip_space(reader);
    if (reader->position >= reader->size)
    {
        return -1;
    }
    return (unsigned char)reader->text[reader->position];
}

static int json_consume(Json_Reader* reader, char expected)
{
    if (json_peek(reader) != (unsigned char)expected)
    {
        reader->error = "unexpected character";
        return 0;
    }
    ++reader->position;
    return 1;
}

static int json_read_string(
    Json_Reader* reader,
    char* out,
    size_t out_size)
{
    size_t length = 0;
    if (!json_consume(reader, '"'))
    {
        return 0;
    }
    while (reader->position < reader->size)
    {
        const char c = reader->text[reader->position++];
        if (c == '"')
        {
            out[length] = 0;
            return 1;
        }
        if (c == '\\')
        {
            if (reader->position >= reader->size)
            {
                break;
            }
            const char escaped = reader->text[reader->position++];
            char resolved;
            switch (escaped)
            {
            case '"': resolved = '"'; break;
            case '\\': resolved = '\\'; break;
            case '/': resolved = '/'; break;
            case 'n': resolved = '\n'; break;
            case 't': resolved = '\t'; break;
            case 'r': resolved = '\r'; break;
            default:
                reader->error = "unsupported string escape";
                return 0;
            }
            if (length + 1 >= out_size)
            {
                reader->error = "string is too long";
                return 0;
            }
            out[length++] = resolved;
            continue;
        }
        if (length + 1 >= out_size)
        {
            reader->error = "string is too long";
            return 0;
        }
        out[length++] = c;
    }
    reader->error = "unterminated string";
    return 0;
}

static int json_skip_value(Json_Reader* reader);

static int json_skip_container(Json_Reader* reader, char open, char close)
{
    if (!json_consume(reader, open))
    {
        return 0;
    }
    if (json_peek(reader) == (unsigned char)close)
    {
        ++reader->position;
        return 1;
    }
    for (;;)
    {
        if (open == '{')
        {
            char key[Res_Max_Name];
            if (!json_read_string(reader, key, sizeof(key)) ||
                !json_consume(reader, ':'))
            {
                return 0;
            }
        }
        if (!json_skip_value(reader))
        {
            return 0;
        }
        const int next = json_peek(reader);
        if (next == ',')
        {
            ++reader->position;
            continue;
        }
        if (next == (unsigned char)close)
        {
            ++reader->position;
            return 1;
        }
        reader->error = "malformed container";
        return 0;
    }
}

static int json_skip_value(Json_Reader* reader)
{
    const int next = json_peek(reader);
    if (next == '"')
    {
        char scratch[Res_Max_Path];
        return json_read_string(reader, scratch, sizeof(scratch));
    }
    if (next == '{')
    {
        return json_skip_container(reader, '{', '}');
    }
    if (next == '[')
    {
        return json_skip_container(reader, '[', ']');
    }
    while (reader->position < reader->size)
    {
        const char c = reader->text[reader->position];
        if (c == ',' || c == '}' || c == ']' ||
            c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            return 1;
        }
        ++reader->position;
    }
    return 1;
}

static int json_read_bool(Json_Reader* reader, int* out_value)
{
    json_skip_space(reader);
    if (reader->position + 4 <= reader->size &&
        memcmp(reader->text + reader->position, "true", 4) == 0)
    {
        reader->position += 4;
        *out_value = 1;
        return 1;
    }
    if (reader->position + 5 <= reader->size &&
        memcmp(reader->text + reader->position, "false", 5) == 0)
    {
        reader->position += 5;
        *out_value = 0;
        return 1;
    }
    reader->error = "expected true or false";
    return 0;
}

/* -------------------------------------------------- handle inference */

static int res_infer_handle(
    const char* prefix,
    const char* path,
    int drop_first_component,
    char* out,
    size_t out_size)
{
    const char* cursor = path;
    size_t length = 0;

    if (drop_first_component)
    {
        const char* slash = strchr(path, '/');
        if (slash != 0 && slash[1] != 0)
        {
            cursor = slash + 1;
        }
    }

    length = strlen(prefix);
    if (length >= out_size)
    {
        return 0;
    }
    memcpy(out, prefix, length);

    for (; *cursor != 0; ++cursor)
    {
        char c = *cursor;
        if (c == '.')
        {
            /* Drop the extension: stop at the last dot. */
            if (strchr(cursor + 1, '.') == 0)
            {
                break;
            }
        }
        if (c >= 'A' && c <= 'Z')
        {
            c = (char)(c - 'A' + 'a');
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
        {
            c = '_';
        }
        if (length + 1 >= out_size)
        {
            return 0;
        }
        out[length++] = c;
    }
    out[length] = 0;
    return length > strlen(prefix);
}

/* ------------------------------------------------------- validation */

static int res_fail(const char* manifest_path, const char* message)
{
    fprintf(stderr, "%s: error: %s\n", manifest_path, message);
    return 0;
}

static int res_check_relative_path(const char* path)
{
    if (path[0] == 0 || path[0] == '/' || strchr(path, '\\') != 0 ||
        strstr(path, "..") != 0 ||
        (path[0] != 0 && path[1] == ':'))
    {
        return 0;
    }
    return 1;
}

static int res_file_exists(const char* base_directory, const char* path)
{
    char full[Res_Max_Path * 2];
    FILE* file;
    if (snprintf(full, sizeof(full), "%s/%s", base_directory, path) >=
        (int)sizeof(full))
    {
        return 0;
    }
    file = fopen(full, "rb");
    if (file == 0)
    {
        return 0;
    }
    fclose(file);
    return 1;
}

static int res_register_handle(
    const char* manifest_path,
    char seen[][Res_Max_Name],
    uint32_t* seen_count,
    const char* handle)
{
    uint32_t index;
    for (index = 0; index < *seen_count; ++index)
    {
        if (strcmp(seen[index], handle) == 0)
        {
            fprintf(
                stderr,
                "%s: error: duplicate resource handle %s\n",
                manifest_path,
                handle);
            return 0;
        }
    }
    if (*seen_count >= Res_Max_Entries * 2)
    {
        return res_fail(manifest_path, "too many resources");
    }
    strcpy(seen[*seen_count], handle);
    ++*seen_count;
    return 1;
}

/* ---------------------------------------------------------- parsing */

typedef struct Res_Object_Field
{
    char key[Res_Max_Name];
    char string_value[Res_Max_Path];
    int bool_value;
    int is_string;
    int is_bool;
} Res_Object_Field;

static int res_parse_entry_object(
    Json_Reader* reader,
    const char* manifest_path,
    Res_Object_Field* fields,
    uint32_t max_fields,
    uint32_t* out_count)
{
    uint32_t count = 0;
    if (!json_consume(reader, '{'))
    {
        return res_fail(manifest_path, "expected an entry object");
    }
    if (json_peek(reader) == '}')
    {
        ++reader->position;
        *out_count = 0;
        return 1;
    }
    for (;;)
    {
        if (count >= max_fields)
        {
            return res_fail(manifest_path, "entry has too many keys");
        }
        Res_Object_Field* field = &fields[count];
        memset(field, 0, sizeof(*field));
        if (!json_read_string(reader, field->key, sizeof(field->key)) ||
            !json_consume(reader, ':'))
        {
            return res_fail(manifest_path, "malformed entry key");
        }
        if (strcmp(field->key, "id") == 0)
        {
            return res_fail(
                manifest_path,
                "authored ids are rejected; identity is inferred from "
                "paths");
        }
        const int next = json_peek(reader);
        if (next == '"')
        {
            if (!json_read_string(
                    reader,
                    field->string_value,
                    sizeof(field->string_value)))
            {
                return res_fail(manifest_path, "malformed entry string");
            }
            field->is_string = 1;
        }
        else if (next == 't' || next == 'f')
        {
            if (!json_read_bool(reader, &field->bool_value))
            {
                return res_fail(manifest_path, "malformed entry boolean");
            }
            field->is_bool = 1;
        }
        else
        {
            return res_fail(
                manifest_path,
                "entry values must be strings or booleans");
        }
        ++count;
        const int after = json_peek(reader);
        if (after == ',')
        {
            ++reader->position;
            continue;
        }
        if (after == '}')
        {
            ++reader->position;
            *out_count = count;
            return 1;
        }
        return res_fail(manifest_path, "malformed entry object");
    }
}

static const Res_Object_Field* res_find_field(
    const Res_Object_Field* fields,
    uint32_t count,
    const char* key)
{
    uint32_t index;
    for (index = 0; index < count; ++index)
    {
        if (strcmp(fields[index].key, key) == 0)
        {
            return &fields[index];
        }
    }
    return 0;
}

static int res_parse_manifest(
    const char* manifest_path,
    const char* base_directory,
    const char* text,
    size_t size,
    Res_Manifest* manifest)
{
    Json_Reader reader = { text, size, 0, 0 };
    char seen_handles[Res_Max_Entries * 2][Res_Max_Name];
    uint32_t seen_count = 0;
    int saw_schema = 0;

    if (!json_consume(&reader, '{'))
    {
        return res_fail(manifest_path, "the manifest must be one object");
    }

    for (;;)
    {
        char key[Res_Max_Name];
        if (json_peek(&reader) == '}')
        {
            ++reader.position;
            break;
        }
        if (!json_read_string(&reader, key, sizeof(key)) ||
            !json_consume(&reader, ':'))
        {
            return res_fail(manifest_path, "malformed manifest key");
        }

        if (strcmp(key, "$schema") == 0)
        {
            char schema[Res_Max_Name];
            if (!json_read_string(&reader, schema, sizeof(schema)))
            {
                return res_fail(manifest_path, "malformed $schema");
            }
            if (strcmp(schema, "nveditor.res_manifest/v1") != 0)
            {
                return res_fail(
                    manifest_path,
                    "unsupported manifest $schema");
            }
            saw_schema = 1;
        }
        else if (strcmp(key, "vault") == 0)
        {
            char vault[Res_Max_Path];
            if (!json_read_string(&reader, vault, sizeof(vault)))
            {
                return res_fail(manifest_path, "malformed vault path");
            }
            /* Machine-local; recorded by staging, ignored here. */
        }
        else if (strcmp(key, "shaders") == 0 ||
                 strcmp(key, "textures") == 0)
        {
            const int is_shader = key[0] == 's';
            if (!json_consume(&reader, '['))
            {
                return res_fail(manifest_path, "expected an entry array");
            }
            if (json_peek(&reader) == ']')
            {
                ++reader.position;
            }
            else
            {
                for (;;)
                {
                    Res_Object_Field fields[8];
                    uint32_t field_count = 0;
                    if (!res_parse_entry_object(
                            &reader,
                            manifest_path,
                            fields,
                            8,
                            &field_count))
                    {
                        return 0;
                    }

                    if (is_shader)
                    {
                        const Res_Object_Field* program =
                            res_find_field(fields, field_count, "program");
                        const Res_Object_Field* source =
                            res_find_field(fields, field_count, "source");
                        if (program == 0 || !program->is_string ||
                            source == 0 || !source->is_string)
                        {
                            return res_fail(
                                manifest_path,
                                "a shader entry needs program and source "
                                "strings");
                        }
                        if (!res_check_relative_path(source->string_value))
                        {
                            return res_fail(
                                manifest_path,
                                "shader source paths are res/-relative "
                                "with forward slashes");
                        }
                        if (!res_file_exists(
                                base_directory,
                                source->string_value))
                        {
                            fprintf(
                                stderr,
                                "%s: error: missing shader source %s\n",
                                manifest_path,
                                source->string_value);
                            return 0;
                        }
                        if (manifest->shader_count >= Res_Max_Entries)
                        {
                            return res_fail(
                                manifest_path,
                                "too many shaders");
                        }
                        Res_Shader* shader =
                            &manifest->shaders[manifest->shader_count];
                        strcpy(shader->program, program->string_value);
                        strcpy(shader->source, source->string_value);
                        if (!res_infer_handle(
                                "res_shader_",
                                shader->program,
                                0,
                                shader->handle,
                                sizeof(shader->handle)))
                        {
                            return res_fail(
                                manifest_path,
                                "could not infer a shader handle");
                        }
                        shader->id =
                            res_hash64("shader:", shader->source);
                        if (!res_register_handle(
                                manifest_path,
                                seen_handles,
                                &seen_count,
                                shader->handle))
                        {
                            return 0;
                        }
                        ++manifest->shader_count;
                    }
                    else
                    {
                        const Res_Object_Field* path =
                            res_find_field(fields, field_count, "path");
                        const Res_Object_Field* srgb =
                            res_find_field(fields, field_count, "srgb");
                        if (path == 0 || !path->is_string)
                        {
                            return res_fail(
                                manifest_path,
                                "a texture entry needs a path string");
                        }
                        if (!res_check_relative_path(path->string_value))
                        {
                            return res_fail(
                                manifest_path,
                                "texture paths are res/-relative with "
                                "forward slashes");
                        }
                        if (!res_file_exists(
                                base_directory,
                                path->string_value))
                        {
                            fprintf(
                                stderr,
                                "%s: error: missing texture %s\n",
                                manifest_path,
                                path->string_value);
                            return 0;
                        }
                        if (manifest->texture_count >= Res_Max_Entries)
                        {
                            return res_fail(
                                manifest_path,
                                "too many textures");
                        }
                        Res_Texture* texture =
                            &manifest->textures[manifest->texture_count];
                        strcpy(texture->path, path->string_value);
                        texture->srgb =
                            srgb != 0 && srgb->is_bool
                                ? srgb->bool_value
                                : 1;
                        if (!res_infer_handle(
                                "res_tex_",
                                texture->path,
                                1,
                                texture->handle,
                                sizeof(texture->handle)))
                        {
                            return res_fail(
                                manifest_path,
                                "could not infer a texture handle");
                        }
                        texture->id = res_hash64("texture:", texture->path);
                        if (!res_register_handle(
                                manifest_path,
                                seen_handles,
                                &seen_count,
                                texture->handle))
                        {
                            return 0;
                        }
                        ++manifest->texture_count;
                    }

                    const int after = json_peek(&reader);
                    if (after == ',')
                    {
                        ++reader.position;
                        continue;
                    }
                    if (after == ']')
                    {
                        ++reader.position;
                        break;
                    }
                    return res_fail(manifest_path, "malformed entry array");
                }
            }
        }
        else
        {
            fprintf(
                stderr,
                "%s: error: unknown manifest key %s\n",
                manifest_path,
                key);
            return 0;
        }

        const int after_value = json_peek(&reader);
        if (after_value == ',')
        {
            ++reader.position;
            continue;
        }
        if (after_value == '}')
        {
            ++reader.position;
            break;
        }
        return res_fail(manifest_path, "malformed manifest object");
    }

    if (reader.error != 0)
    {
        return res_fail(manifest_path, reader.error);
    }
    if (!saw_schema)
    {
        return res_fail(
            manifest_path,
            "the manifest must declare $schema nveditor.res_manifest/v1");
    }
    return 1;
}

/* --------------------------------------------------------- emission */

static void res_emit(FILE* out, const Res_Manifest* manifest)
{
    uint32_t index;

    fprintf(out, "/* generated by res_manifest_tool; do not edit. */\n");
    fprintf(out, "#ifndef KALAM_RES_MANIFEST_GENERATED_H\n");
    fprintf(out, "#define KALAM_RES_MANIFEST_GENERATED_H\n\n");
    fprintf(out, "#include <stdint.h>\n\n");

    fprintf(out, "#define res_shader_xmacro(_) \\\n");
    for (index = 0; index < manifest->shader_count; ++index)
    {
        const Res_Shader* shader = &manifest->shaders[index];
        fprintf(
            out,
            "    _(%s, UINT64_C(0x%016llX), \"res/%s\")%s\n",
            shader->handle,
            (unsigned long long)shader->id,
            shader->source,
            index + 1 < manifest->shader_count ? " \\" : "");
    }
    if (manifest->shader_count == 0)
    {
        fprintf(out, "    /* empty */\n");
    }
    fprintf(out, "\n");

    fprintf(out, "#define res_tex_xmacro(_) \\\n");
    for (index = 0; index < manifest->texture_count; ++index)
    {
        const Res_Texture* texture = &manifest->textures[index];
        fprintf(
            out,
            "    _(%s, UINT64_C(0x%016llX), \"res/%s\", %d)%s\n",
            texture->handle,
            (unsigned long long)texture->id,
            texture->path,
            texture->srgb,
            index + 1 < manifest->texture_count ? " \\" : "");
    }
    if (manifest->texture_count == 0)
    {
        fprintf(out, "    /* empty */\n");
    }
    fprintf(out, "\n");

    fprintf(out, "enum\n{\n");
    for (index = 0; index < manifest->shader_count; ++index)
    {
        fprintf(
            out,
            "    %s = %u,\n",
            manifest->shaders[index].handle,
            index);
    }
    fprintf(out, "    Res_Shader_Count = %u\n};\n\n", manifest->shader_count);

    fprintf(out, "enum\n{\n");
    for (index = 0; index < manifest->texture_count; ++index)
    {
        fprintf(
            out,
            "    %s = %u,\n",
            manifest->textures[index].handle,
            index);
    }
    fprintf(out, "    Res_Tex_Count = %u\n};\n\n", manifest->texture_count);

    fprintf(out, "typedef struct Res_Shader_Entry\n{\n");
    fprintf(out, "    uint64_t id;\n");
    fprintf(out, "    const char* path;\n");
    fprintf(out, "} Res_Shader_Entry;\n\n");
    fprintf(out, "typedef struct Res_Tex_Entry\n{\n");
    fprintf(out, "    uint64_t id;\n");
    fprintf(out, "    const char* path;\n");
    fprintf(out, "    int srgb;\n");
    fprintf(out, "} Res_Tex_Entry;\n\n");

    fprintf(
        out,
        "static const Res_Shader_Entry res_shader_table[] =\n{\n");
    for (index = 0; index < manifest->shader_count; ++index)
    {
        const Res_Shader* shader = &manifest->shaders[index];
        fprintf(
            out,
            "    { UINT64_C(0x%016llX), \"res/%s\" },\n",
            (unsigned long long)shader->id,
            shader->source);
    }
    fprintf(out, "    { 0, 0 }\n};\n\n");

    fprintf(out, "static const Res_Tex_Entry res_tex_table[] =\n{\n");
    for (index = 0; index < manifest->texture_count; ++index)
    {
        const Res_Texture* texture = &manifest->textures[index];
        fprintf(
            out,
            "    { UINT64_C(0x%016llX), \"res/%s\", %d },\n",
            (unsigned long long)texture->id,
            texture->path,
            texture->srgb);
    }
    fprintf(out, "    { 0, 0, 0 }\n};\n\n");

    fprintf(out, "#endif\n");
}

/* --------------------------------------------------------------- io */

static char* res_read_file(const char* path, size_t* out_size)
{
    FILE* file = fopen(path, "rb");
    long size;
    char* text;
    if (file == 0)
    {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return 0;
    }
    text = (char*)malloc((size_t)size + 1);
    if (text == 0)
    {
        fclose(file);
        return 0;
    }
    if (fread(text, 1, (size_t)size, file) != (size_t)size)
    {
        free(text);
        fclose(file);
        return 0;
    }
    fclose(file);
    text[size] = 0;
    *out_size = (size_t)size;
    return text;
}

int main(int argument_count, char** arguments)
{
    const char* manifest_path = 0;
    const char* output_path = 0;
    int index;

    for (index = 1; index < argument_count; ++index)
    {
        if (strcmp(arguments[index], "--manifest") == 0 &&
            index + 1 < argument_count)
        {
            manifest_path = arguments[++index];
        }
        else if (strcmp(arguments[index], "--output") == 0 &&
                 index + 1 < argument_count)
        {
            output_path = arguments[++index];
        }
        else
        {
            fprintf(
                stderr,
                "usage: res_manifest_tool --manifest <res.manifest.json> "
                "--output <generated-header>\n");
            return 2;
        }
    }
    if (manifest_path == 0 || output_path == 0)
    {
        fprintf(
            stderr,
            "usage: res_manifest_tool --manifest <res.manifest.json> "
            "--output <generated-header>\n");
        return 2;
    }

    size_t manifest_size = 0;
    char* manifest_text = res_read_file(manifest_path, &manifest_size);
    if (manifest_text == 0)
    {
        fprintf(
            stderr,
            "%s: error: could not read the manifest\n",
            manifest_path);
        return 1;
    }

    /* Paths are relative to the manifest's directory. */
    char base_directory[Res_Max_Path];
    {
        const char* last_slash = strrchr(manifest_path, '/');
        const char* last_backslash = strrchr(manifest_path, '\\');
        if (last_backslash > last_slash)
        {
            last_slash = last_backslash;
        }
        if (last_slash == 0)
        {
            strcpy(base_directory, ".");
        }
        else
        {
            const size_t length =
                (size_t)(last_slash - manifest_path);
            if (length >= sizeof(base_directory))
            {
                fprintf(
                    stderr,
                    "%s: error: manifest path is too long\n",
                    manifest_path);
                free(manifest_text);
                return 1;
            }
            memcpy(base_directory, manifest_path, length);
            base_directory[length] = 0;
        }
    }

    static Res_Manifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    if (!res_parse_manifest(
            manifest_path,
            base_directory,
            manifest_text,
            manifest_size,
            &manifest))
    {
        free(manifest_text);
        return 1;
    }
    free(manifest_text);

    /* Render to memory, then write only when the content changed. */
    FILE* memory = tmpfile();
    if (memory == 0)
    {
        fprintf(stderr, "error: could not create a scratch file\n");
        return 1;
    }
    res_emit(memory, &manifest);
    const long rendered_size = ftell(memory);
    if (rendered_size < 0)
    {
        fclose(memory);
        return 1;
    }
    char* rendered = (char*)malloc((size_t)rendered_size + 1);
    if (rendered == 0)
    {
        fclose(memory);
        return 1;
    }
    rewind(memory);
    if (fread(rendered, 1, (size_t)rendered_size, memory) !=
        (size_t)rendered_size)
    {
        free(rendered);
        fclose(memory);
        return 1;
    }
    fclose(memory);
    rendered[rendered_size] = 0;

    size_t existing_size = 0;
    char* existing = res_read_file(output_path, &existing_size);
    const int unchanged = existing != 0 &&
        existing_size == (size_t)rendered_size &&
        memcmp(existing, rendered, existing_size) == 0;
    free(existing);

    if (!unchanged)
    {
        FILE* out = fopen(output_path, "wb");
        if (out == 0)
        {
            fprintf(
                stderr,
                "%s: error: could not open the output for writing\n",
                output_path);
            free(rendered);
            return 1;
        }
        if (fwrite(rendered, 1, (size_t)rendered_size, out) !=
            (size_t)rendered_size)
        {
            fclose(out);
            free(rendered);
            return 1;
        }
        if (fclose(out) != 0)
        {
            free(rendered);
            return 1;
        }
    }
    free(rendered);
    return 0;
}
