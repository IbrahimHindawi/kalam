#include "authored_state.h"

#include <float.h>
#include <math.h>
#include <string.h>

typedef struct Json_Parser
{
    const char* data;
    uint64_t size;
    uint64_t position;
    const char* error_message;
    const char* error_path;
} Json_Parser;

typedef struct Parsed_Document
{
    char schema_id[64];
    Authored_Scene scene;
    Authored_Post_Process post_process;
    uint32_t root_fields;
    uint32_t cube_fields;
    uint32_t transform_fields;
} Parsed_Document;

enum
{
    Parsed_Root_Schema = 1u << 0,
    Parsed_Root_Cube = 1u << 1,
    Parsed_Root_Exposure = 1u << 2,
    Parsed_Root_Bloom_Threshold = 1u << 3,
    Parsed_Root_Vignette = 1u << 4,
    Parsed_Root_Tint = 1u << 5,

    Parsed_Cube_Color = 1u << 0,
    Parsed_Cube_Transform = 1u << 1,

    Parsed_Transform_Translation = 1u << 0,
    Parsed_Transform_Rotation = 1u << 1,
    Parsed_Transform_Scale = 1u << 2
};

static String_View
authored_string_view(const char* text)
{
    String_View result;
    result.data = text;
    result.size = text == 0 ? 0 : (uint64_t)strlen(text);
    return result;
}

static Game_Result
authored_fail(
    Game_Result result,
    const char* message,
    const char* path,
    Game_Error* out_error)
{
    if (out_error != 0)
    {
        out_error->message = authored_string_view(message);
        out_error->json_path = authored_string_view(path);
    }
    return result;
}

static void
authored_clear_error(Game_Error* out_error)
{
    if (out_error != 0)
    {
        memset(out_error, 0, sizeof(*out_error));
    }
}

static void
json_skip_whitespace(Json_Parser* parser)
{
    while (parser->position < parser->size)
    {
        char value = parser->data[parser->position];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n')
        {
            break;
        }
        parser->position += 1;
    }
}

static uint32_t
json_fail(Json_Parser* parser, const char* message, const char* path)
{
    if (parser->error_message == 0)
    {
        parser->error_message = message;
        parser->error_path = path;
    }
    return 0;
}

static uint32_t
json_consume(Json_Parser* parser, char expected, const char* path)
{
    json_skip_whitespace(parser);
    if (parser->position >= parser->size ||
        parser->data[parser->position] != expected)
    {
        return json_fail(parser, "unexpected JSON token", path);
    }
    parser->position += 1;
    return 1;
}

static int32_t
json_hex_digit(char value)
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }
    return -1;
}

static uint32_t
json_parse_hex4(Json_Parser* parser, uint32_t* out_value, const char* path)
{
    uint32_t value = 0;
    uint32_t index = 0;

    if (parser->size - parser->position < 4)
    {
        return json_fail(parser, "incomplete Unicode escape", path);
    }

    for (index = 0; index < 4; index += 1)
    {
        int32_t digit = json_hex_digit(parser->data[parser->position + index]);
        if (digit < 0)
        {
            return json_fail(parser, "invalid Unicode escape", path);
        }
        value = value * 16u + (uint32_t)digit;
    }

    parser->position += 4;
    *out_value = value;
    return 1;
}

static uint32_t
json_append_utf8(
    uint32_t codepoint,
    char* output,
    uint32_t output_capacity,
    uint32_t* in_out_size,
    Json_Parser* parser,
    const char* path)
{
    uint32_t needed = 0;
    uint32_t size = *in_out_size;

    if (codepoint <= 0x7fu)
    {
        needed = 1;
    }
    else if (codepoint <= 0x7ffu)
    {
        needed = 2;
    }
    else if (codepoint <= 0xffffu)
    {
        needed = 3;
    }
    else if (codepoint <= 0x10ffffu)
    {
        needed = 4;
    }
    else
    {
        return json_fail(parser, "invalid Unicode codepoint", path);
    }

    if (size > output_capacity || needed > output_capacity - size)
    {
        return json_fail(parser, "JSON string is too long", path);
    }

    if (needed == 1)
    {
        output[size] = (char)codepoint;
    }
    else if (needed == 2)
    {
        output[size] = (char)(0xc0u | (codepoint >> 6));
        output[size + 1] = (char)(0x80u | (codepoint & 0x3fu));
    }
    else if (needed == 3)
    {
        output[size] = (char)(0xe0u | (codepoint >> 12));
        output[size + 1] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        output[size + 2] = (char)(0x80u | (codepoint & 0x3fu));
    }
    else
    {
        output[size] = (char)(0xf0u | (codepoint >> 18));
        output[size + 1] = (char)(0x80u | ((codepoint >> 12) & 0x3fu));
        output[size + 2] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        output[size + 3] = (char)(0x80u | (codepoint & 0x3fu));
    }

    *in_out_size = size + needed;
    return 1;
}

