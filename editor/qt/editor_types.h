#ifndef KALAM_EDITOR_QT_EDITOR_TYPES_H
#define KALAM_EDITOR_QT_EDITOR_TYPES_H

#include <QByteArray>
#include <QMetaType>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

#include "author_schema.h"

namespace Kalam::Editor
{

struct Cursor_Geometry
{
    qint64 grid = 1;
    qint64 row = 0;
    qint64 column = 0;
    QSize cell_size;
    QPoint grid_origin;
    QRect cursor_rect;
};

struct Source_Range
{
    qint64 start_row = 0;
    qint64 start_column = 0;
    qint64 end_row = 0;
    qint64 end_column = 0;

    bool is_valid() const
    {
        return start_row >= 0 &&
            start_column >= 0 &&
            end_row >= start_row &&
            end_column >= 0;
    }
};

struct Buffer_Snapshot
{
    qint64 buffer = 0;
    quint64 changedtick = 0;
    QString path;
    QByteArray text_utf8;
};

struct Source_Context
{
    qint64 buffer = 0;
    quint64 changedtick = 0;
    QString json_path;
    QByteArray node_type;
    Source_Range range;
    QByteArray source_text_utf8;
    QString document_path;
    QByteArray document_text_utf8;
    bool range_fallback = false;
};

struct Diagnostic
{
    QString file;
    int line = 0;
    int column = 0;
    QString type;
    QString code;
    QString message;
};

struct Field_Descriptor
{
    Author_Field_Id id = 0;
    QByteArray name;
    QByteArray json_path;
    QByteArray group;
    Author_Field_Kind kind = Author_Field_F32;
    Author_Field_Semantic semantic = Author_Semantic_None;
    float minimum[4] = {};
    float maximum[4] = {};
    float step[4] = {};
    quint32 flags = 0;
};

struct Schema_Descriptor
{
    QByteArray schema_id;
    quint32 schema_version = 0;
    quint32 flags = 0;
    QVector<Field_Descriptor> fields;
};

/*
 * Qt signals must not carry Author_Value directly because its string member
 * is a borrowed ABI view. This value owns any string payload and converts to
 * an Author_Value only for the duration of one synchronous module call.
 */
struct Author_Value_Owned
{
    Author_Field_Kind kind = Author_Field_F32;
    float f32[4] = {};
    qint32 i32 = 0;
    bool boolean = false;
    qint64 enum_value = 0;
    QByteArray string_utf8;
    Author_Transform_Value transform = {};

    Author_Value borrow() const
    {
        Author_Value value = {};
        value.kind = kind;
        switch (kind)
        {
        case Author_Field_I32:
            value.data.i32 = i32;
            break;
        case Author_Field_Bool:
            value.data.boolean = boolean ? 1u : 0u;
            break;
        case Author_Field_Enum:
            value.data.enum_value = enum_value;
            break;
        case Author_Field_String:
        case Author_Field_Asset_Reference:
            value.data.string.data = string_utf8.constData();
            value.data.string.size =
                static_cast<uint64_t>(string_utf8.size());
            break;
        case Author_Field_Transform:
            value.data.transform = transform;
            break;
        default:
            for (int index = 0; index < 4; ++index)
            {
                value.data.f32[index] = f32[index];
            }
            break;
        }
        return value;
    }

    static Author_Value_Owned copy_of(const Author_Value& value)
    {
        Author_Value_Owned result;
        result.kind = value.kind;
        switch (value.kind)
        {
        case Author_Field_I32:
            result.i32 = value.data.i32;
            break;
        case Author_Field_Bool:
            result.boolean = value.data.boolean != 0;
            break;
        case Author_Field_Enum:
            result.enum_value = value.data.enum_value;
            break;
        case Author_Field_String:
        case Author_Field_Asset_Reference:
            if (value.data.string.data != nullptr &&
                value.data.string.size != 0)
            {
                result.string_utf8 = QByteArray(
                    value.data.string.data,
                    static_cast<qsizetype>(value.data.string.size));
            }
            break;
        case Author_Field_Transform:
            result.transform = value.data.transform;
            break;
        default:
            for (int index = 0; index < 4; ++index)
            {
                result.f32[index] = value.data.f32[index];
            }
            break;
        }
        return result;
    }
};

struct Document_Handle
{
    Author_Document_Id document_id = 0;
    Author_Object_Id root_object_id = 0;
    quint64 revision = 0;
    qint64 buffer = 0;
    quint64 changedtick = 0;
    QByteArray schema_id;
    QString path;

    bool is_valid() const
    {
        return document_id != 0 && root_object_id != 0;
    }
};

struct Viewport_Interaction_Result
{
    quint32 flags = 0;
    Author_Document_Id document_id = 0;
    Author_Object_Id object_id = 0;
    Author_Field_Id field_id = 0;
    Author_Value_Owned value;
};

} // namespace Kalam::Editor

Q_DECLARE_METATYPE(Kalam::Editor::Cursor_Geometry)
Q_DECLARE_METATYPE(Kalam::Editor::Buffer_Snapshot)
Q_DECLARE_METATYPE(Kalam::Editor::Source_Context)
Q_DECLARE_METATYPE(Kalam::Editor::Diagnostic)
Q_DECLARE_METATYPE(Kalam::Editor::Author_Value_Owned)
Q_DECLARE_METATYPE(Kalam::Editor::Viewport_Interaction_Result)

#endif
