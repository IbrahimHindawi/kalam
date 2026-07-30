#ifndef KALAM_REFLECTION_TOOL_H
#define KALAM_REFLECTION_TOOL_H

#include <stdint.h>

typedef uint32_t Reflection_Result;

enum
{
    Reflection_Result_Ok = 0,
    Reflection_Result_Invalid_Argument = 1,
    Reflection_Result_Out_Of_Memory = 2,
    Reflection_Result_Parse_Error = 3,
    Reflection_Result_Duplicate_Schema_Id = 4,
    Reflection_Result_Duplicate_Schema_Symbol = 5,
    Reflection_Result_Duplicate_Field_Id = 6,
    Reflection_Result_Fnv64_Collision = 7,
    Reflection_Result_Invalid_Field_Id = 8,
    Reflection_Result_Io_Error = 9
};

enum
{
    Reflection_Diagnostic_Path_Capacity = 512,
    Reflection_Diagnostic_Message_Capacity = 512
};

typedef struct Reflection_Diagnostic
{
    uint32_t line;
    uint32_t column;
    char path[Reflection_Diagnostic_Path_Capacity];
    char message[Reflection_Diagnostic_Message_Capacity];
} Reflection_Diagnostic;

typedef struct Reflection_Source
{
    const char* path;
    const char* text;
    uint64_t text_size;
} Reflection_Source;

typedef struct Reflection_Output
{
    char* data;
    uint64_t size;
    uint64_t capacity;
} Reflection_Output;

typedef uint64_t (*Reflection_Hash_Function)(
    const char* data,
    uint64_t size,
    void* user
);

typedef struct Reflection_Generate_Options
{
    Reflection_Hash_Function hash_function;
    void* hash_user;
} Reflection_Generate_Options;

void reflection_output_init(Reflection_Output* output);
void reflection_output_release(Reflection_Output* output);

uint64_t reflection_fnv1a64(
    const char* data,
    uint64_t size,
    void* user
);

Reflection_Result reflection_generate(
    const Reflection_Source* sources,
    uint32_t source_count,
    const Reflection_Generate_Options* options,
    Reflection_Output* output,
    Reflection_Diagnostic* diagnostic
);

/*
 * Writes a temporary file beside output_path, flushes it, and atomically
 * replaces output_path. If the existing bytes already match, no file is
 * opened for writing and *out_changed is zero.
 */
Reflection_Result reflection_write_if_changed_atomic(
    const char* output_path,
    const char* data,
    uint64_t size,
    uint32_t* out_changed,
    Reflection_Diagnostic* diagnostic
);

#endif /* KALAM_REFLECTION_TOOL_H */
