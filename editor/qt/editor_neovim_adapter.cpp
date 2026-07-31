#include "editor_neovim_adapter.h"

#include "author_transaction_controller.h"
#include "editor_neovim_shell.h"

#include "auto/neovimapi0.h"
#include "msgpackrequest.h"
#include "neovimconnector.h"

#include <QFileInfo>
#include <QTimer>
#include <QVariantMap>

namespace Kalam::Editor
{

namespace
{

QVariantMap unwrap_lua_result(
    const QVariant& response,
    bool* succeeded,
    QString* error)
{
    const QVariantMap wrapper = response.toMap();
    const bool ok = wrapper.value(QStringLiteral("ok")).toBool();
    if (succeeded != nullptr)
    {
        *succeeded = ok;
    }
    if (error != nullptr)
    {
        *error = wrapper.value(QStringLiteral("error")).toString();
        if (!ok && error->isEmpty())
        {
            *error = QStringLiteral("Neovim helper rejected the request");
        }
    }
    return wrapper.value(QStringLiteral("result")).toMap();
}

} // namespace

Editor_Neovim_Adapter::Editor_Neovim_Adapter(
    NeovimQt::NeovimConnector* connector,
    Editor_Neovim_Shell* shell,
    QObject* parent)
    : QObject(parent), connector_(connector), shell_(shell)
{
    Q_ASSERT(connector != nullptr);
    Q_ASSERT(shell != nullptr);

    connect(
        connector,
        &NeovimQt::NeovimConnector::ready,
        this,
        &Editor_Neovim_Adapter::handle_ready);
    connect(
        connector,
        &NeovimQt::NeovimConnector::error,
        this,
        [this](NeovimQt::NeovimConnector::NeovimError)
        {
            if (connector_ != nullptr)
            {
                emit disconnected(connector_->errorString());
            }
        });
    connect(
        connector,
        &NeovimQt::NeovimConnector::processExited,
        this,
        [this](int exit_code)
        {
            emit disconnected(
                QStringLiteral("Neovim exited with code %1").arg(exit_code));
        });

    if (connector->isReady())
    {
        handle_ready();
    }
}

void Editor_Neovim_Adapter::initialize_runtime(
    const QString& runtime_directory)
{
    runtime_directory_ = QFileInfo(runtime_directory).absoluteFilePath();
    run_runtime_initialization();
}

void Editor_Neovim_Adapter::open_file(const QString& path)
{
    if (!is_ready())
    {
        pending_open_file_ = path;
        return;
    }

    const QByteArray command =
        QStringLiteral("execute 'edit ' . fnameescape('%1')")
            .arg(vim_single_quote(QFileInfo(path).absoluteFilePath()))
            .toUtf8();
    api_->vim_command(command);
}

void Editor_Neovim_Adapter::open_file_location(
    const QString& path,
    qint64 zero_based_row,
    qint64 zero_based_column)
{
    if (!is_ready())
    {
        pending_open_file_ = path;
        return;
    }

    QVariantMap argument;
    argument.insert(QStringLiteral("path"), QFileInfo(path).absoluteFilePath());
    argument.insert(QStringLiteral("row"), zero_based_row);
    argument.insert(QStringLiteral("column"), zero_based_column);
    call_lua(
        QByteArrayLiteral(
            "(function() "
            "vim.cmd.edit(vim.fn.fnameescape(_A.path)); "
            "vim.api.nvim_win_set_cursor(0,{_A.row+1,_A.column}); "
            "return true end)()"),
        argument);
}

quint64 Editor_Neovim_Adapter::request_context_for_path(
    qint64 buffer,
    const QString& json_path)
{
    const quint64 request_id = allocate_request_id();
    QVariantMap argument;
    argument.insert(QStringLiteral("bufnr"), buffer);
    argument.insert(QStringLiteral("json_path"), json_path);

    NeovimQt::MsgpackRequest* request = call_lua(
        QByteArrayLiteral(
            "(function() local r,e="
            "require('nveditor').context_for_path(_A.bufnr,_A.json_path); "
            "return {ok=r~=nil,result=r,error=e} end)()"),
        argument);
    if (request == nullptr)
    {
        QTimer::singleShot(0, this, [this, request_id]()
        {
            emit contextForPathFinished(
                request_id,
                false,
                Source_Context{},
                QStringLiteral("Neovim is not ready"));
        });
        return request_id;
    }

    connect(
        request,
        &NeovimQt::MsgpackRequest::finished,
        this,
        [this, request_id](quint32, quint64, const QVariant& response)
        {
            bool succeeded = false;
            QString error;
            const QVariantMap result =
                unwrap_lua_result(response, &succeeded, &error);
            const Source_Context context = succeeded
                ? context_from_map(result)
                : Source_Context{};
            report_range_fallback(context);
            emit contextForPathFinished(
                request_id,
                succeeded,
                context,
                error);
        });
    connect(
        request,
        &NeovimQt::MsgpackRequest::error,
        this,
        [this, request_id](quint32, quint64, const QVariant& error)
        {
            emit contextForPathFinished(
                request_id,
                false,
                Source_Context{},
                error.toString());
        });
    return request_id;
}

quint64 Editor_Neovim_Adapter::patch_text(
    const Source_Context& context,
    const QByteArray& replacement_utf8)
{
    const quint64 request_id = allocate_request_id();
    QVariantMap argument;
    argument.insert(QStringLiteral("bufnr"), context.buffer);
    argument.insert(
        QStringLiteral("expected_changedtick"),
        context.changedtick);
    argument.insert(QStringLiteral("start_row"), context.range.start_row);
    argument.insert(
        QStringLiteral("start_column"),
        context.range.start_column);
    argument.insert(QStringLiteral("end_row"), context.range.end_row);
    argument.insert(QStringLiteral("end_column"), context.range.end_column);
    argument.insert(
        QStringLiteral("replacement"),
        QString::fromUtf8(replacement_utf8));

    NeovimQt::MsgpackRequest* request = call_lua(
        QByteArrayLiteral(
            "(function() local r,e=require('nveditor').patch_text(_A); "
            "return {ok=r~=nil,result=r,error=e} end)()"),
        argument);
    if (request == nullptr)
    {
        QTimer::singleShot(0, this, [this, request_id]()
        {
            emit textPatchFinished(
                request_id,
                false,
                0,
                QStringLiteral("Neovim is not ready"));
        });
        return request_id;
    }

    connect(
        request,
        &NeovimQt::MsgpackRequest::finished,
        this,
        [this, request_id](quint32, quint64, const QVariant& response)
        {
            bool succeeded = false;
            QString error;
            const QVariantMap result =
                unwrap_lua_result(response, &succeeded, &error);
            emit textPatchFinished(
                request_id,
                succeeded,
                result.value(QStringLiteral("changedtick")).toULongLong(),
                error);
        });
    connect(
        request,
        &NeovimQt::MsgpackRequest::error,
        this,
        [this, request_id](quint32, quint64, const QVariant& error)
        {
            emit textPatchFinished(
                request_id,
                false,
                0,
                error.toString());
        });
    return request_id;
}

void Editor_Neovim_Adapter::open_context_session(
    const QVector<Field_Descriptor>& fields,
    const QVector<Author_Value_Owned>& values,
    const Source_Context& context)
{
    if (fields.size() != values.size())
    {
        notify_user(QStringLiteral("context field/value count mismatch"), true);
        return;
    }

    QVariantList field_specs;
    field_specs.reserve(fields.size());
    for (qsizetype index = 0; index < fields.size(); ++index)
    {
        QVariantMap field;
        field.insert(
            QStringLiteral("name"),
            QString::fromUtf8(fields.at(index).name));
        field.insert(
            QStringLiteral("value_json"),
            QString::fromUtf8(
                Author_Transaction_Controller::format_value_json(
                    fields.at(index),
                    values.at(index))));
        field_specs.push_back(field);
    }

    QVariantMap argument;
    argument.insert(QStringLiteral("fields"), field_specs);
    argument.insert(QStringLiteral("source_buffer"), context.buffer);
    argument.insert(QStringLiteral("json_path"), context.json_path);
    call_lua(
        QByteArrayLiteral(
            "(function() local r,e=require('nveditor').open_context_session(_A); "
            "if not r then vim.notify(e,vim.log.levels.ERROR) end; return r end)()"),
        argument);
}

void Editor_Neovim_Adapter::update_context_session_field(
    qint64 session_id,
    const QByteArray& field_name,
    const QByteArray& value_json)
{
    QVariantMap argument;
    argument.insert(QStringLiteral("session_id"), session_id);
    argument.insert(QStringLiteral("name"), QString::fromUtf8(field_name));
    argument.insert(QStringLiteral("value_json"), QString::fromUtf8(value_json));
    call_lua(
        QByteArrayLiteral(
            "(function() local r,e=require('nveditor').update_context_session_field("
            "_A.session_id,_A.name,_A.value_json); if not r then "
            "vim.notify(e,vim.log.levels.ERROR) end; return r end)()"),
        argument);
}

void Editor_Neovim_Adapter::finish_context_session_write(
    qint64 session_id,
    bool succeeded,
    const QString& message)
{
    QVariantMap argument;
    argument.insert(QStringLiteral("session_id"), session_id);
    argument.insert(QStringLiteral("succeeded"), succeeded);
    argument.insert(QStringLiteral("message"), message);
    call_lua(
        QByteArrayLiteral(
            "require('nveditor').finish_context_session_write("
            "_A.session_id,_A.succeeded,_A.message)"),
        argument);
}

void Editor_Neovim_Adapter::request_context_session_write(
    qint64 session_id,
    bool close_after_write)
{
    QVariantMap argument;
    argument.insert(QStringLiteral("session_id"), session_id);
    argument.insert(QStringLiteral("close_after_write"), close_after_write);
    call_lua(
        QByteArrayLiteral(
            "(function() local r,e=require('nveditor').request_context_session_write("
            "_A.session_id,_A.close_after_write); if not r then "
            "vim.notify(e,vim.log.levels.ERROR) end; return r end)()"),
        argument);
}

void Editor_Neovim_Adapter::close_context_session(
    qint64 session_id,
    bool force)
{
    QVariantMap argument;
    argument.insert(QStringLiteral("session_id"), session_id);
    argument.insert(QStringLiteral("force"), force);
    call_lua(
        QByteArrayLiteral(
            "(function() local r,e=require('nveditor').close_context_session("
            "_A.session_id,_A.force); if not r then "
            "vim.notify(e,vim.log.levels.ERROR) end; return r end)()"),
        argument);
}

void Editor_Neovim_Adapter::send_input(const QByteArray& keys)
{
    if (is_ready())
    {
        api_->vim_input(keys);
    }
}

void Editor_Neovim_Adapter::set_quickfix(
    const QVector<Diagnostic>& diagnostics,
    const QString& title,
    bool open_first_error)
{
    if (!is_ready())
    {
        return;
    }

    QVariantList items;
    items.reserve(diagnostics.size());
    for (const Diagnostic& diagnostic : diagnostics)
    {
        QVariantMap item;
        item.insert(QStringLiteral("filename"), diagnostic.file);
        item.insert(QStringLiteral("lnum"), qMax(1, diagnostic.line));
        item.insert(QStringLiteral("col"), qMax(1, diagnostic.column));
        item.insert(QStringLiteral("text"), diagnostic.message);
        item.insert(
            QStringLiteral("type"),
            diagnostic.type.startsWith(
                QStringLiteral("warning"),
                Qt::CaseInsensitive)
                ? QStringLiteral("W")
                : QStringLiteral("E"));
        items.push_back(item);
    }

    QVariantMap argument;
    argument.insert(QStringLiteral("items"), items);
    argument.insert(QStringLiteral("title"), title);
    argument.insert(QStringLiteral("open"), open_first_error);
    call_lua(
        QByteArrayLiteral(
            "(function() require('nveditor').set_quickfix(_A.items,_A.title); "
            "if _A.open and #_A.items>0 then vim.cmd('cfirst') end; "
            "return true end)()"),
        argument);
}

void Editor_Neovim_Adapter::notify_user(
    const QString& message,
    bool is_error)
{
    QVariantMap argument;
    argument.insert(QStringLiteral("message"), message);
    argument.insert(QStringLiteral("is_error"), is_error);
    call_lua(
        QByteArrayLiteral(
            "(function() vim.notify(_A.message, _A.is_error and "
            "vim.log.levels.ERROR or vim.log.levels.INFO); return true end)()"),
        argument);
}

void Editor_Neovim_Adapter::focus_editor()
{
    if (shell_ != nullptr)
    {
        shell_->setFocus(Qt::OtherFocusReason);
    }
}

void Editor_Neovim_Adapter::request_neovim_close(bool force)
{
    if (is_ready())
    {
        api_->vim_command(
            force
                ? QByteArrayLiteral("qall!")
                : QByteArrayLiteral("confirm qall"));
    }
}

bool Editor_Neovim_Adapter::is_ready() const
{
    return connector_ != nullptr && api_ != nullptr && connector_->isReady();
}

Editor_Neovim_Shell* Editor_Neovim_Adapter::shell() const
{
    return shell_;
}

void Editor_Neovim_Adapter::handle_ready()
{
    if (connector_ == nullptr || api_ != nullptr)
    {
        return;
    }

    api_ = connector_->api0();
    if (api_ == nullptr)
    {
        emit disconnected(QStringLiteral("Neovim API level 0 is unavailable"));
        return;
    }

    connect(
        api_,
        &NeovimQt::NeovimApi0::neovimNotification,
        this,
        &Editor_Neovim_Adapter::handle_notification);
    run_runtime_initialization();
    if (!pending_open_file_.isEmpty())
    {
        const QString path = pending_open_file_;
        pending_open_file_.clear();
        open_file(path);
    }
    emit ready();
}

void Editor_Neovim_Adapter::handle_notification(
    const QByteArray& name,
    const QVariantList& arguments)
{
    const QVariantMap payload = arguments.isEmpty()
        ? QVariantMap{}
        : arguments.first().toMap();

    if (name == QByteArrayLiteral("nveditor.focus_editor"))
    {
        emit focusEditorRequested();
    }
    else if (name == QByteArrayLiteral("nveditor.focus_viewport"))
    {
        emit focusViewportRequested();
    }
    else if (name == QByteArrayLiteral("nveditor.toggle_viewport"))
    {
        emit toggleViewportRequested();
    }
    else if (name == QByteArrayLiteral("nveditor.play"))
    {
        emit playRequested();
    }
    else if (name == QByteArrayLiteral("nveditor.stop"))
    {
        emit stopRequested();
    }
    else if (name == QByteArrayLiteral("nveditor.context_edit"))
    {
        const Source_Context context = context_from_map(payload);
        report_range_fallback(context);
        emit contextEditRequested(context);
    }
    else if (name == QByteArrayLiteral("nveditor.form_edit"))
    {
        emit formEditRequested(snapshot_from_map(payload));
    }
    else if (name == QByteArrayLiteral("nveditor.build_reload"))
    {
        emit buildReloadRequested();
    }
    else if (name == QByteArrayLiteral("nveditor.document_valid"))
    {
        emit validDocumentReceived(snapshot_from_map(payload));
    }
    else if (name == QByteArrayLiteral("nveditor.document_invalid"))
    {
        emit invalidDocumentReceived(
            snapshot_from_map(payload),
            first_diagnostic(payload));
    }
    else if (name == QByteArrayLiteral("nveditor.context_session_changed"))
    {
        emit contextSessionChanged(context_session_from_map(payload));
    }
    else if (name == QByteArrayLiteral("nveditor.context_session_write"))
    {
        emit contextSessionWriteRequested(context_session_from_map(payload));
    }
    else if (name == QByteArrayLiteral("nveditor.context_session_closed"))
    {
        emit contextSessionClosed(
            payload.value(QStringLiteral("session_id")).toLongLong(),
            payload.value(QStringLiteral("write_pending")).toBool());
    }
}

quint64 Editor_Neovim_Adapter::allocate_request_id()
{
    const quint64 result = next_request_id_;
    next_request_id_ = next_request_id_ == ~quint64(0)
        ? 1
        : next_request_id_ + 1;
    return result;
}

NeovimQt::MsgpackRequest* Editor_Neovim_Adapter::call_lua(
    const QByteArray& expression,
    const QVariant& argument)
{
    if (!is_ready())
    {
        return nullptr;
    }
    return api_->vim_call_function(
        QByteArrayLiteral("luaeval"),
        QVariantList{expression, argument});
}

void Editor_Neovim_Adapter::run_runtime_initialization()
{
    if (!is_ready() || runtime_initialized_ || runtime_directory_.isEmpty())
    {
        return;
    }

    const QString escaped = vim_single_quote(runtime_directory_);
    api_->vim_command(
        QStringLiteral("execute 'set runtimepath^=' . fnameescape('%1')")
            .arg(escaped)
            .toUtf8());
    api_->vim_set_var(
        QByteArrayLiteral("nveditor_channel_id"),
        QVariant::fromValue<qulonglong>(connector_->channel()));
    api_->vim_command(QByteArrayLiteral("runtime plugin/nveditor.lua"));
    runtime_initialized_ = true;
}

void Editor_Neovim_Adapter::report_range_fallback(
    const Source_Context& context)
{
    if (!context.range_fallback || range_fallback_reported_)
    {
        return;
    }
    range_fallback_reported_ = true;
    notify_user(
        QStringLiteral(
            "JSON Tree-sitter parser unavailable; exact source editing is "
            "limited to the fixed Revision 1 scene and post-process paths."),
        false);
}

Buffer_Snapshot Editor_Neovim_Adapter::snapshot_from_map(
    const QVariantMap& map)
{
    Buffer_Snapshot snapshot;
    snapshot.buffer = map.value(QStringLiteral("bufnr")).toLongLong();
    snapshot.changedtick =
        map.value(QStringLiteral("changedtick")).toULongLong();
    snapshot.path = map.value(QStringLiteral("path")).toString();
    snapshot.text_utf8 = map.value(QStringLiteral("text")).toString().toUtf8();
    return snapshot;
}

Source_Context Editor_Neovim_Adapter::context_from_map(
    const QVariantMap& map)
{
    Source_Context context;
    context.buffer = map.value(QStringLiteral("bufnr")).toLongLong();
    context.changedtick =
        map.value(QStringLiteral("changedtick")).toULongLong();
    context.json_path = map.value(QStringLiteral("json_path")).toString();
    context.node_type = map.value(QStringLiteral("node_type")).toByteArray();
    context.range.start_row =
        map.value(QStringLiteral("start_row")).toLongLong();
    context.range.start_column =
        map.value(QStringLiteral("start_column")).toLongLong();
    context.range.end_row = map.value(QStringLiteral("end_row")).toLongLong();
    context.range.end_column =
        map.value(QStringLiteral("end_column")).toLongLong();
    context.source_text_utf8 =
        map.value(QStringLiteral("source_text")).toString().toUtf8();
    context.document_path = map.value(QStringLiteral("path")).toString();
    context.document_text_utf8 =
        map.value(QStringLiteral("text")).toString().toUtf8();
    context.range_fallback =
        map.value(QStringLiteral("range_fallback")).toBool();
    return context;
}

Context_Session_State Editor_Neovim_Adapter::context_session_from_map(
    const QVariantMap& map)
{
    Context_Session_State state;
    state.session_id = map.value(QStringLiteral("session_id")).toLongLong();
    state.buffer = map.value(QStringLiteral("bufnr")).toLongLong();
    state.window = map.value(QStringLiteral("window")).toLongLong();
    state.changedtick = map.value(QStringLiteral("changedtick")).toULongLong();
    state.text_utf8 = map.value(QStringLiteral("text")).toString().toUtf8();
    state.mode = map.value(QStringLiteral("mode")).toByteArray();
    state.cursor_row = map.value(QStringLiteral("cursor_row")).toLongLong();
    state.cursor_column = map.value(QStringLiteral("cursor_column")).toLongLong();
    state.window_row = map.value(QStringLiteral("window_row")).toLongLong();
    state.window_column = map.value(QStringLiteral("window_column")).toLongLong();
    state.window_width = map.value(QStringLiteral("window_width")).toLongLong();
    state.window_height = map.value(QStringLiteral("window_height")).toLongLong();
    state.modified = map.value(QStringLiteral("modified")).toBool();
    state.write_pending = map.value(QStringLiteral("write_pending")).toBool();
    return state;
}

QString Editor_Neovim_Adapter::first_diagnostic(const QVariantMap& map)
{
    const QVariantList diagnostics =
        map.value(QStringLiteral("diagnostics")).toList();
    if (diagnostics.isEmpty())
    {
        return QStringLiteral("Invalid JSON document");
    }
    return diagnostics.first()
        .toMap()
        .value(QStringLiteral("message"))
        .toString();
}

QString Editor_Neovim_Adapter::vim_single_quote(const QString& value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return escaped;
}

} // namespace Kalam::Editor
