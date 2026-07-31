#include "author_transaction_controller.h"
#include "game_api.h"

#include <QCoreApplication>
#include <QDebug>
#include <QStringList>

/*
 * Author_Transaction_Controller is intentionally compiled into this isolated
 * test executable against exact, minimal fakes. The production controller is
 * coupled to final concrete Host and Adapter classes, so there is no injectable
 * seam. Suppressing those two dependency headers here keeps production code
 * unchanged while exercising the real controller implementation and state
 * machine deterministically.
 */
#define KALAM_EDITOR_QT_EDITOR_NEOVIM_ADAPTER_H
#define KALAM_EDITOR_QT_EDITOR_RUNTIME_HOST_H

namespace Kalam::Editor
{

class Editor_Neovim_Adapter final : public QObject
{
    Q_OBJECT

public:
    QStringList* events = nullptr;
    quint64 next_request_id = 101;
    int context_request_count = 0;
    int patch_request_count = 0;
    qint64 requested_buffer = 0;
    QString requested_json_path;
    Source_Context patched_context;
    QByteArray patched_replacement;

    quint64 request_context_for_path(
        qint64 buffer,
        const QString& json_path)
    {
        const quint64 request_id = next_request_id++;
        ++context_request_count;
        requested_buffer = buffer;
        requested_json_path = json_path;
        append_event(
            QStringLiteral("context-request:%1").arg(request_id));
        return request_id;
    }

    quint64 patch_text(
        const Source_Context& context,
        const QByteArray& replacement_utf8)
    {
        const quint64 request_id = next_request_id++;
        ++patch_request_count;
        patched_context = context;
        patched_replacement = replacement_utf8;
        append_event(QStringLiteral("patch-request:%1").arg(request_id));
        return request_id;
    }

    void finish_context(
        quint64 request_id,
        bool succeeded,
        const Source_Context& context,
        const QString& error = QString())
    {
        append_event(
            QStringLiteral("context-finished:%1").arg(request_id));
        emit contextForPathFinished(
            request_id,
            succeeded,
            context,
            error);
    }

    void finish_patch(
        quint64 request_id,
        bool succeeded,
        quint64 resulting_changedtick,
        const QString& error = QString())
    {
        append_event(QStringLiteral("patch-finished:%1").arg(request_id));
        emit textPatchFinished(
            request_id,
            succeeded,
            resulting_changedtick,
            error);
    }

signals:
    void contextForPathFinished(
        quint64 request_id,
        bool succeeded,
        Kalam::Editor::Source_Context context,
        QString error);
    void textPatchFinished(
        quint64 request_id,
        bool succeeded,
        quint64 resulting_changedtick,
        QString error);

private:
    void append_event(const QString& event)
    {
        if (events != nullptr)
        {
            events->push_back(event);
        }
    }
};

class Editor_Runtime_Host final : public QObject
{
    Q_OBJECT

public:
    QStringList* events = nullptr;
    Document_Handle document;
    Schema_Descriptor schema;
    Author_Value_Owned current_value;
    Author_Value_Owned rehydrated_value;
    int accept_document_count = 0;
    int preview_count = 0;
    int rehydrate_count = 0;
    Author_Document_Id rehydrated_document_id = 0;
    bool rehydrate_succeeds = true;
    QString rehydrate_error;

    bool accept_valid_document(
        const Buffer_Snapshot& snapshot,
        QString*)
    {
        ++accept_document_count;
        document.buffer = snapshot.buffer;
        document.changedtick = snapshot.changedtick;
        document.path = snapshot.path;
        return document.is_valid();
    }

    Document_Handle document_for_buffer(qint64 buffer) const
    {
        return document.buffer == buffer ? document : Document_Handle();
    }

    Document_Handle document_by_id(
        Author_Document_Id document_id) const
    {
        return document.document_id == document_id
            ? document
            : Document_Handle();
    }

    bool schema_for_document(
        Author_Document_Id document_id,
        Schema_Descriptor* result) const
    {
        if (document.document_id != document_id || result == nullptr)
        {
            return false;
        }
        *result = schema;
        return true;
    }

    bool read_author_field(
        Author_Document_Id document_id,
        Author_Object_Id object_id,
        Author_Field_Id,
        Author_Value_Owned* value,
        QString*) const
    {
        if (document.document_id != document_id ||
            document.root_object_id != object_id ||
            value == nullptr)
        {
            return false;
        }
        *value = current_value;
        return true;
    }

