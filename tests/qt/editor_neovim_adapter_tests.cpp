#include "editor_neovim_adapter.h"
#include "editor_neovim_shell.h"

#include "msgpackrequest.h"
#include "neovimconnector.h"

#include <QApplication>
#include <QDebug>
#include <QEventLoop>
#include <QProcess>
#include <QTimer>
#include <QVariant>

namespace
{

int failures = 0;

void check(bool condition, const QString& message)
{
    if (!condition)
    {
        qCritical().noquote() << QStringLiteral("check failed: %1").arg(message);
        ++failures;
    }
}

void test_completed_request_stops_timeout_before_nested_loop()
{
    NeovimQt::MsgpackRequest request(77, nullptr);
    int finished_count = 0;
    int timeout_count = 0;
    QObject::connect(
        &request,
        &NeovimQt::MsgpackRequest::finished,
        &request,
        [&](quint32 request_id, quint64, const QVariant&)
        {
            ++finished_count;
            check(request_id == 77,
                  QStringLiteral("completed vendor request retains its ID"));
        });
    QObject::connect(
        &request,
        &NeovimQt::MsgpackRequest::timeout,
        &request,
        [&](quint32)
        {
            ++timeout_count;
        });

    request.setTimeout(20);
    QObject::connect(
        &request,
        &NeovimQt::MsgpackRequest::finished,
        &request,
        [&]()
        {
            QEventLoop nested_loop;
            QTimer::singleShot(75, &nested_loop, &QEventLoop::quit);
            nested_loop.exec();
        });
    request.finished(77, 9, QVariant());

    check(finished_count == 1,
          QStringLiteral("completed vendor request finishes exactly once"));
    check(timeout_count == 0,
          QStringLiteral(
              "completed vendor request stops timeout before nested events"));
}

void test_not_ready_completions_are_queued(
    Kalam::Editor::Editor_Neovim_Adapter* adapter)
{
    quint64 context_request_id = 0;
    int context_completions = 0;
    bool context_completion_matched = false;
    bool context_succeeded = true;
    QString context_error;
    QEventLoop context_loop;
    QTimer context_timeout;
    context_timeout.setSingleShot(true);
    QObject::connect(
        adapter,
        &Kalam::Editor::Editor_Neovim_Adapter::contextForPathFinished,
        &context_loop,
        [&](quint64 request_id,
            bool succeeded,
            const Kalam::Editor::Source_Context&,
            const QString& error)
        {
            ++context_completions;
            context_completion_matched =
                context_request_id != 0 && request_id == context_request_id;
            context_succeeded = succeeded;
            context_error = error;
            if (context_completion_matched)
            {
                context_request_id = 0;
            }
            context_loop.quit();
        });
    QObject::connect(
        &context_timeout,
        &QTimer::timeout,
        &context_loop,
        &QEventLoop::quit);

    context_request_id =
        adapter->request_context_for_path(41, QStringLiteral("$.cube"));
    check(context_request_id != 0,
          QStringLiteral("context request allocates a caller-visible ID"));
    check(context_completions == 0,
          QStringLiteral("not-ready context failure is not synchronous"));
    if (context_completions == 0)
    {
        context_timeout.start(1000);
        context_loop.exec();
    }
    check(context_completions == 1,
          QStringLiteral("not-ready context failure completes exactly once"));
    check(context_completion_matched && context_request_id == 0,
          QStringLiteral(
              "context completion matches and clears caller bookkeeping"));
    check(!context_succeeded &&
              context_error == QStringLiteral("Neovim is not ready"),
          QStringLiteral("context completion reports the not-ready failure"));

    Kalam::Editor::Source_Context context;
    context.buffer = 41;
    context.changedtick = 17;
    context.json_path = QStringLiteral("$.cube.color");
    context.range.start_row = 3;
    context.range.start_column = 12;
    context.range.end_row = 3;
    context.range.end_column = 36;

    quint64 patch_request_id = 0;
    int patch_completions = 0;
    bool patch_completion_matched = false;
    bool patch_succeeded = true;
    quint64 resulting_changedtick = 99;
    QString patch_error;
    QEventLoop patch_loop;
    QTimer patch_timeout;
    patch_timeout.setSingleShot(true);
    QObject::connect(
        adapter,
        &Kalam::Editor::Editor_Neovim_Adapter::textPatchFinished,
        &patch_loop,
        [&](quint64 request_id,
            bool succeeded,
            quint64 changedtick,
            const QString& error)
        {
            ++patch_completions;
            patch_completion_matched =
                patch_request_id != 0 && request_id == patch_request_id;
            patch_succeeded = succeeded;
            resulting_changedtick = changedtick;
            patch_error = error;
            if (patch_completion_matched)
            {
                patch_request_id = 0;
            }
            patch_loop.quit();
        });
    QObject::connect(
        &patch_timeout,
        &QTimer::timeout,
        &patch_loop,
        &QEventLoop::quit);

    patch_request_id =
        adapter->patch_text(context, QByteArrayLiteral("[1.0,0.0,0.0,1.0]"));
    check(patch_request_id != 0,
          QStringLiteral("text patch allocates a caller-visible ID"));
    check(patch_completions == 0,
          QStringLiteral("not-ready text-patch failure is not synchronous"));
    if (patch_completions == 0)
    {
        patch_timeout.start(1000);
        patch_loop.exec();
    }
    check(patch_completions == 1,
          QStringLiteral("not-ready text-patch failure completes exactly once"));
    check(patch_completion_matched && patch_request_id == 0,
          QStringLiteral(
              "text-patch completion matches and clears caller bookkeeping"));
    check(!patch_succeeded && resulting_changedtick == 0 &&
              patch_error == QStringLiteral("Neovim is not ready"),
          QStringLiteral("text patch reports the not-ready failure"));
}

void test_context_notification_preserves_source_screen_position(
    Kalam::Editor::Editor_Neovim_Adapter* adapter)
{
    Kalam::Editor::Source_Context received;
    bool signalled = false;
    QObject::connect(
        adapter,
        &Kalam::Editor::Editor_Neovim_Adapter::contextEditRequested,
        adapter,
        [&](const Kalam::Editor::Source_Context& context)
        {
            received = context;
            signalled = true;
        });

    QVariantMap payload;
    payload.insert(QStringLiteral("bufnr"), 4);
    payload.insert(QStringLiteral("changedtick"), 11);
    payload.insert(QStringLiteral("json_path"), QStringLiteral("$.cube.color"));
    payload.insert(QStringLiteral("screen_row"), 8);
    payload.insert(QStringLiteral("screen_column"), 27);
    payload.insert(QStringLiteral("start_row"), 2);
    payload.insert(QStringLiteral("start_column"), 10);
    payload.insert(QStringLiteral("end_row"), 2);
    payload.insert(QStringLiteral("end_column"), 34);

    const bool invoked = QMetaObject::invokeMethod(
        adapter,
        "handle_notification",
        Qt::DirectConnection,
        Q_ARG(QByteArray, QByteArrayLiteral("nveditor.context_edit")),
        Q_ARG(QVariantList, QVariantList{payload}));
    check(invoked && signalled,
          QStringLiteral("context notification reaches the adapter signal"));
    check(
        received.screen_row == 8 && received.screen_column == 27,
        QStringLiteral("adapter preserves the captured source screen position"));
}

} // namespace

int main(int argument_count, char** arguments)
{
    QApplication application(argument_count, arguments);
    application.setQuitOnLastWindowClosed(false);

    test_completed_request_stops_timeout_before_nested_loop();

    auto* device = new QProcess();
    auto* connector = new NeovimQt::NeovimConnector(device);
    Kalam::Editor::Editor_Neovim_Shell shell(connector);
    Kalam::Editor::Editor_Neovim_Adapter adapter(connector, &shell);
    check(!adapter.is_ready(),
          QStringLiteral("closed connector remains deterministically not ready"));
    test_not_ready_completions_are_queued(&adapter);
    test_context_notification_preserves_source_screen_position(&adapter);

    if (failures == 0)
    {
        qInfo().noquote() <<
            QStringLiteral("all Neovim adapter regressions passed");
    }
    return failures == 0 ? 0 : 1;
}
