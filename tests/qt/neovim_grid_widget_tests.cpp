#include "neovim_font_names.h"
#include "neovim_grid_widget.h"
#include "neovim_input_translation.h"

#include <QApplication>
#include <QDebug>
#include <QFontDatabase>
#include <QImage>
#include <QVariantList>
#include <QVariantMap>

#include <string.h>

using Kalam::Editor::Mouse_Translation;
using Kalam::Editor::Neovim_Grid_Widget;
using Kalam::Editor::input_notation;
using Kalam::Editor::literal_input_escaped;
using Kalam::Editor::modifier_prefix;
using Kalam::Editor::mouse_translation;
using Kalam::Editor::wheel_translation;

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

void check_notation(
    int key,
    Qt::KeyboardModifiers modifiers,
    const QString& text,
    const QString& expected)
{
    const QString actual = input_notation(key, modifiers, text);
    check(
        actual == expected,
        QStringLiteral("key 0x%1 text '%2' maps to '%3', expected '%4'")
            .arg(key, 0, 16)
            .arg(text, actual, expected));
}

void test_input_notation_table()
{
    /* Plain printable characters pass through bare. */
    check_notation(Qt::Key_A, Qt::NoModifier, QStringLiteral("a"),
                   QStringLiteral("a"));
    check_notation(Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A"),
                   QStringLiteral("A"));
    check_notation(Qt::Key_5, Qt::ShiftModifier, QStringLiteral("%"),
                   QStringLiteral("%"));

    /* Control chords resynthesize the printable form. */
    check_notation(Qt::Key_A, Qt::ControlModifier, QString(QChar(0x01)),
                   QStringLiteral("<C-a>"));
    check_notation(
        Qt::Key_A,
        Qt::ControlModifier | Qt::ShiftModifier,
        QString(QChar(0x01)),
        QStringLiteral("<C-S-A>"));

    /* Special keys use their names, with modifiers. */
    check_notation(Qt::Key_Escape, Qt::NoModifier, QStringLiteral("\x1b"),
                   QStringLiteral("<Esc>"));
    check_notation(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"),
                   QStringLiteral("<Enter>"));
    check_notation(Qt::Key_F5, Qt::NoModifier, QString(),
                   QStringLiteral("<F5>"));
    check_notation(Qt::Key_F5, Qt::ShiftModifier, QString(),
                   QStringLiteral("<S-F5>"));
    check_notation(Qt::Key_Up, Qt::ControlModifier, QString(),
                   QStringLiteral("<C-Up>"));
    check_notation(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "),
                   QStringLiteral("<Space>"));
    check_notation(Qt::Key_Space, Qt::ControlModifier, QString(QChar(0x00)),
                   QStringLiteral("<C-Space>"));
    check_notation(Qt::Key_Backspace, Qt::NoModifier, QStringLiteral("\b"),
                   QStringLiteral("<BS>"));
    check_notation(Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"),
                   QStringLiteral("<Tab>"));

    /* "<" is escaped and its implied Shift dropped. */
    check_notation(Qt::Key_Less, Qt::ShiftModifier, QStringLiteral("<"),
                   QStringLiteral("<lt>"));
    check_notation(
        Qt::Key_Less,
        Qt::ControlModifier | Qt::ShiftModifier,
        QStringLiteral("<"),
        QStringLiteral("<C-lt>"));

    /* Ctrl with 6 or ^ normalizes to <C-^>. */
    check_notation(Qt::Key_6, Qt::ControlModifier, QString(),
                   QStringLiteral("<C-^>"));
    check_notation(
        Qt::Key_AsciiCircum,
        Qt::ControlModifier | Qt::ShiftModifier,
        QString(),
        QStringLiteral("<C-^>"));

    /* Backslash uses the named form. */
    check_notation(Qt::Key_Backslash, Qt::NoModifier, QStringLiteral("\\"),
                   QStringLiteral("<Bslash>"));

    /* Alt chords. */
    check_notation(Qt::Key_J, Qt::AltModifier, QStringLiteral("j"),
                   QStringLiteral("<A-j>"));

    /*
     * AltGr arrives as Ctrl+Alt on Windows; the composed character must
     * pass through with no prefix at all.
     */
    check_notation(
        Qt::Key_E,
        Qt::ControlModifier | Qt::AltModifier,
        QString::fromUtf8("\xc3\xa9"),
        QString::fromUtf8("\xc3\xa9"));

    /* Modifier-only and media keys send nothing. */
    check_notation(Qt::Key_Shift, Qt::ShiftModifier, QString(), QString());
    check_notation(Qt::Key_Control, Qt::ControlModifier, QString(),
                   QString());
    check_notation(Qt::Key_CapsLock, Qt::NoModifier, QString(), QString());
    check_notation(Qt::Key_VolumeUp, Qt::NoModifier, QString(), QString());

    /* Keypad keys use their k-names. */
    check_notation(Qt::Key_Enter, Qt::KeypadModifier, QStringLiteral("\r"),
                   QStringLiteral("<kEnter>"));
    check_notation(Qt::Key_7, Qt::KeypadModifier, QStringLiteral("7"),
                   QStringLiteral("<k7>"));
    check_notation(
        Qt::Key_Plus,
        Qt::KeypadModifier | Qt::ControlModifier,
        QStringLiteral("+"),
        QStringLiteral("<C-kPlus>"));

    /* Non-ASCII input keeps its text. */
    check_notation(
        Qt::Key_Odiaeresis,
        Qt::NoModifier,
        QString::fromUtf8("\xc3\xb6"),
        QString::fromUtf8("\xc3\xb6"));

    /* The Win32 prefix rule: Ctrl and Alt never combine. */
    check(
        modifier_prefix(Qt::ControlModifier | Qt::AltModifier).isEmpty(),
        QStringLiteral("Ctrl+Alt yields no prefix on Windows"));
    check(
        modifier_prefix(
            Qt::ControlModifier | Qt::ShiftModifier) ==
            QStringLiteral("C-S-"),
        QStringLiteral("Ctrl+Shift composes in order"));

    /* Literal IME text escaping. */
    check(
        literal_input_escaped(QStringLiteral("a<b><c")) ==
            QStringLiteral("a<lt>b><lt>c"),
        QStringLiteral("every '<' in literal text is escaped"));
}

