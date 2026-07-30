#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "reflection_tool.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

typedef struct Reflection_String
{
    char* data;
    uint64_t size;
} Reflection_String;

typedef struct Reflection_Field_Record
{
    Reflection_String stable_key;
    Reflection_String name;
    Reflection_String json_path;
    Reflection_String group;
    Reflection_String ranges[3][4];

    uint64_t id;
    uint32_t id_is_automatic;
    uint32_t kind_index;
    uint32_t semantic_index;
    uint32_t flags;

    const char* source_path;
    uint32_t source_line;
    uint32_t source_column;
} Reflection_Field_Record;

typedef struct Reflection_Schema_Record
{
    Reflection_String symbol;
    Reflection_String schema_id;
    uint32_t schema_version;
    uint32_t flags;

    Reflection_Field_Record* fields;
    uint32_t field_count;
    uint32_t field_capacity;

    const char* source_path;
    uint32_t source_line;
    uint32_t source_column;
} Reflection_Schema_Record;

typedef struct Reflection_Database
{
    Reflection_Schema_Record* schemas;
    uint32_t schema_count;
    uint32_t schema_capacity;
} Reflection_Database;

typedef struct Reflection_Parse_Cursor
{
    const Reflection_Source* source;
    const char* line_start;
    const char* at;
    const char* end;
    uint32_t line;
} Reflection_Parse_Cursor;

typedef struct Reflection_Name_Map
{
    const char* annotation_name;
    const char* emitted_name;
} Reflection_Name_Map;

static const Reflection_Name_Map g_field_kinds[] =
{
    { "F32", "Author_Field_F32" },
    { "I32", "Author_Field_I32" },
    { "Bool", "Author_Field_Bool" },
    { "Vec2", "Author_Field_Vec2" },
    { "Vec3", "Author_Field_Vec3" },
    { "Vec4", "Author_Field_Vec4" },
    { "Color3", "Author_Field_Color3" },
    { "Color4", "Author_Field_Color4" },
    { "Euler_Degrees3", "Author_Field_Euler_Degrees3" },
    { "Transform", "Author_Field_Transform" },
    { "Enum", "Author_Field_Enum" },
    { "String", "Author_Field_String" },
    { "Asset_Reference", "Author_Field_Asset_Reference" }
};

static const Reflection_Name_Map g_field_semantics[] =
{
    { "None", "Author_Semantic_None" },
    { "Color", "Author_Semantic_Color" },
    { "Position", "Author_Semantic_Position" },
    { "Rotation", "Author_Semantic_Rotation" },
    { "Scale", "Author_Semantic_Scale" },
    { "Transform", "Author_Semantic_Transform" },
    { "Angle", "Author_Semantic_Angle" },
    { "Percentage", "Author_Semantic_Percentage" },
    { "Asset", "Author_Semantic_Asset" }
};

static void reflection_diagnostic_clear(Reflection_Diagnostic* diagnostic)
{
    if (diagnostic != 0)
    {
        memset(diagnostic, 0, sizeof(*diagnostic));
    }
}

static void reflection_copy_c_string(char* destination, uint32_t capacity, const char* source)
{
    uint32_t index = 0;

    if ((destination == 0) || (capacity == 0))
    {
        return;
    }

    if (source != 0)
    {
        while ((source[index] != 0) && ((index + 1) < capacity))
        {
            destination[index] = source[index];
            index += 1;
        }
    }

    destination[index] = 0;
}

static Reflection_Result reflection_fail(
    Reflection_Result result,
    Reflection_Diagnostic* diagnostic,
    const char* path,
    uint32_t line,
    uint32_t column,
    const char* format,
    ...
)
{
    if (diagnostic != 0)
    {
        va_list arguments;

        diagnostic->line = line;
        diagnostic->column = column;
        reflection_copy_c_string(
            diagnostic->path,
            Reflection_Diagnostic_Path_Capacity,
            path
        );

        va_start(arguments, format);
        vsnprintf(
            diagnostic->message,
            Reflection_Diagnostic_Message_Capacity,
            format,
            arguments
        );
        va_end(arguments);

        diagnostic->message[Reflection_Diagnostic_Message_Capacity - 1] = 0;
    }

    return result;
}

static uint32_t reflection_u64_fits_allocation(uint64_t value)
{
    return value <= (uint64_t)SIZE_MAX;
}

static void reflection_string_release(Reflection_String* string)
{
    if (string != 0)
    {
        free(string->data);
        memset(string, 0, sizeof(*string));
    }
}

static Reflection_Result reflection_string_copy(
    Reflection_String* destination,
    const char* data,
    uint64_t size,
    Reflection_Diagnostic* diagnostic,
    const char* path,
    uint32_t line,
    uint32_t column
)
{
    char* copy = 0;

    if ((destination == 0) || ((data == 0) && (size != 0)))
    {
        return reflection_fail(
            Reflection_Result_Invalid_Argument,
            diagnostic,
            path,
            line,
            column,
            "invalid string input"
        );
    }

    if (!reflection_u64_fits_allocation(size + 1))
    {
        return reflection_fail(
            Reflection_Result_Out_Of_Memory,
            diagnostic,
            path,
            line,
            column,
            "string is too large"
        );
    }

    copy = (char*)malloc((size_t)(size + 1));
    if (copy == 0)
    {
        return reflection_fail(
            Reflection_Result_Out_Of_Memory,
            diagnostic,
            path,
            line,
            column,
            "out of memory while copying annotation text"
        );
    }

    if (size != 0)
    {
        memcpy(copy, data, (size_t)size);
    }
    copy[size] = 0;

    destination->data = copy;
    destination->size = size;
    return Reflection_Result_Ok;
}

static uint32_t reflection_string_equal(
    const Reflection_String* left,
    const Reflection_String* right
)
{
    if (left->size != right->size)
    {
        return 0;
    }

    if (left->size == 0)
    {
        return 1;
    }

    return memcmp(left->data, right->data, (size_t)left->size) == 0;
}

static int reflection_string_compare(
    const Reflection_String* left,
    const Reflection_String* right
)
{
    uint64_t common_size = left->size < right->size ? left->size : right->size;
    int comparison = 0;

    if (common_size != 0)
    {
        comparison = memcmp(left->data, right->data, (size_t)common_size);
    }

    if (comparison != 0)
    {
        return comparison;
    }
    if (left->size < right->size)
    {
        return -1;
    }
    if (left->size > right->size)
    {
        return 1;
    }
    return 0;
}

static void reflection_field_release(Reflection_Field_Record* field)
{
    uint32_t range_index = 0;
    uint32_t component_index = 0;

    if (field == 0)
    {
        return;
    }

    reflection_string_release(&field->stable_key);
    reflection_string_release(&field->name);
    reflection_string_release(&field->json_path);
    reflection_string_release(&field->group);

    for (range_index = 0; range_index < 3; ++range_index)
    {
        for (component_index = 0; component_index < 4; ++component_index)
        {
            reflection_string_release(&field->ranges[range_index][component_index]);
        }
    }

    memset(field, 0, sizeof(*field));
}

static void reflection_schema_release(Reflection_Schema_Record* schema)
{
    uint32_t field_index = 0;

    if (schema == 0)
    {
        return;
    }

    for (field_index = 0; field_index < schema->field_count; ++field_index)
    {
        reflection_field_release(&schema->fields[field_index]);
    }

    free(schema->fields);
    reflection_string_release(&schema->symbol);
    reflection_string_release(&schema->schema_id);
    memset(schema, 0, sizeof(*schema));
}

static void reflection_database_release(Reflection_Database* database)
{
    uint32_t schema_index = 0;

    if (database == 0)
    {
        return;
    }

    for (schema_index = 0; schema_index < database->schema_count; ++schema_index)
    {
        reflection_schema_release(&database->schemas[schema_index]);
    }

    free(database->schemas);
    memset(database, 0, sizeof(*database));
}

