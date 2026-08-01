#ifndef KALAM_EDITOR_QT_EDITOR_NEOVIM_ADAPTER_H
#define KALAM_EDITOR_QT_EDITOR_NEOVIM_ADAPTER_H

#include "editor_types.h"

#include <QObject>
#include <QPointer>
#include <QVariantList>

namespace Kalam::Editor
{

class Editor_Neovim_Shell;
class Neovim_Rpc_Client;
class Neovim_Rpc_Request;

class Editor_Neovim_Adapter final : public QObject
{
    Q_OBJECT

public:
    explicit Editor_Neovim_Adapter(
        Neovim_Rpc_Client* client,
        Editor_Neovim_Shell* shell,
        QObject* parent = nullptr);

    void initialize_runtime(const QString& runtime_directory);
    void open_file(const QString& path);
    void open_file_location(
        const QString& path,
        qint64 zero_based_row,
        qint64 zero_based_column);

    quint64 request_context_for_path(qint64 buffer, const QString& json_path);
    quint64 patch_text(
        const Source_Context& context,
        const QByteArray& replacement_utf8);

    void set_quickfix(
        const QVector<Diagnostic>& diagnostics,
        const QString& title,
        bool open_first_error);
    void notify_user(const QString& message, bool is_error);
    void focus_editor();
    void request_neovim_close(bool force = false);

    bool is_ready() const;
    Editor_Neovim_Shell* shell() const;

signals:
    void ready();
    void disconnected(QString reason);
    void contextEditRequested(Kalam::Editor::Source_Context context);
    void formEditRequested(Kalam::Editor::Buffer_Snapshot snapshot);
    void buildReloadRequested();
    void focusEditorRequested();
    void focusViewportRequested();
    void toggleViewportRequested();
    void playRequested();
    void stopRequested();
    void viewportWindowsChanged(
        QVector<qlonglong> windows,
        qlonglong active_window);
    void validDocumentReceived(Kalam::Editor::Buffer_Snapshot snapshot);
    void invalidDocumentReceived(
        Kalam::Editor::Buffer_Snapshot snapshot,
        QString diagnostic);
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


private slots:
    void handle_ready();
    void handle_notification(
        const QByteArray& name,
        const QVariantList& arguments);

private:
    QPointer<Neovim_Rpc_Client> client_;
    QPointer<Editor_Neovim_Shell> shell_;
    QString runtime_directory_;
    QString pending_open_file_;
    quint64 next_request_id_ = 1;
    bool runtime_initialized_ = false;
    bool range_fallback_reported_ = false;

    quint64 allocate_request_id();
    Neovim_Rpc_Request* call_lua(
        const QByteArray& expression,
        const QVariant& argument);
    void run_command(const QByteArray& command);
    void run_runtime_initialization();
    void report_range_fallback(const Source_Context& context);

    static Buffer_Snapshot snapshot_from_map(const QVariantMap& map);
    static Source_Context context_from_map(const QVariantMap& map);
    static QString first_diagnostic(const QVariantMap& map);
    static QString vim_single_quote(const QString& value);
};

} // namespace Kalam::Editor

#endif
