#include "author_transaction_controller.h"

#include "editor_neovim_adapter.h"
#include "editor_runtime_host.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QtMath>


namespace Kalam::Editor
{

namespace
{

int context_component_count(Author_Field_Kind kind)
{
    switch (kind)
    {
    case Author_Field_Vec2: return 2;
    case Author_Field_Vec3:
    case Author_Field_Color3:
    case Author_Field_Euler_Degrees3:
        return 3;
    case Author_Field_Vec4:
    case Author_Field_Color4:
        return 4;
    default:
        return 1;
    }
}

bool json_float(const QJsonValue& json, float* value)
{
    if (!json.isDouble() || !qIsFinite(json.toDouble()) || value == nullptr)
    {
        return false;
    }
    const double number = json.toDouble();
    const float converted = static_cast<float>(number);
    if (!qIsFinite(converted)) return false;
    *value = converted;
    return true;
}

bool json_integer(const QJsonValue& json, qint64 minimum, qint64 maximum, qint64* value)
{
    if (!json.isDouble() || value == nullptr)
    {
        return false;
    }
    const double number = json.toDouble();
    if (!qIsFinite(number) || qFloor(number) != number ||
        number < static_cast<double>(minimum) ||
        number > static_cast<double>(maximum))
    {
        return false;
    }
    *value = static_cast<qint64>(number);
    return true;
}

bool json_float_array(
    const QJsonValue& json,
    int count,
    float* values)
{
    if (!json.isArray() || json.toArray().size() != count)
    {
        return false;
    }
    const QJsonArray array = json.toArray();
    for (int index = 0; index < count; ++index)
    {
        if (!json_float(array.at(index), &values[index]))
        {
            return false;
        }
    }
    return true;
}

bool context_value_from_json(
    const Field_Descriptor& field,
    const QJsonValue& json,
    Author_Value_Owned* value)
{
    if (value == nullptr)
    {
        return false;
    }
    *value = {};
    value->kind = field.kind;
    if (field.kind == Author_Field_Bool)
    {
        if (!json.isBool()) return false;
        value->boolean = json.toBool();
        return true;
    }
    if (field.kind == Author_Field_String ||
        field.kind == Author_Field_Asset_Reference)
    {
        if (!json.isString()) return false;
        value->string_utf8 = json.toString().toUtf8();
        return true;
    }
    if (field.kind == Author_Field_I32 || field.kind == Author_Field_Enum)
    {
        qint64 integer = 0;
        const qint64 minimum = field.kind == Author_Field_I32
            ? -2147483648LL
            : -9007199254740991LL;
        const qint64 maximum = field.kind == Author_Field_I32
            ? 2147483647LL
            : 9007199254740991LL;
        if (!json_integer(json, minimum, maximum, &integer)) return false;
        if (field.kind == Author_Field_I32)
            value->i32 = static_cast<qint32>(integer);
        else
            value->enum_value = integer;
        return true;
    }
    if (field.kind == Author_Field_Transform)
    {
        if (!json.isObject()) return false;
        const QJsonObject object = json.toObject();
        if (object.size() != 3 ||
            !json_float_array(object.value(QStringLiteral("translation")), 3,
                              value->transform.translation) ||
            !json_float_array(object.value(QStringLiteral("rotation_degrees")), 3,
                              value->transform.rotation_degrees) ||
            !json_float_array(object.value(QStringLiteral("scale")), 3,
                              value->transform.scale))
        {
            return false;
        }
        return true;
    }

    const int count = context_component_count(field.kind);
    return count == 1
        ? json_float(json, &value->f32[0])
        : json_float_array(json, count, value->f32);
}

} // namespace

Author_Transaction_Controller::Author_Transaction_Controller(
    Editor_Runtime_Host* host,
    Editor_Neovim_Adapter* neovim,
    QObject* parent)
    : QObject(parent), host_(host), neovim_(neovim)
{
    Q_ASSERT(host != nullptr);
    Q_ASSERT(neovim != nullptr);
    connect(
        neovim,
        &Editor_Neovim_Adapter::contextForPathFinished,
        this,
        &Author_Transaction_Controller::context_for_path_finished);
    connect(
        neovim,
        &Editor_Neovim_Adapter::textPatchFinished,
        this,
        &Author_Transaction_Controller::text_patch_finished);
    connect(
        host,
        &Editor_Runtime_Host::aboutToCommitModuleReload,
        this,
        [this]()
        {
            cancel();
        });
}

bool Author_Transaction_Controller::begin_context_edit(
    const Source_Context& context,
    QString* error)
{
    if (is_active())
    {
        cancel();
    }

    Document_Handle document = host_->document_for_buffer(context.buffer);
    if ((!document.is_valid() || document.changedtick != context.changedtick) &&
        !context.document_text_utf8.isEmpty())
    {
        Buffer_Snapshot snapshot;
        snapshot.buffer = context.buffer;
        snapshot.changedtick = context.changedtick;
        snapshot.path = context.document_path;
        snapshot.text_utf8 = context.document_text_utf8;
        if (!host_->accept_valid_document(snapshot, error))
        {
            return false;
        }
        document = host_->document_for_buffer(context.buffer);
    }
    if (!document.is_valid() || document.changedtick != context.changedtick)
    {
        if (error != nullptr)
        {
            *error = QStringLiteral(
                "the validated runtime snapshot does not match the cursor buffer");
        }
        return false;
    }

    QVector<Field_Descriptor> fields;
    if (!populate_fields_for_path(document, context.json_path, &fields, error))
    {
        return false;
    }

    QVector<Author_Value_Owned> values;
    values.reserve(fields.size());
    for (const Field_Descriptor& field : fields)
    {
        Author_Value_Owned value;
        if (!host_->read_author_field(
                document.document_id,
                document.root_object_id,
                field.id,
                &value,
                error))
        {
            return false;
        }
        values.push_back(value);
    }

    transaction_ = {};
    transaction_.origin = Origin::Context;
    transaction_.document = document;
    transaction_.source = context;
    transaction_.target_json_path = context.json_path;
    transaction_.fields = fields;
    transaction_.original_values = values;
    transaction_.current_values = values;
    transaction_.source_is_resolved = true;
    emit contextEditReady(fields, values, context);
    return true;
}

bool Author_Transaction_Controller::begin_form_field_edit(
    const Document_Handle& document,
    const Field_Descriptor& field,
    const Author_Value_Owned& original,
    QString* error)
{
    if (transaction_.origin == Origin::Form &&
        transaction_.document.document_id == document.document_id &&
        transaction_.fields.size() == 1 &&
        transaction_.fields.first().id == field.id &&
        !is_waiting_for_rpc())
    {
        return true;
    }
    if (!document.is_valid())
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("generated form document is unavailable");
        }
        return false;
    }
    if (is_active())
    {
        cancel();
    }

    transaction_ = {};
    transaction_.origin = Origin::Form;
    transaction_.document = document;
    transaction_.target_json_path = QString::fromUtf8(field.json_path);
    transaction_.fields.push_back(field);
    transaction_.original_values.push_back(original);
    transaction_.current_values.push_back(original);
    return true;
}

