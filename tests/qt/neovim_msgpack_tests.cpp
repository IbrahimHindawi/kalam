#include "neovim_msgpack.h"

#include <QCoreApplication>
#include <QDebug>
#include <QPointF>
#include <QRandomGenerator>

using Kalam::Editor::Msgpack_Bin;
using Kalam::Editor::Msgpack_Decode_Result;
using Kalam::Editor::Msgpack_Ext;
using Kalam::Editor::Msgpack_Limits;
using Kalam::Editor::Msgpack_Status;
using Kalam::Editor::msgpack_decode;
using Kalam::Editor::msgpack_encode;
using Kalam::Editor::msgpack_status_string;

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

QByteArray encode_or_fail(const QVariant& value, const QString& label)
{
    QByteArray out;
    const Msgpack_Status status = msgpack_encode(value, out);
    check(
        status == Msgpack_Status::Ok,
        QStringLiteral("%1: encode status is %2")
            .arg(label, QString::fromUtf8(msgpack_status_string(status))));
    return out;
}

bool variants_equal(const QVariant& a, const QVariant& b);

bool lists_equal(const QVariantList& a, const QVariantList& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (int index = 0; index < a.size(); ++index)
    {
        if (!variants_equal(a.at(index), b.at(index)))
        {
            return false;
        }
    }
    return true;
}

bool maps_equal(const QVariantMap& a, const QVariantMap& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (auto iterator = a.constBegin(); iterator != a.constEnd(); ++iterator)
    {
        if (!b.contains(iterator.key()) ||
            !variants_equal(iterator.value(), b.value(iterator.key())))
        {
            return false;
        }
    }
    return true;
}

bool variants_equal(const QVariant& a, const QVariant& b)
{
    if (!a.isValid() || !b.isValid())
    {
        return a.isValid() == b.isValid();
    }
    if (a.userType() == qMetaTypeId<Msgpack_Bin>() ||
        b.userType() == qMetaTypeId<Msgpack_Bin>())
    {
        return a.userType() == b.userType() &&
            a.value<Msgpack_Bin>() == b.value<Msgpack_Bin>();
    }
    if (a.userType() == qMetaTypeId<Msgpack_Ext>() ||
        b.userType() == qMetaTypeId<Msgpack_Ext>())
    {
        return a.userType() == b.userType() &&
            a.value<Msgpack_Ext>() == b.value<Msgpack_Ext>();
    }
    if (a.userType() == QMetaType::QVariantList)
    {
        return b.userType() == QMetaType::QVariantList &&
            lists_equal(a.toList(), b.toList());
    }
    if (a.userType() == QMetaType::QVariantMap)
    {
        return b.userType() == QMetaType::QVariantMap &&
            maps_equal(a.toMap(), b.toMap());
    }
    return a.userType() == b.userType() && a == b;
}

void check_round_trip(const QVariant& value, const QString& label)
{
    const QByteArray wire = encode_or_fail(value, label);
    const Msgpack_Decode_Result result = msgpack_decode(wire);
    check(
        result.status == Msgpack_Status::Ok,
        QStringLiteral("%1: decode status is %2")
            .arg(label,
                 QString::fromUtf8(msgpack_status_string(result.status))));
    if (result.status != Msgpack_Status::Ok)
    {
        return;
    }
    check(
        result.bytes_consumed == wire.size(),
        QStringLiteral("%1: decode consumed %2 of %3 bytes")
            .arg(label)
            .arg(result.bytes_consumed)
            .arg(wire.size()));
    check(
        variants_equal(result.value, value),
        QStringLiteral("%1: value survived the round trip").arg(label));
}

void check_wire(
    const QVariant& value,
    const QByteArray& expected,
    const QString& label)
{
    const QByteArray wire = encode_or_fail(value, label);
    check(
        wire == expected,
        QStringLiteral("%1: wire bytes are %2, expected %3")
            .arg(label,
                 QString::fromUtf8(wire.toHex()),
                 QString::fromUtf8(expected.toHex())));
}

