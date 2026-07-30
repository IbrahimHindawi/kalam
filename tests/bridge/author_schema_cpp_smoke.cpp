#include "author_schema.h"

#include <cstddef>
#include <cstdint>

static_assert(sizeof(std::uint32_t) == 4, "uint32_t must be 32 bits");
static_assert(sizeof(std::uint64_t) == 8, "uint64_t must be 64 bits");
static_assert(sizeof(float) == 4, "float must be 32 bits");

static_assert(Author_Field_F32 == 0, "field kind values are ABI");
static_assert(Author_Field_Asset_Reference == 12, "field kind values are ABI");
static_assert(Author_Semantic_None == 0, "semantic values are ABI");
static_assert(Author_Semantic_Asset == 8, "semantic values are ABI");
static_assert(
    Author_Schema_Flag_Full_Form_Supported == 1,
    "schema flag values are ABI");

static_assert(offsetof(String_View, data) == 0, "String_View.data offset");
static_assert(offsetof(String_View, size) == 8, "String_View.size offset");
static_assert(sizeof(String_View) == 16, "String_View size");
static_assert(alignof(String_View) == 8, "String_View alignment");

static_assert(sizeof(Author_Document_Id) == 8, "document ID size");
static_assert(sizeof(Author_Object_Id) == 8, "object ID size");
static_assert(sizeof(Author_Field_Id) == 8, "field ID size");
static_assert(sizeof(Author_Field_Kind) == 4, "field kind size");
static_assert(sizeof(Author_Field_Semantic) == 4, "field semantic size");

static_assert(offsetof(Author_Transform_Value, translation) == 0, "transform translation offset");
static_assert(offsetof(Author_Transform_Value, rotation_degrees) == 12, "transform rotation offset");
static_assert(offsetof(Author_Transform_Value, scale) == 24, "transform scale offset");
static_assert(sizeof(Author_Transform_Value) == 36, "transform size");
static_assert(alignof(Author_Transform_Value) == 4, "transform alignment");

static_assert(offsetof(Author_Value, kind) == 0, "Author_Value.kind offset");
static_assert(offsetof(Author_Value, reserved) == 4, "Author_Value.reserved offset");
static_assert(offsetof(Author_Value, data) == 8, "Author_Value.data offset");
static_assert(sizeof(Author_Value::data) == 40, "Author_Value union size");
static_assert(sizeof(Author_Value) == 48, "Author_Value size");
static_assert(alignof(Author_Value) == 8, "Author_Value alignment");

static_assert(offsetof(Author_Field_Desc, id) == 0, "field id offset");
static_assert(offsetof(Author_Field_Desc, name) == 8, "field name offset");
static_assert(offsetof(Author_Field_Desc, json_path) == 24, "field path offset");
static_assert(offsetof(Author_Field_Desc, group) == 40, "field group offset");
static_assert(offsetof(Author_Field_Desc, kind) == 56, "field kind offset");
static_assert(offsetof(Author_Field_Desc, semantic) == 60, "field semantic offset");
static_assert(offsetof(Author_Field_Desc, minimum) == 64, "field minimum offset");
static_assert(offsetof(Author_Field_Desc, maximum) == 80, "field maximum offset");
static_assert(offsetof(Author_Field_Desc, step) == 96, "field step offset");
static_assert(offsetof(Author_Field_Desc, flags) == 112, "field flags offset");
static_assert(sizeof(Author_Field_Desc) == 120, "field descriptor size");
static_assert(alignof(Author_Field_Desc) == 8, "field descriptor alignment");

static_assert(offsetof(Author_Schema_View, schema_id) == 0, "schema id offset");
static_assert(offsetof(Author_Schema_View, schema_version) == 16, "schema version offset");
#if UINTPTR_MAX == UINT64_MAX
static_assert(offsetof(Author_Schema_View, fields) == 24, "schema fields offset");
static_assert(offsetof(Author_Schema_View, field_count) == 32, "schema field count offset");
static_assert(offsetof(Author_Schema_View, flags) == 36, "schema flags offset");
static_assert(sizeof(Author_Schema_View) == 40, "schema view size");
static_assert(alignof(Author_Schema_View) == 8, "schema view alignment");
#elif UINTPTR_MAX == UINT32_MAX
static_assert(offsetof(Author_Schema_View, fields) == 20, "schema fields offset");
static_assert(offsetof(Author_Schema_View, field_count) == 24, "schema field count offset");
static_assert(offsetof(Author_Schema_View, flags) == 28, "schema flags offset");
static_assert(sizeof(Author_Schema_View) == 32, "schema view size");
static_assert(alignof(Author_Schema_View) == 8, "schema view alignment");
#else
#error Unsupported pointer size
#endif

static_assert(offsetof(Author_Object_Source, object_id) == 0, "object id offset");
static_assert(offsetof(Author_Object_Source, source_id) == 8, "source id offset");
static_assert(offsetof(Author_Object_Source, json_path) == 24, "object path offset");
static_assert(sizeof(Author_Object_Source) == 40, "object source size");
static_assert(alignof(Author_Object_Source) == 8, "object source alignment");

static_assert(offsetof(Author_Document_State, struct_size) == 0, "document size offset");
static_assert(offsetof(Author_Document_State, flags) == 4, "document flags offset");
static_assert(offsetof(Author_Document_State, document_id) == 8, "document id offset");
static_assert(offsetof(Author_Document_State, root_object_id) == 16, "root object offset");
static_assert(offsetof(Author_Document_State, revision) == 24, "document revision offset");
static_assert(offsetof(Author_Document_State, schema_id) == 32, "document schema offset");
static_assert(offsetof(Author_Document_State, project_relative_path) == 48, "document path offset");
static_assert(offsetof(Author_Document_State, json_utf8) == 64, "document JSON offset");
static_assert(offsetof(Author_Document_State, objects) == 80, "document objects offset");
#if UINTPTR_MAX == UINT64_MAX
static_assert(offsetof(Author_Document_State, object_count) == 88, "document object count offset");
static_assert(offsetof(Author_Document_State, reserved) == 92, "document reserved offset");
#elif UINTPTR_MAX == UINT32_MAX
static_assert(offsetof(Author_Document_State, object_count) == 84, "document object count offset");
static_assert(offsetof(Author_Document_State, reserved) == 88, "document reserved offset");
#else
#error Unsupported pointer size
#endif
static_assert(sizeof(Author_Document_State) == 96, "document state size");
static_assert(alignof(Author_Document_State) == 8, "document state alignment");

int main()
{
    return 0;
}
