#ifndef KALAM_EDITOR_QT_NEOVIM_MSGPACK_H
#define KALAM_EDITOR_QT_NEOVIM_MSGPACK_H

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVariant>

namespace Kalam::Editor
{

/*
 * Msgpack codec for the Neovim RPC channel (spec_2.md section 3).
 *
 * Values map to QVariant as follows:
 *
 *   nil        <-> invalid QVariant
 *   boolean    <-> bool
 *   integer    <-> qlonglong; qulonglong only for values above INT64_MAX
 *   float32/64  -> double; double -> float64
 *   str        <-> QByteArray (a QString encodes as UTF-8 str for
 *                  convenience and decodes back as QByteArray)
 *   bin        <-> Msgpack_Bin
 *   QVariantList holds msgpack arrays
 *   QVariantMap holds msgpack maps with string keys
 *   ext        <-> Msgpack_Ext, opaque; Neovim encodes Buffer, Window, and
 *                  Tabpage handles as ext and the payload is never
 *                  interpreted beyond equality
 */

struct Msgpack_Bin
{
    QByteArray bytes;

    friend bool operator==(const Msgpack_Bin& a, const Msgpack_Bin& b)
    {
        return a.bytes == b.bytes;
    }
};

struct Msgpack_Ext
{
    qint8 type = 0;
    QByteArray payload;

    friend bool operator==(const Msgpack_Ext& a, const Msgpack_Ext& b)
    {
        return a.type == b.type && a.payload == b.payload;
    }
};

struct Msgpack_Limits
{
    int maximum_depth = 64;
    qint64 maximum_container_elements = qint64(1) << 22;
    qint64 maximum_byte_length = qint64(1) << 30;
};

enum class Msgpack_Status
{
    Ok,
    Need_More_Data,
    Malformed,
    Limit_Exceeded,
    Unsupported_Type
};

struct Msgpack_Decode_Result
{
    Msgpack_Status status = Msgpack_Status::Malformed;
    QVariant value;
    qint64 bytes_consumed = 0;
    QString error;
};

/*
 * Appends the encoding of value to out. On failure out is left unchanged
 * and the status names the first offending condition. Only Ok,
 * Limit_Exceeded, and Unsupported_Type are possible.
 */
[[nodiscard]] Msgpack_Status msgpack_encode(
    const QVariant& value,
    QByteArray& out,
    const Msgpack_Limits& limits = Msgpack_Limits());

/*
 * Decodes one complete value starting at offset. Need_More_Data reports a
 * valid but truncated prefix; nothing is consumed unless the status is Ok.
 * Malformed and Limit_Exceeded carry a diagnostic in error.
 */
[[nodiscard]] Msgpack_Decode_Result msgpack_decode(
    const QByteArray& buffer,
    qint64 offset = 0,
    const Msgpack_Limits& limits = Msgpack_Limits());

const char* msgpack_status_string(Msgpack_Status status);

/*
 * Decodes the integer payload Neovim documents for its Buffer, Window, and
 * Tabpage ext types. The only sanctioned interpretation of ext payloads
 * (docs/buffer_hosted_viewport.md section 2); everything else treats them
 * as opaque. Returns -1 and sets ok to false when the payload is not one
 * complete msgpack integer.
 */
qint64 msgpack_ext_handle_value(const Msgpack_Ext& ext, bool* ok = nullptr);

} // namespace Kalam::Editor

Q_DECLARE_METATYPE(Kalam::Editor::Msgpack_Bin)
Q_DECLARE_METATYPE(Kalam::Editor::Msgpack_Ext)

#endif