static uint32_t
json_parse_string(
    Json_Parser* parser,
    char* output,
    uint32_t output_capacity,
    const char* path)
{
    uint32_t output_size = 0;

    if (output == 0 || output_capacity == 0 ||
        !json_consume(parser, '"', path))
    {
        return 0;
    }

    while (parser->position < parser->size)
    {
        unsigned char value = (unsigned char)parser->data[parser->position++];
        if (value == '"')
        {
            if (output_size >= output_capacity)
            {
                return json_fail(parser, "JSON string is too long", path);
            }
            output[output_size] = '\0';
            return 1;
        }

        if (value < 0x20u)
        {
            return json_fail(parser, "control byte in JSON string", path);
        }

        if (value != '\\')
        {
            if (output_size + 1 >= output_capacity)
            {
                return json_fail(parser, "JSON string is too long", path);
            }
            output[output_size++] = (char)value;
            continue;
        }

        if (parser->position >= parser->size)
        {
            return json_fail(parser, "incomplete JSON escape", path);
        }

        value = (unsigned char)parser->data[parser->position++];
        if (value == '"' || value == '\\' || value == '/')
        {
            if (output_size + 1 >= output_capacity)
            {
                return json_fail(parser, "JSON string is too long", path);
            }
            output[output_size++] = (char)value;
        }
        else if (value == 'b' || value == 'f' || value == 'n' ||
                 value == 'r' || value == 't')
        {
            static const char escaped_values[] = {
                '\b', '\f', '\n', '\r', '\t'
            };
            uint32_t escape_index =
                value == 'b' ? 0u :
                value == 'f' ? 1u :
                value == 'n' ? 2u :
                value == 'r' ? 3u : 4u;

            if (output_size + 1 >= output_capacity)
            {
                return json_fail(parser, "JSON string is too long", path);
            }
            output[output_size++] = escaped_values[escape_index];
        }
        else if (value == 'u')
        {
            uint32_t first = 0;
            uint32_t codepoint = 0;

            if (!json_parse_hex4(parser, &first, path))
            {
                return 0;
            }

            codepoint = first;
            if (first >= 0xd800u && first <= 0xdbffu)
            {
                uint32_t second = 0;
                if (parser->size - parser->position < 6 ||
                    parser->data[parser->position] != '\\' ||
                    parser->data[parser->position + 1] != 'u')
                {
                    return json_fail(parser, "missing low surrogate", path);
                }
                parser->position += 2;
                if (!json_parse_hex4(parser, &second, path) ||
                    second < 0xdc00u || second > 0xdfffu)
                {
                    return json_fail(parser, "invalid low surrogate", path);
                }
                codepoint =
                    0x10000u +
                    ((first - 0xd800u) << 10) +
                    (second - 0xdc00u);
            }
            else if (first >= 0xdc00u && first <= 0xdfffu)
            {
                return json_fail(parser, "unexpected low surrogate", path);
            }

            if (!json_append_utf8(
                    codepoint,
                    output,
                    output_capacity - 1,
                    &output_size,
                    parser,
                    path))
            {
                return 0;
            }
        }
        else
        {
            return json_fail(parser, "invalid JSON escape", path);
        }
    }

    return json_fail(parser, "unterminated JSON string", path);
}

static uint32_t
json_parse_number(Json_Parser* parser, double* out_value, const char* path)
{
    uint64_t position = 0;
    uint32_t negative = 0;
    double value = 0.0;
    double fraction_scale = 0.1;
    int32_t exponent = 0;
    uint32_t exponent_negative = 0;

    json_skip_whitespace(parser);
    position = parser->position;

    if (position < parser->size && parser->data[position] == '-')
    {
        negative = 1;
        position += 1;
    }

    if (position >= parser->size)
    {
        return json_fail(parser, "expected JSON number", path);
    }

    if (parser->data[position] == '0')
    {
        position += 1;
        if (position < parser->size &&
            parser->data[position] >= '0' &&
            parser->data[position] <= '9')
        {
            return json_fail(parser, "leading zero in JSON number", path);
        }
    }
    else if (parser->data[position] >= '1' &&
             parser->data[position] <= '9')
    {
        while (position < parser->size &&
               parser->data[position] >= '0' &&
               parser->data[position] <= '9')
        {
            value =
                value * 10.0 +
                (double)(parser->data[position] - '0');
            position += 1;
        }
    }
    else
    {
        return json_fail(parser, "expected JSON number", path);
    }

    if (position < parser->size && parser->data[position] == '.')
    {
        position += 1;
        if (position >= parser->size ||
            parser->data[position] < '0' ||
            parser->data[position] > '9')
        {
            return json_fail(parser, "invalid JSON fraction", path);
        }

        while (position < parser->size &&
               parser->data[position] >= '0' &&
               parser->data[position] <= '9')
        {
            value +=
                (double)(parser->data[position] - '0') *
                fraction_scale;
            fraction_scale *= 0.1;
            position += 1;
        }
    }

    if (position < parser->size &&
        (parser->data[position] == 'e' ||
         parser->data[position] == 'E'))
    {
        position += 1;
        if (position < parser->size &&
            (parser->data[position] == '+' ||
             parser->data[position] == '-'))
        {
            exponent_negative = parser->data[position] == '-' ? 1u : 0u;
            position += 1;
        }

        if (position >= parser->size ||
            parser->data[position] < '0' ||
            parser->data[position] > '9')
        {
            return json_fail(parser, "invalid JSON exponent", path);
        }

        while (position < parser->size &&
               parser->data[position] >= '0' &&
               parser->data[position] <= '9')
        {
            if (exponent < 10000)
            {
                exponent =
                    exponent * 10 +
                    (int32_t)(parser->data[position] - '0');
            }
            position += 1;
        }
    }

    if (exponent_negative != 0)
    {
        exponent = -exponent;
    }

    if (exponent != 0)
    {
        value *= pow(10.0, (double)exponent);
    }
    if (negative != 0)
    {
        value = -value;
    }

    if (!isfinite(value))
    {
        return json_fail(parser, "JSON number is out of range", path);
    }

    parser->position = position;
    *out_value = value;
    return 1;
}