static Reflection_Result reflection_reserve_schemas(
    Reflection_Database* database,
    uint32_t required,
    Reflection_Diagnostic* diagnostic,
    const char* path,
    uint32_t line,
    uint32_t column
)
{
    uint32_t new_capacity = 0;
    Reflection_Schema_Record* replacement = 0;

    if (required <= database->schema_capacity)
    {
        return Reflection_Result_Ok;
    }

    new_capacity = database->schema_capacity == 0 ? 4 : database->schema_capacity;
    while (new_capacity < required)
    {
        if (new_capacity > (UINT32_MAX / 2))
        {
            new_capacity = required;
            break;
        }
        new_capacity *= 2;
    }

    if ((uint64_t)new_capacity > ((uint64_t)SIZE_MAX / sizeof(*replacement)))
    {
        return reflection_fail(
            Reflection_Result_Out_Of_Memory,
            diagnostic,
            path,
            line,
            column,
            "too many author schemas"
        );
    }

    replacement = (Reflection_Schema_Record*)realloc(
        database->schemas,
        (size_t)new_capacity * sizeof(*replacement)
    );
    if (replacement == 0)
    {
        return reflection_fail(
            Reflection_Result_Out_Of_Memory,
            diagnostic,
            path,
            line,
            column,
            "out of memory while growing schema records"
        );
    }

    if (new_capacity > database->schema_capacity)
    {
        memset(
            replacement + database->schema_capacity,
            0,
            (size_t)(new_capacity - database->schema_capacity) * sizeof(*replacement)
        );
    }

    database->schemas = replacement;
    database->schema_capacity = new_capacity;
    return Reflection_Result_Ok;
}

static Reflection_Result reflection_reserve_fields(
    Reflection_Schema_Record* schema,
    uint32_t required,
    Reflection_Diagnostic* diagnostic,
    const char* path,
    uint32_t line,
    uint32_t column
)
{
    uint32_t new_capacity = 0;
    Reflection_Field_Record* replacement = 0;

    if (required <= schema->field_capacity)
    {
        return Reflection_Result_Ok;
    }

    new_capacity = schema->field_capacity == 0 ? 8 : schema->field_capacity;
    while (new_capacity < required)
    {
        if (new_capacity > (UINT32_MAX / 2))
        {
            new_capacity = required;
            break;
        }
        new_capacity *= 2;
    }

    if ((uint64_t)new_capacity > ((uint64_t)SIZE_MAX / sizeof(*replacement)))
    {
        return reflection_fail(
            Reflection_Result_Out_Of_Memory,
            diagnostic,
            path,
            line,
            column,
            "too many fields in schema"
        );
    }

    replacement = (Reflection_Field_Record*)realloc(
        schema->fields,
        (size_t)new_capacity * sizeof(*replacement)
    );
    if (replacement == 0)
    {
        return reflection_fail(
            Reflection_Result_Out_Of_Memory,
            diagnostic,
            path,
            line,
            column,
            "out of memory while growing field records"
        );
    }

    if (new_capacity > schema->field_capacity)
    {
        memset(
            replacement + schema->field_capacity,
            0,
            (size_t)(new_capacity - schema->field_capacity) * sizeof(*replacement)
        );
    }

    schema->fields = replacement;
    schema->field_capacity = new_capacity;
    return Reflection_Result_Ok;
}

static uint32_t reflection_is_space(char character)
{
    return (character == ' ') || (character == '\t');
}

static uint32_t reflection_is_digit(char character)
{
    return (character >= '0') && (character <= '9');
}

static uint32_t reflection_is_hex_digit(char character)
{
    return reflection_is_digit(character)
        || ((character >= 'a') && (character <= 'f'))
        || ((character >= 'A') && (character <= 'F'));
}

static uint32_t reflection_hex_value(char character)
{
    if ((character >= '0') && (character <= '9'))
    {
        return (uint32_t)(character - '0');
    }
    if ((character >= 'a') && (character <= 'f'))
    {
        return 10u + (uint32_t)(character - 'a');
    }
    return 10u + (uint32_t)(character - 'A');
}

static uint32_t reflection_is_identifier_start(char character)
{
    return ((character >= 'a') && (character <= 'z'))
        || ((character >= 'A') && (character <= 'Z'))
        || (character == '_');
}

static uint32_t reflection_is_identifier_continue(char character)
{
    return reflection_is_identifier_start(character) || reflection_is_digit(character);
}

static uint32_t reflection_cursor_column(const Reflection_Parse_Cursor* cursor)
{
    uint64_t column = (uint64_t)(cursor->at - cursor->line_start) + 1;
    return column > UINT32_MAX ? UINT32_MAX : (uint32_t)column;
}

static void reflection_skip_spaces(Reflection_Parse_Cursor* cursor)
{
    while ((cursor->at < cursor->end) && reflection_is_space(*cursor->at))
    {
        cursor->at += 1;
    }
}

static uint32_t reflection_match_bytes(
    Reflection_Parse_Cursor* cursor,
    const char* expected
)
{
    size_t expected_size = strlen(expected);
    uint64_t remaining = (uint64_t)(cursor->end - cursor->at);

    if ((uint64_t)expected_size > remaining)
    {
        return 0;
    }

    if (memcmp(cursor->at, expected, expected_size) != 0)
    {
        return 0;
    }

    cursor->at += expected_size;
    return 1;
}

static Reflection_Result reflection_expect_named_prefix(
    Reflection_Parse_Cursor* cursor,
    const char* prefix,
    Reflection_Diagnostic* diagnostic
)
{
    reflection_skip_spaces(cursor);
    if (!reflection_match_bytes(cursor, prefix))
    {
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            reflection_cursor_column(cursor),
            "expected '%s'",
            prefix
        );
    }
    return Reflection_Result_Ok;
}

static Reflection_Result reflection_parse_identifier(
    Reflection_Parse_Cursor* cursor,
    Reflection_String* output,
    Reflection_Diagnostic* diagnostic
)
{
    const char* start = cursor->at;

    if ((cursor->at >= cursor->end) || !reflection_is_identifier_start(*cursor->at))
    {
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            reflection_cursor_column(cursor),
            "expected a C identifier"
        );
    }

    cursor->at += 1;
    while ((cursor->at < cursor->end) && reflection_is_identifier_continue(*cursor->at))
    {
        cursor->at += 1;
    }

    return reflection_string_copy(
        output,
        start,
        (uint64_t)(cursor->at - start),
        diagnostic,
        cursor->source->path,
        cursor->line,
        (uint32_t)((start - cursor->line_start) + 1)
    );
}

static Reflection_Result reflection_parse_quoted_string(
    Reflection_Parse_Cursor* cursor,
    Reflection_String* output,
    Reflection_Diagnostic* diagnostic
)
{
    const char* raw_start = 0;
    char* decoded = 0;
    uint64_t decoded_size = 0;
    uint64_t maximum_size = 0;

    if ((cursor->at >= cursor->end) || (*cursor->at != '"'))
    {
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            reflection_cursor_column(cursor),
            "expected a quoted string"
        );
    }

    cursor->at += 1;
    raw_start = cursor->at;
    maximum_size = (uint64_t)(cursor->end - cursor->at);
    if (!reflection_u64_fits_allocation(maximum_size + 1))
    {
        return reflection_fail(
            Reflection_Result_Out_Of_Memory,
            diagnostic,
            cursor->source->path,
            cursor->line,
            reflection_cursor_column(cursor),
            "quoted string is too large"
        );
    }

    decoded = (char*)malloc((size_t)(maximum_size + 1));
    if (decoded == 0)
    {
        return reflection_fail(
            Reflection_Result_Out_Of_Memory,
            diagnostic,
            cursor->source->path,
            cursor->line,
            reflection_cursor_column(cursor),
            "out of memory while decoding a quoted string"
        );
    }

    while (cursor->at < cursor->end)
    {
        unsigned char value = (unsigned char)*cursor->at;
        cursor->at += 1;

        if (value == '"')
        {
            decoded[decoded_size] = 0;
            output->data = decoded;
            output->size = decoded_size;
            return Reflection_Result_Ok;
        }

        if (value == '\\')
        {
            if (cursor->at >= cursor->end)
            {
                free(decoded);
                return reflection_fail(
                    Reflection_Result_Parse_Error,
                    diagnostic,
                    cursor->source->path,
                    cursor->line,
                    (uint32_t)((cursor->at - cursor->line_start) + 1),
                    "unterminated string escape"
                );
            }

            value = (unsigned char)*cursor->at;
            cursor->at += 1;

            if ((value == '\\') || (value == '"'))
            {
                decoded[decoded_size] = (char)value;
                decoded_size += 1;
                continue;
            }

            if (value == 'x')
            {
                uint32_t high = 0;
                uint32_t low = 0;
                if (((cursor->end - cursor->at) < 2)
                    || !reflection_is_hex_digit(cursor->at[0])
                    || !reflection_is_hex_digit(cursor->at[1]))
                {
                    free(decoded);
                    return reflection_fail(
                        Reflection_Result_Parse_Error,
                        diagnostic,
                        cursor->source->path,
                        cursor->line,
                        (uint32_t)((cursor->at - cursor->line_start) + 1),
                        "expected exactly two hexadecimal digits after \\x"
                    );
                }

                high = reflection_hex_value(cursor->at[0]);
                low = reflection_hex_value(cursor->at[1]);
                value = (unsigned char)((high << 4) | low);
                cursor->at += 2;
            }
            else
            {
                free(decoded);
                return reflection_fail(
                    Reflection_Result_Parse_Error,
                    diagnostic,
                    cursor->source->path,
                    cursor->line,
                    (uint32_t)((cursor->at - cursor->line_start) + 1),
                    "unsupported string escape"
                );
            }
        }

        if (value < 0x20)
        {
            free(decoded);
            return reflection_fail(
                Reflection_Result_Parse_Error,
                diagnostic,
                cursor->source->path,
                cursor->line,
                (uint32_t)((raw_start - cursor->line_start) + 1),
                "control bytes are not allowed in annotation strings"
            );
        }

        decoded[decoded_size] = (char)value;
        decoded_size += 1;
    }

    free(decoded);
    return reflection_fail(
        Reflection_Result_Parse_Error,
        diagnostic,
        cursor->source->path,
        cursor->line,
        (uint32_t)((raw_start - cursor->line_start) + 1),
        "unterminated quoted string"
    );
}