/*
 * Terminals spell Nerd Fonts the way GDI does ("IosevkaTerm NF"); Qt only
 * knows the typographic name ("IosevkaTerm Nerd Font") and silently
 * substitutes a default for anything it cannot match. The shell bridges
 * the two so a guifont copied from a terminal config actually applies.
 */
void test_guifont_family_resolution()
{
    /* The abbreviation mapping is pure and testable without any fonts. */
    check(
        Kalam::Editor::neovim_expand_gdi_font_abbreviation(
            QStringLiteral("IosevkaTerm NF")) ==
            QStringLiteral("IosevkaTerm Nerd Font"),
        QStringLiteral("' NF' expands to ' Nerd Font'"));
    check(
        Kalam::Editor::neovim_expand_gdi_font_abbreviation(
            QStringLiteral("IosevkaTerm NFM")) ==
            QStringLiteral("IosevkaTerm Nerd Font Mono"),
        QStringLiteral("' NFM' expands to ' Nerd Font Mono'"));
    check(
        Kalam::Editor::neovim_expand_gdi_font_abbreviation(
            QStringLiteral("IosevkaTerm NFP")) ==
            QStringLiteral("IosevkaTerm Nerd Font Propo"),
        QStringLiteral("' NFP' expands to ' Nerd Font Propo'"));
    check(
        Kalam::Editor::neovim_expand_gdi_font_abbreviation(
            QStringLiteral("Cascadia Code")) ==
            QStringLiteral("Cascadia Code"),
        QStringLiteral("a name without an abbreviation is unchanged"));
    /* "NF" must be a suffix, never a substring hit. */
    check(
        Kalam::Editor::neovim_expand_gdi_font_abbreviation(
            QStringLiteral("NF Sans")) == QStringLiteral("NF Sans"),
        QStringLiteral("a mid-name 'NF' is not expanded"));

    /* An unknown family is returned as asked, with a warning, not a crash. */
    const QString unknown = QStringLiteral("Definitely Not Installed 12345");
    check(
        Kalam::Editor::neovim_resolve_font_family(unknown) == unknown,
        QStringLiteral("an unknown family is returned unchanged"));

    /*
     * When a font database is present (a real platform plugin, not
     * offscreen), an installed family must resolve to itself.
     */
    const QStringList available = QFontDatabase::families();
    if (!available.isEmpty())
    {
        check(
            Kalam::Editor::neovim_resolve_font_family(available.first()) ==
                available.first(),
            QStringLiteral("an installed family resolves to itself"));
    }
}

