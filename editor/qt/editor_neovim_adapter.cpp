#include "editor_neovim_adapter.h"

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
    if (arguments.isEmpty())
    {
        return;
    }
    const QVariantMap payload = arguments.first().toMap();

    if (name == QByteArrayLiteral("nveditor.context_edit"))
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
