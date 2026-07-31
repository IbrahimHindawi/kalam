#include "neovim_rpc_client.h"

#include <QIODevice>
#include <QProcess>
#include <QTimer>
#include <QtDebug>

namespace Kalam::Editor
{

namespace
{

enum
{
    Rpc_Message_Request = 0,
    Rpc_Message_Response = 1,
    Rpc_Message_Notification = 2
};

const qint64 Rpc_Minimum_Api_Level = 11;

QString error_message_from_payload(const QVariant& error)
{
    if (error.userType() == QMetaType::QByteArray)
    {
        return QString::fromUtf8(error.toByteArray());
    }
    if (error.userType() == QMetaType::QVariantList)
    {
        const QVariantList parts = error.toList();
        if (parts.size() == 2 &&
            parts.at(1).userType() == QMetaType::QByteArray)
        {
            return QString::fromUtf8(parts.at(1).toByteArray());
        }
    }
    return QStringLiteral("RPC error");
}

} // namespace

/* ------------------------------------------------- Neovim_Rpc_Request */

Neovim_Rpc_Request::Neovim_Rpc_Request(quint32 message_id, QObject* parent)
    : QObject(parent)
    , message_id_(message_id)
{
}

quint32 Neovim_Rpc_Request::message_id() const
{
    return message_id_;
}

void Neovim_Rpc_Request::set_timeout(int milliseconds)
{
    if (settled_)
    {
        return;
    }
    if (timeout_timer_ == nullptr)
    {
        timeout_timer_ = new QTimer(this);
        timeout_timer_->setSingleShot(true);
        connect(
            timeout_timer_,
            &QTimer::timeout,
            this,
            [this]()
            {
                settle_failed(QVariant(), QStringLiteral("timed out"));
            });
    }
    timeout_timer_->start(milliseconds);
}

void Neovim_Rpc_Request::settle_finished(const QVariant& result)
{
    if (settled_)
    {
        return;
    }
    settled_ = true;
    if (timeout_timer_ != nullptr)
    {
        timeout_timer_->stop();
    }
    emit finished(message_id_, result);
    deleteLater();
}

void Neovim_Rpc_Request::settle_failed(
    const QVariant& error,
    const QString& message)
{
    if (settled_)
    {
        return;
    }
    settled_ = true;
    if (timeout_timer_ != nullptr)
    {
        timeout_timer_->stop();
    }
    emit failed(message_id_, error, message);
    deleteLater();
}

/* -------------------------------------------------- Neovim_Rpc_Client */

Neovim_Rpc_Client::Neovim_Rpc_Client(QObject* parent)
    : QObject(parent)
{
}

qint64 Neovim_Rpc_Client::minimum_api_level()
{
    return Rpc_Minimum_Api_Level;
}

bool Neovim_Rpc_Client::spawn(
    const QString& program,
    const QStringList& extra_arguments)
{
    if (device_ != nullptr)
    {
        qWarning().noquote()
            << QStringLiteral("RPC client is already connected");
        return false;
    }

    auto* process = new QProcess(this);
    process->setProcessChannelMode(QProcess::ForwardedErrorChannel);

    QStringList arguments;
    arguments << QStringLiteral("--embed");
    arguments << extra_arguments;

    connect(
        process,
        &QProcess::finished,
        this,
        [this](int exit_code, QProcess::ExitStatus)
        {
            emit processExited(exit_code);
            report_disconnect(QStringLiteral("the Neovim process exited"));
        });
    connect(
        process,
        &QProcess::errorOccurred,
        this,
        [this, process](QProcess::ProcessError)
        {
            report_disconnect(
                QStringLiteral("Neovim process error: %1")
                    .arg(process->errorString()));
        });

    process->start(program, arguments, QIODevice::ReadWrite);
    if (!process->waitForStarted(10000))
    {
        const QString reason = QStringLiteral("could not start %1: %2")
            .arg(program, process->errorString());
        process->deleteLater();
        report_disconnect(reason);
        return false;
    }

    process_ = process;
    attach_device(process);
    start_handshake();
    return true;
}

void Neovim_Rpc_Client::attach_device(QIODevice* device)
{
    device_ = device;
    disconnect_reported_ = false;
    connect(
        device,
        &QIODevice::readyRead,
        this,
        &Neovim_Rpc_Client::handle_ready_read);
    if (device->bytesAvailable() > 0)
    {
        handle_ready_read();
    }
}

void Neovim_Rpc_Client::start_handshake()
{
    if (handshake_started_ || device_ == nullptr)
    {
        return;
    }
    handshake_started_ = true;

    Neovim_Rpc_Request* request =
        call(QByteArrayLiteral("nvim_get_api_info"), QVariantList());
    connect(
        request,
        &Neovim_Rpc_Request::finished,
        this,
        [this](quint32, const QVariant& result)
        {
            finish_handshake(result);
        });
    connect(
        request,
        &Neovim_Rpc_Request::failed,
        this,
        [this](quint32, const QVariant&, const QString& message)
        {
            report_disconnect(
                QStringLiteral("nvim_get_api_info failed: %1").arg(message));
        });
    request->set_timeout(10000);
}

void Neovim_Rpc_Client::finish_handshake(const QVariant& api_info_result)
{
    const QVariantList info = api_info_result.toList();
    if (info.size() != 2)
    {
        report_disconnect(
            QStringLiteral("nvim_get_api_info returned an unexpected shape"));
        return;
    }

    channel_id_ = info.at(0).toULongLong();
    api_metadata_ = info.at(1).toMap();
    const QVariantMap version =
        api_metadata_.value(QStringLiteral("version")).toMap();
    api_level_ = version.value(QStringLiteral("api_level")).toLongLong();

    if (api_level_ < Rpc_Minimum_Api_Level)
    {
        report_disconnect(
            QStringLiteral("Neovim api_level %1 is below the required %2")
                .arg(api_level_)
                .arg(Rpc_Minimum_Api_Level));
        return;
    }

    QVariantMap client_version;
    client_version.insert(QStringLiteral("major"), 0);
    client_version.insert(QStringLiteral("minor"), 1);
    client_version.insert(QStringLiteral("patch"), 0);

    QVariantList parameters;
    parameters
        << QByteArrayLiteral("nveditor")
        << client_version
        << QByteArrayLiteral("ui")
        << QVariantMap()
        << QVariantMap();

    Neovim_Rpc_Request* request =
        call(QByteArrayLiteral("nvim_set_client_info"), parameters);
    connect(
        request,
        &Neovim_Rpc_Request::finished,
        this,
        [this](quint32, const QVariant&)
        {
            ready_ = true;
            emit ready();
        });
    connect(
        request,
        &Neovim_Rpc_Request::failed,
        this,
        [this](quint32, const QVariant&, const QString& message)
        {
            report_disconnect(
                QStringLiteral("nvim_set_client_info failed: %1").arg(message));
        });
    request->set_timeout(10000);
}

bool Neovim_Rpc_Client::is_connected() const
{
    return device_ != nullptr && !disconnect_reported_;
}

bool Neovim_Rpc_Client::is_ready() const
{
    return ready_ && is_connected();
}

quint64 Neovim_Rpc_Client::channel_id() const
{
    return channel_id_;
}

qint64 Neovim_Rpc_Client::api_level() const
{
    return api_level_;
}

QVariantMap Neovim_Rpc_Client::api_metadata() const
{
    return api_metadata_;
}

QProcess* Neovim_Rpc_Client::process() const
{
    return process_;
}

Neovim_Rpc_Request* Neovim_Rpc_Client::call(
    const QByteArray& method,
    const QVariantList& parameters)
{
    const quint32 message_id = next_message_id_++;
    auto* request = new Neovim_Rpc_Request(message_id, this);
    pending_.insert(message_id, request);

    if (!is_connected())
    {
        QTimer::singleShot(
            0,
            request,
            [request]()
            {
                request->settle_failed(
                    QVariant(),
                    QStringLiteral("the RPC channel is not connected"));
            });
        return request;
    }

    QVariantList message;
    message
        << qlonglong(Rpc_Message_Request)
        << qulonglong(message_id)
        << method
        << QVariant(parameters);
    send_message(message);
    return request;
}

void Neovim_Rpc_Client::notify(
    const QByteArray& method,
    const QVariantList& parameters)
{
    if (!is_connected())
    {
        return;
    }
    QVariantList message;
    message
        << qlonglong(Rpc_Message_Notification)
        << method
        << QVariant(parameters);
    send_message(message);
}

void Neovim_Rpc_Client::send_message(const QVariantList& message)
{
    QByteArray wire;
    const Msgpack_Status status = msgpack_encode(QVariant(message), wire);
    if (status != Msgpack_Status::Ok)
    {
        qCritical().noquote()
            << QStringLiteral("RPC encode failed: %1")
                .arg(QString::fromUtf8(msgpack_status_string(status)));
        return;
    }
    if (device_ != nullptr)
    {
        device_->write(wire);
    }
}

void Neovim_Rpc_Client::handle_ready_read()
{
    if (device_ == nullptr)
    {
        return;
    }
    receive_buffer_.append(device_->readAll());
    drain_receive_buffer();
}

void Neovim_Rpc_Client::drain_receive_buffer()
{
    qint64 offset = 0;
    for (;;)
    {
        const Msgpack_Decode_Result result =
            msgpack_decode(receive_buffer_, offset);
        if (result.status == Msgpack_Status::Need_More_Data)
        {
            break;
        }
        if (result.status != Msgpack_Status::Ok)
        {
            report_disconnect(
                QStringLiteral("protocol error while decoding the RPC "
                               "stream: %1")
                    .arg(result.error.isEmpty()
                        ? QString::fromUtf8(
                              msgpack_status_string(result.status))
                        : result.error));
            return;
        }
        offset += result.bytes_consumed;
        dispatch_message(result.value);
        if (device_ == nullptr || disconnect_reported_)
        {
            return;
        }
    }
    if (offset > 0)
    {
        receive_buffer_.remove(0, offset);
    }
}

void Neovim_Rpc_Client::dispatch_message(const QVariant& value)
{
    if (value.userType() != QMetaType::QVariantList)
    {
        report_disconnect(
            QStringLiteral("the RPC peer sent a non-array message"));
        return;
    }
    const QVariantList message = value.toList();
    if (message.isEmpty() ||
        message.at(0).userType() != QMetaType::LongLong)
    {
        report_disconnect(
            QStringLiteral("the RPC peer sent a message without a type"));
        return;
    }

    const qlonglong type = message.at(0).toLongLong();
    if (type == Rpc_Message_Response && message.size() == 4)
    {
        dispatch_response(message);
        return;
    }
    if (type == Rpc_Message_Request && message.size() == 4)
    {
        dispatch_request(message);
        return;
    }
    if (type == Rpc_Message_Notification && message.size() == 3)
    {
        dispatch_notification(message);
        return;
    }
    report_disconnect(
        QStringLiteral("the RPC peer sent message type %1 with %2 elements")
            .arg(type)
            .arg(message.size()));
}

void Neovim_Rpc_Client::dispatch_response(const QVariantList& message)
{
    const quint32 message_id = quint32(message.at(1).toULongLong());
    const QVariant error = message.at(2);
    const QVariant result = message.at(3);

    const QPointer<Neovim_Rpc_Request> request = pending_.take(message_id);
    if (request.isNull())
    {
        qWarning().noquote()
            << QStringLiteral("dropping a response for unknown msgid %1")
                .arg(message_id);
        return;
    }
    if (error.isValid())
    {
        request->settle_failed(error, error_message_from_payload(error));
        return;
    }
    request->settle_finished(result);
}

void Neovim_Rpc_Client::dispatch_request(const QVariantList& message)
{
    const quint32 message_id = quint32(message.at(1).toULongLong());
    const QByteArray method = message.at(2).toByteArray();
    const QVariantList arguments = message.at(3).toList();

    /* Answer immediately so the channel can never deadlock on us. */
    QVariantList error;
    error
        << qlonglong(0)
        << QByteArrayLiteral("nveditor does not implement RPC requests");
    QVariantList response;
    response
        << qlonglong(Rpc_Message_Response)
        << qulonglong(message_id)
        << QVariant(error)
        << QVariant();
    send_message(response);

    emit requestReceived(message_id, method, arguments);
}

void Neovim_Rpc_Client::dispatch_notification(const QVariantList& message)
{
    const QByteArray name = message.at(1).toByteArray();
    const QVariantList arguments = message.at(2).toList();
    if (name == QByteArrayLiteral("redraw"))
    {
        emit redrawReceived(arguments);
    }
    emit notificationReceived(name, arguments);
}

void Neovim_Rpc_Client::report_disconnect(const QString& reason)
{
    if (disconnect_reported_)
    {
        return;
    }
    disconnect_reported_ = true;
    ready_ = false;

    if (device_ != nullptr)
    {
        disconnect(device_, nullptr, this, nullptr);
    }

    const QList<QPointer<Neovim_Rpc_Request>> outstanding = pending_.values();
    pending_.clear();
    for (const QPointer<Neovim_Rpc_Request>& request : outstanding)
    {
        if (!request.isNull())
        {
            request->settle_failed(
                QVariant(),
                QStringLiteral("disconnected: %1").arg(reason));
        }
    }

    emit disconnected(reason);
}

} // namespace Kalam::Editor