bool Author_Transaction_Controller::begin_gizmo_edit(
    const Viewport_Interaction_Result& result,
    QString* error)
{
    if ((result.flags & Game_Viewport_Result_Gizmo_Active) == 0)
    {
        return false;
    }
    if (is_active())
    {
        cancel();
    }

    const Document_Handle document = host_->document_by_id(result.document_id);
    Schema_Descriptor schema;
    if (!document.is_valid() ||
        !host_->schema_for_document(document.document_id, &schema))
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("selected authored document has no schema");
        }
        return false;
    }

    Field_Descriptor selected_field;
    bool found = false;
    for (const Field_Descriptor& field : schema.fields)
    {
        if (field.id == result.field_id)
        {
            selected_field = field;
            found = true;
            break;
        }
    }
    if (!found)
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("selected gizmo field is not in the schema");
        }
        return false;
    }

    Author_Value_Owned original;
    if (!host_->read_author_field(
            document.document_id,
            result.object_id,
            result.field_id,
            &original,
            error))
    {
        return false;
    }

    transaction_ = {};
    transaction_.origin = Origin::Gizmo;
    transaction_.document = document;
    transaction_.document.root_object_id = result.object_id;
    transaction_.target_json_path = QString::fromUtf8(selected_field.json_path);
    transaction_.fields.push_back(selected_field);
    transaction_.original_values.push_back(original);
    transaction_.current_values.push_back(original);
    return true;
}

