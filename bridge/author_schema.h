#ifndef KALAM_BRIDGE_AUTHOR_SCHEMA_H
#define KALAM_BRIDGE_AUTHOR_SCHEMA_H

#include <stdint.h>

/*
 * All pointers in this header are borrowed views. The owner and lifetime are
 * specified by the API call that supplies the containing structure.
 */
typedef struct String_View
{
    const char* data;
    uint64_t size;
} String_View;

/*
 * Authoring IDs are nonzero, opaque values. They are not pointers, descriptor
 * indices, JSON array positions, runtime handles, or serialized enum ordinals.
 */
typedef uint64_t Author_Document_Id;
typedef uint64_t Author_Object_Id;
typedef uint64_t Author_Field_Id;

typedef uint32_t Author_Field_Kind;

enum
{
    Author_Field_F32 = 0,
    Author_Field_I32 = 1,
    Author_Field_Bool = 2,
    Author_Field_Vec2 = 3,
    Author_Field_Vec3 = 4,
    Author_Field_Vec4 = 5,
    Author_Field_Color3 = 6,
    Author_Field_Color4 = 7,
    Author_Field_Euler_Degrees3 = 8,
    Author_Field_Transform = 9,
    Author_Field_Enum = 10,
    Author_Field_String = 11,
    Author_Field_Asset_Reference = 12
};

typedef uint32_t Author_Field_Semantic;

enum
{
    Author_Semantic_None = 0,
    Author_Semantic_Color = 1,
    Author_Semantic_Position = 2,
    Author_Semantic_Rotation = 3,
    Author_Semantic_Scale = 4,
    Author_Semantic_Transform = 5,
    Author_Semantic_Angle = 6,
    Author_Semantic_Percentage = 7,
    Author_Semantic_Asset = 8
};

enum
{
    Author_Schema_Flag_Full_Form_Supported = 1u << 0,
    Author_Schema_Flag_Context_Edit_Supported = 1u << 1,
    Author_Schema_Flag_Viewport_Gizmo_Supported = 1u << 2
};

enum
{
    Author_Field_Flag_Context_Edit_Supported = 1u << 0,
    Author_Field_Flag_Viewport_Gizmo_Supported = 1u << 1
};

typedef struct Author_Transform_Value
{
    float translation[3];
    float rotation_degrees[3];
    float scale[3];
} Author_Transform_Value;

typedef struct Author_Value
{
    Author_Field_Kind kind;
    uint32_t reserved;

    union
    {
        float f32[4];
        int32_t i32;
        uint32_t boolean;
        int64_t enum_value;
        String_View string;
        Author_Transform_Value transform;
    } data;
} Author_Value;

typedef struct Author_Field_Desc
{
    Author_Field_Id id;

    String_View name;
    String_View json_path;
    String_View group;

    Author_Field_Kind kind;
    Author_Field_Semantic semantic;

    float minimum[4];
    float maximum[4];
    float step[4];

    uint32_t flags;
} Author_Field_Desc;

/*
 * A module owns this view. The host must deep-copy the descriptor array and
 * every nested String_View before another module call or module unload.
 */
typedef struct Author_Schema_View
{
    String_View schema_id;
    uint32_t schema_version;

    const Author_Field_Desc* fields;
    uint32_t field_count;

    uint32_t flags;
} Author_Schema_View;

typedef struct Author_Object_Source
{
    Author_Object_Id object_id;
    String_View source_id;
    String_View json_path;
} Author_Object_Source;

/*
 * A complete, caller-owned UTF-8 document snapshot. Every pointer and view is
 * valid only for the duration of Game_API.apply_document. The root object must
 * be present in objects and have json_path "$".
 */
typedef struct Author_Document_State
{
    uint32_t struct_size;
    uint32_t flags;

    Author_Document_Id document_id;
    Author_Object_Id root_object_id;
    uint64_t revision;

    String_View schema_id;
    String_View project_relative_path;
    String_View json_utf8;

    const Author_Object_Source* objects;
    uint32_t object_count;
    uint32_t reserved;
} Author_Document_State;

#endif /* KALAM_BRIDGE_AUTHOR_SCHEMA_H */
