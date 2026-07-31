#include "editor_main_window.h"
#include "editor_neovim_shell.h"
#include "neovim_rpc_client.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QTimer>

#include <stdio.h>

#ifndef KALAM_SOURCE_ROOT_DEFAULT
#define KALAM_SOURCE_ROOT_DEFAULT ""
#endif
#ifndef KALAM_BUILD_DIRECTORY_DEFAULT
#define KALAM_BUILD_DIRECTORY_DEFAULT ""
#endif
#ifndef KALAM_GAME_MODULE_DEFAULT
#define KALAM_GAME_MODULE_DEFAULT ""
#endif
#ifndef KALAM_HOT_COPY_DIRECTORY_DEFAULT
#define KALAM_HOT_COPY_DIRECTORY_DEFAULT ""
#endif
#ifndef KALAM_NEOVIM_RUNTIME_RELATIVE_PATH
#define KALAM_NEOVIM_RUNTIME_RELATIVE_PATH "nvim-runtime"
#endif

namespace
{

QString option_or_default(
    const QCommandLineParser& parser,
    const QCommandLineOption& option,
    const QString& fallback)
{
    const QString value = parser.value(option);
    return value.isEmpty() ? fallback : value;
}

QString absolute_path(const QString& path)
{
    return path.isEmpty()
        ? QString()
        : QFileInfo(path).absoluteFilePath();
}

} // namespace