    bool preview_author_field(
        Author_Document_Id document_id,
        Author_Object_Id object_id,
        Author_Field_Id,
        const Author_Value_Owned& value,
        QString*)
    {
        if (document.document_id != document_id ||
            document.root_object_id != object_id)
        {
            return false;
        }
        ++preview_count;
        current_value = value;
        append_event(QStringLiteral("preview"));
        return true;
    }

    bool rehydrate_document(
        Author_Document_Id document_id,
        QString* error)
    {
        ++rehydrate_count;
        rehydrated_document_id = document_id;
        append_event(
            QStringLiteral("rehydrate:%1").arg(document_id));
        if (!rehydrate_succeeds)
        {
            if (error != nullptr)
            {
                *error = rehydrate_error;
            }
            return false;
        }
        current_value = rehydrated_value;
        return true;
    }

signals:
    void aboutToCommitModuleReload();

private:
    void append_event(const QString& event)
    {
        if (events != nullptr)
        {
            events->push_back(event);
        }
    }
};

} // namespace Kalam::Editor

// See the isolation note above. This is the implementation under test.
#include "../../editor/qt/author_transaction_controller.cpp"

namespace
{

using Kalam::Editor::Author_Transaction_Controller;
using Kalam::Editor::Author_Value_Owned;
using Kalam::Editor::Document_Handle;
using Kalam::Editor::Editor_Neovim_Adapter;
using Kalam::Editor::Editor_Runtime_Host;
using Kalam::Editor::Field_Descriptor;
using Kalam::Editor::Source_Context;

int failures = 0;

void check(bool condition, const QString& message)
{
    if (!condition)
    {
        qCritical().noquote() <<
            QStringLiteral("check failed: %1").arg(message);
        ++failures;
    }
}

Document_Handle make_document()
{
    Document_Handle document;
    document.document_id = 41;
    document.root_object_id = 73;
    document.revision = 5;
    document.buffer = 19;
    document.changedtick = 7;
    document.schema_id = QByteArrayLiteral("nveditor.post_process/v1");
    document.path = QStringLiteral("post_process.json");
    return document;
}

Field_Descriptor make_field()
{
    Field_Descriptor field;
    field.id = 97;
    field.name = QByteArrayLiteral("exposure");
    field.json_path = QByteArrayLiteral("$.exposure");
    field.kind = Author_Field_F32;
    field.step[0] = 0.001f;
    field.flags = Author_Field_Flag_Context_Edit_Supported;
    return field;
}

Author_Value_Owned make_value(float value)
{
    Author_Value_Owned result;
    result.kind = Author_Field_F32;
    result.f32[0] = value;
    return result;
}

Source_Context make_context(const Document_Handle& document)
{
    Source_Context context;
    context.buffer = document.buffer;
    context.changedtick = document.changedtick;
    context.json_path = QStringLiteral("$.exposure");
    context.range.start_row = 4;
    context.range.start_column = 14;
    context.range.end_row = 4;
    context.range.end_column = 19;
    context.source_text_utf8 = QByteArrayLiteral("1.000");
    return context;
}

struct Fixture
{
    QStringList events;
    Editor_Runtime_Host host;
    Editor_Neovim_Adapter neovim;
    Author_Transaction_Controller controller;
    Document_Handle document = make_document();
    Field_Descriptor field = make_field();
    Author_Value_Owned original = make_value(1.0f);
    int close_count = 0;
    bool last_close_accepted = true;
    int error_count = 0;
    QString last_error;

    Fixture()
        : controller(&host, &neovim)
    {
        host.events = &events;
        host.document = document;
        host.current_value = original;
        host.rehydrated_value = original;
        neovim.events = &events;
        QObject::connect(
            &controller,
            &Author_Transaction_Controller::transactionClosed,
            &controller,
            [this](bool accepted)
            {
                ++close_count;
                last_close_accepted = accepted;
            });
        QObject::connect(
            &controller,
            &Author_Transaction_Controller::transactionError,
            &controller,
            [this](const QString& error)
            {
                ++error_count;
                last_error = error;
            });
    }