void test_scalar_round_trips()
{
    check_round_trip(QVariant(), QStringLiteral("nil"));
    check_round_trip(QVariant(false), QStringLiteral("false"));
    check_round_trip(QVariant(true), QStringLiteral("true"));

    const qlonglong signed_cases[] = {
        0, 1, 0x7f, 0x80, 0xff, 0x100, 0xffff, 0x10000,
        qlonglong(0xffffffff), qlonglong(0x100000000),
        qlonglong(0x7fffffffffffffff),
        -1, -32, -33, -128, -129, -32768, -32769,
        qlonglong(-2147483648LL), qlonglong(-2147483649LL),
        qlonglong(Q_INT64_C(-9223372036854775807) - 1)
    };
    for (qlonglong value : signed_cases)
    {
        check_round_trip(
            QVariant(value),
            QStringLiteral("int %1").arg(value));
    }

    check_round_trip(
        QVariant(qulonglong(0xffffffffffffffffull)),
        QStringLiteral("uint64 max"));
    check_round_trip(QVariant(0.0), QStringLiteral("double 0"));
    check_round_trip(QVariant(1.5), QStringLiteral("double 1.5"));
    check_round_trip(QVariant(-12345.6789), QStringLiteral("double neg"));
}

void test_wire_formats()
{
    check_wire(QVariant(), QByteArrayLiteral("\xc0"), QStringLiteral("nil wire"));
    check_wire(
        QVariant(qlonglong(5)),
        QByteArrayLiteral("\x05"),
        QStringLiteral("positive fixint wire"));
    check_wire(
        QVariant(qlonglong(-1)),
        QByteArrayLiteral("\xff"),
        QStringLiteral("negative fixint wire"));
    check_wire(
        QVariant(qlonglong(128)),
        QByteArrayLiteral("\xcc\x80"),
        QStringLiteral("uint8 wire"));
    check_wire(
        QVariant(qlonglong(-33)),
        QByteArrayLiteral("\xd0\xdf"),
        QStringLiteral("int8 wire"));
    check_wire(
        QVariant(QByteArrayLiteral("abc")),
        QByteArrayLiteral("\xa3" "abc"),
        QStringLiteral("fixstr wire"));

    QVariantList list;
    list << qlonglong(1) << qlonglong(2);
    check_wire(
        QVariant(list),
        QByteArrayLiteral("\x92\x01\x02"),
        QStringLiteral("fixarray wire"));

    /* An empty typed string must stay a string, never become nil. */
    check_wire(
        QVariant(QByteArray()),
        QByteArrayLiteral("\xa0"),
        QStringLiteral("null QByteArray wire"));
    check_wire(
        QVariant(QString()),
        QByteArrayLiteral("\xa0"),
        QStringLiteral("null QString wire"));
}

void test_string_and_bin_boundaries()
{
    const int lengths[] = { 0, 1, 31, 32, 255, 256, 65535, 65536 };
    for (int length : lengths)
    {
        const QByteArray bytes(length, 'x');
        check_round_trip(
            QVariant(bytes),
            QStringLiteral("str length %1").arg(length));

        Msgpack_Bin bin;
        bin.bytes = bytes;
        check_round_trip(
            QVariant::fromValue(bin),
            QStringLiteral("bin length %1").arg(length));
    }

    const QByteArray wire =
        encode_or_fail(QVariant(QByteArray(31, 'y')), QStringLiteral("fixstr"));
    check(
        quint8(wire.at(0)) == 0xbf,
        QStringLiteral("length 31 uses the fixstr format"));
    const QByteArray wire32 =
        encode_or_fail(QVariant(QByteArray(32, 'y')), QStringLiteral("str8"));
    check(
        quint8(wire32.at(0)) == 0xd9,
        QStringLiteral("length 32 promotes to str8"));
}

void test_ext_round_trips()
{
    const int lengths[] = { 1, 2, 4, 8, 16, 3, 17, 255, 256 };
    for (int length : lengths)
    {
        Msgpack_Ext ext;
        ext.type = 42;
        ext.payload = QByteArray(length, char(0xab));
        check_round_trip(
            QVariant::fromValue(ext),
            QStringLiteral("ext length %1").arg(length));
    }

    Msgpack_Ext buffer_handle;
    buffer_handle.type = 0;
    buffer_handle.payload = QByteArrayLiteral("\x01");
    const QByteArray wire = encode_or_fail(
        QVariant::fromValue(buffer_handle),
        QStringLiteral("fixext1"));
    check(
        quint8(wire.at(0)) == 0xd4,
        QStringLiteral("one-byte ext uses the fixext1 format"));

    Msgpack_Ext negative_type;
    negative_type.type = -1;
    negative_type.payload = QByteArrayLiteral("\x00\x01\x02");
    check_round_trip(
        QVariant::fromValue(negative_type),
        QStringLiteral("ext negative type"));
}