static uint32_t
json_parse_float(
    Json_Parser* parser,
    float minimum,
    float maximum,
    float* out_value,
    const char* path)
{
    double value = 0.0;
    if (!json_parse_number(parser, &value, path))
    {
        return 0;
    }
    if (value < (double)minimum || value > (double)maximum ||
        value < -(double)FLT_MAX || value > (double)FLT_MAX)
    {
        return json_fail(parser, "authored value is out of range", path);
    }
    *out_value = (float)value;
    return 1;
}

static uint32_t
json_parse_float_array(
    Json_Parser* parser,
    float* values,
    uint32_t value_count,
    float minimum,
    float maximum,
    const char* path)
{
    uint32_t index = 0;

    if (!json_consume(parser, '[', path))
    {
        return 0;
    }

    for (index = 0; index < value_count; index += 1)
    {
        if (index != 0 && !json_consume(parser, ',', path))
        {
            return 0;
        }
        if (!json_parse_float(
                parser,
                minimum,
                maximum,
                &values[index],
                path))
        {
            return 0;
        }
    }

    return json_consume(parser, ']', path);
}

static uint32_t
json_set_once(
    Json_Parser* parser,
    uint32_t* fields,
    uint32_t field,
    const char* path)
{
    if ((*fields & field) != 0)
    {
        return json_fail(parser, "duplicate JSON object key", path);
    }
    *fields |= field;
    return 1;
}

static uint32_t
json_parse_transform(Json_Parser* parser, Parsed_Document* document)
{
    uint32_t first = 1;

    if (!json_consume(parser, '{', "$.cube.transform"))
    {
        return 0;
    }

    for (;;)
    {
        char key[64];
        json_skip_whitespace(parser);
        if (parser->position < parser->size &&
            parser->data[parser->position] == '}')
        {
            parser->position += 1;
            break;
        }

        if (!first && !json_consume(parser, ',', "$.cube.transform"))
        {
            return 0;
        }
        first = 0;

        if (!json_parse_string(
                parser,
                key,
                (uint32_t)sizeof(key),
                "$.cube.transform") ||
            !json_consume(parser, ':', "$.cube.transform"))
        {
            return 0;
        }

        if (strcmp(key, "translation") == 0)
        {
            if (!json_set_once(
                    parser,
                    &document->transform_fields,
                    Parsed_Transform_Translation,
                    "$.cube.transform.translation") ||
                !json_parse_float_array(
                    parser,
                    document->scene.cube_transform.translation,
                    3,
                    -10000.0f,
                    10000.0f,
                    "$.cube.transform.translation"))
            {
                return 0;
            }
        }
        else if (strcmp(key, "rotation_degrees") == 0)
        {
            if (!json_set_once(
                    parser,
                    &document->transform_fields,
                    Parsed_Transform_Rotation,
                    "$.cube.transform.rotation_degrees") ||
                !json_parse_float_array(
                    parser,
                    document->scene.cube_transform.rotation_degrees,
                    3,
                    -360000.0f,
                    360000.0f,
                    "$.cube.transform.rotation_degrees"))
            {
                return 0;
            }
        }
        else if (strcmp(key, "scale") == 0)
        {
            if (!json_set_once(
                    parser,
                    &document->transform_fields,
                    Parsed_Transform_Scale,
                    "$.cube.transform.scale") ||
                !json_parse_float_array(
                    parser,
                    document->scene.cube_transform.scale,
                    3,
                    0.001f,
                    1000.0f,
                    "$.cube.transform.scale"))
            {
                return 0;
            }
        }
        else
        {
            return json_fail(
                parser,
                "unknown transform field",
                "$.cube.transform");
        }
    }

    if (document->transform_fields !=
        (Parsed_Transform_Translation |
         Parsed_Transform_Rotation |
         Parsed_Transform_Scale))
    {
        return json_fail(
            parser,
            "transform requires translation, rotation_degrees, and scale",
            "$.cube.transform");
    }
    return 1;
}