static Reflection_Result reflection_parse_u64(
    Reflection_Parse_Cursor* cursor,
    uint64_t* output,
    Reflection_Diagnostic* diagnostic
)
{
    uint64_t value = 0;
    uint32_t base = 10;
    uint32_t digit_count = 0;
    uint32_t column = reflection_cursor_column(cursor);

    if (((cursor->end - cursor->at) >= 2)
        && (cursor->at[0] == '0')
        && ((cursor->at[1] == 'x') || (cursor->at[1] == 'X')))
    {
        base = 16;
        cursor->at += 2;
    }

    while (cursor->at < cursor->end)
    {
        uint32_t digit = 0;
        char character = *cursor->at;

        if (reflection_is_digit(character))
        {
            digit = (uint32_t)(character - '0');
        }
        else if ((base == 16) && (character >= 'a') && (character <= 'f'))
        {
            digit = 10u + (uint32_t)(character - 'a');
        }
        else if ((base == 16) && (character >= 'A') && (character <= 'F'))
        {
            digit = 10u + (uint32_t)(character - 'A');
        }
        else
        {
            break;
        }

        if (digit >= base)
        {
            break;
        }
        if (value > ((UINT64_MAX - digit) / base))
        {
            return reflection_fail(
                Reflection_Result_Parse_Error,
                diagnostic,
                cursor->source->path,
                cursor->line,
                column,
                "unsigned integer overflows 64 bits"
            );
        }

        value = (value * base) + digit;
        digit_count += 1;
        cursor->at += 1;
    }

    if (digit_count == 0)
    {
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            column,
            "expected an unsigned integer"
        );
    }

    *output = value;
    return Reflection_Result_Ok;
}

static Reflection_Result reflection_parse_u32(
    Reflection_Parse_Cursor* cursor,
    uint32_t* output,
    Reflection_Diagnostic* diagnostic
)
{
    uint64_t value = 0;
    Reflection_Result result = reflection_parse_u64(cursor, &value, diagnostic);

    if (result != Reflection_Result_Ok)
    {
        return result;
    }
    if (value > UINT32_MAX)
    {
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            reflection_cursor_column(cursor),
            "unsigned integer overflows 32 bits"
        );
    }

    *output = (uint32_t)value;
    return Reflection_Result_Ok;
}

static Reflection_Result reflection_parse_float_token(
    Reflection_Parse_Cursor* cursor,
    Reflection_String* output,
    Reflection_Diagnostic* diagnostic
)
{
    const char* start = cursor->at;
    const char* digits_start = 0;

    if ((cursor->at < cursor->end) && ((*cursor->at == '+') || (*cursor->at == '-')))
    {
        cursor->at += 1;
    }

    digits_start = cursor->at;
    while ((cursor->at < cursor->end) && reflection_is_digit(*cursor->at))
    {
        cursor->at += 1;
    }
    if (cursor->at == digits_start)
    {
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            (uint32_t)((start - cursor->line_start) + 1),
            "expected a decimal float"
        );
    }

    if ((cursor->at < cursor->end) && (*cursor->at == '.'))
    {
        cursor->at += 1;
        digits_start = cursor->at;
        while ((cursor->at < cursor->end) && reflection_is_digit(*cursor->at))
        {
            cursor->at += 1;
        }
        if (cursor->at == digits_start)
        {
            return reflection_fail(
                Reflection_Result_Parse_Error,
                diagnostic,
                cursor->source->path,
                cursor->line,
                (uint32_t)((start - cursor->line_start) + 1),
                "a decimal point must be followed by digits"
            );
        }
    }

    if ((cursor->at < cursor->end) && ((*cursor->at == 'e') || (*cursor->at == 'E')))
    {
        cursor->at += 1;
        if ((cursor->at < cursor->end) && ((*cursor->at == '+') || (*cursor->at == '-')))
        {
            cursor->at += 1;
        }

        digits_start = cursor->at;
        while ((cursor->at < cursor->end) && reflection_is_digit(*cursor->at))
        {
            cursor->at += 1;
        }
        if (cursor->at == digits_start)
        {
            return reflection_fail(
                Reflection_Result_Parse_Error,
                diagnostic,
                cursor->source->path,
                cursor->line,
                (uint32_t)((start - cursor->line_start) + 1),
                "an exponent must contain digits"
            );
        }
    }

    return reflection_string_copy(
        output,
        start,
        (uint64_t)(cursor->at - start),
        diagnostic,
        cursor->source->path,
        cursor->line,
        (uint32_t)((start - cursor->line_start) + 1)
    );
}

static Reflection_Result reflection_parse_float4(
    Reflection_Parse_Cursor* cursor,
    Reflection_String output[4],
    Reflection_Diagnostic* diagnostic
)
{
    uint32_t component_index = 0;
    Reflection_Result result = Reflection_Result_Ok;

    if ((cursor->at >= cursor->end) || (*cursor->at != '['))
    {
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            reflection_cursor_column(cursor),
            "expected a four-component range beginning with '['"
        );
    }
    cursor->at += 1;

    for (component_index = 0; component_index < 4; ++component_index)
    {
        reflection_skip_spaces(cursor);
        result = reflection_parse_float_token(cursor, &output[component_index], diagnostic);
        if (result != Reflection_Result_Ok)
        {
            return result;
        }

        reflection_skip_spaces(cursor);
        if (component_index < 3)
        {
            if ((cursor->at >= cursor->end) || (*cursor->at != ','))
            {
                return reflection_fail(
                    Reflection_Result_Parse_Error,
                    diagnostic,
                    cursor->source->path,
                    cursor->line,
                    reflection_cursor_column(cursor),
                    "expected ',' in four-component range"
                );
            }
            cursor->at += 1;
        }
    }

    if ((cursor->at >= cursor->end) || (*cursor->at != ']'))
    {
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            reflection_cursor_column(cursor),
            "expected ']' after four range components"
        );
    }
    cursor->at += 1;
    return Reflection_Result_Ok;
}

