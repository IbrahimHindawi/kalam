#include "neovim_msgpack.h"

#include <QtEndian>

#include <string.h>

namespace Kalam::Editor
{

namespace
{

/* ---------------------------------------------------------------- encode */

struct Encoder
{
    QByteArray* out;
    const Msgpack_Limits* limits;
    Msgpack_Status status = Msgpack_Status::Ok;
};

void put_byte(Encoder& encoder, quint8 value)
{
    encoder.out->append(char(value));
}

template <typename Integer>
void put_big_endian(Encoder& encoder, Integer value)
{
    quint8 raw[sizeof(Integer)];
    qToBigEndian(value, raw);
    encoder.out->append(reinterpret_cast<const char*>(raw), sizeof(Integer));
}

void encode_signed(Encoder& encoder, qint64 value)
{
    if (value >= 0)
    {
        if (value <= 0x7f)
        {
            put_byte(encoder, quint8(value));
        }
        else if (value <= 0xff)
        {
            put_byte(encoder, 0xcc);
            put_byte(encoder, quint8(value));
        }
        else if (value <= 0xffff)
        {
            put_byte(encoder, 0xcd);
            put_big_endian(encoder, quint16(value));
        }
        else if (value <= qint64(0xffffffff))
        {
            put_byte(encoder, 0xce);
            put_big_endian(encoder, quint32(value));
        }
        else
        {
            put_byte(encoder, 0xcf);
            put_big_endian(encoder, quint64(value));
        }
        return;
    }

    if (value >= -32)
    {
        put_byte(encoder, quint8(value));
    }
    else if (value >= -128)
    {
        put_byte(encoder, 0xd0);
        put_byte(encoder, quint8(qint8(value)));
    }
    else if (value >= -32768)
    {
        put_byte(encoder, 0xd1);
        put_big_endian(encoder, quint16(qint16(value)));
    }
    else if (value >= -qint64(0x80000000))
    {
        put_byte(encoder, 0xd2);
        put_big_endian(encoder, quint32(qint32(value)));
    }
    else
    {
        put_byte(encoder, 0xd3);
        put_big_endian(encoder, quint64(value));
    }
}

void encode_unsigned(Encoder& encoder, quint64 value)
{
    if (value <= quint64(0x7fffffffffffffffull))
    {
        encode_signed(encoder, qint64(value));
        return;
    }
    put_byte(encoder, 0xcf);
    put_big_endian(encoder, value);
}

void encode_double(Encoder& encoder, double value)
{
    quint64 bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    put_byte(encoder, 0xcb);
    put_big_endian(encoder, bits);
}

bool encode_length_checked(Encoder& encoder, qint64 length)
{
    if (length > encoder.limits->maximum_byte_length)
    {
        encoder.status = Msgpack_Status::Limit_Exceeded;
        return false;
    }
    return true;
}

void encode_str(Encoder& encoder, const QByteArray& bytes)
{
    const qint64 length = bytes.size();
    if (!encode_length_checked(encoder, length))
    {
        return;
    }
    if (length <= 31)
    {
        put_byte(encoder, quint8(0xa0 | quint8(length)));
    }
    else if (length <= 0xff)
    {
        put_byte(encoder, 0xd9);
        put_byte(encoder, quint8(length));
    }
    else if (length <= 0xffff)
    {
        put_byte(encoder, 0xda);
        put_big_endian(encoder, quint16(length));
    }
    else
    {
        put_byte(encoder, 0xdb);
        put_big_endian(encoder, quint32(length));
    }
    encoder.out->append(bytes);
}

void encode_bin(Encoder& encoder, const QByteArray& bytes)
{
    const qint64 length = bytes.size();
    if (!encode_length_checked(encoder, length))
    {
        return;
    }
    if (length <= 0xff)
    {
        put_byte(encoder, 0xc4);
        put_byte(encoder, quint8(length));
    }
    else if (length <= 0xffff)
    {
        put_byte(encoder, 0xc5);
        put_big_endian(encoder, quint16(length));
    }
    else
    {
        put_byte(encoder, 0xc6);
        put_big_endian(encoder, quint32(length));
    }
    encoder.out->append(bytes);
}

void encode_ext(Encoder& encoder, const Msgpack_Ext& ext)
{
    const qint64 length = ext.payload.size();
    if (!encode_length_checked(encoder, length))
    {
        return;
    }
    switch (length)
    {
    case 1: put_byte(encoder, 0xd4); break;
    case 2: put_byte(encoder, 0xd5); break;
    case 4: put_byte(encoder, 0xd6); break;
    case 8: put_byte(encoder, 0xd7); break;
    case 16: put_byte(encoder, 0xd8); break;
    default:
        if (length <= 0xff)
        {
            put_byte(encoder, 0xc7);
            put_byte(encoder, quint8(length));
        }
        else if (length <= 0xffff)
        {
            put_byte(encoder, 0xc8);
            put_big_endian(encoder, quint16(length));
        }
        else
        {
            put_byte(encoder, 0xc9);
            put_big_endian(encoder, quint32(length));
        }
        break;
    }
    put_byte(encoder, quint8(ext.type));
    encoder.out->append(ext.payload);
}

void encode_value(Encoder& encoder, const QVariant& value, int depth);

void encode_array(Encoder& encoder, const QVariantList& list, int depth)
{
    const qint64 count = list.size();
    if (count > encoder.limits->maximum_container_elements)
    {
        encoder.status = Msgpack_Status::Limit_Exceeded;
        return;
    }
    if (count <= 15)
    {
        put_byte(encoder, quint8(0x90 | quint8(count)));
    }
    else if (count <= 0xffff)
    {
        put_byte(encoder, 0xdc);
        put_big_endian(encoder, quint16(count));
    }
    else
    {
        put_byte(encoder, 0xdd);
        put_big_endian(encoder, quint32(count));
    }
    for (const QVariant& element : list)
    {
        encode_value(encoder, element, depth + 1);
        if (encoder.status != Msgpack_Status::Ok)
        {
            return;
        }
    }
}

void encode_map(Encoder& encoder, const QVariantMap& map, int depth)
{
    const qint64 count = map.size();
    if (count > encoder.limits->maximum_container_elements)
    {
        encoder.status = Msgpack_Status::Limit_Exceeded;
        return;
    }
    if (count <= 15)
    {
        put_byte(encoder, quint8(0x80 | quint8(count)));
    }
    else if (count <= 0xffff)
    {
        put_byte(encoder, 0xde);
        put_big_endian(encoder, quint16(count));
    }
    else
    {
        put_byte(encoder, 0xdf);
        put_big_endian(encoder, quint32(count));
    }
    for (auto iterator = map.constBegin();
         iterator != map.constEnd();
         ++iterator)
    {
        encode_str(encoder, iterator.key().toUtf8());
        if (encoder.status != Msgpack_Status::Ok)
        {
            return;
        }
        encode_value(encoder, iterator.value(), depth + 1);
        if (encoder.status != Msgpack_Status::Ok)
        {
            return;
        }
    }
}

void encode_value(Encoder& encoder, const QVariant& value, int depth)
{
    if (depth > encoder.limits->maximum_depth)
    {
        encoder.status = Msgpack_Status::Limit_Exceeded;
        return;
    }

    /*
     * Only an invalid QVariant (and Nullptr below) is nil. isNull() would
     * also match null-but-typed values such as QString() and turn empty
     * strings into nil on the wire.
     */
    if (!value.isValid())
    {
        put_byte(encoder, 0xc0);
        return;
    }

    const int type_id = value.userType();
    if (type_id == qMetaTypeId<Msgpack_Bin>())
    {
        encode_bin(encoder, value.value<Msgpack_Bin>().bytes);
        return;
    }
    if (type_id == qMetaTypeId<Msgpack_Ext>())
    {
        encode_ext(encoder, value.value<Msgpack_Ext>());
        return;
    }

    switch (type_id)
    {
    case QMetaType::Nullptr:
        put_byte(encoder, 0xc0);
        return;
    case QMetaType::Bool:
        put_byte(encoder, value.toBool() ? 0xc3 : 0xc2);
        return;
    case QMetaType::Char:
    case QMetaType::SChar:
    case QMetaType::Short:
    case QMetaType::Int:
    case QMetaType::Long:
    case QMetaType::LongLong:
        encode_signed(encoder, value.toLongLong());
        return;
    case QMetaType::UChar:
    case QMetaType::UShort:
    case QMetaType::UInt:
    case QMetaType::ULong:
    case QMetaType::ULongLong:
        encode_unsigned(encoder, value.toULongLong());
        return;
    case QMetaType::Float:
    case QMetaType::Double:
        encode_double(encoder, value.toDouble());
        return;
    case QMetaType::QByteArray:
        encode_str(encoder, value.toByteArray());
        return;
    case QMetaType::QString:
        encode_str(encoder, value.toString().toUtf8());
        return;
    case QMetaType::QStringList:
    case QMetaType::QVariantList:
        encode_array(encoder, value.toList(), depth);
        return;
    case QMetaType::QVariantHash:
    case QMetaType::QVariantMap:
        encode_map(encoder, value.toMap(), depth);
        return;
    default:
        encoder.status = Msgpack_Status::Unsupported_Type;
        return;
    }
}

/* ---------------------------------------------------------------- decode */

struct Decoder
{
    const quint8* data = nullptr;
    qint64 size = 0;
    qint64 position = 0;
    const Msgpack_Limits* limits = nullptr;
    Msgpack_Status status = Msgpack_Status::Ok;
    QString error;
};

void fail(Decoder& decoder, Msgpack_Status status, const QString& error)
{
    if (decoder.status == Msgpack_Status::Ok)
    {
        decoder.status = status;
        decoder.error = error;
    }
}

bool need(Decoder& decoder, qint64 count)
{
    if (decoder.status != Msgpack_Status::Ok)
    {
        return false;
    }
    if (decoder.position + count > decoder.size)
    {
        fail(decoder, Msgpack_Status::Need_More_Data, QString());
        return false;
    }
    return true;
}

quint8 take_u8(Decoder& decoder)
{
    return decoder.data[decoder.position++];
}

quint16 take_u16(Decoder& decoder)
{
    const quint16 value =
        qFromBigEndian<quint16>(decoder.data + decoder.position);
    decoder.position += 2;
    return value;
}

quint32 take_u32(Decoder& decoder)
{
    const quint32 value =
        qFromBigEndian<quint32>(decoder.data + decoder.position);
    decoder.position += 4;
    return value;
}

quint64 take_u64(Decoder& decoder)
{
    const quint64 value =
        qFromBigEndian<quint64>(decoder.data + decoder.position);
    decoder.position += 8;
    return value;
}

QByteArray take_bytes(Decoder& decoder, qint64 length)
{
    const QByteArray bytes(
        reinterpret_cast<const char*>(decoder.data + decoder.position),
        length);
    decoder.position += length;
    return bytes;
}

QVariant from_unsigned(quint64 value)
{
    if (value <= quint64(0x7fffffffffffffffull))
    {
        return QVariant(qlonglong(value));
    }
    return QVariant(qulonglong(value));
}

bool length_allowed(Decoder& decoder, qint64 length)
{
    if (length > decoder.limits->maximum_byte_length)
    {
        fail(
            decoder,
            Msgpack_Status::Limit_Exceeded,
            QStringLiteral("byte length %1 exceeds the limit").arg(length));
        return false;
    }
    return true;
}

bool element_count_allowed(Decoder& decoder, qint64 count)
{
    if (count > decoder.limits->maximum_container_elements)
    {
        fail(
            decoder,
            Msgpack_Status::Limit_Exceeded,
            QStringLiteral("container size %1 exceeds the limit").arg(count));
        return false;
    }
    return true;
}

QVariant decode_value(Decoder& decoder, int depth);

QVariant decode_str_body(Decoder& decoder, qint64 length)
{
    if (!length_allowed(decoder, length) || !need(decoder, length))
    {
        return QVariant();
    }
    return QVariant(take_bytes(decoder, length));
}

QVariant decode_bin_body(Decoder& decoder, qint64 length)
{
    if (!length_allowed(decoder, length) || !need(decoder, length))
    {
        return QVariant();
    }
    Msgpack_Bin bin;
    bin.bytes = take_bytes(decoder, length);
    return QVariant::fromValue(bin);
}

QVariant decode_ext_body(Decoder& decoder, qint64 length)
{
    if (!length_allowed(decoder, length) || !need(decoder, 1 + length))
    {
        return QVariant();
    }
    Msgpack_Ext ext;
    ext.type = qint8(take_u8(decoder));
    ext.payload = take_bytes(decoder, length);
    return QVariant::fromValue(ext);
}

QVariant decode_array_body(Decoder& decoder, qint64 count, int depth)
{
    if (!element_count_allowed(decoder, count))
    {
        return QVariant();
    }
    QVariantList list;
    list.reserve(int(qMin(count, qint64(4096))));
    for (qint64 index = 0; index < count; ++index)
    {
        QVariant element = decode_value(decoder, depth + 1);
        if (decoder.status != Msgpack_Status::Ok)
        {
            return QVariant();
        }
        list.append(element);
    }
    return QVariant(list);
}

QVariant decode_map_body(Decoder& decoder, qint64 count, int depth)
{
    if (!element_count_allowed(decoder, count))
    {
        return QVariant();
    }
    QVariantMap map;
    for (qint64 index = 0; index < count; ++index)
    {
        const QVariant key = decode_value(decoder, depth + 1);
        if (decoder.status != Msgpack_Status::Ok)
        {
            return QVariant();
        }
        QString key_text;
        if (key.userType() == QMetaType::QByteArray)
        {
            key_text = QString::fromUtf8(key.toByteArray());
        }
        else if (key.userType() == QMetaType::LongLong)
        {
            key_text = QString::number(key.toLongLong());
        }
        else
        {
            fail(
                decoder,
                Msgpack_Status::Malformed,
                QStringLiteral("map key is neither str nor integer"));
            return QVariant();
        }
        const QVariant value = decode_value(decoder, depth + 1);
        if (decoder.status != Msgpack_Status::Ok)
        {
            return QVariant();
        }
        map.insert(key_text, value);
    }
    return QVariant(map);
}

QVariant decode_value(Decoder& decoder, int depth)
{
    if (depth > decoder.limits->maximum_depth)
    {
        fail(
            decoder,
            Msgpack_Status::Limit_Exceeded,
            QStringLiteral("nesting depth exceeds the limit"));
        return QVariant();
    }
    if (!need(decoder, 1))
    {
        return QVariant();
    }

    const quint8 format = take_u8(decoder);

    if (format <= 0x7f)
    {
        return QVariant(qlonglong(format));
    }
    if (format >= 0xe0)
    {
        return QVariant(qlonglong(qint8(format)));
    }
    if (format >= 0x80 && format <= 0x8f)
    {
        return decode_map_body(decoder, format & 0x0f, depth);
    }
    if (format >= 0x90 && format <= 0x9f)
    {
        return decode_array_body(decoder, format & 0x0f, depth);
    }
    if (format >= 0xa0 && format <= 0xbf)
    {
        return decode_str_body(decoder, format & 0x1f);
    }

    switch (format)
    {
    case 0xc0:
        return QVariant();
    case 0xc1:
        fail(
            decoder,
            Msgpack_Status::Malformed,
            QStringLiteral("format byte 0xc1 is never valid"));
        return QVariant();
    case 0xc2:
        return QVariant(false);
    case 0xc3:
        return QVariant(true);
    case 0xc4:
        if (!need(decoder, 1)) return QVariant();
        return decode_bin_body(decoder, take_u8(decoder));
    case 0xc5:
        if (!need(decoder, 2)) return QVariant();
        return decode_bin_body(decoder, take_u16(decoder));
    case 0xc6:
        if (!need(decoder, 4)) return QVariant();
        return decode_bin_body(decoder, qint64(take_u32(decoder)));
    case 0xc7:
        if (!need(decoder, 1)) return QVariant();
        return decode_ext_body(decoder, take_u8(decoder));
    case 0xc8:
        if (!need(decoder, 2)) return QVariant();
        return decode_ext_body(decoder, take_u16(decoder));
    case 0xc9:
        if (!need(decoder, 4)) return QVariant();
        return decode_ext_body(decoder, qint64(take_u32(decoder)));
    case 0xca:
    {
        if (!need(decoder, 4)) return QVariant();
        const quint32 bits = take_u32(decoder);
        float value = 0.0f;
        memcpy(&value, &bits, sizeof(value));
        return QVariant(double(value));
    }
    case 0xcb:
    {
        if (!need(decoder, 8)) return QVariant();
        const quint64 bits = take_u64(decoder);
        double value = 0.0;
        memcpy(&value, &bits, sizeof(value));
        return QVariant(value);
    }
    case 0xcc:
        if (!need(decoder, 1)) return QVariant();
        return QVariant(qlonglong(take_u8(decoder)));
    case 0xcd:
        if (!need(decoder, 2)) return QVariant();
        return QVariant(qlonglong(take_u16(decoder)));
    case 0xce:
        if (!need(decoder, 4)) return QVariant();
        return QVariant(qlonglong(take_u32(decoder)));
    case 0xcf:
        if (!need(decoder, 8)) return QVariant();
        return from_unsigned(take_u64(decoder));
    case 0xd0:
        if (!need(decoder, 1)) return QVariant();
        return QVariant(qlonglong(qint8(take_u8(decoder))));
    case 0xd1:
        if (!need(decoder, 2)) return QVariant();
        return QVariant(qlonglong(qint16(take_u16(decoder))));
    case 0xd2:
        if (!need(decoder, 4)) return QVariant();
        return QVariant(qlonglong(qint32(take_u32(decoder))));
    case 0xd3:
        if (!need(decoder, 8)) return QVariant();
        return QVariant(qlonglong(qint64(take_u64(decoder))));
    case 0xd4:
        return decode_ext_body(decoder, 1);
    case 0xd5:
        return decode_ext_body(decoder, 2);
    case 0xd6:
        return decode_ext_body(decoder, 4);
    case 0xd7:
        return decode_ext_body(decoder, 8);
    case 0xd8:
        return decode_ext_body(decoder, 16);
    case 0xd9:
        if (!need(decoder, 1)) return QVariant();
        return decode_str_body(decoder, take_u8(decoder));
    case 0xda:
        if (!need(decoder, 2)) return QVariant();
        return decode_str_body(decoder, take_u16(decoder));
    case 0xdb:
        if (!need(decoder, 4)) return QVariant();
        return decode_str_body(decoder, qint64(take_u32(decoder)));
    case 0xdc:
        if (!need(decoder, 2)) return QVariant();
        return decode_array_body(decoder, take_u16(decoder), depth);
    case 0xdd:
        if (!need(decoder, 4)) return QVariant();
        return decode_array_body(decoder, qint64(take_u32(decoder)), depth);
    case 0xde:
        if (!need(decoder, 2)) return QVariant();
        return decode_map_body(decoder, take_u16(decoder), depth);
    case 0xdf:
        if (!need(decoder, 4)) return QVariant();
        return decode_map_body(decoder, qint64(take_u32(decoder)), depth);
    default:
        fail(
            decoder,
            Msgpack_Status::Malformed,
            QStringLiteral("unhandled format byte 0x%1")
                .arg(format, 2, 16, QLatin1Char('0')));
        return QVariant();
    }
}

} // namespace

Msgpack_Status msgpack_encode(
    const QVariant& value,
    QByteArray& out,
    const Msgpack_Limits& limits)
{
    QByteArray scratch;
    Encoder encoder;
    encoder.out = &scratch;
    encoder.limits = &limits;
    encode_value(encoder, value, 0);
    if (encoder.status == Msgpack_Status::Ok)
    {
        out.append(scratch);
    }
    return encoder.status;
}

Msgpack_Decode_Result msgpack_decode(
    const QByteArray& buffer,
    qint64 offset,
    const Msgpack_Limits& limits)
{
    Msgpack_Decode_Result result;
    if (offset < 0 || offset > buffer.size())
    {
        result.status = Msgpack_Status::Malformed;
        result.error = QStringLiteral("offset is outside the buffer");
        return result;
    }

    Decoder decoder;
    decoder.data = reinterpret_cast<const quint8*>(buffer.constData()) + offset;
    decoder.size = buffer.size() - offset;
    decoder.limits = &limits;

    const QVariant value = decode_value(decoder, 0);
    result.status = decoder.status;
    result.error = decoder.error;
    if (decoder.status == Msgpack_Status::Ok)
    {
        result.value = value;
        result.bytes_consumed = decoder.position;
    }
    return result;
}

qint64 msgpack_ext_handle_value(const Msgpack_Ext& ext, bool* ok)
{
    const Msgpack_Decode_Result decoded = msgpack_decode(ext.payload);
    const bool is_integer = decoded.status == Msgpack_Status::Ok &&
        decoded.bytes_consumed == ext.payload.size() &&
        (decoded.value.userType() == QMetaType::LongLong ||
         decoded.value.userType() == QMetaType::ULongLong);
    if (ok != nullptr)
    {
        *ok = is_integer;
    }
    return is_integer ? decoded.value.toLongLong() : -1;
}

const char* msgpack_status_string(Msgpack_Status status)
{
    switch (status)
    {
    case Msgpack_Status::Ok: return "ok";
    case Msgpack_Status::Need_More_Data: return "need more data";
    case Msgpack_Status::Malformed: return "malformed";
    case Msgpack_Status::Limit_Exceeded: return "limit exceeded";
    case Msgpack_Status::Unsupported_Type: return "unsupported type";
    }
    return "unknown";
}

} // namespace Kalam::Editor
