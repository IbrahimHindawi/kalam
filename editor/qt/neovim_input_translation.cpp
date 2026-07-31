#include "neovim_input_translation.h"

#include <QHash>

namespace Kalam::Editor
{

namespace
{

const QHash<int, QString>& special_keys()
{
    static const QHash<int, QString> keys = {
        { Qt::Key_Up, QStringLiteral("Up") },
        { Qt::Key_Down, QStringLiteral("Down") },
        { Qt::Key_Left, QStringLiteral("Left") },
        { Qt::Key_Right, QStringLiteral("Right") },
        { Qt::Key_F1, QStringLiteral("F1") },
        { Qt::Key_F2, QStringLiteral("F2") },
        { Qt::Key_F3, QStringLiteral("F3") },
        { Qt::Key_F4, QStringLiteral("F4") },
        { Qt::Key_F5, QStringLiteral("F5") },
        { Qt::Key_F6, QStringLiteral("F6") },
        { Qt::Key_F7, QStringLiteral("F7") },
        { Qt::Key_F8, QStringLiteral("F8") },
        { Qt::Key_F9, QStringLiteral("F9") },
        { Qt::Key_F10, QStringLiteral("F10") },
        { Qt::Key_F11, QStringLiteral("F11") },
        { Qt::Key_F12, QStringLiteral("F12") },
        { Qt::Key_F13, QStringLiteral("F13") },
        { Qt::Key_F14, QStringLiteral("F14") },
        { Qt::Key_F15, QStringLiteral("F15") },
        { Qt::Key_F16, QStringLiteral("F16") },
        { Qt::Key_F17, QStringLiteral("F17") },
        { Qt::Key_F18, QStringLiteral("F18") },
        { Qt::Key_F19, QStringLiteral("F19") },
        { Qt::Key_F20, QStringLiteral("F20") },
        { Qt::Key_F21, QStringLiteral("F21") },
        { Qt::Key_F22, QStringLiteral("F22") },
        { Qt::Key_F23, QStringLiteral("F23") },
        { Qt::Key_F24, QStringLiteral("F24") },
        { Qt::Key_Backspace, QStringLiteral("BS") },
        { Qt::Key_Delete, QStringLiteral("Del") },
        { Qt::Key_Insert, QStringLiteral("Insert") },
        { Qt::Key_Home, QStringLiteral("Home") },
        { Qt::Key_End, QStringLiteral("End") },
        { Qt::Key_PageUp, QStringLiteral("PageUp") },
        { Qt::Key_PageDown, QStringLiteral("PageDown") },
        { Qt::Key_Return, QStringLiteral("Enter") },
        { Qt::Key_Enter, QStringLiteral("Enter") },
        { Qt::Key_Tab, QStringLiteral("Tab") },
        { Qt::Key_Backtab, QStringLiteral("Tab") },
        { Qt::Key_Escape, QStringLiteral("Esc") },
        { Qt::Key_Backslash, QStringLiteral("Bslash") },
        { Qt::Key_Space, QStringLiteral("Space") },
    };
    return keys;
}

const QHash<int, QString>& keypad_keys()
{
    static const QHash<int, QString> keys = {
        { Qt::Key_Home, QStringLiteral("kHome") },
        { Qt::Key_End, QStringLiteral("kEnd") },
        { Qt::Key_PageUp, QStringLiteral("kPageUp") },
        { Qt::Key_PageDown, QStringLiteral("kPageDown") },
        { Qt::Key_Plus, QStringLiteral("kPlus") },
        { Qt::Key_Minus, QStringLiteral("kMinus") },
        { Qt::Key_multiply, QStringLiteral("kMultiply") },
        { Qt::Key_division, QStringLiteral("kDivide") },
        { Qt::Key_Enter, QStringLiteral("kEnter") },
        { Qt::Key_Period, QStringLiteral("kPoint") },
        { Qt::Key_0, QStringLiteral("k0") },
        { Qt::Key_1, QStringLiteral("k1") },
        { Qt::Key_2, QStringLiteral("k2") },
        { Qt::Key_3, QStringLiteral("k3") },
        { Qt::Key_4, QStringLiteral("k4") },
        { Qt::Key_5, QStringLiteral("k5") },
        { Qt::Key_6, QStringLiteral("k6") },
        { Qt::Key_7, QStringLiteral("k7") },
        { Qt::Key_8, QStringLiteral("k8") },
        { Qt::Key_9, QStringLiteral("k9") },
    };
    return keys;
}

QString bracketed(const QString& prefix, const QString& name)
{
    return QStringLiteral("<%1%2>").arg(prefix, name);
}

QString text_from_key(int qt_key, Qt::KeyboardModifiers modifiers)
{
    QString text = QString(QChar(qt_key));
    if (!(modifiers & Qt::ShiftModifier))
    {
        text = text.toLower();
    }
    return text;
}

} // namespace

QString modifier_prefix(Qt::KeyboardModifiers modifiers)
{
    QString prefix;
    /*
     * Win32 rule: Ctrl+Alt never combine because AltGr arrives as both;
     * see Vim's os_win32.c and Neovim-Qt input_win32.cpp.
     */
    if ((modifiers & Qt::ControlModifier) && !(modifiers & Qt::AltModifier))
    {
        prefix += QStringLiteral("C-");
    }
    if (modifiers & Qt::ShiftModifier)
    {
        prefix += QStringLiteral("S-");
    }
    if ((modifiers & Qt::AltModifier) && !(modifiers & Qt::ControlModifier))
    {
        prefix += QStringLiteral("A-");
    }
    return prefix;
}

QString input_notation(
    int qt_key,
    Qt::KeyboardModifiers modifiers,
    const QString& event_text)
{
    QString text = event_text;

    if ((modifiers & Qt::KeypadModifier) && keypad_keys().contains(qt_key))
    {
        return bracketed(
            modifier_prefix(modifiers),
            keypad_keys().value(qt_key));
    }

    /* Ctrl+Space can arrive as a non-printable control character. */
    if (qt_key == Qt::Key_Space &&
        !text.isEmpty() &&
        !text.at(0).isPrint())
    {
        text = QStringLiteral(" ");
    }

    /*
     * Some international layouts commit an accent character on the space
     * key; send the composed text itself.
     */
    if (qt_key == Qt::Key_Space &&
        !text.isEmpty() &&
        text != QStringLiteral(" "))
    {
        if (modifiers != Qt::NoModifier)
        {
            return bracketed(modifier_prefix(modifiers), text);
        }
        return text;
    }

    if (special_keys().contains(qt_key))
    {
        return bracketed(
            modifier_prefix(modifiers),
            special_keys().value(qt_key));
    }

    /* "<" is sent as <lt>; the Shift that produced it is implied. */
    if (text == QStringLiteral("<"))
    {
        return bracketed(
            modifier_prefix(modifiers & ~Qt::ShiftModifier),
            QStringLiteral("lt"));
    }

    /* Ctrl combined with 6 or ^ always sends <C-^>. */
    if ((qt_key == Qt::Key_6 || qt_key == Qt::Key_AsciiCircum) &&
        (modifiers & Qt::ControlModifier))
    {
        return bracketed(
            modifier_prefix(modifiers & ~Qt::ShiftModifier),
            QStringLiteral("^"));
    }

    if (text == QStringLiteral("\\"))
    {
        return bracketed(
            modifier_prefix(modifiers),
            QStringLiteral("Bslash"));
    }

    if (text.isEmpty())
    {
        /* Modifier-only presses send nothing. */
        if (qt_key == Qt::Key_Alt ||
            qt_key == Qt::Key_AltGr ||
            qt_key == Qt::Key_CapsLock ||
            qt_key == Qt::Key_Control ||
            qt_key == Qt::Key_Meta ||
            qt_key == Qt::Key_Shift ||
            qt_key == Qt::Key_Super_L ||
            qt_key == Qt::Key_Super_R ||
            qt_key == Qt::Key_Hyper_L ||
            qt_key == Qt::Key_Hyper_R)
        {
            return QString();
        }
        /* Media keys report key codes that would type letters. */
        if (qt_key == Qt::Key_VolumeDown ||
            qt_key == Qt::Key_VolumeMute ||
            qt_key == Qt::Key_VolumeUp)
        {
            return QString();
        }
        if (qt_key >= 0x01000000)
        {
            /* Other function-area keys Neovim has no name for. */
            return QString();
        }
        text = text_from_key(qt_key, modifiers);
    }

    const QChar first = text.at(0);

    /*
     * Shift is dropped once it has produced a printable character; it
     * stays when Ctrl or Alt make this a chord.
     */
    if ((first.unicode() >= 0x80 || first.isPrint()) &&
        !(modifiers & Qt::ControlModifier) &&
        !(modifiers & Qt::AltModifier))
    {
        modifiers &= ~Qt::ShiftModifier;
    }

    /* Control characters resynthesize their printable form. */
    if (first.unicode() < 0x20)
    {
        text = text_from_key(qt_key, modifiers);
    }

    const QString prefix = modifier_prefix(modifiers);
    if (!prefix.isEmpty())
    {
        return bracketed(prefix, text);
    }
    return text;
}

QString literal_input_escaped(const QString& text)
{
    QString escaped = text;
    escaped.replace(
        QStringLiteral("<"),
        QStringLiteral("<lt>"));
    return escaped;
}

Mouse_Translation mouse_translation(
    Qt::MouseButton button,
    QEvent::Type event_type,
    Qt::KeyboardModifiers modifiers)
{
    Mouse_Translation translation;

    switch (button)
    {
    case Qt::LeftButton:
        translation.button = QByteArrayLiteral("left");
        break;
    case Qt::RightButton:
        translation.button = QByteArrayLiteral("right");
        break;
    case Qt::MiddleButton:
        translation.button = QByteArrayLiteral("middle");
        break;
    default:
        return translation;
    }

    switch (event_type)
    {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
        translation.action = QByteArrayLiteral("press");
        break;
    case QEvent::MouseButtonRelease:
        translation.action = QByteArrayLiteral("release");
        break;
    case QEvent::MouseMove:
        translation.action = QByteArrayLiteral("drag");
        break;
    default:
        return translation;
    }

    translation.modifiers = modifier_prefix(modifiers).toUtf8();
    translation.valid = true;
    return translation;
}

Mouse_Translation wheel_translation(
    int vertical_delta,
    Qt::KeyboardModifiers modifiers)
{
    Mouse_Translation translation;
    if (vertical_delta == 0)
    {
        return translation;
    }
    translation.button = QByteArrayLiteral("wheel");
    translation.action = vertical_delta > 0
        ? QByteArrayLiteral("up")
        : QByteArrayLiteral("down");
    translation.modifiers = modifier_prefix(modifiers).toUtf8();
    translation.valid = true;
    return translation;
}

} // namespace Kalam::Editor
