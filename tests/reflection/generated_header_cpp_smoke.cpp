#include "author_schemas_fixture.generated.h"

#include <cstdint>

static_assert(
    (sizeof(kalam_generated_author_fields_Scene_Fixture)
        / sizeof(kalam_generated_author_fields_Scene_Fixture[0])) == 2,
    "scene fixture field count"
);
static_assert(
    (sizeof(kalam_generated_author_fields_Post_Process_Fixture)
        / sizeof(kalam_generated_author_fields_Post_Process_Fixture[0])) == 1,
    "post-process fixture field count"
);
static_assert(
    kalam_generated_author_schema_count == UINT32_C(2),
    "fixture schema count"
);

int main()
{
    if (kalam_generated_author_schemas[0].field_count != UINT32_C(1))
    {
        return 1;
    }
    if (kalam_generated_author_schemas[1].field_count != UINT32_C(2))
    {
        return 2;
    }
    return 0;
}