bool Author_Transaction_Controller::preview_field(
    Author_Field_Id field_id,
    const Author_Value_Owned& value,
    QString* error)
{
    const int index = field_index(field_id);
    if (index < 0 || is_waiting_for_rpc())
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("no editable field transaction is active");
        }
        return false;
    }
    if (!host_->preview_author_field(
            transaction_.document.document_id,
            transaction_.document.root_object_id,
            field_id,
            value,
            error))
    {
        return false;
    }
    transaction_.current_values[index] = value;
    return true;
}

void Author_Transaction_Controller::note_gizmo_value(
    const Viewport_Interaction_Result& result)
{
    if (transaction_.origin == Origin::Gizmo &&
        transaction_.document.document_id == result.document_id &&
        !transaction_.current_values.isEmpty())
    {
        transaction_.current_values[0] = result.value;
    }
}

void Author_Transaction_Controller::accept()
{
    if (!is_active() || is_waiting_for_rpc())
    {
        return;
    }
    request_source_or_patch();
}

void Author_Transaction_Controller::accept_gizmo(
    const Viewport_Interaction_Result& result)
{
    if (transaction_.origin != Origin::Gizmo ||
        transaction_.document.document_id != result.document_id ||
        (result.flags & Game_Viewport_Result_Commit_Requested) == 0)
    {
        return;
    }
    note_gizmo_value(result);
    accept();
}

void Author_Transaction_Controller::cancel()
{
    if (!is_active())
    {
        return;
    }

    const Document_Handle latest =
        host_->document_by_id(transaction_.document.document_id);
    if (latest.is_valid() &&
        latest.changedtick == transaction_.document.changedtick &&
        transaction_.origin != Origin::Gizmo &&
        !is_waiting_for_rpc())
    {
        bool restored = true;
        QString restore_error;
        for (qsizetype index = 0;
             index < transaction_.fields.size();
             ++index)
        {
            restored = host_->preview_author_field(
                transaction_.document.document_id,
                transaction_.document.root_object_id,
                transaction_.fields.at(index).id,
                transaction_.original_values.at(index),
                &restore_error) && restored;
        }
        if (!restored)
        {
            cancel_and_rehydrate(restore_error, true);
            return;
        }
        clear_transaction(false);
        return;
    }
    cancel_and_rehydrate(QString(), false);
}

void Author_Transaction_Controller::note_gizmo_cancelled()
{
    if (transaction_.origin == Origin::Gizmo)
    {
        clear_transaction(false);
    }
}

void Author_Transaction_Controller::document_changed(
    const Buffer_Snapshot& snapshot)
{
    if (!is_active() ||
        snapshot.buffer != transaction_.document.buffer ||
        snapshot.changedtick == transaction_.document.changedtick)
    {
        return;
    }
    cancel_and_rehydrate(
        QStringLiteral("buffer changed during the authoring transaction"),
        false);
}

bool Author_Transaction_Controller::is_active() const
{
    return transaction_.origin != Origin::None;
}

bool Author_Transaction_Controller::is_waiting_for_rpc() const
{
    return transaction_.context_request_id != 0 ||
        transaction_.patch_request_id != 0;
}

void Author_Transaction_Controller::context_for_path_finished(
    quint64 request_id,
    bool succeeded,
    Source_Context context,
    QString error)
{
    if (!is_active() || transaction_.context_request_id != request_id)
    {
        return;
    }
    transaction_.context_request_id = 0;
    if (!succeeded ||
        context.buffer != transaction_.document.buffer ||
        context.changedtick != transaction_.document.changedtick ||
        context.json_path != transaction_.target_json_path)
    {
        if (error.isEmpty())
        {
            error = QStringLiteral(
                "buffer changedtick or JSON path no longer matches transaction");
        }
        cancel_and_rehydrate(error, true);
        return;
    }
    transaction_.source = context;
    transaction_.source_is_resolved = true;
    patch_resolved_source();
}

void Author_Transaction_Controller::text_patch_finished(
    quint64 request_id,
    bool succeeded,
    quint64 resulting_changedtick,
    QString error)
{
    if (!is_active() || transaction_.patch_request_id != request_id)
    {
        return;
    }
    transaction_.patch_request_id = 0;
    if (!succeeded || resulting_changedtick <= transaction_.document.changedtick)
    {
        if (error.isEmpty())
        {
            error = QStringLiteral("Neovim returned an unexpected changedtick");
        }
        cancel_and_rehydrate(error, true);
        return;
    }
    clear_transaction(true);
}