    void begin_and_preview(float value)
    {
        QString error;
        check(
            controller.begin_form_field_edit(
                document,
                field,
                original,
                &error),
            QStringLiteral("form transaction begins: %1").arg(error));
        check(
            controller.preview_field(
                field.id,
                make_value(value),
                &error),
            QStringLiteral("form preview succeeds: %1").arg(error));
        events.clear();
    }
};

void check_rejected_and_rehydrated(
    const Fixture& fixture,
    const QString& scenario)
{
    check(
        fixture.host.rehydrate_count == 1,
        scenario + QStringLiteral(" rehydrates exactly once"));
    check(
        fixture.host.rehydrated_document_id ==
            fixture.document.document_id,
        scenario + QStringLiteral(" rehydrates the active document"));
    check(
        fixture.close_count == 1 && !fixture.last_close_accepted,
        scenario + QStringLiteral(" closes rejected exactly once"));
    check(
        !fixture.controller.is_active() &&
            !fixture.controller.is_waiting_for_rpc(),
        scenario + QStringLiteral(" clears active and RPC state"));
}

void test_changedtick_race_cancels_and_rehydrates()
{
    Fixture fixture;
    fixture.host.rehydrated_value = make_value(3.0f);
    fixture.begin_and_preview(2.0f);

    fixture.controller.accept();
    check(
        fixture.controller.is_waiting_for_rpc(),
        QStringLiteral("changedtick case waits for context RPC"));
    check(
        fixture.neovim.context_request_count == 1 &&
            fixture.neovim.next_request_id == 102,
        QStringLiteral("changedtick case allocates context request 101"));

    /*
     * Model a buffer notification racing the context RPC. The context reply is
     * internally consistent with the transaction, but the host has already
     * accepted the newer changedtick before the surgical patch can be issued.
     */
    fixture.host.document.changedtick = 8;
    fixture.neovim.finish_context(
        101,
        true,
        make_context(fixture.document));

    check(
        fixture.events == QStringList({
            QStringLiteral("context-request:101"),
            QStringLiteral("context-finished:101"),
            QStringLiteral("rehydrate:41")}),
        QStringLiteral(
            "changedtick race request/completion/rehydrate order"));
    check(
        fixture.neovim.patch_request_count == 0,
        QStringLiteral("changedtick race never patches a stale range"));
    check_rejected_and_rehydrated(
        fixture,
        QStringLiteral("changedtick race"));
    check(
        fixture.host.current_value.f32[0] == 3.0f,
        QStringLiteral(
            "changedtick race keeps newest text-authoritative value"));
    check(
        fixture.error_count == 1 &&
            fixture.last_error ==
                QStringLiteral(
                    "buffer changed before the surgical text patch"),
        QStringLiteral("changedtick race reports the precise rejection"));
}

void test_rpc_patch_failure_cancels_and_rehydrates()
{
    Fixture fixture;
    fixture.begin_and_preview(2.0f);

    fixture.controller.accept();
    fixture.neovim.finish_context(
        101,
        true,
        make_context(fixture.document));
    check(
        fixture.controller.is_waiting_for_rpc(),
        QStringLiteral("RPC failure case waits for patch request 102"));
    check(
        fixture.neovim.context_request_count == 1 &&
            fixture.neovim.patch_request_count == 1 &&
            fixture.neovim.next_request_id == 103,
        QStringLiteral(
            "RPC failure case allocates context 101 then patch 102"));

    fixture.neovim.finish_patch(
        102,
        false,
        0,
        QStringLiteral("forced RPC patch failure"));

    check(
        fixture.events == QStringList({
            QStringLiteral("context-request:101"),
            QStringLiteral("context-finished:101"),
            QStringLiteral("patch-request:102"),
            QStringLiteral("patch-finished:102"),
            QStringLiteral("rehydrate:41")}),
        QStringLiteral("RPC failure completion precedes rehydrate"));
    check_rejected_and_rehydrated(
        fixture,
        QStringLiteral("RPC patch failure"));
    check(
        fixture.error_count == 1 &&
            fixture.last_error ==
                QStringLiteral("forced RPC patch failure"),
        QStringLiteral("RPC patch failure is reported exactly"));
}

void test_accept_issues_one_patch_and_closes_accepted()
{
    Fixture fixture;
    fixture.begin_and_preview(2.5f);

    fixture.controller.accept();
    fixture.neovim.finish_context(
        101,
        true,
        make_context(fixture.document));
    fixture.neovim.finish_patch(102, true, 8);

    check(
        fixture.events == QStringList({
            QStringLiteral("context-request:101"),
            QStringLiteral("context-finished:101"),
            QStringLiteral("patch-request:102"),
            QStringLiteral("patch-finished:102")}),
        QStringLiteral("accepted transaction has exact RPC order"));
    check(
        fixture.neovim.context_request_count == 1 &&
            fixture.neovim.patch_request_count == 1,
        QStringLiteral(
            "one accepted control transaction issues one text patch"));
    check(
        fixture.neovim.requested_buffer == fixture.document.buffer &&
            fixture.neovim.requested_json_path ==
                QStringLiteral("$.exposure"),
        QStringLiteral("accepted transaction resolves the intended source"));
    check(
        fixture.neovim.patched_context.buffer ==
                fixture.document.buffer &&
            fixture.neovim.patched_context.changedtick ==
                fixture.document.changedtick &&
            fixture.neovim.patched_replacement ==
                QByteArrayLiteral("2.500"),
        QStringLiteral(
            "accepted transaction patches the validated final value"));
    check(
        fixture.host.rehydrate_count == 0,
        QStringLiteral("accepted transaction does not rehydrate"));
    check(
        fixture.close_count == 1 && fixture.last_close_accepted,
        QStringLiteral("accepted transaction closes accepted exactly once"));
    check(
        fixture.error_count == 0,
        QStringLiteral("accepted transaction reports no error"));
    check(
        !fixture.controller.is_active() &&
            !fixture.controller.is_waiting_for_rpc(),
        QStringLiteral("accepted transaction clears controller state"));
}

void test_context_staging_parser()
{
    QVector<Field_Descriptor> fields;
    Field_Descriptor exposure = make_field();
    fields.push_back(exposure);

    Field_Descriptor enabled;
    enabled.id = 98;
    enabled.name = QByteArrayLiteral("enabled");
    enabled.kind = Author_Field_Bool;
    fields.push_back(enabled);

    Field_Descriptor position;
    position.id = 99;
    position.name = QByteArrayLiteral("position");
    position.kind = Author_Field_Vec3;
    fields.push_back(position);

    Field_Descriptor label;
    label.id = 100;
    label.name = QByteArrayLiteral("label");
    label.kind = Author_Field_String;
    fields.push_back(label);

    Field_Descriptor transform;
    transform.id = 101;
    transform.name = QByteArrayLiteral("transform");
    transform.kind = Author_Field_Transform;
    fields.push_back(transform);

    const QByteArray text = QByteArrayLiteral(
        "{\n"
        "  \"exposure\": 1.25,\n"
        "  \"enabled\": true,\n"
        "  \"position\": [1,2,3],\n"
        "  \"label\": \"key light\",\n"
        "  \"transform\": {\"translation\":[4,5,6],"
        "\"rotation_degrees\":[10,20,30],\"scale\":[1,1,1]}\n"
        "}");
    QVector<Author_Value_Owned> values;
    QString error;
    check(
        Author_Transaction_Controller::parse_context_values(
            fields, text, &values, &error),
        QStringLiteral("typed context staging JSON parses: %1").arg(error));
    check(
        values.size() == 5 && values.at(0).f32[0] == 1.25f &&
            values.at(1).boolean && values.at(2).f32[2] == 3.0f &&
            values.at(3).string_utf8 == QByteArrayLiteral("key light") &&
            values.at(4).transform.translation[1] == 5.0f &&
            values.at(4).transform.rotation_degrees[2] == 30.0f,
        QStringLiteral("typed context values retain schema kinds and components"));

    check(
        !Author_Transaction_Controller::parse_context_values(
            fields,
            QByteArrayLiteral("{\"exposure\":\"wrong\"}"),
            &values,
            &error),
        QStringLiteral("wrong field set and type are rejected"));
    check(
        !Author_Transaction_Controller::parse_context_values(
            QVector<Field_Descriptor>{exposure},
            QByteArrayLiteral("{\"exposure\":1.0,\"extra\":2}"),
            &values,
            &error),
        QStringLiteral("unexpected staging fields are rejected"));
    check(
        !Author_Transaction_Controller::parse_context_values(
            QVector<Field_Descriptor>{position},
            QByteArrayLiteral("{\"position\":[1,2]}"),
            &values,
            &error),
        QStringLiteral("wrong vector arity is rejected"));
}
} // namespace

int main(int argument_count, char** arguments)
{
    QCoreApplication application(argument_count, arguments);
    test_changedtick_race_cancels_and_rehydrates();
    test_rpc_patch_failure_cancels_and_rehydrates();
    test_accept_issues_one_patch_and_closes_accepted();
    test_context_staging_parser();
    if (failures == 0)
    {
        qInfo().noquote() <<
            QStringLiteral(
                "all author transaction regressions passed");
    }
    return failures == 0 ? 0 : 1;
}

#include "author_transaction_regression_tests.moc"