static Reflection_Result reflection_parse_mapped_name(
    Reflection_Parse_Cursor* cursor,
    const Reflection_Name_Map* names,
    uint32_t name_count,
    uint32_t* output_index,
    Reflection_Diagnostic* diagnostic,
    const char* description
)
{
    const char* start = cursor->at;
    uint64_t size = 0;
    uint32_t index = 0;

    if ((cursor->at >= cursor->end) || !reflection_is_identifier_start(*cursor->at))
    {
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            reflection_cursor_column(cursor),
            "expected %s",
            description
        );
    }

    cursor->at += 1;
    while ((cursor->at < cursor->end) && reflection_is_identifier_continue(*cursor->at))
    {
        cursor->at += 1;
    }
    size = (uint64_t)(cursor->at - start);

    for (index = 0; index < name_count; ++index)
    {
        size_t candidate_size = strlen(names[index].annotation_name);
        if (((uint64_t)candidate_size == size)
            && (memcmp(start, names[index].annotation_name, candidate_size) == 0))
        {
            *output_index = index;
            return Reflection_Result_Ok;
        }
    }

    return reflection_fail(
        Reflection_Result_Parse_Error,
        diagnostic,
        cursor->source->path,
        cursor->line,
        (uint32_t)((start - cursor->line_start) + 1),
        "unknown %s '%.*s'",
        description,
        (int)(size > (uint64_t)INT_MAX ? INT_MAX : size),
        start
    );
}

static Reflection_Result reflection_expect_end_of_annotation(
    Reflection_Parse_Cursor* cursor,
    Reflection_Diagnostic* diagnostic
)
{
    reflection_skip_spaces(cursor);
    if (cursor->at != cursor->end)
    {
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            reflection_cursor_column(cursor),
            "unexpected trailing annotation text"
        );
    }
    return Reflection_Result_Ok;
}

static uint32_t reflection_schema_version_matches(
    const Reflection_String* schema_id,
    uint32_t schema_version
)
{
    uint64_t slash_index = schema_id->size;
    uint64_t cursor = 0;
    uint64_t parsed_version = 0;

    while (slash_index > 0)
    {
        slash_index -= 1;
        if (schema_id->data[slash_index] == '/')
        {
            break;
        }
    }

    if ((slash_index + 2) >= schema_id->size)
    {
        return 0;
    }
    if ((schema_id->data[slash_index] != '/') || (schema_id->data[slash_index + 1] != 'v'))
    {
        return 0;
    }

    cursor = slash_index + 2;
    while (cursor < schema_id->size)
    {
        uint32_t digit = 0;
        if (!reflection_is_digit(schema_id->data[cursor]))
        {
            return 0;
        }

        digit = (uint32_t)(schema_id->data[cursor] - '0');
        if (parsed_version > ((UINT32_MAX - digit) / 10))
        {
            return 0;
        }

        parsed_version = (parsed_version * 10) + digit;
        cursor += 1;
    }

    return parsed_version == schema_version;
}

uint64_t reflection_fnv1a64(const char* data, uint64_t size, void* user)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    uint64_t index = 0;

    (void)user;

    for (index = 0; index < size; ++index)
    {
        hash ^= (uint8_t)data[index];
        hash *= UINT64_C(1099511628211);
    }

    return hash;
}

