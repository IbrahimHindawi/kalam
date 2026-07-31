#include "neovim_theme_bridge.h"
#include "context_popup.h"
#include "qt_value_widgets.h"

#include <QApplication>
#include <QDebug>
#include <QFrame>
#include <QLabel>

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

QVariantMap highlight(const QString& foreground, const QString& background)
{
    QVariantMap result;
    if (!foreground.isEmpty())
    {
        result.insert(QStringLiteral("fg"), foreground);
    }
    if (!background.isEmpty())
    {
        result.insert(QStringLiteral("bg"), background);
    }
    return result;
}

QPalette fallback_palette()
{
    QPalette result;
    result.setColor(QPalette::Active, QPalette::Window, QColor("#121820"));
    result.setColor(QPalette::Active, QPalette::WindowText, QColor("#d9e2f0"));
    return result;
}

void test_highlight_role_mapping()
{
    QVariantMap highlights;
    highlights.insert(
        QStringLiteral("Normal"),
        highlight(QStringLiteral("#d8e4f2"), QStringLiteral("#121820")));
    highlights.insert(
        QStringLiteral("NormalFloat"),
        highlight(QStringLiteral("#edf4ff"), QStringLiteral("#1b2634")));
    highlights.insert(
        QStringLiteral("FloatBorder"),
        highlight(QStringLiteral("#86b6e6"), {}));
    highlights.insert(
        QStringLiteral("PmenuSel"),
        highlight(QStringLiteral("#ffffff"), QStringLiteral("#315b84")));
    highlights.insert(
        QStringLiteral("Pmenu"),
        highlight(QStringLiteral("#dce8f6"), QStringLiteral("#25384a")));
    highlights.insert(
        QStringLiteral("StatusLine"),
        highlight(QStringLiteral("#ffffff"), QStringLiteral("#3b628b")));
    highlights.insert(
        QStringLiteral("StatusLineNC"),
        highlight(QStringLiteral("#bac6d4"), QStringLiteral("#202c38")));
    highlights.insert(
        QStringLiteral("DiagnosticError"),
        highlight(QStringLiteral("#ff7b72"), {}));
    highlights.insert(
        QStringLiteral("DiagnosticWarn"),
        highlight(QStringLiteral("#e3b341"), {}));
    highlights.insert(
        QStringLiteral("DiagnosticInfo"),
        highlight(QStringLiteral("#79c0ff"), {}));
    highlights.insert(
        QStringLiteral("DiagnosticHint"),
        highlight(QStringLiteral("#7ee787"), {}));

    QVariantMap payload;
    payload.insert(QStringLiteral("highlights"), highlights);
    payload.insert(QStringLiteral("guifont"), QStringLiteral("Kalam Mono:h13:b"));

    const Kalam::Editor::Neovim_Theme theme =
        Kalam::Editor::Neovim_Theme_Bridge::theme_from_payload(
            payload,
            fallback_palette(),
            QFont(QStringLiteral("Fallback Mono"), 10));

    check(theme.normal_background == QColor("#121820"),
          QStringLiteral("Normal supplies the Qt base background"));
    check(theme.float_background == QColor("#1b2634"),
          QStringLiteral("NormalFloat supplies transient surface background"));
    check(theme.border == QColor("#86b6e6"),
          QStringLiteral("FloatBorder supplies transient surface border"));
    check(theme.selection_background == QColor("#315b84"),
          QStringLiteral("PmenuSel supplies active selection"));
    check(theme.hover_background == QColor("#25384a"),
          QStringLiteral("Pmenu supplies inactive row or hover"));
    check(theme.active_header_background == QColor("#3b628b"),
          QStringLiteral("StatusLine supplies active header"));
    check(theme.inactive_header_background == QColor("#202c38"),
          QStringLiteral("StatusLineNC supplies inactive header"));
    check(theme.error == QColor("#ff7b72"),
          QStringLiteral("DiagnosticError supplies error role"));
    check(theme.warning == QColor("#e3b341"),
          QStringLiteral("DiagnosticWarn supplies warning role"));
    check(theme.information == QColor("#79c0ff"),
          QStringLiteral("DiagnosticInfo supplies information role"));
    check(theme.hint == QColor("#7ee787"),
          QStringLiteral("DiagnosticHint supplies hint role"));
    check(theme.has_neovim_font &&
              theme.font.family() == QStringLiteral("Kalam Mono") &&
              qFuzzyCompare(theme.font.pointSizeF(), 13.0) &&
              theme.font.bold(),
          QStringLiteral("guifont supplies Qt chrome font"));
}

