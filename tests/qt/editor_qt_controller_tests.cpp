#include "author_transaction_controller.h"
#include "build_controller.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QTimer>
#include <QVector>

#ifndef KALAM_QT_FAKE_BUILD_WORKER
#define KALAM_QT_FAKE_BUILD_WORKER ""
#endif

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

QStringList worker_arguments(int delay_milliseconds)
{
    QStringList result;
    result.push_back(QStringLiteral("--delay"));
    result.push_back(QString::number(delay_milliseconds));
    result.push_back(QStringLiteral("--exit-code"));
    result.push_back(QStringLiteral("0"));
    return result;
}

void test_diagnostic_parsing()
{
    const QVector<Kalam::Editor::Diagnostic> diagnostics =
        Kalam::Editor::Build_Controller::parse_diagnostics(
            QStringLiteral(
                "C:\\work\\cube.cpp(42,7): error C2065: missing_name\n"
                "C:/work/post.cpp:8:3: warning: value is unused\n"));
    check(diagnostics.size() == 2, QStringLiteral("two compiler diagnostics"));
    if (diagnostics.size() == 2)
    {
        check(diagnostics.at(0).line == 42, QStringLiteral("MSVC line parsed"));
        check(diagnostics.at(0).column == 7, QStringLiteral("MSVC column parsed"));
        check(diagnostics.at(0).message.contains(QStringLiteral("C2065")),
              QStringLiteral("MSVC code retained"));
        check(diagnostics.at(1).line == 8, QStringLiteral("clang line parsed"));
        check(diagnostics.at(1).type == QStringLiteral("warning"),
              QStringLiteral("clang severity parsed"));
    }
}

void test_value_formatting()
{
    Kalam::Editor::Field_Descriptor color_field;
    color_field.kind = Author_Field_Color4;
    for (int component = 0; component < 4; ++component)
    {
        color_field.step[component] = 0.001f;
    }
    Kalam::Editor::Author_Value_Owned color;
    color.kind = Author_Field_Color4;
    color.f32[0] = 0.1f;
    color.f32[1] = 0.2f;
    color.f32[2] = 0.3f;
    color.f32[3] = 1.0f;
    check(
        Kalam::Editor::Author_Transaction_Controller::format_value_json(
            color_field,
            color) == QByteArrayLiteral("[0.100,0.200,0.300,1.000]"),
        QStringLiteral("color formatting follows schema step"));

    Kalam::Editor::Field_Descriptor transform_field;
    transform_field.kind = Author_Field_Transform;
    Kalam::Editor::Author_Value_Owned transform;
    transform.kind = Author_Field_Transform;
    transform.transform.translation[0] = 1.25f;
    transform.transform.translation[1] = -2.0f;
    transform.transform.translation[2] = 0.0f;
    transform.transform.scale[0] = 1.0f;
    transform.transform.scale[1] = 1.0f;
    transform.transform.scale[2] = 1.0f;
    const QByteArray formatted =
        Kalam::Editor::Author_Transaction_Controller::format_value_json(
            transform_field,
            transform);
    check(
        formatted.contains(
            QByteArrayLiteral(R"("translation":[1.250,-2.000,0.000])")),
        QStringLiteral("transform translation formatting"));
    check(
        formatted.contains(
            QByteArrayLiteral(R"("scale":[1.000,1.000,1.000])")),
        QStringLiteral("transform scale formatting"));
}

