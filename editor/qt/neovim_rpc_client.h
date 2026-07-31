#ifndef KALAM_EDITOR_QT_NEOVIM_RPC_CLIENT_H
#define KALAM_EDITOR_QT_NEOVIM_RPC_CLIENT_H

#include "neovim_msgpack.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>

class QIODevice;
class QProcess;
class QTimer;

namespace Kalam::Editor
{

/*
 * One outstanding msgpack-RPC request. The client owns the object and
 * destroys it after exactly one of finished or failed has been emitted.
 */
class Neovim_Rpc_Request final : public QObject
{
    Q_OBJECT

public:
    explicit Neovim_Rpc_Request(quint32 message_id, QObject* parent = nullptr);

    quint32 message_id() const;

    /* After the timeout the request fails with a "timed out" message. */
    void set_timeout(int milliseconds);

signals:
    void finished(quint32 message_id, QVariant result);
    void failed(quint32 message_id, QVariant error, QString message);

private:
    friend class Neovim_Rpc_Client;

    quint32 message_id_ = 0;
    QTimer* timeout_timer_ = nullptr;
    bool settled_ = false;

    void settle_finished(const QVariant& result);
    void settle_failed(const QVariant& error, const QString& message);
};

/*
 * msgpack-RPC over one nvim --embed child process (spec_2.md section 4).
 * The wire accepts exactly three shapes: [0, msgid, method, params],
 * [1, msgid, error, result], and [2, method, params]. Anything else is a
 * protocol error and disconnects.
 *
 * Requests arriving from Neovim are answered immediately with a
 * "method not implemented" error so the channel can never deadlock, and
 * are surfaced through requestReceived for observability.
 */
class Neovim_Rpc_Client final : public QObject
{
    Q_OBJECT

public:
    explicit Neovim_Rpc_Client(QObject* parent = nullptr);

    /* Spawns "program --embed extra_arguments..." and starts the handshake. */
    bool spawn(
        const QString& program,
        const QStringList& extra_arguments = QStringList());

    /*
     * Test seam: speak the protocol over an already-open duplex device the
     * caller owns. No handshake is started; call start_handshake() when the
     * peer is a real Neovim.
     */
    void attach_device(QIODevice* device);
    void start_handshake();

    bool is_connected() const;
    bool is_ready() const;
    quint64 channel_id() const;
    qint64 api_level() const;
    QVariantMap api_metadata() const;
    QProcess* process() const;

    Neovim_Rpc_Request* call(
        const QByteArray& method,
        const QVariantList& parameters);
    void notify(const QByteArray& method, const QVariantList& parameters);

    static qint64 minimum_api_level();

signals:
    void ready();
    void notificationReceived(QByteArray name, QVariantList arguments);
    void redrawReceived(QVariantList events);
    void requestReceived(
        quint32 message_id,
        QByteArray method,
        QVariantList arguments);
    void disconnected(QString reason);
    void processExited(int exit_code);

private slots:
    void handle_ready_read();

private:
    QPointer<QIODevice> device_;
    QProcess* process_ = nullptr;
    QByteArray receive_buffer_;
    QHash<quint32, QPointer<Neovim_Rpc_Request>> pending_;
    quint32 next_message_id_ = 1;
    quint64 channel_id_ = 0;
    qint64 api_level_ = 0;
    QVariantMap api_metadata_;
    bool ready_ = false;
    bool disconnect_reported_ = false;
    bool handshake_started_ = false;

    void send_message(const QVariantList& message);
    void drain_receive_buffer();
    void dispatch_message(const QVariant& value);
    void dispatch_response(const QVariantList& message);
    void dispatch_request(const QVariantList& message);
    void dispatch_notification(const QVariantList& message);
    void finish_handshake(const QVariant& api_info_result);
    void report_disconnect(const QString& reason);
};

} // namespace Kalam::Editor

#endif