void test_fallbacks_and_application_refresh(QApplication* application)
{
    QVariantMap highlights;
    highlights.insert(
        QStringLiteral("Normal"),
        highlight(QStringLiteral("#d9e2f0"), QStringLiteral("#00000000")));
    highlights.insert(
        QStringLiteral("WinSeparator"),
        highlight(QStringLiteral("#7592ab"), {}));
    highlights.insert(
        QStringLiteral("Visual"),
        highlight(QStringLiteral("#ffffff"), QStringLiteral("#3c668e")));
    highlights.insert(
        QStringLiteral("CursorLine"),
        highlight({}, QStringLiteral("#223242")));

    QVariantMap payload;
    payload.insert(QStringLiteral("highlights"), highlights);
    const QPalette fallback = fallback_palette();
    const Kalam::Editor::Neovim_Theme theme =
        Kalam::Editor::Neovim_Theme_Bridge::theme_from_payload(
            payload,
            fallback,
            QFont(QStringLiteral("Fallback Mono"), 10));

    check(theme.normal_background == QColor("#121820"),
          QStringLiteral("transparent Normal background falls back to Qt palette"));
    check(theme.float_background == theme.normal_background,
          QStringLiteral("missing NormalFloat falls back to Normal"));
    check(theme.border == QColor("#7592ab"),
          QStringLiteral("missing FloatBorder falls back to WinSeparator"));
    check(theme.selection_background == QColor("#3c668e"),
          QStringLiteral("missing PmenuSel falls back to Visual"));
    check(theme.hover_background == QColor("#223242"),
          QStringLiteral("missing Pmenu falls back to CursorLine"));
    check(theme.error.isValid() && theme.warning.isValid() &&
              theme.information.isValid() && theme.hint.isValid(),
          QStringLiteral("missing diagnostics use visible defaults"));

    QFrame popup;
    popup.setObjectName(QStringLiteral("contextPopup"));
    Kalam::Editor::Neovim_Theme_Bridge::apply_to_application(
        application,
        theme);
    check(
        application->palette().color(QPalette::Active, QPalette::Window) ==
            theme.normal_background,
        QStringLiteral("application palette receives Normal background"));
    check(
        application->palette().color(QPalette::Active, QPalette::Highlight) ==
            theme.selection_background,
        QStringLiteral("application palette receives selection background"));
    check(
        application->property("kalam.theme.viewportBackground").value<QColor>() ==
            theme.viewport_background,
        QStringLiteral("viewport chrome receives shared theme property"));
    check(
        application->styleSheet().contains(QStringLiteral("QFrame#contextPopup")) &&
            application->styleSheet().contains(theme.float_background.name()),
        QStringLiteral("context popup stylesheet refreshes with the theme"));
}