static uint32_t
json_parse_cube(Json_Parser* parser, Parsed_Document* document)
{
    uint32_t first = 1;

    if (!json_consume(parser, '{', "$.cube"))
    {
        return 0;
    }

    for (;;)
    {
        char key[64];
        json_skip_whitespace(parser);
        if (parser->position < parser->size &&
            parser->data[parser->position] == '}')
        {
            parser->position += 1;
            break;
        }

        if (!first && !json_consume(parser, ',', "$.cube"))
        {
            return 0;
        }
        first = 0;

        if (!json_parse_string(
                parser,
                key,
                (uint32_t)sizeof(key),
                "$.cube") ||
            !json_consume(parser, ':', "$.cube"))
        {
            return 0;
        }

        if (strcmp(key, "color") == 0)
        {
            if (!json_set_once(
                    parser,
                    &document->cube_fields,
                    Parsed_Cube_Color,
                    "$.cube.color") ||
                !json_parse_float_array(
                    parser,
                    document->scene.cube_color,
                    4,
                    0.0f,
                    1.0f,
                    "$.cube.color"))
            {
                return 0;
            }
        }
        else if (strcmp(key, "transform") == 0)
        {
            if (!json_set_once(
                    parser,
                    &document->cube_fields,
                    Parsed_Cube_Transform,
                    "$.cube.transform") ||
                !json_parse_transform(parser, document))
            {
                return 0;
            }
        }
        else
        {
            return json_fail(parser, "unknown cube field", "$.cube");
        }
    }

    if (document->cube_fields !=
        (Parsed_Cube_Color | Parsed_Cube_Transform))
    {
        return json_fail(
            parser,
            "cube requires color and transform",
            "$.cube");
    }
    return 1;
}

static uint32_t
json_parse_root(Json_Parser* parser, Parsed_Document* document)
{
    uint32_t first = 1;

    if (!json_consume(parser, '{', "$"))
    {
        return 0;
    }

    for (;;)
    {
        char key[64];
        json_skip_whitespace(parser);
        if (parser->position < parser->size &&
            parser->data[parser->position] == '}')
        {
            parser->position += 1;
            break;
        }

        if (!first && !json_consume(parser, ',', "$"))
        {
            return 0;
        }
        first = 0;

        if (!json_parse_string(
                parser,
                key,
                (uint32_t)sizeof(key),
                "$") ||
            !json_consume(parser, ':', "$"))
        {
            return 0;
        }

        if (strcmp(key, "$schema") == 0)
        {
            if (!json_set_once(
                    parser,
                    &document->root_fields,
                    Parsed_Root_Schema,
                    "$.$schema") ||
                !json_parse_string(
                    parser,
                    document->schema_id,
                    (uint32_t)sizeof(document->schema_id),
                    "$.$schema"))
            {
                return 0;
            }
        }
        else if (strcmp(key, "cube") == 0)
        {
            if (!json_set_once(
                    parser,
                    &document->root_fields,
                    Parsed_Root_Cube,
                    "$.cube") ||
                !json_parse_cube(parser, document))
            {
                return 0;
            }
        }
        else if (strcmp(key, "exposure") == 0)
        {
            if (!json_set_once(
                    parser,
                    &document->root_fields,
                    Parsed_Root_Exposure,
                    "$.exposure") ||
                !json_parse_float(
                    parser,
                    0.0f,
                    16.0f,
                    &document->post_process.exposure,
                    "$.exposure"))
            {
                return 0;
            }
        }
        else if (strcmp(key, "bloom_threshold") == 0)
        {
            if (!json_set_once(
                    parser,
                    &document->root_fields,
                    Parsed_Root_Bloom_Threshold,
                    "$.bloom_threshold") ||
                !json_parse_float(
                    parser,
                    0.0f,
                    16.0f,
                    &document->post_process.bloom_threshold,
                    "$.bloom_threshold"))
            {
                return 0;
            }
        }
        else if (strcmp(key, "vignette") == 0)
        {
            if (!json_set_once(
                    parser,
                    &document->root_fields,
                    Parsed_Root_Vignette,
                    "$.vignette") ||
                !json_parse_float(
                    parser,
                    0.0f,
                    1.0f,
                    &document->post_process.vignette,
                    "$.vignette"))
            {
                return 0;
            }
        }
        else if (strcmp(key, "tint") == 0)
        {
            if (!json_set_once(
                    parser,
                    &document->root_fields,
                    Parsed_Root_Tint,
                    "$.tint") ||
                !json_parse_float_array(
                    parser,
                    document->post_process.tint,
                    4,
                    0.0f,
                    1.0f,
                    "$.tint"))
            {
                return 0;
            }
        }
        else
        {
            return json_fail(parser, "unknown document field", "$");
        }
    }

    json_skip_whitespace(parser);
    if (parser->position != parser->size)
    {
        return json_fail(parser, "trailing data after JSON document", "$");
    }
    if ((document->root_fields & Parsed_Root_Schema) == 0)
    {
        return json_fail(parser, "document requires $schema", "$.$schema");
    }
    return 1;
}

