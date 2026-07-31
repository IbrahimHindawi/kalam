#ifndef KALAM_EDITOR_QT_NEOVIM_INPUT_TRANSLATION_H
#define KALAM_EDITOR_QT_NEOVIM_INPUT_TRANSLATION_H

#include <QByteArray>
#include <QEvent>
#include <QString>

namespace Kalam::Editor
{

/*
 * Pure translation from Qt input events to Neovim input notation
 * (spec_2.md section 7). No widget, no process, fully table-testable.
 *
 * The rules are an audited extraction from the pinned Neovim-Qt v0.2.19
 * input.cpp / input_win32.cpp, rewritten for Windows-only semantics:
 *
 * - Ctrl and Alt never combine in a prefix because Win32 cannot tell
 *   Ctrl+Alt from AltGr; whichever appears without the other wins.
 * - Shift is dropped when it already determined a printable character.
 * - "<" becomes <lt> with the implied Shift removed.
 * - Ctrl combined with 6 or ^ always sends <C-^>.
 * - Backslash uses the named <Bslash> form.
 * - Modifier-only presses and media keys translate to nothing.
 */

/* Returns Neovim notation, or an empty string when nothing must be sent. */
QString input_notation(
    int qt_key,
    Qt::KeyboardModifiers modifiers,
    const QString& event_text);

/* "C-", "S-", "C-S-", ... following the Win32 Ctrl/Alt exclusion rule. */
QString modifier_prefix(Qt::KeyboardModifiers modifiers);

/* Escapes literal text typed by an input method for nvim_input. */
QString literal_input_escaped(const QString& text);

/* Structured arguments for nvim_input_mouse. */
struct Mouse_Translation
{
    bool valid = false;
    QByteArray button;    /* "left", "middle", "right", "wheel" */
    QByteArray action;    /* "press", "drag", "release", "up", "down" */
    QByteArray modifiers; /* "C-", "S-", ... same prefix grammar */
};

Mouse_Translation mouse_translation(
    Qt::MouseButton button,
    QEvent::Type event_type,
    Qt::KeyboardModifiers modifiers);

Mouse_Translation wheel_translation(
    int vertical_delta,
    Qt::KeyboardModifiers modifiers);

} // namespace Kalam::Editor

#endif