bool Author_Transaction_Controller::populate_fields_for_path(
    const Document_Handle& document,
    const QString& json_path,
    QVector<Field_Descriptor>* fields,
    QString* error) const
{
    Schema_Descriptor schema;
    if (!host_->schema_for_document(document.document_id, &schema))
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("no author schema matches the active document");
        }
        return false;
    }

    const QByteArray wanted = json_path.toUtf8();
    for (const Field_Descriptor& field : schema.fields)
    {
        if (field.json_path == wanted &&
            (field.flags & Author_Field_Flag_Context_Edit_Supported) != 0)
        {
            fields->push_back(field);
            return true;
        }
    }

    const QByteArray prefix = wanted + '.';
    for (const Field_Descriptor& field : schema.fields)
    {
        if ((field.flags & Author_Field_Flag_Context_Edit_Supported) == 0 ||
            !field.json_path.startsWith(prefix))
        {
            continue;
        }
        const QByteArray relative = field.json_path.mid(prefix.size());
        if (!relative.contains('.'))
        {
            fields->push_back(field);
        }
    }

    if (fields->isEmpty())
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("the JSON path has no contextual author control");
        }
        return false;
    }
    return true;
}

void Author_Transaction_Controller::request_source_or_patch()
{
    if (transaction_.source_is_resolved)
    {
        patch_resolved_source();
        return;
    }
    transaction_.context_request_id = neovim_->request_context_for_path(
        transaction_.document.buffer,
        transaction_.target_json_path);
}

void Author_Transaction_Controller::patch_resolved_source()
{
    const Document_Handle latest =
        host_->document_by_id(transaction_.document.document_id);
    if (!latest.is_valid() ||
        latest.changedtick != transaction_.document.changedtick ||
        transaction_.source.changedtick != transaction_.document.changedtick ||
        !transaction_.source.range.is_valid())
    {
        cancel_and_rehydrate(
            QStringLiteral("buffer changed before the surgical text patch"),
            true);
        return;
    }

    transaction_.patch_request_id = neovim_->patch_text(
        transaction_.source,
        format_transaction_json());
}

QByteArray Author_Transaction_Controller::format_transaction_json() const
{
    if (transaction_.fields.size() == 1)
    {
        return format_value_json(
            transaction_.fields.first(),
            transaction_.current_values.first());
    }

    QByteArray result("{");
    for (qsizetype index = 0; index < transaction_.fields.size(); ++index)
    {
        if (index != 0)
        {
            result += ',';
        }
        result += quote_json(transaction_.fields.at(index).name);
        result += ':';
        result += format_value_json(
            transaction_.fields.at(index),
            transaction_.current_values.at(index));
    }
    result += '}';
    return result;
}

void Author_Transaction_Controller::cancel_and_rehydrate(
    const QString& reason,
    bool report_error)
{
    QString rehydrate_error;
    const bool restored = host_->rehydrate_document(
        transaction_.document.document_id,
        &rehydrate_error);
    QString final_error = reason;
    if (!restored && !rehydrate_error.isEmpty())
    {
        if (!final_error.isEmpty())
        {
            final_error += QStringLiteral("; ");
        }
        final_error += rehydrate_error;
    }
    clear_transaction(false);
    if ((report_error || !restored) && !final_error.isEmpty())
    {
        emit transactionError(final_error);
    }
}

void Author_Transaction_Controller::clear_transaction(bool accepted)
{
    transaction_ = {};
    emit transactionClosed(accepted);
}

int Author_Transaction_Controller::field_index(
    Author_Field_Id field_id) const
{
    for (qsizetype index = 0; index < transaction_.fields.size(); ++index)
    {
        if (transaction_.fields.at(index).id == field_id)
        {
            return static_cast<int>(index);
        }
    }
    return -1;
}