void test_graphical_cursor_projection(QApplication* application)
{
    Kalam::Editor::Field_Descriptor transform_field;
    transform_field.id = 1;
    transform_field.name = QByteArrayLiteral("transform");
    transform_field.kind = Author_Field_Transform;
    Kalam::Editor::Author_Value_Owned transform_value;
    transform_value.kind = Author_Field_Transform;
    transform_value.transform.scale[0] = 1.0f;
    transform_value.transform.scale[1] = 1.0f;
    transform_value.transform.scale[2] = 1.0f;

    Kalam::Editor::Field_Descriptor color_field;
    color_field.id = 2;
    color_field.name = QByteArrayLiteral("color");
    color_field.kind = Author_Field_Color4;
    Kalam::Editor::Author_Value_Owned color_value;
    color_value.kind = Author_Field_Color4;
    color_value.f32[0] = 0.8f;
    color_value.f32[1] = 0.2f;
    color_value.f32[2] = 0.1f;
    color_value.f32[3] = 1.0f;

    Kalam::Editor::Context_Popup popup(
        {transform_field, color_field},
        {transform_value, color_value});
    popup.resize(900, 500);
    popup.show();
    application->processEvents();

    const QByteArray transform_line = QByteArrayLiteral(
        "  \"transform\": {\"translation\":[0.000,0.000,0.000],"
        "\"rotation_degrees\":[0.000,23.000,0.000],"
        "\"scale\":[1.000,1.000,1.000]},");
    const QByteArray color_line = QByteArrayLiteral(
        "  \"color\": [0.800,0.200,0.100,1.000]");
    const QByteArray text = QByteArrayLiteral("{\n") +
        transform_line + QByteArrayLiteral("\n") +
        color_line + QByteArrayLiteral("\n}");

    popup.set_mode(QByteArrayLiteral("n"));
    popup.set_buffer_cursor(
        text,
        1,
        transform_line.indexOf(QByteArrayLiteral("23.000")) + 2);
    application->processEvents();

    const QList<Kalam::Editor::Author_Value_Widget*> editors =
        popup.findChildren<Kalam::Editor::Author_Value_Widget*>();
    check(editors.size() == 2,
          QStringLiteral("context projection exposes both graphical fields"));
    if (editors.size() == 2)
    {
        check(editors.at(0)->has_projected_cursor(),
              QStringLiteral("active transform receives the visible cursor"));
        check(editors.at(0)->property("kalamCursorComponent").toInt() == 4,
              QStringLiteral("transform cursor maps to the second rotation component"));
        check(!editors.at(1)->has_projected_cursor(),
              QStringLiteral("inactive color does not display a cursor"));
    }
    check(popup.has_projected_cursor(),
          QStringLiteral("context reports a visible graphical cursor"));
    check(popup.findChild<QLabel*>(
              QStringLiteral("kalamContextBufferLine")) == nullptr,
          QStringLiteral("context no longer uses a separate raw JSON cursor strip"));

    const QByteArray invalid_color_line = QByteArrayLiteral(
        "  \"color\": [0.800,0.200,0.,1.000]");
    const QByteArray invalid_text = QByteArrayLiteral("{\n") +
        transform_line + QByteArrayLiteral("\n") +
        invalid_color_line + QByteArrayLiteral("\n}");
    popup.set_mode(QByteArrayLiteral("i"));
    popup.set_buffer_cursor(
        invalid_text,
        2,
        invalid_color_line.indexOf(QByteArrayLiteral("0."), 24) + 2);
    application->processEvents();

    if (editors.size() == 2)
    {
        check(!editors.at(0)->has_projected_cursor() &&
                  editors.at(1)->has_projected_cursor(),
              QStringLiteral("j/k movement transfers the cursor between GUI fields"));
        check(editors.at(1)->property("kalamCursorComponent").toInt() == 2,
              QStringLiteral("color cursor maps to its third component"));
        check(editors.at(1)->property("kalamCursorToken").toByteArray() ==
                  QByteArrayLiteral("0."),
              QStringLiteral("partially typed JSON remains visible in the GUI control"));
    }
    QLabel* cursor = popup.findChild<QLabel*>(
        QStringLiteral("kalamProjectedCursor"));
    check(cursor != nullptr &&
              cursor->property("kalamInsertCursor").toBool(),
          QStringLiteral("projected GUI cursor tracks Neovim Insert mode"));

    QWidget shell;
    shell.resize(1280, 720);
    shell.show();
    Kalam::Editor::Context_Popup compact_popup(
        {transform_field, color_field},
        {transform_value, color_value},
        &shell);
    Kalam::Editor::Context_Session_State state;
    state.window_row = 0;
    state.window_column = 0;
    state.window_width = 160;
    state.window_height = 50;
    const QRect source_cursor(600, 300, 8, 18);
    compact_popup.project_to(
        &shell,
        state,
        source_cursor,
        QSize(8, 18));
    application->processEvents();

    check(
        compact_popup.isVisible() &&
            compact_popup.width() <= 520 &&
            compact_popup.height() <= 420,
        QStringLiteral("context popup stays compact even when Neovim reports an editor-sized window"));
    check(
        compact_popup.geometry().left() > source_cursor.right(),
        QStringLiteral("context popup opens beside the captured source cursor"));
    check(
        compact_popup.geometry().right() < shell.width() &&
            compact_popup.geometry().bottom() <= shell.height() - 22,
        QStringLiteral("context popup remains inside the editor and preserves statusline space"));
}

} // namespace

int main(int argument_count, char** arguments)
{
    QApplication application(argument_count, arguments);
    application.setQuitOnLastWindowClosed(false);
    test_highlight_role_mapping();
    test_fallbacks_and_application_refresh(&application);
    test_graphical_cursor_projection(&application);
    if (failures == 0)
    {
        qInfo().noquote() << QStringLiteral("all Neovim theme bridge tests passed");
    }
    return failures == 0 ? 0 : 1;
}
