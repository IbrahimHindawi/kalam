#ifndef KALAM_EDITOR_QT_AUTHOR_TRANSACTION_CONTROLLER_H
#define KALAM_EDITOR_QT_AUTHOR_TRANSACTION_CONTROLLER_H

#include "editor_types.h"

#include <QObject>
#include <QVector>

namespace Kalam::Editor
{

class Editor_Neovim_Adapter;
class Editor_Runtime_Host;

class Author_Transaction_Controller final : public QObject
{
    Q_OBJECT

public:
    explicit Author_Transaction_Controller(
        Editor_Runtime_Host* host,
        Editor_Neovim_Adapter* neovim,
        QObject* parent = nullptr);

    bool begin_context_edit(const Source_Context& context, QString* error);
    bool begin_form_field_edit(
        const Document_Handle& document,
        const Field_Descriptor& field,
        const Author_Value_Owned& original,
        QString* error);
    bool begin_gizmo_edit(
        const Viewport_Interaction_Result& result,
        QString* error);

    bool preview_field(
        Author_Field_Id field_id,
        const Author_Value_Owned& value,
        QString* error);
    void note_gizmo_value(const Viewport_Interaction_Result& result);
    void accept();
    void accept_gizmo(const Viewport_Interaction_Result& result);
    void cancel();
    void note_gizmo_cancelled();
    void document_changed(const Buffer_Snapshot& snapshot);

    bool is_active() const;
    bool is_waiting_for_rpc() const;

    static QByteArray format_value_json(
        const Field_Descriptor& field,
        const Author_Value_Owned& value);
    static bool parse_context_values(
        const QVector<Field_Descriptor>& fields,
        const QByteArray& text_utf8,
        QVector<Author_Value_Owned>* values,
        QString* error);

signals:
    void contextEditReady(
        QVector<Kalam::Editor::Field_Descriptor> fields,
        QVector<Kalam::Editor::Author_Value_Owned> values,
        Kalam::Editor::Source_Context context);
    void transactionClosed(bool accepted);
    void transactionError(QString error);

private slots:
    void context_for_path_finished(
        quint64 request_id,
        bool succeeded,
        Kalam::Editor::Source_Context context,
        QString error);
    void text_patch_finished(
        quint64 request_id,
        bool succeeded,
        quint64 resulting_changedtick,
        QString error);

private:
    enum class Origin
    {
        None,
        Context,
        Form,
        Gizmo
    };

    struct Transaction
    {
        Origin origin = Origin::None;
        Document_Handle document;
        Source_Context source;
        QString target_json_path;
        QVector<Field_Descriptor> fields;
        QVector<Author_Value_Owned> original_values;
        QVector<Author_Value_Owned> current_values;
        quint64 context_request_id = 0;
        quint64 patch_request_id = 0;
        bool source_is_resolved = false;
    };

    Editor_Runtime_Host* host_ = nullptr;
    Editor_Neovim_Adapter* neovim_ = nullptr;
    Transaction transaction_;

    bool populate_fields_for_path(
        const Document_Handle& document,
        const QString& json_path,
        QVector<Field_Descriptor>* fields,
        QString* error) const;
    void request_source_or_patch();
    void patch_resolved_source();
    QByteArray format_transaction_json() const;
    void cancel_and_rehydrate(const QString& reason, bool report_error);
    void clear_transaction(bool accepted);
    int field_index(Author_Field_Id field_id) const;

    static int component_count(Author_Field_Kind kind);
    static int numeric_precision(float step);
    static QByteArray format_number(float value, int precision);
    static QByteArray quote_json(const QByteArray& utf8);
};

} // namespace Kalam::Editor

#endif
