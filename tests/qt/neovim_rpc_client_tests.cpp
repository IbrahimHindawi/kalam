#include "neovim_msgpack.h"
#include "neovim_rpc_client.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QIODevice>
#include <QProcess>
#include <QVariantList>

#include <string.h>

using Kalam::Editor::Msgpack_Decode_Result;
using Kalam::Editor::Msgpack_Status;
using Kalam::Editor::Neovim_Rpc_Client;
using Kalam::Editor::Neovim_Rpc_Request;
using Kalam::Editor::msgpack_decode;
using Kalam::Editor::msgpack_encode;

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

template <typename Predicate>
bool spin_until(Predicate predicate, int timeout_milliseconds = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate())
    {
        if (timer.elapsed() > timeout_milliseconds)
        {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

/*
 * One end of an in-process duplex pipe. Writes appear as readable bytes on
 * the partner end; readyRead is delivered through the event loop so the
 * client never observes reentrant delivery inside its own write.
 */
class Pipe_Device final : public QIODevice
{
public:
    explicit Pipe_Device(QObject* parent = nullptr)
        : QIODevice(parent)
    {
        open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    }

    void set_partner(Pipe_Device* partner)
    {
        partner_ = partner;
    }

    bool isSequential() const override
    {
        return true;
    }

    qint64 bytesAvailable() const override
    {
        return incoming_.size() + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 maximum_size) override
    {
        const qint64 count =
            qMin(maximum_size, qint64(incoming_.size()));
        if (count > 0)
        {
            memcpy(data, incoming_.constData(), size_t(count));
            incoming_.remove(0, int(count));
        }
        return count;
    }

    qint64 writeData(const char* data, qint64 size) override
    {
        if (partner_ != nullptr)
        {
            partner_->deliver(QByteArray(data, int(size)));
        }
        return size;
    }

private:
    Pipe_Device* partner_ = nullptr;
    QByteArray incoming_;

    void deliver(const QByteArray& bytes)
    {
        incoming_.append(bytes);
        QMetaObject::invokeMethod(
            this,
            [this]()
            {
                emit readyRead();
            },
            Qt::QueuedConnection);
    }
};

/* Decodes complete msgpack values arriving on its end of the pipe. */
class Fake_Peer final : public QObject
{
public:
    explicit Fake_Peer(Pipe_Device* device, QObject* parent = nullptr)
        : QObject(parent)
        , device_(device)
    {
        connect(
            device_,
            &QIODevice::readyRead,
            this,
            [this]()
            {
                buffer_.append(device_->readAll());
                qint64 offset = 0;
                for (;;)
                {
                    const Msgpack_Decode_Result result =
                        msgpack_decode(buffer_, offset);
                    if (result.status != Msgpack_Status::Ok)
                    {
                        break;
                    }
                    offset += result.bytes_consumed;
                    messages.append(result.value.toList());
                }
                if (offset > 0)
                {
                    buffer_.remove(0, int(offset));
                }
            });
    }

    QList<QVariantList> messages;

    void send_value(const QVariant& value)
    {
        QByteArray wire;
        const Msgpack_Status status = msgpack_encode(value, wire);
        check(
            status == Msgpack_Status::Ok,
            QStringLiteral("the fake peer encodes its own traffic"));
        device_->write(wire);
    }

    void respond(quint32 message_id, const QVariant& error, const QVariant& result)
    {
        QVariantList message;
        message << qlonglong(1) << qulonglong(message_id) << error << result;
        send_value(QVariant(message));
    }

    void notify(const QByteArray& name, const QVariantList& arguments)
    {
        QVariantList message;
        message << qlonglong(2) << name << QVariant(arguments);
        send_value(QVariant(message));
    }

    void send_raw(const QByteArray& bytes)
    {
        device_->write(bytes);
    }

private:
    Pipe_Device* device_;
    QByteArray buffer_;
};

struct Fake_Rig
{
    Pipe_Device* client_end;
    Pipe_Device* peer_end;
    Fake_Peer* peer;
    Neovim_Rpc_Client* client;
};

Fake_Rig make_rig(QObject* parent)
{
    Fake_Rig rig;
    rig.client_end = new Pipe_Device(parent);
    rig.peer_end = new Pipe_Device(parent);
    rig.client_end->set_partner(rig.peer_end);
    rig.peer_end->set_partner(rig.client_end);
    rig.peer = new Fake_Peer(rig.peer_end, parent);
    rig.client = new Neovim_Rpc_Client(parent);
    rig.client->attach_device(rig.client_end);
    return rig;
}

void test_request_response_correlation()
{
    QObject arena;
    Fake_Rig rig = make_rig(&arena);

    QVariant first_result;
    QVariant second_result;
    int finished_count = 0;

    Neovim_Rpc_Request* first =
        rig.client->call(QByteArrayLiteral("first_method"), QVariantList());
    QObject::connect(
        first,
        &Neovim_Rpc_Request::finished,
        &arena,
        [&](quint32, const QVariant& result)
        {
            first_result = result;
            ++finished_count;
        });

    Neovim_Rpc_Request* second =
        rig.client->call(QByteArrayLiteral("second_method"), QVariantList());
    QObject::connect(
        second,
        &Neovim_Rpc_Request::finished,
        &arena,
        [&](quint32, const QVariant& result)
        {
            second_result = result;
            ++finished_count;
        });

    check(
        spin_until([&]() { return rig.peer->messages.size() == 2; }),
        QStringLiteral("the peer receives both requests"));
    check(
        rig.peer->messages.at(0).at(2).toByteArray() ==
            QByteArrayLiteral("first_method"),
        QStringLiteral("the first request carries its method name"));

    const quint32 first_id =
        quint32(rig.peer->messages.at(0).at(1).toULongLong());
    const quint32 second_id =
        quint32(rig.peer->messages.at(1).at(1).toULongLong());
    check(
        first_id != second_id,
        QStringLiteral("message ids are distinct"));

    /* Answer in reverse order; correlation must hold. */
    rig.peer->respond(second_id, QVariant(), QByteArrayLiteral("two"));
    rig.peer->respond(first_id, QVariant(), QByteArrayLiteral("one"));

    check(
        spin_until([&]() { return finished_count == 2; }),
        QStringLiteral("both requests finish"));
    check(
        first_result.toByteArray() == QByteArrayLiteral("one"),
        QStringLiteral("the first request receives its own result"));
    check(
        second_result.toByteArray() == QByteArrayLiteral("two"),
        QStringLiteral("the second request receives its own result"));
}

void test_error_response_and_unknown_msgid()
{
    QObject arena;
    Fake_Rig rig = make_rig(&arena);

    QString failure_message;
    Neovim_Rpc_Request* request =
        rig.client->call(QByteArrayLiteral("failing_method"), QVariantList());
    QObject::connect(
        request,
        &Neovim_Rpc_Request::failed,
        &arena,
        [&](quint32, const QVariant&, const QString& message)
        {
            failure_message = message;
        });

    check(
        spin_until([&]() { return rig.peer->messages.size() == 1; }),
        QStringLiteral("the peer receives the failing request"));
    const quint32 message_id =
        quint32(rig.peer->messages.at(0).at(1).toULongLong());

    /* A response for a message id that was never issued is dropped. */
    rig.peer->respond(
        message_id + 700,
        QVariant(),
        QByteArrayLiteral("orphan"));

    QVariantList error;
    error << qlonglong(0) << QByteArrayLiteral("boom");
    rig.peer->respond(message_id, QVariant(error), QVariant());

    check(
        spin_until([&]() { return !failure_message.isEmpty(); }),
        QStringLiteral("the error response fails the request"));
    check(
        failure_message == QStringLiteral("boom"),
        QStringLiteral("the error message is extracted from the payload"));
    check(
        rig.client->is_connected(),
        QStringLiteral("an unknown msgid does not disconnect the channel"));
}

void test_notification_dispatch()
{
    QObject arena;
    Fake_Rig rig = make_rig(&arena);

    QList<QByteArray> notification_names;
    int redraw_count = 0;
    QVariantList redraw_events;

    QObject::connect(
        rig.client,
        &Neovim_Rpc_Client::notificationReceived,
        &arena,
        [&](const QByteArray& name, const QVariantList&)
        {
            notification_names.append(name);
        });
    QObject::connect(
        rig.client,
        &Neovim_Rpc_Client::redrawReceived,
        &arena,
        [&](const QVariantList& events)
        {
            ++redraw_count;
            redraw_events = events;
        });

    QVariantList flush_event;
    flush_event << QByteArrayLiteral("flush") << QVariant(QVariantList());
    rig.peer->notify(
        QByteArrayLiteral("redraw"),
        QVariantList() << QVariant(flush_event));
    rig.peer->notify(
        QByteArrayLiteral("nveditor_event"),
        QVariantList() << QByteArrayLiteral("payload"));

    check(
        spin_until([&]() { return notification_names.size() == 2; }),
        QStringLiteral("both notifications arrive"));
    check(
        redraw_count == 1,
        QStringLiteral("redraw is routed to the dedicated signal"));
    check(
        redraw_events.size() == 1 &&
            redraw_events.at(0).toList().at(0).toByteArray() ==
                QByteArrayLiteral("flush"),
        QStringLiteral("redraw events pass through intact"));
    check(
        notification_names.at(1) == QByteArrayLiteral("nveditor_event"),
        QStringLiteral("other notifications keep their names"));
}

void test_incoming_request_is_answered()
{
    QObject arena;
    Fake_Rig rig = make_rig(&arena);

    QByteArray seen_method;
    QObject::connect(
        rig.client,
        &Neovim_Rpc_Client::requestReceived,
        &arena,
        [&](quint32, const QByteArray& method, const QVariantList&)
        {
            seen_method = method;
        });

    QVariantList request;
    request
        << qlonglong(0)
        << qulonglong(31)
        << QByteArrayLiteral("do_something")
        << QVariant(QVariantList());
    rig.peer->send_value(QVariant(request));

    check(
        spin_until([&]() { return rig.peer->messages.size() == 1; }),
        QStringLiteral("the client answers the peer request"));
    const QVariantList response = rig.peer->messages.at(0);
    check(
        response.at(0).toLongLong() == 1 &&
            quint32(response.at(1).toULongLong()) == 31 &&
            response.at(2).isValid(),
        QStringLiteral("the answer is an error response for msgid 31"));
    check(
        seen_method == QByteArrayLiteral("do_something"),
        QStringLiteral("the request is surfaced for observability"));
}

void test_garbage_disconnects_and_fails_pending()
{
    QObject arena;
    Fake_Rig rig = make_rig(&arena);

    int disconnect_count = 0;
    QString disconnect_reason;
    QObject::connect(
        rig.client,
        &Neovim_Rpc_Client::disconnected,
        &arena,
        [&](const QString& reason)
        {
            ++disconnect_count;
            disconnect_reason = reason;
        });

    QString pending_failure;
    Neovim_Rpc_Request* request =
        rig.client->call(QByteArrayLiteral("never_answered"), QVariantList());
    QObject::connect(
        request,
        &Neovim_Rpc_Request::failed,
        &arena,
        [&](quint32, const QVariant&, const QString& message)
        {
            pending_failure = message;
        });

    rig.peer->send_raw(QByteArrayLiteral("\xc1\xc1\xc1"));

    check(
        spin_until([&]() { return disconnect_count == 1; }),
        QStringLiteral("garbage on the wire disconnects"));
    check(
        !disconnect_reason.isEmpty(),
        QStringLiteral("the disconnect carries a reason"));
    check(
        spin_until([&]() { return !pending_failure.isEmpty(); }),
        QStringLiteral("pending requests fail on disconnect"));
    check(
        !rig.client->is_connected(),
        QStringLiteral("the client reports itself disconnected"));

    /* More garbage must not produce a second disconnect. */
    rig.peer->send_raw(QByteArrayLiteral("\xc1"));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    check(
        disconnect_count == 1,
        QStringLiteral("disconnected is reported exactly once"));
}

void test_request_timeout()
{
    QObject arena;
    Fake_Rig rig = make_rig(&arena);

    QString failure_message;
    Neovim_Rpc_Request* request =
        rig.client->call(QByteArrayLiteral("slow_method"), QVariantList());
    QObject::connect(
        request,
        &Neovim_Rpc_Request::failed,
        &arena,
        [&](quint32, const QVariant&, const QString& message)
        {
            failure_message = message;
        });
    request->set_timeout(50);

    check(
        spin_until([&]() { return !failure_message.isEmpty(); }),
        QStringLiteral("an unanswered request times out"));
    check(
        failure_message == QStringLiteral("timed out"),
        QStringLiteral("the timeout is reported as such"));
    check(
        rig.client->is_connected(),
        QStringLiteral("a timeout does not disconnect the channel"));
}

void run_real_neovim_tests(const QString& neovim_program)
{
    QObject arena;
    auto* client = new Neovim_Rpc_Client(&arena);

    int ready_count = 0;
    int exit_count = 0;
    QObject::connect(
        client,
        &Neovim_Rpc_Client::ready,
        &arena,
        [&]()
        {
            ++ready_count;
        });
    QObject::connect(
        client,
        &Neovim_Rpc_Client::processExited,
        &arena,
        [&](int)
        {
            ++exit_count;
        });

    check(
        client->spawn(neovim_program, QStringList()
            << QStringLiteral("--clean")),
        QStringLiteral("nvim --embed spawns"));
    check(
        spin_until([&]() { return ready_count == 1; }, 15000),
        QStringLiteral("the handshake completes against real Neovim"));
    check(
        client->is_ready(),
        QStringLiteral("the client is ready after the handshake"));
    check(
        client->channel_id() > 0,
        QStringLiteral("the channel id is recorded"));
    check(
        client->api_level() >= Neovim_Rpc_Client::minimum_api_level(),
        QStringLiteral("the api level meets the floor (got %1)")
            .arg(client->api_level()));

    QVariant evaluated;
    Neovim_Rpc_Request* eval_request = client->call(
        QByteArrayLiteral("nvim_eval"),
        QVariantList() << QByteArrayLiteral("2 + 2"));
    QObject::connect(
        eval_request,
        &Neovim_Rpc_Request::finished,
        &arena,
        [&](quint32, const QVariant& result)
        {
            evaluated = result;
        });
    check(
        spin_until([&]() { return evaluated.isValid(); }, 10000),
        QStringLiteral("nvim_eval answers"));
    check(
        evaluated.toLongLong() == 4,
        QStringLiteral("nvim_eval computes over the channel"));

    client->call(
        QByteArrayLiteral("nvim_command"),
        QVariantList() << QByteArrayLiteral("qall!"));
    check(
        spin_until([&]() { return exit_count == 1; }, 10000),
        QStringLiteral("qall! terminates the embedded process"));
}

} // namespace

int main(int argument_count, char** arguments)
{
    QCoreApplication application(argument_count, arguments);

    QString neovim_program;
    const QStringList application_arguments =
        QCoreApplication::arguments();
    const int neovim_flag =
        application_arguments.indexOf(QStringLiteral("--nvim"));
    if (neovim_flag >= 0 &&
        neovim_flag + 1 < application_arguments.size())
    {
        neovim_program = application_arguments.at(neovim_flag + 1);
    }

    if (neovim_program.isEmpty())
    {
        test_request_response_correlation();
        test_error_response_and_unknown_msgid();
        test_notification_dispatch();
        test_incoming_request_is_answered();
        test_garbage_disconnects_and_fails_pending();
        test_request_timeout();
    }
    else
    {
        run_real_neovim_tests(neovim_program);
    }

    if (failures == 0)
    {
        qInfo().noquote() << QStringLiteral("all RPC client tests passed");
    }
    return failures == 0 ? 0 : 1;
}