void test_container_round_trips()
{
    QVariantList list;
    list << QVariant() << true << qlonglong(-7)
         << QByteArrayLiteral("text") << 3.25;
    check_round_trip(QVariant(list), QStringLiteral("mixed array"));

    QVariantMap map;
    map.insert(QStringLiteral("name"), QByteArrayLiteral("kalam"));
    map.insert(QStringLiteral("count"), qlonglong(3));
    map.insert(QStringLiteral("nested"), QVariant(list));
    check_round_trip(QVariant(map), QStringLiteral("nested map"));

    QVariantList big;
    for (int index = 0; index < 20; ++index)
    {
        big << qlonglong(index);
    }
    check_round_trip(QVariant(big), QStringLiteral("array16"));

    QVariantMap big_map;
    for (int index = 0; index < 20; ++index)
    {
        big_map.insert(QString::number(index), qlonglong(index));
    }
    check_round_trip(QVariant(big_map), QStringLiteral("map16"));

    check_round_trip(QVariant(QVariantList()), QStringLiteral("empty array"));
    check_round_trip(QVariant(QVariantMap()), QStringLiteral("empty map"));
}

void test_truncated_input_resumption()
{
    QVariantMap map;
    map.insert(QStringLiteral("key"), QByteArrayLiteral("value"));
    QVariantList value;
    value << qlonglong(2)
          << QByteArrayLiteral("redraw")
          << QVariant(QVariantList() << QVariant(map));
    const QByteArray wire =
        encode_or_fail(QVariant(value), QStringLiteral("truncation corpus"));

    for (int length = 0; length < wire.size(); ++length)
    {
        const Msgpack_Decode_Result result =
            msgpack_decode(wire.left(length));
        check(
            result.status == Msgpack_Status::Need_More_Data,
            QStringLiteral("prefix of %1 bytes reports Need_More_Data")
                .arg(length));
        check(
            result.bytes_consumed == 0,
            QStringLiteral("prefix of %1 bytes consumes nothing").arg(length));
    }

    const Msgpack_Decode_Result full = msgpack_decode(wire);
    check(
        full.status == Msgpack_Status::Ok &&
            full.bytes_consumed == wire.size(),
        QStringLiteral("the complete buffer decodes cleanly"));

    /* Two concatenated values decode one at a time via the offset. */
    QByteArray doubled = wire;
    doubled.append(wire);
    const Msgpack_Decode_Result first = msgpack_decode(doubled);
    check(
        first.status == Msgpack_Status::Ok &&
            first.bytes_consumed == wire.size(),
        QStringLiteral("the first of two concatenated values decodes"));
    const Msgpack_Decode_Result second =
        msgpack_decode(doubled, first.bytes_consumed);
    check(
        second.status == Msgpack_Status::Ok &&
            second.bytes_consumed == wire.size() &&
            variants_equal(second.value, first.value),
        QStringLiteral("the second concatenated value decodes at the offset"));
}