void test_single_build()
{
    Kalam::Editor::Build_Controller controller;
    controller.configure(
        QDir::tempPath(),
        QDir(QDir::tempPath()).filePath(QStringLiteral("game_module.dll")),
        QString::fromUtf8(KALAM_QT_FAKE_BUILD_WORKER),
        worker_arguments(25));

    int starts = 0;
    int publishes = 0;
    quint64 published_generation = 0;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(
        &controller,
        &Kalam::Editor::Build_Controller::buildStarted,
        &loop,
        [&starts](quint64)
        {
            ++starts;
        });
    QObject::connect(
        &controller,
        &Kalam::Editor::Build_Controller::moduleBuilt,
        &loop,
        [&loop, &publishes, &published_generation](quint64 generation, const QString&)
        {
            ++publishes;
            published_generation = generation;
            loop.quit();
        });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    controller.request_build();
    loop.exec();
    check(starts == 1, QStringLiteral("single request starts once"));
    check(publishes == 1, QStringLiteral("single success publishes once"));
    check(published_generation == 1, QStringLiteral("single generation is one"));
    check(!controller.is_building(), QStringLiteral("single build becomes idle"));
}

void test_rapid_build_coalescing()
{
    Kalam::Editor::Build_Controller controller;
    controller.configure(
        QDir::tempPath(),
        QDir(QDir::tempPath()).filePath(QStringLiteral("game_module.dll")),
        QString::fromUtf8(KALAM_QT_FAKE_BUILD_WORKER),
        worker_arguments(100));

    QVector<quint64> starts;
    QVector<quint64> publishes;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(
        &controller,
        &Kalam::Editor::Build_Controller::buildStarted,
        &loop,
        [&starts](quint64 generation)
        {
            starts.push_back(generation);
        });
    QObject::connect(
        &controller,
        &Kalam::Editor::Build_Controller::moduleBuilt,
        &loop,
        [&loop, &publishes](quint64 generation, const QString&)
        {
            publishes.push_back(generation);
            if (generation == 3)
            {
                loop.quit();
            }
        });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    controller.request_build();
    controller.request_build();
    controller.request_build();
    loop.exec();

    check(starts.size() == 2, QStringLiteral("rapid requests start current and latest"));
    if (starts.size() == 2)
    {
        check(starts.at(0) == 1, QStringLiteral("current generation starts"));
        check(starts.at(1) == 3, QStringLiteral("only latest queued generation starts"));
    }
    check(publishes.size() == 1 && publishes.first() == 3,
          QStringLiteral("stale successful DLL cannot publish"));
    check(!controller.is_building(), QStringLiteral("coalesced build becomes idle"));
}

void test_failed_start_unwedges()
{
    Kalam::Editor::Build_Controller controller;
    controller.configure(
        QDir::tempPath(),
        QDir(QDir::tempPath()).filePath(QStringLiteral("game_module.dll")),
        QDir(QDir::tempPath()).filePath(
            QStringLiteral("missing-kalam-build-worker.exe")));

    int failures_seen = 0;
    QEventLoop first_loop;
    QEventLoop second_loop;
    QEventLoop* waiting_loop = &first_loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(
        &controller,
        &Kalam::Editor::Build_Controller::buildFailed,
        &first_loop,
        [&failures_seen, &waiting_loop](quint64, const QString&)
        {
            ++failures_seen;
            waiting_loop->quit();
        });
    QObject::connect(
        &timeout,
        &QTimer::timeout,
        &first_loop,
        [&waiting_loop]()
        {
            waiting_loop->quit();
        });

    timeout.start(5000);
    controller.request_build();
    first_loop.exec();
    check(failures_seen == 1, QStringLiteral("failed start reports failure"));
    check(!controller.is_building(), QStringLiteral("failed start becomes idle"));

    waiting_loop = &second_loop;
    timeout.start(5000);
    controller.request_build();
    second_loop.exec();
    check(failures_seen == 2, QStringLiteral("build can retry after failed start"));
    check(!controller.is_building(), QStringLiteral("retry failure becomes idle"));
}

} // namespace

int main(int argument_count, char** arguments)
{
    QCoreApplication application(argument_count, arguments);
    test_diagnostic_parsing();
    test_value_formatting();
    test_single_build();
    test_rapid_build_coalescing();
    test_failed_start_unwedges();
    if (failures == 0)
    {
        qInfo().noquote() << QStringLiteral("all Qt editor controller tests passed");
    }
    return failures == 0 ? 0 : 1;
}
