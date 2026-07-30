#ifndef KALAM_EDITOR_QT_BUILD_CONTROLLER_H
#define KALAM_EDITOR_QT_BUILD_CONTROLLER_H

#include "editor_types.h"

#include <QObject>
#include <QProcess>
#include <QStringList>

namespace Kalam::Editor
{

class Build_Controller final : public QObject
{
    Q_OBJECT

public:
    explicit Build_Controller(QObject* parent = nullptr);
    ~Build_Controller() override;

    void configure(
        const QString& build_directory,
        const QString& canonical_module_dll,
        const QString& ninja_program = QStringLiteral("ninja"),
        const QStringList& ninja_arguments =
            QStringList{QStringLiteral("game_module")});
    quint64 request_build();
    void set_generation_floor(quint64 generation);
    void shutdown();

    bool is_building() const;
    quint64 latest_requested_generation() const;

    static QVector<Diagnostic> parse_diagnostics(const QString& output);

signals:
    void buildStarted(quint64 generation);
    void buildStatus(QString message);
    void quickfixReady(
        QVector<Kalam::Editor::Diagnostic> diagnostics,
        QString title,
        bool open_first_error);
    void moduleBuilt(
        quint64 generation,
        QString canonical_module_dll);
    void buildFailed(quint64 generation, QString summary);

private slots:
    void read_standard_output();
    void read_standard_error();
    void process_finished(int exit_code, QProcess::ExitStatus exit_status);
    void process_error(QProcess::ProcessError error);

private:
    QProcess process_;
    QString build_directory_;
    QString canonical_module_dll_;
    QString ninja_program_ = QStringLiteral("ninja");
    QStringList ninja_arguments_{QStringLiteral("game_module")};
    QByteArray combined_output_;
    quint64 latest_requested_generation_ = 0;
    quint64 active_generation_ = 0;
    bool pending_newer_build_ = false;
    bool shutting_down_ = false;
    bool process_error_seen_ = false;

    void start_build(quint64 generation);
    void append_output(const QByteArray& bytes);
    void start_coalesced_build_if_needed();
};

} // namespace Kalam::Editor

#endif
