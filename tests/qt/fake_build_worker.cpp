#include <QCoreApplication>
#include <QDebug>
#include <QStringList>
#include <QThread>

int main(int argument_count, char** arguments)
{
    QCoreApplication application(argument_count, arguments);
    const QStringList values = application.arguments();
    int delay_milliseconds = 0;
    int exit_code = 0;
    for (qsizetype index = 1; index + 1 < values.size(); ++index)
    {
        if (values.at(index) == QStringLiteral("--delay"))
        {
            delay_milliseconds = values.at(index + 1).toInt();
            ++index;
        }
        else if (values.at(index) == QStringLiteral("--exit-code"))
        {
            exit_code = values.at(index + 1).toInt();
            ++index;
        }
    }
    if (delay_milliseconds > 0)
    {
        QThread::msleep(static_cast<unsigned long>(delay_milliseconds));
    }
    qInfo().noquote() << QStringLiteral("fake module build completed");
    return exit_code;
}
