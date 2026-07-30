#include "build_controller.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>

namespace Kalam::Editor
{

Build_Controller::Build_Controller(QObject* parent)
    : QObject(parent)
{
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(
        &process_,
        &QProcess::readyReadStandardOutput,
        this,
        &Build_Controller::read_standard_output);
    connect(
        &process_,
        &QProcess::readyReadStandardError,
        this,
        &Build_Controller::read_standard_error);
    connect(
        &process_,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this,
        &Build_Controller::process_finished);
    connect(
        &process_,
        &QProcess::errorOccurred,
        this,
        &Build_Controller::process_error);
}

Build_Controller::~Build_Controller()
{
    shutdown();
}

void Build_Controller::configure(
    const QString& build_directory,
    const QString& canonical_module_dll,
    const QString& ninja_program,
    const QStringList& ninja_arguments)
{
    Q_ASSERT(!is_building());
    build_directory_ = QFileInfo(build_directory).absoluteFilePath();
    canonical_module_dll_ =
        QFileInfo(canonical_module_dll).absoluteFilePath();
    ninja_program_ = ninja_program;
    ninja_arguments_ = ninja_arguments;
}

quint64 Build_Controller::request_build()
{
    if (shutting_down_)
    {
        return latest_requested_generation_;
    }
    ++latest_requested_generation_;
    if (latest_requested_generation_ == 0)
    {
        ++latest_requested_generation_;
    }

    if (is_building())
    {
        pending_newer_build_ = true;
        emit buildStatus(QStringLiteral("newer module build coalesced"));
    }
    else
    {
        start_build(latest_requested_generation_);
    }
    return latest_requested_generation_;
}

void Build_Controller::set_generation_floor(quint64 generation)
{
    if (!is_building() && generation > latest_requested_generation_)
    {
        latest_requested_generation_ = generation;
    }
}

void Build_Controller::shutdown()
{
    if (shutting_down_)
    {
        return;
    }
    shutting_down_ = true;
    pending_newer_build_ = false;
    if (is_building())
    {
        process_.terminate();
        if (!process_.waitForFinished(1500))
        {
            process_.kill();
            process_.waitForFinished(500);
        }
    }
}

bool Build_Controller::is_building() const
{
    return active_generation_ != 0 ||
        process_.state() != QProcess::NotRunning;
}

quint64 Build_Controller::latest_requested_generation() const
{
    return latest_requested_generation_;
}

QVector<Diagnostic> Build_Controller::parse_diagnostics(
    const QString& output)
{
    static const QRegularExpression msvc_pattern(
        QStringLiteral(
            R"(^(.+)\((\d+)(?:,(\d+))?\)\s*:\s*)"
            R"((fatal error|error|warning|note)\s*)"
            R"((?:([A-Za-z]+\d+)\s*:\s*)?(.*)$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression clang_pattern(
        QStringLiteral(
            R"(^(.+?):(\d+):(\d+):\s*)"
            R"((fatal error|error|warning|note)\s*:\s*(.*)$)"),
        QRegularExpression::CaseInsensitiveOption);

    QVector<Diagnostic> diagnostics;
    const QStringList lines = output.split(
        QRegularExpression(QStringLiteral("[\r\n]+")),
        Qt::SkipEmptyParts);
    for (const QString& raw_line : lines)
    {
        const QString line = raw_line.trimmed();
        QRegularExpressionMatch match = msvc_pattern.match(line);
        Diagnostic diagnostic;
        if (match.hasMatch())
        {
            diagnostic.file = QDir::fromNativeSeparators(match.captured(1));
            diagnostic.line = match.captured(2).toInt();
            diagnostic.column = match.captured(3).isEmpty()
                ? 1
                : match.captured(3).toInt();
            diagnostic.type = match.captured(4).toLower();
            diagnostic.code = match.captured(5);
            diagnostic.message = match.captured(6).trimmed();
            if (!diagnostic.code.isEmpty())
            {
                diagnostic.message.prepend(diagnostic.code + QStringLiteral(": "));
            }
            diagnostics.push_back(diagnostic);
            continue;
        }

        match = clang_pattern.match(line);
        if (match.hasMatch())
        {
            diagnostic.file = QDir::fromNativeSeparators(match.captured(1));
            diagnostic.line = match.captured(2).toInt();
            diagnostic.column = match.captured(3).toInt();
            diagnostic.type = match.captured(4).toLower();
            diagnostic.message = match.captured(5).trimmed();
            diagnostics.push_back(diagnostic);
        }
    }
    return diagnostics;
}

void Build_Controller::read_standard_output()
{
    append_output(process_.readAllStandardOutput());
}

void Build_Controller::read_standard_error()
{
    append_output(process_.readAllStandardError());
}

void Build_Controller::process_finished(
    int exit_code,
    QProcess::ExitStatus exit_status)
{
    if (active_generation_ == 0)
    {
        return;
    }
    read_standard_output();
    read_standard_error();
    const quint64 completed_generation = active_generation_;
    active_generation_ = 0;

    const bool stale =
        completed_generation != latest_requested_generation_;
    const bool succeeded = exit_status == QProcess::NormalExit &&
        exit_code == 0 &&
        !process_error_seen_;
    if (!stale && !shutting_down_)
    {
        const QString output = QString::fromLocal8Bit(combined_output_);
        if (succeeded)
        {
            emit quickfixReady(
                {},
                QStringLiteral("Kalam module build"),
                false);
            emit buildStatus(
                QStringLiteral("module build %1 succeeded")
                    .arg(completed_generation));
            emit moduleBuilt(completed_generation, canonical_module_dll_);
        }
        else
        {
            QVector<Diagnostic> diagnostics = parse_diagnostics(output);
            if (diagnostics.isEmpty())
            {
                Diagnostic diagnostic;
                diagnostic.type = QStringLiteral("error");
                diagnostic.message = output.trimmed();
                if (diagnostic.message.isEmpty())
                {
                    diagnostic.message = QStringLiteral(
                        "Ninja failed without a compiler diagnostic");
                }
                diagnostics.push_back(diagnostic);
            }
            emit quickfixReady(
                diagnostics,
                QStringLiteral("Kalam module build"),
                true);
            emit buildFailed(
                completed_generation,
                QStringLiteral("module build failed with exit code %1")
                    .arg(exit_code));
        }
    }

    start_coalesced_build_if_needed();
}

void Build_Controller::process_error(QProcess::ProcessError error)
{
    process_error_seen_ = true;
    append_output(
        QStringLiteral("ninja process error %1: %2\n")
            .arg(static_cast<int>(error))
            .arg(process_.errorString())
            .toLocal8Bit());
    if (error == QProcess::FailedToStart)
    {
        QTimer::singleShot(
            0,
            this,
            [this]()
            {
                if (active_generation_ != 0 &&
                    process_.state() == QProcess::NotRunning)
                {
                    process_finished(-1, QProcess::CrashExit);
                }
            });
    }
}

void Build_Controller::start_build(quint64 generation)
{
    if (shutting_down_ || is_building())
    {
        return;
    }

    active_generation_ = generation;
    pending_newer_build_ = false;
    process_error_seen_ = false;
    combined_output_.clear();
    process_.setWorkingDirectory(build_directory_);
    process_.setProgram(ninja_program_);
    process_.setArguments(ninja_arguments_);
    process_.start();
    emit buildStarted(generation);
    emit buildStatus(
        QStringLiteral("building game module (generation %1)").arg(generation));
}

void Build_Controller::append_output(const QByteArray& bytes)
{
    if (bytes.isEmpty())
    {
        return;
    }
    combined_output_.append(bytes);
    emit buildStatus(QString::fromLocal8Bit(bytes).trimmed());
}

void Build_Controller::start_coalesced_build_if_needed()
{
    if (shutting_down_)
    {
        return;
    }
    if (pending_newer_build_)
    {
        pending_newer_build_ = false;
        start_build(latest_requested_generation_);
    }
}

} // namespace Kalam::Editor