void test_mouse_translation()
{
    const Mouse_Translation press = mouse_translation(
        Qt::LeftButton, QEvent::MouseButtonPress, Qt::NoModifier);
    check(
        press.valid &&
            press.button == QByteArrayLiteral("left") &&
            press.action == QByteArrayLiteral("press"),
        QStringLiteral("a left press translates"));

    const Mouse_Translation drag = mouse_translation(
        Qt::LeftButton, QEvent::MouseMove, Qt::ShiftModifier);
    check(
        drag.valid &&
            drag.action == QByteArrayLiteral("drag") &&
            drag.modifiers == QByteArrayLiteral("S-"),
        QStringLiteral("a shift drag translates"));

    const Mouse_Translation release = mouse_translation(
        Qt::RightButton, QEvent::MouseButtonRelease, Qt::NoModifier);
    check(
        release.valid &&
            release.button == QByteArrayLiteral("right") &&
            release.action == QByteArrayLiteral("release"),
        QStringLiteral("a right release translates"));

    const Mouse_Translation double_click = mouse_translation(
        Qt::LeftButton, QEvent::MouseButtonDblClick, Qt::NoModifier);
    check(
        double_click.valid &&
            double_click.action == QByteArrayLiteral("press"),
        QStringLiteral("a double click is a press; Neovim counts clicks"));

    const Mouse_Translation invalid = mouse_translation(
        Qt::BackButton, QEvent::MouseButtonPress, Qt::NoModifier);
    check(
        !invalid.valid,
        QStringLiteral("unsupported buttons are invalid"));

    const Mouse_Translation up = wheel_translation(120, Qt::NoModifier);
    check(
        up.valid &&
            up.button == QByteArrayLiteral("wheel") &&
            up.action == QByteArrayLiteral("up"),
        QStringLiteral("a positive wheel delta scrolls up"));

    const Mouse_Translation down =
        wheel_translation(-120, Qt::ControlModifier);
    check(
        down.valid &&
            down.action == QByteArrayLiteral("down") &&
            down.modifiers == QByteArrayLiteral("C-"),
        QStringLiteral("a negative wheel delta scrolls down with C-"));

    check(
        !wheel_translation(0, Qt::NoModifier).valid,
        QStringLiteral("a zero wheel delta sends nothing"));
}

/* ------------------------------------------------ render fixture test */

QVariant event(const QByteArray& name, const QVariantList& tuple)
{
    QVariantList entry;
    entry << name << QVariant(tuple);
    return QVariant(entry);
}

QVariant cell(const QByteArray& text, qlonglong highlight_id)
{
    return QVariant(QVariantList() << text << highlight_id);
}

int count_pixels_closer_to(
    const QImage& image,
    const QRect& area,
    const QColor& target,
    const QColor& other)
{
    int count = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
    {
        for (int x = area.left(); x <= area.right(); ++x)
        {
            const QColor pixel = image.pixelColor(x, y);
            const int to_target =
                qAbs(pixel.red() - target.red()) +
                qAbs(pixel.green() - target.green()) +
                qAbs(pixel.blue() - target.blue());
            const int to_other =
                qAbs(pixel.red() - other.red()) +
                qAbs(pixel.green() - other.green()) +
                qAbs(pixel.blue() - other.blue());
            if (to_target < to_other)
            {
                ++count;
            }
        }
    }
    return count;
}