QByteArray Author_Transaction_Controller::format_value_json(
    const Field_Descriptor& field,
    const Author_Value_Owned& value)
{
    if (field.kind == Author_Field_Transform)
    {
        const Author_Transform_Value& transform = value.transform;
        QByteArray result;
        result += '{';
        result += quote_json(QByteArray("translation"));
        result += ":[";
        for (int component = 0; component < 3; ++component)
        {
            if (component != 0) result += ',';
            result += format_number(transform.translation[component], 3);
        }
        result += "],";
        result += quote_json(QByteArray("rotation_degrees"));
        result += ":[";
        for (int component = 0; component < 3; ++component)
        {
            if (component != 0) result += ',';
            result += format_number(transform.rotation_degrees[component], 3);
        }
        result += "],";
        result += quote_json(QByteArray("scale"));
        result += ":[";
        for (int component = 0; component < 3; ++component)
        {
            if (component != 0) result += ',';
            result += format_number(transform.scale[component], 3);
        }
        result += "]}";
        return result;
    }

    if (field.kind == Author_Field_I32)
    {
        return QByteArray::number(value.i32);
    }
    if (field.kind == Author_Field_Bool)
    {
        return value.boolean ? QByteArray("true") : QByteArray("false");
    }
    if (field.kind == Author_Field_Enum)
    {
        return QByteArray::number(value.enum_value);
    }
    if (field.kind == Author_Field_String ||
        field.kind == Author_Field_Asset_Reference)
    {
        return quote_json(value.string_utf8);
    }

    const int count = component_count(field.kind);
    const int precision = numeric_precision(field.step[0]);
    if (count <= 1)
    {
        return format_number(value.f32[0], precision);
    }

    QByteArray result("[");
    for (int component = 0; component < count; ++component)
    {
        if (component != 0)
        {
            result += ',';
        }
        const int component_precision = numeric_precision(
            field.step[component] > 0.0f
                ? field.step[component]
                : field.step[0]);
        result += format_number(value.f32[component], component_precision);
    }
    result += ']';
    return result;
}

bool Author_Transaction_Controller::parse_context_values(
    const QVector<Field_Descriptor>& fields,
    const QByteArray& text_utf8,
    QVector<Author_Value_Owned>* values,
    QString* error)
{
    if (values == nullptr)
    {
        if (error != nullptr) *error = QStringLiteral("missing context value output");
        return false;
    }
    values->clear();

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(text_utf8, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        if (error != nullptr)
        {
            *error = parse_error.error == QJsonParseError::NoError
                ? QStringLiteral("context buffer must contain a JSON object")
                : QStringLiteral("invalid context JSON: %1").arg(parse_error.errorString());
        }
        return false;
    }

    const QJsonObject object = document.object();
    if (object.size() != fields.size())
    {
        if (error != nullptr)
            *error = QStringLiteral("context buffer field set does not match the schema");
        return false;
    }

    values->reserve(fields.size());
    for (const Field_Descriptor& field : fields)
    {
        const QString name = QString::fromUtf8(field.name);
        if (!object.contains(name))
        {
            if (error != nullptr)
                *error = QStringLiteral("missing context field: %1").arg(name);
            values->clear();
            return false;
        }
        Author_Value_Owned value;
        if (!context_value_from_json(field, object.value(name), &value))
        {
            if (error != nullptr)
                *error = QStringLiteral("invalid value for context field: %1").arg(name);
            values->clear();
            return false;
        }
        values->push_back(value);
    }
    return true;
}

int Author_Transaction_Controller::component_count(Author_Field_Kind kind)
{
    switch (kind)
    {
    case Author_Field_Vec2: return 2;
    case Author_Field_Vec3:
    case Author_Field_Color3:
    case Author_Field_Euler_Degrees3:
        return 3;
    case Author_Field_Vec4:
    case Author_Field_Color4:
        return 4;
    default:
        return 1;
    }
}

int Author_Transaction_Controller::numeric_precision(float step)
{
    if (!(step > 0.0f))
    {
        return 3;
    }
    int precision = 0;
    float scaled = step;
    while (precision < 6 && qAbs(scaled - qRound(scaled)) > 0.00001f)
    {
        scaled *= 10.0f;
        ++precision;
    }
    return precision;
}

QByteArray Author_Transaction_Controller::format_number(
    float value,
    int precision)
{
    if (value == 0.0f)
    {
        value = 0.0f;
    }
    return QByteArray::number(
        static_cast<double>(value),
        'f',
        qBound(0, precision, 6));
}

QByteArray Author_Transaction_Controller::quote_json(const QByteArray& utf8)
{
    QJsonArray wrapper;
    wrapper.push_back(QString::fromUtf8(utf8));
    QByteArray encoded = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
    if (encoded.size() >= 2)
    {
        encoded.remove(encoded.size() - 1, 1);
        encoded.remove(0, 1);
    }
    return encoded;
}

} // namespace Kalam::Editor