static Game_Result
authored_parse_document(
    const Author_Document_State* source,
    Authored_Document* out_document,
    Game_Error* out_error)
{
    Json_Parser parser;
    Parsed_Document parsed;
    uint32_t expected_scene_fields =
        Parsed_Root_Schema | Parsed_Root_Cube;
    uint32_t expected_post_fields =
        Parsed_Root_Schema |
        Parsed_Root_Exposure |
        Parsed_Root_Bloom_Threshold |
        Parsed_Root_Vignette |
        Parsed_Root_Tint;

    memset(&parser, 0, sizeof(parser));
    memset(&parsed, 0, sizeof(parsed));
    memset(out_document, 0, sizeof(*out_document));
    parser.data = source->json_utf8.data;
    parser.size = source->json_utf8.size;

    if (!json_parse_root(&parser, &parsed))
    {
        return authored_fail(
            Game_Result_Invalid_Document,
            parser.error_message != 0
                ? parser.error_message
                : "invalid authored JSON",
            parser.error_path != 0 ? parser.error_path : "$",
            out_error);
    }

    out_document->document_id = source->document_id;
    out_document->root_object_id = source->root_object_id;
    out_document->revision = source->revision;
    out_document->active = 1;

    if (strcmp(parsed.schema_id, "nveditor.scene/v1") == 0)
    {
        if (parsed.root_fields != expected_scene_fields)
        {
            return authored_fail(
                Game_Result_Invalid_Document,
                "scene document has missing or unrelated fields",
                "$",
                out_error);
        }
        out_document->kind = Authored_Document_Scene;
        out_document->value.scene = parsed.scene;
    }
    else if (strcmp(parsed.schema_id, "nveditor.post_process/v1") == 0)
    {
        if (parsed.root_fields != expected_post_fields)
        {
            return authored_fail(
                Game_Result_Invalid_Document,
                "post-process document has missing or unrelated fields",
                "$",
                out_error);
        }
        out_document->kind = Authored_Document_Post_Process;
        out_document->value.post_process = parsed.post_process;
    }
    else
    {
        return authored_fail(
            Game_Result_Unsupported_Schema,
            "unsupported author schema",
            "$.$schema",
            out_error);
    }

    return Game_Result_Ok;
}

static uint32_t
authored_view_equal_literal(String_View view, const char* literal)
{
    uint64_t literal_size = literal == 0 ? 0 : (uint64_t)strlen(literal);
    return view.size == literal_size &&
        (literal_size == 0 ||
         (view.data != 0 &&
          memcmp(view.data, literal, (size_t)literal_size) == 0));
}

static Game_Result
authored_validate_document_boundary(
    const Author_Document_State* document,
    Game_Error* out_error)
{
    uint32_t index = 0;
    uint32_t root_count = 0;

    if (document == 0 ||
        document->struct_size < sizeof(*document) ||
        document->document_id == 0 ||
        document->root_object_id == 0 ||
        document->revision == 0 ||
        document->schema_id.data == 0 ||
        document->schema_id.size == 0 ||
        document->project_relative_path.data == 0 ||
        document->project_relative_path.size == 0 ||
        document->json_utf8.data == 0 ||
        document->json_utf8.size == 0 ||
        document->objects == 0 ||
        document->object_count == 0)
    {
        return authored_fail(
            Game_Result_Invalid_Argument,
            "invalid author document boundary",
            "",
            out_error);
    }

    for (index = 0; index < document->object_count; index += 1)
    {
        const Author_Object_Source* object = &document->objects[index];
        uint32_t prior = 0;

        if (object->object_id == 0 ||
            object->json_path.data == 0 ||
            object->json_path.size == 0 ||
            (object->source_id.size != 0 &&
             object->source_id.data == 0))
        {
            return authored_fail(
                Game_Result_Invalid_Document,
                "invalid authored object source",
                "$",
                out_error);
        }

        for (prior = 0; prior < index; prior += 1)
        {
            const Author_Object_Source* earlier = &document->objects[prior];
            if (earlier->object_id == object->object_id)
            {
                return authored_fail(
                    Game_Result_Invalid_Document,
                    "duplicate authored object ID",
                    "$",
                    out_error);
            }
            if (object->source_id.size != 0 &&
                earlier->source_id.size == object->source_id.size &&
                memcmp(
                    earlier->source_id.data,
                    object->source_id.data,
                    (size_t)object->source_id.size) == 0)
            {
                return authored_fail(
                    Game_Result_Invalid_Document,
                    "duplicate authored source ID",
                    "$",
                    out_error);
            }
        }

        if (object->object_id == document->root_object_id)
        {
            if (!authored_view_equal_literal(object->json_path, "$") ||
                object->source_id.size != 0)
            {
                return authored_fail(
                    Game_Result_Invalid_Document,
                    "root object must use path $ and an empty source ID",
                    "$",
                    out_error);
            }
            root_count += 1;
        }
    }

    if (root_count != 1)
    {
        return authored_fail(
            Game_Result_Invalid_Document,
            "document must contain exactly one root object",
            "$",
            out_error);
    }
    return Game_Result_Ok;
}