void test_render_fixture()
{
    Neovim_Grid_Widget widget(nullptr);

    const QColor default_foreground(0xff, 0xff, 0xff);
    const QColor default_background(0x20, 0x20, 0x20);
    const QColor red(0xff, 0x00, 0x00);
    const QColor blue(0x00, 0x00, 0xff);

    QVariantMap red_on_blue;
    red_on_blue.insert(QStringLiteral("foreground"), qlonglong(0xff0000));
    red_on_blue.insert(QStringLiteral("background"), qlonglong(0x0000ff));

    QVariantList events;
    events << event(
        QByteArrayLiteral("grid_resize"), QVariantList() << 1 << 10 << 3);
    events << event(
        QByteArrayLiteral("default_colors_set"),
        QVariantList()
            << qlonglong(0xffffff) << qlonglong(0x202020) << qlonglong(-1));
    events << event(
        QByteArrayLiteral("hl_attr_define"),
        QVariantList()
            << qlonglong(1) << red_on_blue << QVariantMap()
            << QVariantList());
    events << event(
        QByteArrayLiteral("grid_line"),
        QVariantList()
            << 1 << 0 << 0
            << QVariant(QVariantList()
                << cell(QByteArrayLiteral("A"), 1)
                << cell(QByteArrayLiteral("B"), 1)));
    events << event(QByteArrayLiteral("flush"), QVariantList());
    widget.handle_redraw(events);

    check(
        widget.model().columns() == 10 && widget.model().rows() == 3,
        QStringLiteral("the fixture resizes the model"));

    widget.resize(widget.sizeHint());
    const QImage image = widget.grab().toImage();
    const QSize cell_size = widget.cell_size();
    check(
        image.width() >= cell_size.width() * 10 &&
            image.height() >= cell_size.height() * 3,
        QStringLiteral("the grab covers the whole grid"));

    /* Cell (0,0) holds a red 'A' on blue. */
    const QRect first_cell(
        0, 0, cell_size.width() - 1, cell_size.height() - 1);
    const int glyph_pixels =
        count_pixels_closer_to(image, first_cell, red, blue);
    const int background_pixels =
        count_pixels_closer_to(image, first_cell, blue, red);
    check(
        glyph_pixels > 0,
        QStringLiteral("the glyph paints in the highlight foreground"));
    check(
        background_pixels > glyph_pixels,
        QStringLiteral("the cell background dominates the glyph"));

    /* An untouched cell shows the default background. */
    const QColor empty_cell_pixel = image.pixelColor(
        cell_size.width() * 5 + cell_size.width() / 2,
        cell_size.height() * 1 + cell_size.height() / 2);
    check(
        qAbs(empty_cell_pixel.red() - default_background.red()) < 24 &&
            qAbs(empty_cell_pixel.green() - default_background.green()) < 24 &&
            qAbs(empty_cell_pixel.blue() - default_background.blue()) < 24,
        QStringLiteral("untouched cells keep the default background"));

    Q_UNUSED(default_foreground);
}

void test_cursor_geometry()
{
    Neovim_Grid_Widget widget(nullptr);

    QVariantList events;
    events << event(
        QByteArrayLiteral("grid_resize"), QVariantList() << 1 << 10 << 4);
    events << event(
        QByteArrayLiteral("grid_line"),
        QVariantList()
            << 1 << 1 << 0
            << QVariant(QVariantList()
                << cell(QByteArrayLiteral("\xe6\xbc\xa2"), 0)
                << QVariant(QVariantList() << QByteArrayLiteral(""))));
    events << event(
        QByteArrayLiteral("grid_cursor_goto"),
        QVariantList() << 1 << 1 << 0);
    events << event(QByteArrayLiteral("flush"), QVariantList());

    QRect reported;
    QObject::connect(
        &widget,
        &Neovim_Grid_Widget::cursorRectangleChanged,
        &widget,
        [&](QRect rectangle)
        {
            reported = rectangle;
        });
    widget.handle_redraw(events);

    const QSize cell_size = widget.cell_size();
    const QRect expected(
        0,
        cell_size.height(),
        cell_size.width() * 2,
        cell_size.height());
    check(
        widget.cursor_cell_rectangle() == expected,
        QStringLiteral("the cursor rectangle spans a double-width cell"));
    check(
        reported == expected,
        QStringLiteral("the flush publishes the cursor rectangle"));

    QVariantList move_events;
    move_events << event(
        QByteArrayLiteral("grid_cursor_goto"),
        QVariantList() << 1 << 2 << 3);
    move_events << event(QByteArrayLiteral("flush"), QVariantList());
    widget.handle_redraw(move_events);

    check(
        widget.cursor_cell_rectangle() ==
            QRect(
                cell_size.width() * 3,
                cell_size.height() * 2,
                cell_size.width(),
                cell_size.height()),
        QStringLiteral("a single-width cell reports a one-cell rectangle"));
}

} // namespace

int main(int argument_count, char** arguments)
{
    QApplication application(argument_count, arguments);
    application.setQuitOnLastWindowClosed(false);

    test_input_notation_table();
    test_guifont_family_resolution();
    test_mouse_translation();
    test_render_fixture();
    test_cursor_geometry();

    if (failures == 0)
    {
        qInfo().noquote() << QStringLiteral("all grid widget tests passed");
    }
    return failures == 0 ? 0 : 1;
}
