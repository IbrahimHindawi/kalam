#ifndef KALAM_ENGINE_CORE_STRING_VIEW_H
#define KALAM_ENGINE_CORE_STRING_VIEW_H

#include "core_types.h"

typedef struct string_view
{
    const char* data;
    usize size;
} string_view;

static inline string_view
string_view_make(const char* data, usize size)
{
    string_view result;
    result.data = data;
    result.size = size;
    return result;
}

static inline string_view
string_view_empty(void)
{
    return string_view_make(0, 0);
}

static inline b32
string_view_is_valid(string_view value)
{
    return value.size == 0 || value.data != 0;
}

static inline string_view
string_view_from_c_string(const char* c_string)
{
    usize size = 0;

    if (c_string == 0)
    {
        return string_view_empty();
    }

    while (c_string[size] != '\0')
    {
        size += 1;
    }

    return string_view_make(c_string, size);
}

static inline b32
string_view_equal(string_view a, string_view b)
{
    usize index = 0;

    if (!string_view_is_valid(a) || !string_view_is_valid(b) || a.size != b.size)
    {
        return CORE_FALSE;
    }

    for (index = 0; index < a.size; index += 1)
    {
        if (a.data[index] != b.data[index])
        {
            return CORE_FALSE;
        }
    }

    return CORE_TRUE;
}

static inline b32
string_view_subview(
    string_view value,
    usize first,
    usize count,
    string_view* out_subview)
{
    usize end = 0;

    if (out_subview == 0)
    {
        return CORE_FALSE;
    }

    *out_subview = string_view_empty();

    if (!string_view_is_valid(value) ||
        !core_checked_add_usize(first, count, &end) ||
        end > value.size)
    {
        return CORE_FALSE;
    }

    if (count == 0)
    {
        out_subview->data = value.data == 0 ? 0 : value.data + first;
        return CORE_TRUE;
    }

    out_subview->data = value.data + first;
    out_subview->size = count;
    return CORE_TRUE;
}

#endif