int main(int argument_count, char** arguments)
{
    QApplication application(argument_count, arguments);
    QCoreApplication::setApplicationName(QStringLiteral("nveditor"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("Kalam"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Kalam integrated Neovim and D3D11 editor"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption smoke_option(
        QStringLiteral("acceptance-smoke"),
        QStringLiteral("Run the hidden-WARP integrated acceptance path."));
    QCommandLineOption smoke_timeout_option(
        QStringLiteral("smoke-timeout"),
        QStringLiteral("Smoke timeout in milliseconds."),
        QStringLiteral("milliseconds"),
        QStringLiteral("15000"));
    QCommandLineOption smoke_screenshot_option(
        QStringLiteral("smoke-screenshot"),
        QStringLiteral("Save the integrated context surface screenshot."),
        QStringLiteral("file"));
    QCommandLineOption source_root_option(
        QStringLiteral("source-root"),
        QStringLiteral("Project source root."),
        QStringLiteral("directory"));
    QCommandLineOption build_directory_option(
        QStringLiteral("build-directory"),
        QStringLiteral("Ninja build directory."),
        QStringLiteral("directory"));
    QCommandLineOption module_option(
        QStringLiteral("module"),
        QStringLiteral("Canonical game module DLL."),
        QStringLiteral("file"));
    QCommandLineOption hot_directory_option(
        QStringLiteral("hot-directory"),
        QStringLiteral("Directory for generation-tagged hot DLL copies."),
        QStringLiteral("directory"));
    QCommandLineOption runtime_option(
        QStringLiteral("runtime-directory"),
        QStringLiteral("Staged Neovim runtime directory."),
        QStringLiteral("directory"));
    QCommandLineOption neovim_option(
        QStringLiteral("neovim"),
        QStringLiteral("Neovim executable."),
        QStringLiteral("program"),
        QStringLiteral("nvim"));
    QCommandLineOption ninja_option(
        QStringLiteral("ninja"),
        QStringLiteral("Ninja executable."),
        QStringLiteral("program"),
        QStringLiteral("ninja"));

    parser.addOption(smoke_option);
    parser.addOption(smoke_timeout_option);
    parser.addOption(smoke_screenshot_option);
    parser.addOption(source_root_option);
    parser.addOption(build_directory_option);
    parser.addOption(module_option);
    parser.addOption(hot_directory_option);
    parser.addOption(runtime_option);
    parser.addOption(neovim_option);
    parser.addOption(ninja_option);
    parser.addPositionalArgument(
        QStringLiteral("file"),
        QStringLiteral("Authored JSON file to open."),
        QStringLiteral("[file]"));
    parser.process(application);

    const QString source_root = absolute_path(option_or_default(
        parser,
        source_root_option,
        QString::fromUtf8(KALAM_SOURCE_ROOT_DEFAULT)));
    const QString build_directory = absolute_path(option_or_default(
        parser,
        build_directory_option,
        QString::fromUtf8(KALAM_BUILD_DIRECTORY_DEFAULT)));
    const QString canonical_module = absolute_path(option_or_default(
        parser,
        module_option,
        QString::fromUtf8(KALAM_GAME_MODULE_DEFAULT)));
    const QString hot_directory = absolute_path(option_or_default(
        parser,
        hot_directory_option,
        QString::fromUtf8(KALAM_HOT_COPY_DIRECTORY_DEFAULT)));
    const QString runtime_directory = absolute_path(option_or_default(
        parser,
        runtime_option,
        QDir(QCoreApplication::applicationDirPath()).filePath(
            QString::fromUtf8(KALAM_NEOVIM_RUNTIME_RELATIVE_PATH))));

    const QString smoke_screenshot = absolute_path(
        parser.value(smoke_screenshot_option));

    if (source_root.isEmpty() || build_directory.isEmpty() ||
        canonical_module.isEmpty() || hot_directory.isEmpty())
    {
        qCritical().noquote()
            << QStringLiteral("editor paths are incomplete; use --help");
        return 2;
    }

    Kalam::Editor::Editor_Configuration configuration;
    configuration.source_root = source_root;
    configuration.build_directory = build_directory;
    configuration.canonical_module_dll = canonical_module;
    configuration.hot_copy_directory = hot_directory;
    configuration.neovim_runtime_directory = runtime_directory;
    configuration.ninja_program = parser.value(ninja_option);
    configuration.force_warp = parser.isSet(smoke_option);
    configuration.acceptance_smoke = parser.isSet(smoke_option);
    const QStringList positional_arguments = parser.positionalArguments();
    configuration.initial_file = positional_arguments.isEmpty()
        ? QDir(source_root).filePath(QStringLiteral("examples/cube.scene.json"))
        : absolute_path(positional_arguments.first());

    auto* client = new Kalam::Editor::Neovim_Rpc_Client(&application);
    if (!client->spawn(parser.value(neovim_option)))
    {
        qCritical().noquote() << QStringLiteral("could not create Neovim process");
        return 2;
    }

    auto* shell = new Kalam::Editor::Editor_Neovim_Shell(client);
    Kalam::Editor::Editor_Main_Window window(
        client,
        shell,
        configuration);
    bool awaiting_smoke_shutdown = false;
    int smoke_exit_code = 1;
    QObject::connect(
        client,
        &Kalam::Editor::Neovim_Rpc_Client::processExited,
        &application,
        [&application, &awaiting_smoke_shutdown, &smoke_exit_code](int)
        {
            if (awaiting_smoke_shutdown)
            {
                awaiting_smoke_shutdown = false;
                application.exit(smoke_exit_code);
            }
        });
    QObject::connect(
        &window,
        &Kalam::Editor::Editor_Main_Window::acceptanceSmokeFinished,
        &application,
        [&application,
         &window,
         client,
         &awaiting_smoke_shutdown,
         &smoke_exit_code](bool succeeded, const QString& reason)
        {
            const QByteArray reason_utf8 = reason.toUtf8();
            FILE* stream = succeeded ? stdout : stderr;
            fputs(succeeded ? "editor smoke passed: " : "editor smoke failed: ",
                  stream);
            fwrite(
                reason_utf8.constData(),
                1,
                static_cast<size_t>(reason_utf8.size()),
                stream);
            fputc('\n', stream);
            fflush(stream);

            smoke_exit_code = succeeded ? 0 : 1;
            awaiting_smoke_shutdown = true;
            window.begin_acceptance_shutdown();
            QTimer::singleShot(
                2000,
                &application,
                [&application,
                 client,
                 &awaiting_smoke_shutdown,
                 &smoke_exit_code]()
                {
                    if (!awaiting_smoke_shutdown)
                    {
                        return;
                    }
                    QProcess* process = client->process();
                    if (process != nullptr &&
                        process->state() != QProcess::NotRunning)
                    {
                        process->kill();
                        process->waitForFinished(1000);
                    }
                    if (awaiting_smoke_shutdown)
                    {
                        awaiting_smoke_shutdown = false;
                        application.exit(smoke_exit_code);
                    }
                });
        });

    window.show();
    if (parser.isSet(smoke_option))
    {
        bool timeout_ok = false;
        const int timeout = parser.value(smoke_timeout_option).toInt(&timeout_ok);
        const int effective_timeout = timeout_ok ? timeout : 15000;
        QTimer::singleShot(
            0,
            &window,
            [&window, source_root, effective_timeout, smoke_screenshot]()
            {
                const QDir source(source_root);
                window.start_acceptance_smoke(
                    source.filePath(QStringLiteral("examples/cube.scene.json")),
                    source.filePath(QStringLiteral("examples/post_process.json")),
                    effective_timeout,
                    smoke_screenshot);
            });
    }
    return application.exec();
}
