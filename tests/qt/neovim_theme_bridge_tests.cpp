#include "neovim_theme_bridge.h"
#include "context_popup.h"
#include "generated_form.h"
#include "qt_value_widgets.h"

#include <QApplication>

#include <stdio.h>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QLabel>
#include <QScrollArea>
#include <QKeyEvent>

namespace
{

int failures = 0;

void check(bool condition, const QString& message)
{
    if (!condition)
    {
        fprintf(stderr, "check failed: %s\n", qUtf8Printable(message));
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

/*
 * Qt surfaces must render in the same font as the Neovim grid. With
 * 'guifont' unset the theme adopts the grid's font instead of the Qt
 * default; with it set, the parsed family and height win.
 */
void test_editor_font_is_shared_with_qt_surfaces()
{
    QVariantMap payload;
    payload.insert(QStringLiteral("highlights"), QVariantMap());

    QFont grid_font(QStringLiteral("Cascadia Mono"), 13);
    const Kalam::Editor::Neovim_Theme without_guifont =
        Kalam::Editor::Neovim_Theme_Bridge::theme_from_payload(
            payload,
            fallback_palette(),
            grid_font);
    check(
        !without_guifont.has_neovim_font,
        "an unset guifont is reported as absent");
    check(
        without_guifont.font.family() == QStringLiteral("Cascadia Mono") &&
            qFuzzyCompare(without_guifont.font.pointSizeF(), 13.0),
        "an unset guifont adopts the grid font, not the Qt default");

    payload.insert(
        QStringLiteral("guifont"),
        QStringLiteral("JetBrains\\ Mono:h15"));
    const Kalam::Editor::Neovim_Theme with_guifont =
        Kalam::Editor::Neovim_Theme_Bridge::theme_from_payload(
            payload,
            fallback_palette(),
            grid_font);
    check(
        with_guifont.has_neovim_font,
        "a set guifont is reported as present");
    check(
        with_guifont.font.family() == QStringLiteral("JetBrains Mono"),
        "an escaped guifont family is unescaped");
    check(
        qFuzzyCompare(with_guifont.font.pointSizeF(), 15.0),
        "the guifont height becomes the point size");
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
            application->styleSheet().contains(theme.float_background.name()) &&
            application->styleSheet().contains(
                QStringLiteral("kalamContextStatusline")),
        QStringLiteral("context popup stylesheet refreshes with the theme"));
}

void test_modal_context_popup(QApplication* application)
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

    QWidget shell;
    shell.resize(1280, 720);
    shell.show();
    Kalam::Editor::Context_Popup popup(
        {transform_field, color_field},
        {transform_value, color_value},
        &shell);

    const QSize cell_size(8, 18);
    const QRect source_cursor(600, 670, 8, 18);
    popup.project_to(&shell, source_cursor, cell_size);
    popup.focus_modal();
    application->processEvents();

    auto send_key = [application](
        QWidget* target,
        int key,
        const QString& text = QString(),
        Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        if (target == nullptr)
            return;
        QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
        application->sendEvent(target, &event);
        application->processEvents();
    };

    check(
        popup.isVisible() &&
            popup.width() <= popup.compact_size_limit().width() &&
            popup.height() <= popup.compact_size_limit().height(),
        QStringLiteral("context popup remains a compact transient surface"));
    /*
     * The popup shows all of its content rather than staying beside the
     * cursor: a wide monospace face makes the form wider than the space
     * next to the cursor, and clipping a field is worse than overlapping.
     * What must hold is that it is fully on screen and never truncated.
     */
    check(
        shell.rect().contains(popup.geometry()),
        QStringLiteral("context popup stays inside the editor surface"));
    check(
        popup.width() >= qMin(popup.sizeHint().width(), shell.width()) &&
            popup.height() >= qMin(popup.sizeHint().height(), shell.height()),
        QStringLiteral("context popup shows its content without cuts"));
    check(
        popup.geometry().bottom() < source_cursor.top() &&
            shell.height() - popup.geometry().bottom() >=
                2 * cell_size.height(),
        QStringLiteral("a low cursor places the popup above it and preserves editor statusline space"));

    QLabel* statusline = popup.findChild<QLabel*>(
        QStringLiteral("kalamContextStatusline"));
    check(
        statusline != nullptr &&
            statusline->geometry().bottom() >= popup.height() - 4,
        QStringLiteral("context statusline is the popup's bottom-most row"));
    check(
        popup.mode() == QByteArrayLiteral("n") &&
            QApplication::focusWidget() == &popup &&
            popup.active_field() == 0 &&
            popup.active_component() == 0 &&
            popup.has_modal_selection(),
        QStringLiteral("popup opens with its own focused Normal mode and visible selection"));

    send_key(&popup, Qt::Key_L, QStringLiteral("l"));
    check(
        popup.active_component() == 1,
        QStringLiteral("Normal-mode l moves between graphical components"));
    send_key(&popup, Qt::Key_J, QStringLiteral("j"));
    check(
        popup.active_field() == 1,
        QStringLiteral("Normal-mode j moves between graphical fields"));

    send_key(&popup, Qt::Key_I, QStringLiteral("i"));
    QWidget* insert_target = QApplication::focusWidget();
    check(
        popup.mode() == QByteArrayLiteral("i") &&
            insert_target != nullptr &&
            insert_target != &popup &&
            popup.isAncestorOf(insert_target),
        QStringLiteral("i enters popup-owned Insert mode and focuses the selected Qt control"));

    for (int index = 0; index < 5; ++index)
        send_key(insert_target, Qt::Key_I, QStringLiteral("i"));
    check(
        popup.mode() == QByteArrayLiteral("i") &&
            QApplication::focusWidget() != nullptr &&
            popup.isAncestorOf(QApplication::focusWidget()),
        QStringLiteral("repeated Insert-mode i keystrokes remain inside the Qt control"));

    bool preview_requested = false;
    QObject::connect(
        &popup,
        &Kalam::Editor::Context_Popup::fieldPreviewRequested,
        &popup,
        [&preview_requested](
            Author_Field_Id,
            const Kalam::Editor::Author_Value_Owned&)
        {
            preview_requested = true;
        });
    const QList<QDoubleSpinBox*> spins =
        popup.findChildren<QDoubleSpinBox*>();
    if (!spins.isEmpty())
        spins.last()->setValue(spins.last()->value() + 0.1);
    application->processEvents();
    check(
        preview_requested && popup.is_dirty(),
        QStringLiteral("Qt field edits become dirty staged previews"));

    send_key(QApplication::focusWidget(), Qt::Key_Escape);
    check(
        popup.mode() == QByteArrayLiteral("n") &&
            QApplication::focusWidget() == &popup,
        QStringLiteral("Escape leaves the Qt field and returns to popup Normal mode"));

    bool write_requested = false;
    bool close_after_write = true;
    QObject::connect(
        &popup,
        &Kalam::Editor::Context_Popup::applyRequested,
        &popup,
        [&write_requested, &close_after_write](bool close_after)
        {
            write_requested = true;
            close_after_write = close_after;
        });
    send_key(&popup, Qt::Key_Colon, QStringLiteral(":"));
    check(
        popup.mode() == QByteArrayLiteral("c"),
        QStringLiteral("colon opens the popup-owned command line"));
    send_key(&popup, Qt::Key_W, QStringLiteral("w"));
    send_key(&popup, Qt::Key_Return);
    check(
        write_requested && !close_after_write,
        QStringLiteral(":w requests a staged transaction commit without closing"));

    check(
        popup.findChild<QLabel*>(
            QStringLiteral("kalamContextBufferLine")) == nullptr,
        QStringLiteral("context popup has no raw or hidden Neovim staging buffer"));
}

void test_generated_form_fits_its_schema(QApplication* application)
{
    Kalam::Editor::Document_Handle document;
    document.document_id = 1;
    document.root_object_id = 2;
    document.path = QStringLiteral("scene.json");

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
    color_value.f32[3] = 1.0f;

    /*
     * A generated form is sized to its schema, so no field is ever cut off
     * horizontally. The width depends on the schema and on whatever font
     * Neovim asked for, which is exactly why a fixed size failed.
     *
     * The schema below is deliberately wider than the old hardcoded 460px:
     * with a narrower one this check passes even against the bug, because
     * the offscreen platform has no font database and its fallback metrics
     * are small enough to fit. Verified by reintroducing the fixed size
     * and confirming these assertions fail.
     */
        Kalam::Editor::Field_Descriptor wide_field;
        wide_field.id = 3;
        wide_field.name =
            QByteArrayLiteral("chromatic_aberration_strength_horizontal");
        wide_field.group = QByteArrayLiteral("post_process/lens/distortion");
        wide_field.kind = Author_Field_Transform;
        Kalam::Editor::Author_Value_Owned wide_value;
        wide_value.kind = Author_Field_Transform;
        wide_value.transform.scale[0] = 1.0f;
        wide_value.transform.scale[1] = 1.0f;
        wide_value.transform.scale[2] = 1.0f;

        Kalam::Editor::Schema_Descriptor wide_schema;
        wide_schema.fields = {
            transform_field, color_field, wide_field, wide_field};
        /*
         * Heap-allocated on purpose: Generated_Form sets WA_DeleteOnClose,
         * so close() deletes it. Closing a stack instance would delete a
         * stack object.
         */
        auto* wide_form = new Kalam::Editor::Generated_Form(
            document,
            wide_schema,
            {transform_value, color_value, wide_value, wide_value});
        wide_form->show();
        application->processEvents();

        QScrollArea* scroll = wide_form->findChild<QScrollArea*>();
        check(
            scroll != nullptr,
            QStringLiteral("the generated form scrolls its field area"));
        if (scroll != nullptr)
        {
            check(
                scroll->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
                QStringLiteral("the generated form never scrolls sideways"));
            QWidget* contents = scroll->widget();
            const int needed =
                contents == nullptr ? 0 : contents->sizeHint().width();
            /*
             * Guard the guard: if the fixture stops being wider than the
             * old fixed size, this test silently stops proving anything.
             */
            check(
                needed > 460,
                QStringLiteral("the fixture is wide enough to detect a "
                               "fixed-width regression (needs %1)")
                    .arg(needed));
            check(
                contents != nullptr &&
                    scroll->viewport()->width() >= needed,
                QStringLiteral("the field area is wide enough for every "
                               "field (viewport %1, needs %2)")
                    .arg(scroll->viewport()->width())
                    .arg(needed));
        }
        wide_form->close();
        application->processEvents();
}

void test_modal_generated_form(QApplication* application)
{
    Kalam::Editor::Document_Handle document;
    document.document_id = 9;
    document.root_object_id = 12;
    document.buffer = 3;
    document.changedtick = 7;
    document.path = QStringLiteral("modal.scene.json");

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
    color_value.f32[3] = 1.0f;

    Kalam::Editor::Schema_Descriptor schema;
    schema.fields = {transform_field, color_field};
    Kalam::Editor::Generated_Form form(
        document,
        schema,
        {transform_value, color_value});
    form.show();
    form.focus_modal();
    application->processEvents();


    auto send_key = [application](
        QWidget* target,
        int key,
        const QString& text = QString())
    {
        QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier, text);
        application->sendEvent(target, &event);
        application->processEvents();
    };