static Authored_Document*
authored_find_document(
    Authored_State* state,
    Author_Document_Id document_id)
{
    uint32_t index = 0;
    for (index = 0; index < Authored_Document_Capacity; index += 1)
    {
        if (state->documents[index].active != 0 &&
            state->documents[index].document_id == document_id)
        {
            return &state->documents[index];
        }
    }
    return 0;
}

static const Authored_Document*
authored_find_document_const(
    const Authored_State* state,
    Author_Document_Id document_id)
{
    return authored_find_document((Authored_State*)state, document_id);
}

static Authored_Document*
authored_find_free_document(Authored_State* state)
{
    uint32_t index = 0;
    for (index = 0; index < Authored_Document_Capacity; index += 1)
    {
        if (state->documents[index].active == 0)
        {
            return &state->documents[index];
        }
    }
    return 0;
}

static void
authored_transform_to_value(
    const Authored_Transform* transform,
    Author_Value* out_value)
{
    out_value->kind = Author_Field_Transform;
    memcpy(
        out_value->data.transform.translation,
        transform->translation,
        sizeof(transform->translation));
    memcpy(
        out_value->data.transform.rotation_degrees,
        transform->rotation_degrees,
        sizeof(transform->rotation_degrees));
    memcpy(
        out_value->data.transform.scale,
        transform->scale,
        sizeof(transform->scale));
}

void
authored_state_init(Authored_State* state)
{
    if (state != 0)
    {
        memset(state, 0, sizeof(*state));
    }
}

Game_Result
authored_state_apply_document(
    Authored_State* state,
    const Author_Document_State* document,
    Game_Error* out_error)
{
    Authored_Document candidate;
    Authored_Document* destination = 0;
    Game_Result result = Game_Result_Ok;

    authored_clear_error(out_error);
    if (state == 0)
    {
        return authored_fail(
            Game_Result_Invalid_Argument,
            "authored state is null",
            "",
            out_error);
    }

    result = authored_validate_document_boundary(document, out_error);
    if (result != Game_Result_Ok)
    {
        return result;
    }

    destination = authored_find_document(state, document->document_id);
    if (destination != 0 && document->revision <= destination->revision)
    {
        return authored_fail(
            Game_Result_Invalid_Document,
            "document revision did not advance",
            "$",
            out_error);
    }

    result = authored_parse_document(document, &candidate, out_error);
    if (result != Game_Result_Ok)
    {
        return result;
    }

    if (!authored_view_equal_literal(
            document->schema_id,
            candidate.kind == Authored_Document_Scene
                ? "nveditor.scene/v1"
                : "nveditor.post_process/v1"))
    {
        return authored_fail(
            Game_Result_Invalid_Document,
            "snapshot schema_id does not match JSON $schema",
            "$.$schema",
            out_error);
    }

    if (document->object_count != 1)
    {
        return authored_fail(
            Game_Result_Invalid_Document,
            "Revision 1 documents contain only the root authored object",
            "$",
            out_error);
    }

    candidate.primary_object_id = candidate.root_object_id;

    if (destination == 0)
    {
        destination = authored_find_free_document(state);
        if (destination == 0)
        {
            return authored_fail(
                Game_Result_Internal_Error,
                "authored document capacity exhausted",
                "",
                out_error);
        }
        state->document_count += 1;
    }

    *destination = candidate;
    return Game_Result_Ok;
}

Game_Result
authored_state_remove_document(
    Authored_State* state,
    Author_Document_Id document_id,
    Game_Error* out_error)
{
    Authored_Document* document = 0;

    authored_clear_error(out_error);
    if (state == 0 || document_id == 0)
    {
        return authored_fail(
            Game_Result_Invalid_Argument,
            "invalid document removal request",
            "",
            out_error);
    }

    document = authored_find_document(state, document_id);
    if (document != 0)
    {
        memset(document, 0, sizeof(*document));
        state->document_count -= 1;
    }
    return Game_Result_Ok;
}