void test_malformed_and_limits()
{
    const Msgpack_Decode_Result never_used =
        msgpack_decode(QByteArrayLiteral("\xc1"));
    check(
        never_used.status == Msgpack_Status::Malformed,
        QStringLiteral("0xc1 is malformed"));

    /* A map whose key is an array is rejected. */
    const Msgpack_Decode_Result bad_key =
        msgpack_decode(QByteArrayLiteral("\x81\x90\x01"));
    check(
        bad_key.status == Msgpack_Status::Malformed,
        QStringLiteral("an array map key is malformed"));

    /* Depth limit. */
    Msgpack_Limits shallow;
    shallow.maximum_depth = 4;
    QByteArray deep;
    for (int index = 0; index < 8; ++index)
    {
        deep.append(char(0x91));
    }
    deep.append(char(0x01));
    const Msgpack_Decode_Result too_deep = msgpack_decode(deep, 0, shallow);
    check(
        too_deep.status == Msgpack_Status::Limit_Exceeded,
        QStringLiteral("nesting beyond the depth limit is rejected"));

    QVariant nested = QVariant(qlonglong(1));
    for (int index = 0; index < 8; ++index)
    {
        nested = QVariant(QVariantList() << nested);
    }
    QByteArray out;
    check(
        msgpack_encode(nested, out, shallow) ==
            Msgpack_Status::Limit_Exceeded,
        QStringLiteral("encoding beyond the depth limit is rejected"));
    check(
        out.isEmpty(),
        QStringLiteral("a failed encode leaves the output untouched"));

    /*
     * A hostile header claiming a huge container fails on the size limit
     * before asking for more data.
     */
    QByteArray hostile;
    hostile.append(char(0xdd));
    hostile.append(char(0xff));
    hostile.append(char(0xff));
    hostile.append(char(0xff));
    hostile.append(char(0xff));
    const Msgpack_Decode_Result huge = msgpack_decode(hostile);
    check(
        huge.status == Msgpack_Status::Limit_Exceeded,
        QStringLiteral("a hostile array32 header is rejected by the limit"));

    QByteArray hostile_str;
    hostile_str.append(char(0xdb));
    hostile_str.append(char(0xff));
    hostile_str.append(char(0xff));
    hostile_str.append(char(0xff));
    hostile_str.append(char(0xff));
    const Msgpack_Decode_Result huge_str = msgpack_decode(hostile_str);
    check(
        huge_str.status == Msgpack_Status::Limit_Exceeded,
        QStringLiteral("a hostile str32 header is rejected by the limit"));

    /* Unsupported QVariant contents refuse to encode. */
    QByteArray unsupported_out;
    check(
        msgpack_encode(
            QVariant::fromValue(QPointF(1.0, 2.0)),
            unsupported_out) == Msgpack_Status::Unsupported_Type,
        QStringLiteral("a QPointF payload is unsupported"));
}

void test_generated_corpus()
{
    QRandomGenerator generator(20260731u);
    for (int iteration = 0; iteration < 500; ++iteration)
    {
        QVariantList value;
        const int element_count = int(generator.bounded(0, 8));
        for (int index = 0; index < element_count; ++index)
        {
            switch (generator.bounded(0, 7))
            {
            case 0:
                value << QVariant();
                break;
            case 1:
                value << QVariant(generator.bounded(0, 2) == 1);
                break;
            case 2:
                value << QVariant(qlonglong(generator.generate64()));
                break;
            case 3:
                value << QVariant(generator.generateDouble() * 1000.0);
                break;
            case 4:
                value << QVariant(
                    QByteArray(int(generator.bounded(0, 40)), 'q'));
                break;
            case 5:
            {
                Msgpack_Ext ext;
                ext.type = qint8(generator.bounded(-128, 128));
                ext.payload =
                    QByteArray(int(generator.bounded(1, 20)), char(7));
                value << QVariant::fromValue(ext);
                break;
            }
            default:
            {
                QVariantMap map;
                map.insert(
                    QStringLiteral("k%1").arg(generator.bounded(0, 100)),
                    qlonglong(generator.bounded(0, 1000)));
                value << QVariant(map);
                break;
            }
            }
        }
        check_round_trip(
            QVariant(value),
            QStringLiteral("generated corpus iteration %1").arg(iteration));
        if (failures > 0)
        {
            return;
        }
    }
}

} // namespace

int main(int argument_count, char** arguments)
{
    QCoreApplication application(argument_count, arguments);

    test_scalar_round_trips();
    test_wire_formats();
    test_string_and_bin_boundaries();
    test_ext_round_trips();
    test_container_round_trips();
    test_truncated_input_resumption();
    test_malformed_and_limits();
    test_generated_corpus();

    if (failures == 0)
    {
        qInfo().noquote() << QStringLiteral("all msgpack codec tests passed");
    }
    return failures == 0 ? 0 : 1;
}