    check(
        form.mode() == QByteArrayLiteral("n") &&
            QApplication::focusWidget() == &form &&
            form.active_field() == 0 &&
            form.has_modal_selection(),
        QStringLiteral("generated form opens in focused Normal mode"));
    send_key(&form, Qt::Key_L, QStringLiteral("l"));
    send_key(&form, Qt::Key_J, QStringLiteral("j"));
    check(
        form.active_component() == 1 && form.active_field() == 1,
        QStringLiteral("generated form uses h/j/k/l field and component navigation"));

    send_key(&form, Qt::Key_I, QStringLiteral("i"));
    check(
        form.mode() == QByteArrayLiteral("i") &&
            QApplication::focusWidget() != nullptr &&
            form.isAncestorOf(QApplication::focusWidget()),
        QStringLiteral("generated form i enters the selected real Qt control"));

    const QList<QDoubleSpinBox*> spins =
        form.findChildren<QDoubleSpinBox*>();
    if (!spins.isEmpty())
        spins.last()->setValue(spins.last()->value() + 0.1);
    application->processEvents();
    check(form.is_dirty(), QStringLiteral("generated form edits remain staged"));

    send_key(QApplication::focusWidget(), Qt::Key_Escape);
    bool write_requested = false;
    QObject::connect(
        &form,
        &Kalam::Editor::Generated_Form::writeRequested,
        &form,
        [&write_requested](bool close_after_write)
        {
            write_requested = !close_after_write;
        });
    send_key(&form, Qt::Key_Colon, QStringLiteral(":"));
    send_key(&form, Qt::Key_W, QStringLiteral("w"));
    send_key(&form, Qt::Key_Return);
    check(write_requested, QStringLiteral("generated form :w requests an explicit staged write"));

    QLabel* statusline = form.findChild<QLabel*>(
        QStringLiteral("kalamContextStatusline"));
    check(
        statusline != nullptr &&
            statusline->geometry().bottom() >= form.height() - 4,
        QStringLiteral("generated form owns a bottom status and command line"));
}


} // namespace

int main(int argument_count, char** arguments)
{
    QApplication application(argument_count, arguments);
    application.setQuitOnLastWindowClosed(false);
    test_highlight_role_mapping();
    test_editor_font_is_shared_with_qt_surfaces();
    test_fallbacks_and_application_refresh(&application);
    test_modal_context_popup(&application);
    test_generated_form_fits_its_schema(&application);
    test_modal_generated_form(&application);
    if (failures == 0)
    {
        qInfo().noquote() << QStringLiteral("all Neovim theme bridge tests passed");
    }
    return failures == 0 ? 0 : 1;
}