Game_Result
authored_state_read_field(
    const Authored_State* state,
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    Author_Field_Id field_id,
    Author_Value* out_value,
    Game_Error* out_error)
{
    const Authored_Document* document = 0;

    authored_clear_error(out_error);
    if (state == 0 || document_id == 0 || object_id == 0 ||
        field_id == 0 || out_value == 0)
    {
        return authored_fail(
            Game_Result_Invalid_Argument,
            "invalid authored field read",
            "",
            out_error);
    }

    memset(out_value, 0, sizeof(*out_value));
    document = authored_find_document_const(state, document_id);
    if (document == 0 ||
        (document->root_object_id != object_id &&
         document->primary_object_id != object_id))
    {
        return authored_fail(
            Game_Result_Invalid_Document,
            "authored document or object was not found",
            "$",
            out_error);
    }

    if (document->kind == Authored_Document_Scene)
    {
        const Authored_Scene* scene = &document->value.scene;
        if (field_id == Author_Field_Id_Scene_Cube_Color)
        {
            out_value->kind = Author_Field_Color4;
            memcpy(
                out_value->data.f32,
                scene->cube_color,
                sizeof(scene->cube_color));
            return Game_Result_Ok;
        }
        if (field_id == Author_Field_Id_Scene_Translation)
        {
            out_value->kind = Author_Field_Vec3;
            memcpy(
                out_value->data.f32,
                scene->cube_transform.translation,
                sizeof(scene->cube_transform.translation));
            return Game_Result_Ok;
        }
        if (field_id == Author_Field_Id_Scene_Rotation_Degrees)
        {
            out_value->kind = Author_Field_Euler_Degrees3;
            memcpy(
                out_value->data.f32,
                scene->cube_transform.rotation_degrees,
                sizeof(scene->cube_transform.rotation_degrees));
            return Game_Result_Ok;
        }
        if (field_id == Author_Field_Id_Scene_Scale)
        {
            out_value->kind = Author_Field_Vec3;
            memcpy(
                out_value->data.f32,
                scene->cube_transform.scale,
                sizeof(scene->cube_transform.scale));
            return Game_Result_Ok;
        }
        if (field_id == Author_Field_Id_Scene_Transform)
        {
            authored_transform_to_value(
                &scene->cube_transform,
                out_value);
            return Game_Result_Ok;
        }
    }
    else if (document->kind == Authored_Document_Post_Process)
    {
        const Authored_Post_Process* post = &document->value.post_process;
        if (field_id == Author_Field_Id_Post_Exposure)
        {
            out_value->kind = Author_Field_F32;
            out_value->data.f32[0] = post->exposure;
            return Game_Result_Ok;
        }
        if (field_id == Author_Field_Id_Post_Bloom_Threshold)
        {
            out_value->kind = Author_Field_F32;
            out_value->data.f32[0] = post->bloom_threshold;
            return Game_Result_Ok;
        }
        if (field_id == Author_Field_Id_Post_Vignette)
        {
            out_value->kind = Author_Field_F32;
            out_value->data.f32[0] = post->vignette;
            return Game_Result_Ok;
        }
        if (field_id == Author_Field_Id_Post_Tint)
        {
            out_value->kind = Author_Field_Color4;
            memcpy(out_value->data.f32, post->tint, sizeof(post->tint));
            return Game_Result_Ok;
        }
    }

    return authored_fail(
        Game_Result_Invalid_Argument,
        "authored field ID is not valid for this schema",
        "",
        out_error);
}

static uint32_t
authored_values_in_range(
    const float* values,
    uint32_t count,
    float minimum,
    float maximum)
{
    uint32_t index = 0;
    for (index = 0; index < count; index += 1)
    {
        if (!isfinite(values[index]) ||
            values[index] < minimum ||
            values[index] > maximum)
        {
            return 0;
        }
    }
    return 1;
}