static Reflection_Result reflection_parse_schema(
    Reflection_Parse_Cursor* cursor,
    Reflection_Database* database,
    int64_t* current_schema_index,
    Reflection_Diagnostic* diagnostic
)
{
    Reflection_Schema_Record schema = {};
    Reflection_Result result = Reflection_Result_Ok;
    uint32_t schema_index = 0;

    if (*current_schema_index >= 0)
    {
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            reflection_cursor_column(cursor),
            "schema blocks cannot nest"
        );
    }

    result = reflection_expect_named_prefix(cursor, "symbol=", diagnostic);
    if (result == Reflection_Result_Ok)
    {
        result = reflection_parse_identifier(cursor, &schema.symbol, diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_named_prefix(cursor, "id=", diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_parse_quoted_string(cursor, &schema.schema_id, diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_named_prefix(cursor, "version=", diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_parse_u32(cursor, &schema.schema_version, diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_named_prefix(cursor, "flags=", diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_parse_u32(cursor, &schema.flags, diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_end_of_annotation(cursor, diagnostic);
    }

    if (result != Reflection_Result_Ok)
    {
        reflection_schema_release(&schema);
        return result;
    }
    if ((schema.schema_id.size == 0) || (schema.schema_version == 0))
    {
        reflection_schema_release(&schema);
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            1,
            "schema ID and version must be nonzero"
        );
    }
    if (!reflection_schema_version_matches(&schema.schema_id, schema.schema_version))
    {
        reflection_schema_release(&schema);
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            1,
            "schema ID must end in /vN matching version=N"
        );
    }

    for (schema_index = 0; schema_index < database->schema_count; ++schema_index)
    {
        Reflection_Schema_Record* existing = &database->schemas[schema_index];
        if (reflection_string_equal(&existing->schema_id, &schema.schema_id))
        {
            reflection_schema_release(&schema);
            return reflection_fail(
                Reflection_Result_Duplicate_Schema_Id,
                diagnostic,
                cursor->source->path,
                cursor->line,
                1,
                "duplicate schema ID; first declared at %s(%u)",
                existing->source_path,
                existing->source_line
            );
        }
        if (reflection_string_equal(&existing->symbol, &schema.symbol))
        {
            reflection_schema_release(&schema);
            return reflection_fail(
                Reflection_Result_Duplicate_Schema_Symbol,
                diagnostic,
                cursor->source->path,
                cursor->line,
                1,
                "duplicate schema C symbol; first declared at %s(%u)",
                existing->source_path,
                existing->source_line
            );
        }
    }

    result = reflection_reserve_schemas(
        database,
        database->schema_count + 1,
        diagnostic,
        cursor->source->path,
        cursor->line,
        1
    );
    if (result != Reflection_Result_Ok)
    {
        reflection_schema_release(&schema);
        return result;
    }

    schema.source_path = cursor->source->path;
    schema.source_line = cursor->line;
    schema.source_column = 1;
    database->schemas[database->schema_count] = schema;
    *current_schema_index = (int64_t)database->schema_count;
    database->schema_count += 1;
    return Reflection_Result_Ok;
}

static Reflection_Result reflection_parse_field(
    Reflection_Parse_Cursor* cursor,
    Reflection_Database* database,
    int64_t current_schema_index,
    Reflection_Hash_Function hash_function,
    void* hash_user,
    Reflection_Diagnostic* diagnostic
)
{
    Reflection_Field_Record field = {};
    Reflection_Schema_Record* schema = 0;
    Reflection_Result result = Reflection_Result_Ok;
    uint32_t field_index = 0;

    if ((current_schema_index < 0)
        || ((uint64_t)current_schema_index >= database->schema_count))
    {
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            reflection_cursor_column(cursor),
            "field annotation requires an open schema block"
        );
    }
    schema = &database->schemas[current_schema_index];

    result = reflection_expect_named_prefix(cursor, "key=", diagnostic);
    if (result == Reflection_Result_Ok)
    {
        result = reflection_parse_quoted_string(cursor, &field.stable_key, diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_named_prefix(cursor, "name=", diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_parse_quoted_string(cursor, &field.name, diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_named_prefix(cursor, "path=", diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_parse_quoted_string(cursor, &field.json_path, diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_named_prefix(cursor, "group=", diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_parse_quoted_string(cursor, &field.group, diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_named_prefix(cursor, "kind=", diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_parse_mapped_name(
            cursor,
            g_field_kinds,
            (uint32_t)(sizeof(g_field_kinds) / sizeof(g_field_kinds[0])),
            &field.kind_index,
            diagnostic,
            "field kind"
        );
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_named_prefix(cursor, "semantic=", diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_parse_mapped_name(
            cursor,
            g_field_semantics,
            (uint32_t)(sizeof(g_field_semantics) / sizeof(g_field_semantics[0])),
            &field.semantic_index,
            diagnostic,
            "field semantic"
        );
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_named_prefix(cursor, "min=", diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_parse_float4(cursor, field.ranges[0], diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_named_prefix(cursor, "max=", diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_parse_float4(cursor, field.ranges[1], diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_named_prefix(cursor, "step=", diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_parse_float4(cursor, field.ranges[2], diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_named_prefix(cursor, "flags=", diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_parse_u32(cursor, &field.flags, diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_named_prefix(cursor, "id=", diagnostic);
    }
    if (result == Reflection_Result_Ok)
    {
        if (reflection_match_bytes(cursor, "auto"))
        {
            field.id_is_automatic = 1;
            field.id = hash_function(field.stable_key.data, field.stable_key.size, hash_user);
        }
        else
        {
            field.id_is_automatic = 0;
            result = reflection_parse_u64(cursor, &field.id, diagnostic);
        }
    }
    if (result == Reflection_Result_Ok)
    {
        result = reflection_expect_end_of_annotation(cursor, diagnostic);
    }

    if (result != Reflection_Result_Ok)
    {
        reflection_field_release(&field);
        return result;
    }
    if ((field.stable_key.size == 0) || (field.name.size == 0))
    {
        reflection_field_release(&field);
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            1,
            "field stable key and display name must be nonempty"
        );
    }
    if ((field.json_path.size == 0) || (field.json_path.data[0] != '$'))
    {
        reflection_field_release(&field);
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            cursor->source->path,
            cursor->line,
            1,
            "field JSON path must begin with '$'"
        );
    }
    if (field.id == 0)
    {
        reflection_field_release(&field);
        return reflection_fail(
            Reflection_Result_Invalid_Field_Id,
            diagnostic,
            cursor->source->path,
            cursor->line,
            1,
            "field ID zero is invalid"
        );
    }

    for (field_index = 0; field_index < schema->field_count; ++field_index)
    {
        Reflection_Field_Record* existing = &schema->fields[field_index];

        if (reflection_string_equal(&existing->stable_key, &field.stable_key))
        {
            reflection_field_release(&field);
            return reflection_fail(
                Reflection_Result_Duplicate_Field_Id,
                diagnostic,
                cursor->source->path,
                cursor->line,
                1,
                "duplicate stable field key; first declared at %s(%u)",
                existing->source_path,
                existing->source_line
            );
        }

        if (existing->id == field.id)
        {
            if (existing->id_is_automatic && field.id_is_automatic)
            {
                reflection_field_release(&field);
                return reflection_fail(
                    Reflection_Result_Fnv64_Collision,
                    diagnostic,
                    cursor->source->path,
                    cursor->line,
                    1,
                    "FNV-1a-64 collision with field declared at %s(%u)",
                    existing->source_path,
                    existing->source_line
                );
            }

            reflection_field_release(&field);
            return reflection_fail(
                Reflection_Result_Duplicate_Field_Id,
                diagnostic,
                cursor->source->path,
                cursor->line,
                1,
                "duplicate field ID; first declared at %s(%u)",
                existing->source_path,
                existing->source_line
            );
        }
    }

    result = reflection_reserve_fields(
        schema,
        schema->field_count + 1,
        diagnostic,
        cursor->source->path,
        cursor->line,
        1
    );
    if (result != Reflection_Result_Ok)
    {
        reflection_field_release(&field);
        return result;
    }

    field.source_path = cursor->source->path;
    field.source_line = cursor->line;
    field.source_column = 1;
    schema->fields[schema->field_count] = field;
    schema->field_count += 1;
    return Reflection_Result_Ok;
}

static Reflection_Result reflection_parse_annotation_line(
    const Reflection_Source* source,
    const char* line_start,
    const char* line_end,
    uint32_t line_number,
    Reflection_Database* database,
    int64_t* current_schema_index,
    Reflection_Hash_Function hash_function,
    void* hash_user,
    Reflection_Diagnostic* diagnostic
)
{
    Reflection_Parse_Cursor cursor = {};
    const char* scan = line_start;
    const char* directive_start = 0;
    uint64_t directive_size = 0;

    while ((scan < line_end) && reflection_is_space(*scan))
    {
        scan += 1;
    }
    if (((line_end - scan) < 2) || (scan[0] != '/') || (scan[1] != '/'))
    {
        return Reflection_Result_Ok;
    }
    scan += 2;
    while ((scan < line_end) && reflection_is_space(*scan))
    {
        scan += 1;
    }
    if (((line_end - scan) < 6) || (memcmp(scan, "@kalam", 6) != 0))
    {
        return Reflection_Result_Ok;
    }
    scan += 6;
    if ((scan < line_end) && !reflection_is_space(*scan))
    {
        return Reflection_Result_Ok;
    }

    cursor.source = source;
    cursor.line_start = line_start;
    cursor.at = scan;
    cursor.end = line_end;
    cursor.line = line_number;
    reflection_skip_spaces(&cursor);

    directive_start = cursor.at;
    if ((cursor.at >= cursor.end) || !reflection_is_identifier_start(*cursor.at))
    {
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            source->path,
            line_number,
            reflection_cursor_column(&cursor),
            "expected schema, field, or end after @kalam"
        );
    }
    cursor.at += 1;
    while ((cursor.at < cursor.end) && reflection_is_identifier_continue(*cursor.at))
    {
        cursor.at += 1;
    }
    directive_size = (uint64_t)(cursor.at - directive_start);

    if ((directive_size == 6) && (memcmp(directive_start, "schema", 6) == 0))
    {
        return reflection_parse_schema(
            &cursor,
            database,
            current_schema_index,
            diagnostic
        );
    }
    if ((directive_size == 5) && (memcmp(directive_start, "field", 5) == 0))
    {
        return reflection_parse_field(
            &cursor,
            database,
            *current_schema_index,
            hash_function,
            hash_user,
            diagnostic
        );
    }
    if ((directive_size == 3) && (memcmp(directive_start, "end", 3) == 0))
    {
        Reflection_Result result = Reflection_Result_Ok;
        if (*current_schema_index < 0)
        {
            return reflection_fail(
                Reflection_Result_Parse_Error,
                diagnostic,
                source->path,
                line_number,
                reflection_cursor_column(&cursor),
                "end annotation has no open schema block"
            );
        }

        result = reflection_expect_end_of_annotation(&cursor, diagnostic);
        if (result == Reflection_Result_Ok)
        {
            *current_schema_index = -1;
        }
        return result;
    }

    return reflection_fail(
        Reflection_Result_Parse_Error,
        diagnostic,
        source->path,
        line_number,
        (uint32_t)((directive_start - line_start) + 1),
        "unknown @kalam directive '%.*s'",
        (int)(directive_size > (uint64_t)INT_MAX ? INT_MAX : directive_size),
        directive_start
    );
}

static Reflection_Result reflection_parse_source(
    const Reflection_Source* source,
    Reflection_Database* database,
    Reflection_Hash_Function hash_function,
    void* hash_user,
    Reflection_Diagnostic* diagnostic
)
{
    uint64_t line_start_offset = 0;
    uint64_t cursor = 0;
    uint32_t line_number = 1;
    int64_t current_schema_index = -1;

    while (cursor <= source->text_size)
    {
        uint64_t line_end_offset = cursor;
        Reflection_Result result = Reflection_Result_Ok;

        while ((line_end_offset < source->text_size)
            && (source->text[line_end_offset] != '\n'))
        {
            line_end_offset += 1;
        }

        if ((line_end_offset > line_start_offset)
            && (source->text[line_end_offset - 1] == '\r'))
        {
            line_end_offset -= 1;
        }

        result = reflection_parse_annotation_line(
            source,
            source->text + line_start_offset,
            source->text + line_end_offset,
            line_number,
            database,
            &current_schema_index,
            hash_function,
            hash_user,
            diagnostic
        );
        if (result != Reflection_Result_Ok)
        {
            return result;
        }

        if (cursor >= source->text_size)
        {
            break;
        }

        cursor = line_end_offset;
        if ((cursor < source->text_size) && (source->text[cursor] == '\r'))
        {
            cursor += 1;
        }
        if ((cursor < source->text_size) && (source->text[cursor] == '\n'))
        {
            cursor += 1;
        }
        line_start_offset = cursor;
        line_number += 1;
    }

    if (current_schema_index >= 0)
    {
        Reflection_Schema_Record* schema = &database->schemas[current_schema_index];
        return reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            source->path,
            line_number,
            1,
            "schema '%s' is missing //@kalam end",
            schema->schema_id.data
        );
    }

    return Reflection_Result_Ok;
}

static int reflection_field_compare(
    const Reflection_Field_Record* left,
    const Reflection_Field_Record* right
)
{
    if (left->id < right->id)
    {
        return -1;
    }
    if (left->id > right->id)
    {
        return 1;
    }
    return reflection_string_compare(&left->stable_key, &right->stable_key);
}

static void reflection_sort_database(Reflection_Database* database)
{
    uint32_t schema_index = 0;

    for (schema_index = 1; schema_index < database->schema_count; ++schema_index)
    {
        Reflection_Schema_Record value = database->schemas[schema_index];
        uint32_t insert_index = schema_index;

        while ((insert_index > 0)
            && (reflection_string_compare(
                &value.schema_id,
                &database->schemas[insert_index - 1].schema_id
            ) < 0))
        {
            database->schemas[insert_index] = database->schemas[insert_index - 1];
            insert_index -= 1;
        }
        database->schemas[insert_index] = value;
    }

    for (schema_index = 0; schema_index < database->schema_count; ++schema_index)
    {
        Reflection_Schema_Record* schema = &database->schemas[schema_index];
        uint32_t field_index = 0;

        for (field_index = 1; field_index < schema->field_count; ++field_index)
        {
            Reflection_Field_Record value = schema->fields[field_index];
            uint32_t insert_index = field_index;

            while ((insert_index > 0)
                && (reflection_field_compare(
                    &value,
                    &schema->fields[insert_index - 1]
                ) < 0))
            {
                schema->fields[insert_index] = schema->fields[insert_index - 1];
                insert_index -= 1;
            }
            schema->fields[insert_index] = value;
        }
    }
}

void reflection_output_init(Reflection_Output* output)
{
    if (output != 0)
    {
        memset(output, 0, sizeof(*output));
    }
}

void reflection_output_release(Reflection_Output* output)
{
    if (output != 0)
    {
        free(output->data);
        memset(output, 0, sizeof(*output));
    }
}

static uint32_t reflection_output_reserve(Reflection_Output* output, uint64_t required)
{
    uint64_t new_capacity = 0;
    char* replacement = 0;

    if (required <= output->capacity)
    {
        return 1;
    }
    if (!reflection_u64_fits_allocation(required))
    {
        return 0;
    }

    new_capacity = output->capacity == 0 ? 4096 : output->capacity;
    while (new_capacity < required)
    {
        if (new_capacity > (UINT64_MAX / 2))
        {
            new_capacity = required;
            break;
        }
        new_capacity *= 2;
    }

    if (!reflection_u64_fits_allocation(new_capacity))
    {
        return 0;
    }

    replacement = (char*)realloc(output->data, (size_t)new_capacity);
    if (replacement == 0)
    {
        return 0;
    }

    output->data = replacement;
    output->capacity = new_capacity;
    return 1;
}

static uint32_t reflection_output_append_bytes(
    Reflection_Output* output,
    const char* data,
    uint64_t size
)
{
    uint64_t required = 0;

    if (size > (UINT64_MAX - output->size - 1))
    {
        return 0;
    }
    required = output->size + size + 1;
    if (!reflection_output_reserve(output, required))
    {
        return 0;
    }

    if (size != 0)
    {
        memcpy(output->data + output->size, data, (size_t)size);
    }
    output->size += size;
    output->data[output->size] = 0;
    return 1;
}

static uint32_t reflection_output_append_c_string(
    Reflection_Output* output,
    const char* string
)
{
    return reflection_output_append_bytes(output, string, (uint64_t)strlen(string));
}

static uint32_t reflection_output_append_format(
    Reflection_Output* output,
    const char* format,
    ...
)
{
    va_list arguments;
    va_list copied_arguments;
    int required = 0;
    uint64_t total_required = 0;

    va_start(arguments, format);
    va_copy(copied_arguments, arguments);
    required = vsnprintf(0, 0, format, copied_arguments);
    va_end(copied_arguments);

    if (required < 0)
    {
        va_end(arguments);
        return 0;
    }
    if ((uint64_t)required > (UINT64_MAX - output->size - 1))
    {
        va_end(arguments);
        return 0;
    }

    total_required = output->size + (uint64_t)required + 1;
    if (!reflection_output_reserve(output, total_required))
    {
        va_end(arguments);
        return 0;
    }

    vsnprintf(
        output->data + output->size,
        (size_t)((uint64_t)required + 1),
        format,
        arguments
    );
    va_end(arguments);
    output->size += (uint64_t)required;
    return 1;
}

static uint32_t reflection_output_append_escaped_string(
    Reflection_Output* output,
    const Reflection_String* string
)
{
    uint64_t index = 0;

    if (!reflection_output_append_c_string(output, "\""))
    {
        return 0;
    }

    for (index = 0; index < string->size; ++index)
    {
        unsigned char value = (unsigned char)string->data[index];
        if (value == '"')
        {
            if (!reflection_output_append_c_string(output, "\\\""))
            {
                return 0;
            }
        }
        else if (value == '\\')
        {
            if (!reflection_output_append_c_string(output, "\\\\"))
            {
                return 0;
            }
        }
        else if ((value >= 0x20) && (value <= 0x7e))
        {
            char character = (char)value;
            if (!reflection_output_append_bytes(output, &character, 1))
            {
                return 0;
            }
        }
        else
        {
            if (!reflection_output_append_format(
                output,
                "\\%03o",
                (uint32_t)value
            ))
            {
                return 0;
            }
        }
    }

    return reflection_output_append_c_string(output, "\"");
}

static uint32_t reflection_output_append_string_view(
    Reflection_Output* output,
    const Reflection_String* string
)
{
    return reflection_output_append_c_string(output, "{ ")
        && reflection_output_append_escaped_string(output, string)
        && reflection_output_append_format(
            output,
            ", UINT64_C(%llu) }",
            (unsigned long long)string->size
        );
}

static uint32_t reflection_float_has_fraction_or_exponent(
    const Reflection_String* token
)
{
    uint64_t index = 0;
    for (index = 0; index < token->size; ++index)
    {
        if ((token->data[index] == '.')
            || (token->data[index] == 'e')
            || (token->data[index] == 'E'))
        {
            return 1;
        }
    }
    return 0;
}

static uint32_t reflection_output_append_float(
    Reflection_Output* output,
    const Reflection_String* token
)
{
    if (!reflection_output_append_bytes(output, token->data, token->size))
    {
        return 0;
    }

    if (reflection_float_has_fraction_or_exponent(token))
    {
        return reflection_output_append_c_string(output, "f");
    }
    return reflection_output_append_c_string(output, ".0f");
}

static uint32_t reflection_output_append_float4(
    Reflection_Output* output,
    const Reflection_String values[4]
)
{
    uint32_t index = 0;

    if (!reflection_output_append_c_string(output, "{ "))
    {
        return 0;
    }

    for (index = 0; index < 4; ++index)
    {
        if ((index != 0) && !reflection_output_append_c_string(output, ", "))
        {
            return 0;
        }
        if (!reflection_output_append_float(output, &values[index]))
        {
            return 0;
        }
    }

    return reflection_output_append_c_string(output, " }");
}

static uint32_t reflection_emit_database(
    const Reflection_Database* database,
    Reflection_Output* output
)
{
    uint32_t schema_index = 0;

    output->size = 0;
    if (output->data != 0)
    {
        output->data[0] = 0;
    }

    if (!reflection_output_append_c_string(
        output,
        "/* Generated by Kalam reflection_tool. Do not edit. */\n"
        "#ifndef KALAM_GENERATED_AUTHOR_SCHEMAS_H\n"
        "#define KALAM_GENERATED_AUTHOR_SCHEMAS_H\n"
        "\n"
        "#include \"author_schema.h\"\n"
        "\n"
    ))
    {
        return 0;
    }

    for (schema_index = 0; schema_index < database->schema_count; ++schema_index)
    {
        const Reflection_Schema_Record* schema = &database->schemas[schema_index];
        uint32_t field_index = 0;

        if (schema->field_count == 0)
        {
            continue;
        }

        if (!reflection_output_append_c_string(
            output,
            "static const Author_Field_Desc kalam_generated_author_fields_"
        )
            || !reflection_output_append_bytes(
                output,
                schema->symbol.data,
                schema->symbol.size
            )
            || !reflection_output_append_c_string(output, "[] =\n{\n"))
        {
            return 0;
        }

        for (field_index = 0; field_index < schema->field_count; ++field_index)
        {
            const Reflection_Field_Record* field = &schema->fields[field_index];

            if (!reflection_output_append_format(
                output,
                "    {\n"
                "        UINT64_C(%llu),\n"
                "        ",
                (unsigned long long)field->id
            )
                || !reflection_output_append_string_view(output, &field->name)
                || !reflection_output_append_c_string(output, ",\n        ")
                || !reflection_output_append_string_view(output, &field->json_path)
                || !reflection_output_append_c_string(output, ",\n        ")
                || !reflection_output_append_string_view(output, &field->group)
                || !reflection_output_append_format(
                    output,
                    ",\n"
                    "        %s,\n"
                    "        %s,\n"
                    "        ",
                    g_field_kinds[field->kind_index].emitted_name,
                    g_field_semantics[field->semantic_index].emitted_name
                )
                || !reflection_output_append_float4(output, field->ranges[0])
                || !reflection_output_append_c_string(output, ",\n        ")
                || !reflection_output_append_float4(output, field->ranges[1])
                || !reflection_output_append_c_string(output, ",\n        ")
                || !reflection_output_append_float4(output, field->ranges[2])
                || !reflection_output_append_format(
                    output,
                    ",\n"
                    "        UINT32_C(%u)\n"
                    "    },\n",
                    field->flags
                ))
            {
                return 0;
            }
        }

        if (!reflection_output_append_c_string(output, "};\n\n"))
        {
            return 0;
        }
    }

    if (!reflection_output_append_c_string(
        output,
        "static const Author_Schema_View kalam_generated_author_schemas[] =\n"
        "{\n"
    ))
    {
        return 0;
    }

    for (schema_index = 0; schema_index < database->schema_count; ++schema_index)
    {
        const Reflection_Schema_Record* schema = &database->schemas[schema_index];

        if (!reflection_output_append_c_string(output, "    {\n        ")
            || !reflection_output_append_string_view(output, &schema->schema_id)
            || !reflection_output_append_format(
                output,
                ",\n"
                "        UINT32_C(%u),\n"
                "        ",
                schema->schema_version
            ))
        {
            return 0;
        }

        if (schema->field_count == 0)
        {
            if (!reflection_output_append_c_string(
                output,
                "(const Author_Field_Desc*)0"
            ))
            {
                return 0;
            }
        }
        else
        {
            if (!reflection_output_append_c_string(
                output,
                "kalam_generated_author_fields_"
            )
                || !reflection_output_append_bytes(
                    output,
                    schema->symbol.data,
                    schema->symbol.size
                ))
            {
                return 0;
            }
        }

        if (!reflection_output_append_format(
            output,
            ",\n"
            "        UINT32_C(%u),\n"
            "        UINT32_C(%u)\n"
            "    },\n",
            schema->field_count,
            schema->flags
        ))
        {
            return 0;
        }
    }

    return reflection_output_append_format(
        output,
        "};\n"
        "\n"
        "static const uint32_t kalam_generated_author_schema_count = UINT32_C(%u);\n"
        "\n"
        "#endif /* KALAM_GENERATED_AUTHOR_SCHEMAS_H */\n",
        database->schema_count
    );
}

Reflection_Result reflection_generate(
    const Reflection_Source* sources,
    uint32_t source_count,
    const Reflection_Generate_Options* options,
    Reflection_Output* output,
    Reflection_Diagnostic* diagnostic
)
{
    Reflection_Database database = {};
    Reflection_Hash_Function hash_function = reflection_fnv1a64;
    void* hash_user = 0;
    Reflection_Result result = Reflection_Result_Ok;
    uint32_t source_index = 0;

    reflection_diagnostic_clear(diagnostic);

    if ((sources == 0) || (source_count == 0) || (output == 0))
    {
        return reflection_fail(
            Reflection_Result_Invalid_Argument,
            diagnostic,
            "",
            0,
            0,
            "reflection_generate requires sources and an output buffer"
        );
    }

    if ((options != 0) && (options->hash_function != 0))
    {
        hash_function = options->hash_function;
        hash_user = options->hash_user;
    }

    for (source_index = 0; source_index < source_count; ++source_index)
    {
        const Reflection_Source* source = &sources[source_index];
        if ((source->path == 0)
            || (source->text == 0)
            || ((source->text_size != 0)
                && !reflection_u64_fits_allocation(source->text_size)))
        {
            result = reflection_fail(
                Reflection_Result_Invalid_Argument,
                diagnostic,
                "",
                0,
                0,
                "source %u has invalid path or text",
                source_index
            );
            break;
        }

        result = reflection_parse_source(
            source,
            &database,
            hash_function,
            hash_user,
            diagnostic
        );
        if (result != Reflection_Result_Ok)
        {
            break;
        }
    }

    if ((result == Reflection_Result_Ok) && (database.schema_count == 0))
    {
        result = reflection_fail(
            Reflection_Result_Parse_Error,
            diagnostic,
            sources[0].path,
            0,
            0,
            "no @kalam schema annotations were found"
        );
    }

    if (result == Reflection_Result_Ok)
    {
        reflection_sort_database(&database);
        if (!reflection_emit_database(&database, output))
        {
            result = reflection_fail(
                Reflection_Result_Out_Of_Memory,
                diagnostic,
                "",
                0,
                0,
                "out of memory while emitting generated descriptors"
            );
        }
    }

    if (result != Reflection_Result_Ok)
    {
        output->size = 0;
        if (output->data != 0)
        {
            output->data[0] = 0;
        }
    }

    reflection_database_release(&database);
    return result;
}

static Reflection_Result reflection_existing_file_matches(
    const char* path,
    const char* data,
    uint64_t size,
    uint32_t* out_matches,
    Reflection_Diagnostic* diagnostic
)
{
    FILE* file = 0;
    uint64_t offset = 0;
    unsigned char chunk[4096];

    *out_matches = 0;
    errno = 0;
    file = fopen(path, "rb");
    if (file == 0)
    {
        if (errno == ENOENT)
        {
            return Reflection_Result_Ok;
        }
        return reflection_fail(
            Reflection_Result_Io_Error,
            diagnostic,
            path,
            0,
            0,
            "could not open existing output for comparison (errno %d)",
            errno
        );
    }

    while (offset < size)
    {
        uint64_t remaining = size - offset;
        size_t requested = remaining < sizeof(chunk)
            ? (size_t)remaining
            : sizeof(chunk);
        size_t read_size = fread(chunk, 1, requested, file);

        if (read_size != requested)
        {
            if (ferror(file))
            {
                int read_errno = errno;
                fclose(file);
                return reflection_fail(
                    Reflection_Result_Io_Error,
                    diagnostic,
                    path,
                    0,
                    0,
                    "failed reading existing output (errno %d)",
                    read_errno
                );
            }

            fclose(file);
            return Reflection_Result_Ok;
        }

        if (memcmp(chunk, data + offset, read_size) != 0)
        {
            fclose(file);
            return Reflection_Result_Ok;
        }
        offset += read_size;
    }

    if (fgetc(file) != EOF)
    {
        fclose(file);
        return Reflection_Result_Ok;
    }
    if (ferror(file))
    {
        int read_errno = errno;
        fclose(file);
        return reflection_fail(
            Reflection_Result_Io_Error,
            diagnostic,
            path,
            0,
            0,
            "failed checking existing output length (errno %d)",
            read_errno
        );
    }

    fclose(file);
    *out_matches = 1;
    return Reflection_Result_Ok;
}

#if defined(_WIN32)

static Reflection_Result reflection_atomic_replace_windows(
    const char* output_path,
    const char* data,
    uint64_t size,
    Reflection_Diagnostic* diagnostic
)
{
    static LONG temporary_counter = 0;
    uint64_t path_size = (uint64_t)strlen(output_path);
    char* temporary_path = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    uint32_t attempt = 0;
    LONG counter = InterlockedIncrement(&temporary_counter);

    if (!reflection_u64_fits_allocation(path_size + 80))
    {
        return reflection_fail(
            Reflection_Result_Out_Of_Memory,
            diagnostic,
            output_path,
            0,
            0,
            "output path is too large"
        );
    }

    temporary_path = (char*)malloc((size_t)(path_size + 80));
    if (temporary_path == 0)
    {
        return reflection_fail(
            Reflection_Result_Out_Of_Memory,
            diagnostic,
            output_path,
            0,
            0,
            "out of memory while creating temporary output path"
        );
    }

    for (attempt = 0; attempt < 100; ++attempt)
    {
        snprintf(
            temporary_path,
            (size_t)(path_size + 80),
            "%s.tmp.%lu.%ld.%u",
            output_path,
            (unsigned long)GetCurrentProcessId(),
            (long)counter,
            attempt
        );

        file = CreateFileA(
            temporary_path,
            GENERIC_WRITE,
            0,
            0,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            0
        );
        if (file != INVALID_HANDLE_VALUE)
        {
            break;
        }
        if ((GetLastError() != ERROR_FILE_EXISTS)
            && (GetLastError() != ERROR_ALREADY_EXISTS))
        {
            DWORD error = GetLastError();
            free(temporary_path);
            return reflection_fail(
                Reflection_Result_Io_Error,
                diagnostic,
                output_path,
                0,
                0,
                "could not create same-directory temporary output (Win32 error %lu)",
                (unsigned long)error
            );
        }
    }

    if (file == INVALID_HANDLE_VALUE)
    {
        free(temporary_path);
        return reflection_fail(
            Reflection_Result_Io_Error,
            diagnostic,
            output_path,
            0,
            0,
            "could not allocate a unique temporary output name"
        );
    }

    {
        uint64_t offset = 0;
        while (offset < size)
        {
            uint64_t remaining = size - offset;
            DWORD requested = remaining > UINT32_C(0x40000000)
                ? UINT32_C(0x40000000)
                : (DWORD)remaining;
            DWORD written = 0;

            if (!WriteFile(file, data + offset, requested, &written, 0)
                || (written != requested))
            {
                DWORD error = GetLastError();
                CloseHandle(file);
                DeleteFileA(temporary_path);
                free(temporary_path);
                return reflection_fail(
                    Reflection_Result_Io_Error,
                    diagnostic,
                    output_path,
                    0,
                    0,
                    "failed writing temporary output (Win32 error %lu)",
                    (unsigned long)error
                );
            }
            offset += written;
        }
    }

    if (!FlushFileBuffers(file))
    {
        DWORD error = GetLastError();
        CloseHandle(file);
        DeleteFileA(temporary_path);
        free(temporary_path);
        return reflection_fail(
            Reflection_Result_Io_Error,
            diagnostic,
            output_path,
            0,
            0,
            "failed flushing temporary output (Win32 error %lu)",
            (unsigned long)error
        );
    }
    if (!CloseHandle(file))
    {
        DWORD error = GetLastError();
        DeleteFileA(temporary_path);
        free(temporary_path);
        return reflection_fail(
            Reflection_Result_Io_Error,
            diagnostic,
            output_path,
            0,
            0,
            "failed closing temporary output (Win32 error %lu)",
            (unsigned long)error
        );
    }

    if (!MoveFileExA(
        temporary_path,
        output_path,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
    ))
    {
        DWORD error = GetLastError();
        DeleteFileA(temporary_path);
        free(temporary_path);
        return reflection_fail(
            Reflection_Result_Io_Error,
            diagnostic,
            output_path,
            0,
            0,
            "failed atomically replacing output (Win32 error %lu)",
            (unsigned long)error
        );
    }

    free(temporary_path);
    return Reflection_Result_Ok;
}

#else

static Reflection_Result reflection_atomic_replace_posix(
    const char* output_path,
    const char* data,
    uint64_t size,
    Reflection_Diagnostic* diagnostic
)
{
    static uint32_t temporary_counter = 0;
    uint64_t path_size = (uint64_t)strlen(output_path);
    char* temporary_path = 0;
    int file = -1;
    uint32_t attempt = 0;
    uint32_t counter = ++temporary_counter;

    if (!reflection_u64_fits_allocation(path_size + 80))
    {
        return reflection_fail(
            Reflection_Result_Out_Of_Memory,
            diagnostic,
            output_path,
            0,
            0,
            "output path is too large"
        );
    }

    temporary_path = (char*)malloc((size_t)(path_size + 80));
    if (temporary_path == 0)
    {
        return reflection_fail(
            Reflection_Result_Out_Of_Memory,
            diagnostic,
            output_path,
            0,
            0,
            "out of memory while creating temporary output path"
        );
    }

    for (attempt = 0; attempt < 100; ++attempt)
    {
        snprintf(
            temporary_path,
            (size_t)(path_size + 80),
            "%s.tmp.%ld.%u.%u",
            output_path,
            (long)getpid(),
            counter,
            attempt
        );
        file = open(temporary_path, O_CREAT | O_EXCL | O_WRONLY, 0666);
        if (file >= 0)
        {
            break;
        }
        if (errno != EEXIST)
        {
            int open_errno = errno;
            free(temporary_path);
            return reflection_fail(
                Reflection_Result_Io_Error,
                diagnostic,
                output_path,
                0,
                0,
                "could not create same-directory temporary output (errno %d)",
                open_errno
            );
        }
    }

    if (file < 0)
    {
        free(temporary_path);
        return reflection_fail(
            Reflection_Result_Io_Error,
            diagnostic,
            output_path,
            0,
            0,
            "could not allocate a unique temporary output name"
        );
    }

    {
        uint64_t offset = 0;
        while (offset < size)
        {
            uint64_t remaining = size - offset;
            size_t requested = remaining > (uint64_t)SSIZE_MAX
                ? (size_t)SSIZE_MAX
                : (size_t)remaining;
            ssize_t written = write(file, data + offset, requested);
            if (written <= 0)
            {
                int write_errno = errno;
                close(file);
                unlink(temporary_path);
                free(temporary_path);
                return reflection_fail(
                    Reflection_Result_Io_Error,
                    diagnostic,
                    output_path,
                    0,
                    0,
                    "failed writing temporary output (errno %d)",
                    write_errno
                );
            }
            offset += (uint64_t)written;
        }
    }

    if (fsync(file) != 0)
    {
        int sync_errno = errno;
        close(file);
        unlink(temporary_path);
        free(temporary_path);
        return reflection_fail(
            Reflection_Result_Io_Error,
            diagnostic,
            output_path,
            0,
            0,
            "failed flushing temporary output (errno %d)",
            sync_errno
        );
    }
    if (close(file) != 0)
    {
        int close_errno = errno;
        unlink(temporary_path);
        free(temporary_path);
        return reflection_fail(
            Reflection_Result_Io_Error,
            diagnostic,
            output_path,
            0,
            0,
            "failed closing temporary output (errno %d)",
            close_errno
        );
    }
    if (rename(temporary_path, output_path) != 0)
    {
        int rename_errno = errno;
        unlink(temporary_path);
        free(temporary_path);
        return reflection_fail(
            Reflection_Result_Io_Error,
            diagnostic,
            output_path,
            0,
            0,
            "failed atomically replacing output (errno %d)",
            rename_errno
        );
    }

    free(temporary_path);
    return Reflection_Result_Ok;
}

#endif

Reflection_Result reflection_write_if_changed_atomic(
    const char* output_path,
    const char* data,
    uint64_t size,
    uint32_t* out_changed,
    Reflection_Diagnostic* diagnostic
)
{
    Reflection_Result result = Reflection_Result_Ok;
    uint32_t matches = 0;

    reflection_diagnostic_clear(diagnostic);

    if (out_changed != 0)
    {
        *out_changed = 0;
    }
    if ((output_path == 0)
        || (output_path[0] == 0)
        || ((data == 0) && (size != 0))
        || (out_changed == 0))
    {
        return reflection_fail(
            Reflection_Result_Invalid_Argument,
            diagnostic,
            output_path == 0 ? "" : output_path,
            0,
            0,
            "invalid atomic-write arguments"
        );
    }

    result = reflection_existing_file_matches(
        output_path,
        data,
        size,
        &matches,
        diagnostic
    );
    if (result != Reflection_Result_Ok)
    {
        return result;
    }
    if (matches)
    {
        return Reflection_Result_Ok;
    }

#if defined(_WIN32)
    result = reflection_atomic_replace_windows(output_path, data, size, diagnostic);
#else
    result = reflection_atomic_replace_posix(output_path, data, size, diagnostic);
#endif

    if (result == Reflection_Result_Ok)
    {
        *out_changed = 1;
    }
    return result;
}