Game_Result
authored_state_preview_field(
    Authored_State* state,
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    Author_Field_Id field_id,
    const Author_Value* value,
    Game_Error* out_error)
{
    Authored_Document* document = 0;

    authored_clear_error(out_error);
    if (state == 0 || value == 0)
    {
        return authored_fail(
            Game_Result_Invalid_Argument,
            "invalid authored field preview",
            "",
            out_error);
    }

    document = authored_find_document(state, document_id);
    if (document == 0 ||
        (document->root_object_id != object_id &&
         document->primary_object_id != object_id))
    {
        return authored_fail(
            Game_Result_Invalid_Document,
            "authored document or object was not found",
            "$",
            out_error);
    }

    if (document->kind == Authored_Document_Scene)
    {
        Authored_Scene* scene = &document->value.scene;
        if (field_id == Author_Field_Id_Scene_Cube_Color &&
            value->kind == Author_Field_Color4 &&
            authored_values_in_range(value->data.f32, 4, 0.0f, 1.0f))
        {
            memcpy(scene->cube_color, value->data.f32, sizeof(scene->cube_color));
            return Game_Result_Ok;
        }
        if (field_id == Author_Field_Id_Scene_Translation &&
            value->kind == Author_Field_Vec3 &&
            authored_values_in_range(
                value->data.f32,
                3,
                -10000.0f,
                10000.0f))
        {
            memcpy(
                scene->cube_transform.translation,
                value->data.f32,
                sizeof(scene->cube_transform.translation));
            return Game_Result_Ok;
        }
        if (field_id == Author_Field_Id_Scene_Rotation_Degrees &&
            value->kind == Author_Field_Euler_Degrees3 &&
            authored_values_in_range(
                value->data.f32,
                3,
                -360000.0f,
                360000.0f))
        {
            memcpy(
                scene->cube_transform.rotation_degrees,
                value->data.f32,
                sizeof(scene->cube_transform.rotation_degrees));
            return Game_Result_Ok;
        }
        if (field_id == Author_Field_Id_Scene_Scale &&
            value->kind == Author_Field_Vec3 &&
            authored_values_in_range(value->data.f32, 3, 0.001f, 1000.0f))
        {
            memcpy(
                scene->cube_transform.scale,
                value->data.f32,
                sizeof(scene->cube_transform.scale));
            return Game_Result_Ok;
        }
        if (field_id == Author_Field_Id_Scene_Transform &&
            value->kind == Author_Field_Transform &&
            authored_values_in_range(
                value->data.transform.translation,
                3,
                -10000.0f,
                10000.0f) &&
            authored_values_in_range(
                value->data.transform.rotation_degrees,
                3,
                -360000.0f,
                360000.0f) &&
            authored_values_in_range(
                value->data.transform.scale,
                3,
                0.001f,
                1000.0f))
        {
            memcpy(
                &scene->cube_transform,
                &value->data.transform,
                sizeof(scene->cube_transform));
            return Game_Result_Ok;
        }
    }
    else if (document->kind == Authored_Document_Post_Process)
    {
        Authored_Post_Process* post = &document->value.post_process;
        if (field_id == Author_Field_Id_Post_Exposure &&
            value->kind == Author_Field_F32 &&
            authored_values_in_range(value->data.f32, 1, 0.0f, 16.0f))
        {
            post->exposure = value->data.f32[0];
            return Game_Result_Ok;
        }
        if (field_id == Author_Field_Id_Post_Bloom_Threshold &&
            value->kind == Author_Field_F32 &&
            authored_values_in_range(value->data.f32, 1, 0.0f, 16.0f))
        {
            post->bloom_threshold = value->data.f32[0];
            return Game_Result_Ok;
        }
        if (field_id == Author_Field_Id_Post_Vignette &&
            value->kind == Author_Field_F32 &&
            authored_values_in_range(value->data.f32, 1, 0.0f, 1.0f))
        {
            post->vignette = value->data.f32[0];
            return Game_Result_Ok;
        }
        if (field_id == Author_Field_Id_Post_Tint &&
            value->kind == Author_Field_Color4 &&
            authored_values_in_range(value->data.f32, 4, 0.0f, 1.0f))
        {
            memcpy(post->tint, value->data.f32, sizeof(post->tint));
            return Game_Result_Ok;
        }
    }

    return authored_fail(
        Game_Result_Invalid_Argument,
        "preview value kind, range, or field ID is invalid",
        "",
        out_error);
}

const Authored_Scene*
authored_state_latest_scene(const Authored_State* state)
{
    const Authored_Scene* result = 0;
    uint64_t revision = 0;
    uint32_t index = 0;

    if (state == 0)
    {
        return 0;
    }

    for (index = 0; index < Authored_Document_Capacity; index += 1)
    {
        const Authored_Document* document = &state->documents[index];
        if (document->active != 0 &&
            document->kind == Authored_Document_Scene &&
            document->revision >= revision)
        {
            revision = document->revision;
            result = &document->value.scene;
        }
    }
    return result;
}

const Authored_Post_Process*
authored_state_latest_post_process(const Authored_State* state)
{
    const Authored_Post_Process* result = 0;
    uint64_t revision = 0;
    uint32_t index = 0;

    if (state == 0)
    {
        return 0;
    }

    for (index = 0; index < Authored_Document_Capacity; index += 1)
    {
        const Authored_Document* document = &state->documents[index];
        if (document->active != 0 &&
            document->kind == Authored_Document_Post_Process &&
            document->revision >= revision)
        {
            revision = document->revision;
            result = &document->value.post_process;
        }
    }
    return result;
}
