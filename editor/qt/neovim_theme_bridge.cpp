#include "neovim_theme_bridge.h"

#include "auto/neovimapi0.h"
#include "msgpackrequest.h"
#include "neovimconnector.h"

#include <QApplication>
#include <QCoreApplication>
#include <QList>
#include <QRegularExpression>
#include <QStringList>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>
#include <QtGlobal>

namespace Kalam::Editor
{

namespace
{

constexpr int kMinimum_Control_Luma_Delta = 18;

QColor colour_from_variant(const QVariant& value)
{
    if (!value.isValid() || value.isNull())
    {
        return {};
    }

    QColor result;
    if (value.canConvert<QColor>())
    {
        result = value.value<QColor>();
    }
    if (!result.isValid())
    {
        result = QColor(value.toString());
    }
    if (!result.isValid() || result.alpha() == 0)
    {
        return {};
    }
    return result;
}

QColor first_opaque(const QList<QColor>& candidates)
{
    for (const QColor& candidate : candidates)
    {
        if (candidate.isValid() && candidate.alpha() != 0)
        {
            return candidate;
        }
    }
    return {};
}

QColor mix(const QColor& foreground, const QColor& background, int percent)
{
    const int clamped_percent = qBound(0, percent, 100);
    const int foreground_alpha = foreground.alpha();
    const int effective_percent =
        (clamped_percent * foreground_alpha + 127) / 255;
    const int inverse_percent = 100 - effective_percent;
    return QColor(
        (foreground.red() * effective_percent +
         background.red() * inverse_percent) / 100,
        (foreground.green() * effective_percent +
         background.green() * inverse_percent) / 100,
        (foreground.blue() * effective_percent +
         background.blue() * inverse_percent) / 100);
}

QColor opaque_on(const QColor& colour, const QColor& background)
{
    if (!colour.isValid() || colour.alpha() == 0)
    {
        return {};
    }
    return colour.alpha() == 255 ? colour : mix(colour, background, 100);
}

int luma(const QColor& colour)
{
    return (colour.red() * 2126 + colour.green() * 7152 +
            colour.blue() * 722) /
        10000;
}

QColor distinct_from(const QColor& candidate, const QColor& background)
{
    if (!candidate.isValid())
    {
        return candidate;
    }
    const int distance = qAbs(luma(candidate) - luma(background));
    if (distance >= kMinimum_Control_Luma_Delta)
    {
        return candidate;
    }
    return luma(background) < 128
        ? mix(QColor(Qt::white), background, 58)
        : mix(QColor(Qt::black), background, 58);
}

QColor readable_foreground(const QColor& candidate, const QColor& background)
{
    const QColor resolved = distinct_from(candidate, background);
    if (qAbs(luma(resolved) - luma(background)) >= 76)
    {
        return resolved;
    }
    return luma(background) < 128 ? QColor(Qt::white) : QColor(Qt::black);
}

QColor palette_colour(
    const QPalette& palette,
    QPalette::ColorRole role,
    const QColor& hard_fallback)
{
    const QColor result = palette.color(QPalette::Active, role);
    return result.isValid() && result.alpha() != 0 ? result : hard_fallback;
}

QVariantMap highlights_from_payload(const QVariantMap& payload)
{
    const QVariant highlights = payload.value(QStringLiteral("highlights"));
    return highlights.canConvert<QVariantMap>()
        ? highlights.toMap()
        : payload;
}

QColor highlight_colour(
    const QVariantMap& highlights,
    const QString& group,
    const QString& primary_key,
    const QString& legacy_key = {})
{
    const QVariantMap highlight = highlights.value(group).toMap();
    QColor result = colour_from_variant(highlight.value(primary_key));
    if (!result.isValid() && !legacy_key.isEmpty())
    {
        result = colour_from_variant(highlight.value(legacy_key));
    }
    return result;
}

QColor foreground(const QVariantMap& highlights, const QString& group)
{
    return highlight_colour(
        highlights,
        group,
        QStringLiteral("fg"),
        QStringLiteral("foreground"));
}

QColor background(const QVariantMap& highlights, const QString& group)
{
    return highlight_colour(
        highlights,
        group,
        QStringLiteral("bg"),
        QStringLiteral("background"));
}

QString css_colour(const QColor& colour)
{
    return colour.name(QColor::HexRgb);
}

QFont font_from_option(const QString& option, const QFont& fallback, bool* used)
{
    if (used != nullptr)
    {
        *used = false;
    }

    const QString trimmed = option.trimmed();
    if (trimmed.isEmpty())
    {
        return fallback;
    }

    const int first_attribute = trimmed.indexOf(QLatin1Char(':'));
    QString family = first_attribute >= 0
        ? trimmed.left(first_attribute)
        : trimmed;
    family.replace(QStringLiteral("\\ "), QStringLiteral(" "));
    family.replace(QStringLiteral("\\,"), QStringLiteral(","));
    if (family.isEmpty())
    {
        return fallback;
    }

    QFont result = fallback;
    result.setFamily(family);
    const QRegularExpression height_expression(
        QStringLiteral("(?:^|:)h([0-9]+(?:\\.[0-9]+)?)"));
    const QRegularExpressionMatch height =
        height_expression.match(trimmed);
    if (height.hasMatch())
    {
        bool valid_height = false;
        const qreal point_size = height.capturedView(1).toDouble(&valid_height);
        if (valid_height && point_size > 0.0)
        {
            result.setPointSizeF(point_size);
        }
    }
    result.setBold(trimmed.contains(QStringLiteral(":b")));
    result.setItalic(trimmed.contains(QStringLiteral(":i")));
    if (used != nullptr)
    {
        *used = true;
    }
    return result;
}

QString theme_fingerprint(const Neovim_Theme& theme)
{
    const QList<QColor> colours{
        theme.normal_background,
        theme.normal_foreground,
        theme.float_background,
        theme.float_foreground,
        theme.border,
        theme.selection_background,
        theme.selection_foreground,
        theme.hover_background,
        theme.hover_foreground,
        theme.active_header_background,
        theme.active_header_foreground,
        theme.inactive_header_background,
        theme.inactive_header_foreground,
        theme.error,
        theme.warning,
        theme.information,
        theme.hint,
        theme.viewport_background,
        theme.viewport_focus,
    };
    QStringList parts;
    parts.reserve(colours.size() + 4);
    for (const QColor& colour : colours)
    {
        parts.push_back(colour.name(QColor::HexArgb));
    }
    parts.push_back(theme.font.family());
    parts.push_back(QString::number(theme.font.pointSizeF()));
    parts.push_back(theme.font.bold() ? QStringLiteral("bold") : QString());
    parts.push_back(theme.font.italic() ? QStringLiteral("italic") : QString());
    return parts.join(QLatin1Char('|'));
}

const QByteArray kTheme_Snapshot_Expression = QByteArrayLiteral(
    "(function() "
    "local names={'Normal','NormalFloat','FloatBorder','WinSeparator',"
    "'PmenuSel','Visual','Pmenu','CursorLine','StatusLine','StatusLineNC',"
    "'DiagnosticError','DiagnosticWarn','DiagnosticInfo','DiagnosticHint'}; "
    "local function colour(value) "
    "if type(value)=='number' then return string.format('#%06x',value) end "
    "return nil end; "
    "local function resolve(name) "
    "local ok,hl=pcall(vim.api.nvim_get_hl,0,{name=name,link=false}); "
    "if not ok then "
    "local old_ok,old=pcall(vim.api.nvim_get_hl_by_name,name,true); "
    "hl=old_ok and old or {} end; "
    "return {fg=colour(hl.fg or hl.foreground),"
    "bg=colour(hl.bg or hl.background),sp=colour(hl.sp or hl.special)} end; "
    "local result={background=vim.o.background,guifont=vim.o.guifont,"
    "highlights={}}; "
    "for _,name in ipairs(names) do result.highlights[name]=resolve(name) end; "
    "return result end)()");

const QByteArray kInstall_Live_Refresh_Hooks_Expression = QByteArrayLiteral(
    "(function() "
    "local channel=tonumber(_A); "
    "if not channel or channel<=0 then return false end; "
    "local group=vim.api.nvim_create_augroup('KalamQtThemeBridge',{clear=true}); "
    "local function refresh() pcall(vim.rpcnotify,channel,'kalam.theme_changed') end; "
    "vim.api.nvim_create_autocmd('ColorScheme',{group=group,callback=refresh}); "
    "vim.api.nvim_create_autocmd('OptionSet',{group=group,"
    "pattern={'background','guifont'},callback=refresh}); "
    "return true end)()");

} // namespace

Neovim_Theme_Bridge::Neovim_Theme_Bridge(
    NeovimQt::NeovimConnector* connector,
    QObject* parent)
    : QObject(parent), connector_(connector)
{
    Q_ASSERT(connector != nullptr);
    QApplication* application = qobject_cast<QApplication*>(
        QCoreApplication::instance());
    if (application != nullptr)
    {
        fallback_palette_ = application->palette();
        fallback_font_ = application->font();
    }

    connect(
        connector,
        &NeovimQt::NeovimConnector::ready,
        this,
        [this]() { handle_ready(); });
    connect(
        connector,
        &NeovimQt::NeovimConnector::processExited,
        this,
        [this](int)
        {
            api_ = nullptr;
            request_in_flight_ = false;
            refresh_pending_ = false;
        });
    if (connector->isReady())
    {
        QTimer::singleShot(0, this, [this]() { handle_ready(); });
    }
}

Neovim_Theme Neovim_Theme_Bridge::theme_from_payload(
    const QVariantMap& payload,
    const QPalette& fallback_palette,
    const QFont& fallback_font)
{
    const QVariantMap highlights = highlights_from_payload(payload);
    const QColor fallback_background = palette_colour(
        fallback_palette,
        QPalette::Window,
        QColor(30, 30, 30));
    const QColor fallback_foreground = palette_colour(
        fallback_palette,
        QPalette::WindowText,
        QColor(225, 225, 225));

    Neovim_Theme result;
    result.normal_background = opaque_on(
        first_opaque({background(highlights, QStringLiteral("Normal"))}),
        fallback_background);
    if (!result.normal_background.isValid())
    {
        result.normal_background = fallback_background;
    }
    result.normal_foreground = opaque_on(
        first_opaque({foreground(highlights, QStringLiteral("Normal"))}),
        result.normal_background);
    if (!result.normal_foreground.isValid())
    {
        result.normal_foreground = fallback_foreground;
    }
    result.normal_foreground = readable_foreground(
        result.normal_foreground,
        result.normal_background);

    result.float_background = opaque_on(
        first_opaque({background(highlights, QStringLiteral("NormalFloat"))}),
        result.normal_background);
    if (!result.float_background.isValid())
    {
        result.float_background = result.normal_background;
    }
    result.float_foreground = opaque_on(
        first_opaque({foreground(highlights, QStringLiteral("NormalFloat"))}),
        result.float_background);
    if (!result.float_foreground.isValid())
    {
        result.float_foreground = result.normal_foreground;
    }
    result.float_foreground = readable_foreground(
        result.float_foreground,
        result.float_background);

    result.border = opaque_on(
        first_opaque({
            foreground(highlights, QStringLiteral("FloatBorder")),
            background(highlights, QStringLiteral("FloatBorder")),
            foreground(highlights, QStringLiteral("WinSeparator")),
            background(highlights, QStringLiteral("WinSeparator")),
        }),
        result.float_background);
    if (!result.border.isValid())
    {
        result.border = mix(
            result.normal_foreground,
            result.float_background,
            55);
    }
    result.border = distinct_from(result.border, result.float_background);

    result.selection_background = opaque_on(
        first_opaque({
            background(highlights, QStringLiteral("PmenuSel")),
            background(highlights, QStringLiteral("Visual")),
        }),
        result.normal_background);
    if (!result.selection_background.isValid())
    {
        result.selection_background = mix(
            result.normal_foreground,
            result.normal_background,
            35);
    }
    result.selection_background = distinct_from(
        result.selection_background,
        result.normal_background);
    result.selection_foreground = opaque_on(
        first_opaque({
            foreground(highlights, QStringLiteral("PmenuSel")),
            foreground(highlights, QStringLiteral("Visual")),
        }),
        result.selection_background);
    if (!result.selection_foreground.isValid())
    {
        result.selection_foreground = result.normal_foreground;
    }
    result.selection_foreground = readable_foreground(
        result.selection_foreground,
        result.selection_background);

    result.hover_background = opaque_on(
        first_opaque({
            background(highlights, QStringLiteral("Pmenu")),
            background(highlights, QStringLiteral("CursorLine")),
        }),
        result.normal_background);
    if (!result.hover_background.isValid())
    {
        result.hover_background = mix(
            result.normal_foreground,
            result.normal_background,
            12);
    }
    result.hover_background = distinct_from(
        result.hover_background,
        result.normal_background);
    result.hover_foreground = opaque_on(
        first_opaque({
            foreground(highlights, QStringLiteral("Pmenu")),
            foreground(highlights, QStringLiteral("CursorLine")),
        }),
        result.hover_background);
    if (!result.hover_foreground.isValid())
    {
        result.hover_foreground = result.normal_foreground;
    }
    result.hover_foreground = readable_foreground(
        result.hover_foreground,
        result.hover_background);

    result.active_header_background = opaque_on(
        first_opaque({background(highlights, QStringLiteral("StatusLine"))}),
        result.normal_background);
    if (!result.active_header_background.isValid())
    {
        result.active_header_background = result.selection_background;
    }
    result.active_header_background = distinct_from(
        result.active_header_background,
        result.normal_background);
    result.active_header_foreground = opaque_on(
        first_opaque({foreground(highlights, QStringLiteral("StatusLine"))}),
        result.active_header_background);
    if (!result.active_header_foreground.isValid())
    {
        result.active_header_foreground = result.selection_foreground;
    }
    result.active_header_foreground = readable_foreground(
        result.active_header_foreground,
        result.active_header_background);

    result.inactive_header_background = opaque_on(
        first_opaque({background(highlights, QStringLiteral("StatusLineNC"))}),
        result.normal_background);
    if (!result.inactive_header_background.isValid())
    {
        result.inactive_header_background = result.hover_background;
    }
    result.inactive_header_background = distinct_from(
        result.inactive_header_background,
        result.normal_background);
    result.inactive_header_foreground = opaque_on(
        first_opaque({foreground(highlights, QStringLiteral("StatusLineNC"))}),
        result.inactive_header_background);
    if (!result.inactive_header_foreground.isValid())
    {
        result.inactive_header_foreground = result.hover_foreground;
    }
    result.inactive_header_foreground = readable_foreground(
        result.inactive_header_foreground,
        result.inactive_header_background);

    const bool dark = luma(result.normal_background) < 128;
    result.error = opaque_on(
        first_opaque({foreground(highlights, QStringLiteral("DiagnosticError"))}),
        result.float_background);
    if (!result.error.isValid())
    {
        result.error = dark ? QColor(255, 110, 110) : QColor(176, 0, 32);
    }
    result.warning = opaque_on(
        first_opaque({foreground(highlights, QStringLiteral("DiagnosticWarn"))}),
        result.float_background);
    if (!result.warning.isValid())
    {
        result.warning = dark ? QColor(255, 196, 92) : QColor(140, 90, 0);
    }
    result.information = opaque_on(
        first_opaque({foreground(highlights, QStringLiteral("DiagnosticInfo"))}),
        result.float_background);
    if (!result.information.isValid())
    {
        result.information = dark ? QColor(104, 181, 255) : QColor(0, 84, 163);
    }
    result.hint = opaque_on(
        first_opaque({foreground(highlights, QStringLiteral("DiagnosticHint"))}),
        result.float_background);
    if (!result.hint.isValid())
    {
        result.hint = dark ? QColor(122, 214, 186) : QColor(0, 105, 86);
    }

    result.viewport_background = result.normal_background;
    result.viewport_focus = result.active_header_background;
    result.font = font_from_option(
        payload.value(QStringLiteral("guifont")).toString(),
        fallback_font,
        &result.has_neovim_font);
    return result;
}

QString Neovim_Theme_Bridge::style_sheet(const Neovim_Theme& theme)
{
    QString result = QStringLiteral(R"(
QSplitter::handle {
    background: %1;
}
QSplitter::handle:hover {
    background: %2;
}
QFrame#contextPopup,
QDialog#generatedAuthorForm,
QColorDialog#authorColorDialog {
    background-color: %3;
    color: %4;
    border: 1px solid %5;
}
QFrame#contextPopup QLabel,
QDialog#generatedAuthorForm QLabel,
QColorDialog#authorColorDialog QLabel {
    color: %4;
}
QFrame#contextPopup QLabel#kalamSurfaceTitle,
QDialog#generatedAuthorForm QLabel#kalamSurfaceTitle {
    background-color: %11;
    color: %12;
    padding: 4px 6px;
}
QFrame#contextPopup:active QLabel#kalamSurfaceTitle,
QDialog#generatedAuthorForm:active QLabel#kalamSurfaceTitle {
    background-color: %10;
    color: %9;
}
QFrame#contextPopup QPushButton#kalamSurfaceClose,
QDialog#generatedAuthorForm QPushButton#kalamSurfaceClose {
    background-color: %11;
    color: %12;
    border: 1px solid %5;
    border-radius: 0px;
    padding: 2px 6px;
}
QFrame#contextPopup QPushButton#kalamSurfaceClose:hover,
QDialog#generatedAuthorForm QPushButton#kalamSurfaceClose:hover {
    background-color: %10;
    color: %9;
}
QFrame#contextPopup QLineEdit,
QFrame#contextPopup QAbstractSpinBox,
QDialog#generatedAuthorForm QLineEdit,
QDialog#generatedAuthorForm QAbstractSpinBox,
QColorDialog#authorColorDialog QLineEdit,
QColorDialog#authorColorDialog QAbstractSpinBox {
    background-color: %6;
    color: %7;
    selection-background-color: %8;
    selection-color: %9;
    border: 1px solid %5;
    border-radius: 0px;
    padding: 3px 5px;
}
QFrame#contextPopup QLineEdit:focus,
QFrame#contextPopup QAbstractSpinBox:focus,
QDialog#generatedAuthorForm QLineEdit:focus,
QDialog#generatedAuthorForm QAbstractSpinBox:focus,
QColorDialog#authorColorDialog QLineEdit:focus,
QColorDialog#authorColorDialog QAbstractSpinBox:focus {
    border: 2px solid %10;
}
QFrame#contextPopup QLineEdit[kalamProjectedCursor="true"],
QFrame#contextPopup QAbstractSpinBox[kalamProjectedCursor="true"],
QFrame#contextPopup QCheckBox[kalamProjectedCursor="true"] {
    border: 2px solid %10;
    background-color: %6;
    color: %7;
}
QFrame#contextPopup QPushButton,
QDialog#generatedAuthorForm QPushButton,
QColorDialog#authorColorDialog QPushButton {
    background-color: %11;
    color: %12;
    border: 1px solid %5;
    border-radius: 0px;
    padding: 4px 8px;
}
QFrame#contextPopup QPushButton:hover,
QDialog#generatedAuthorForm QPushButton:hover,
QColorDialog#authorColorDialog QPushButton:hover {
    background-color: %13;
    color: %14;
}
QFrame#contextPopup QPushButton:focus,
QDialog#generatedAuthorForm QPushButton:focus,
QColorDialog#authorColorDialog QPushButton:focus,
QFrame#contextPopup QPushButton:default,
QColorDialog#authorColorDialog QPushButton:default {
    background-color: %10;
    color: %9;
    border: 1px solid %10;
}
QFrame#contextPopup QScrollArea,
QDialog#generatedAuthorForm QScrollArea,
QFrame#contextPopup QScrollBar,
QDialog#generatedAuthorForm QScrollBar {
    background-color: %3;
    color: %4;
    border: none;
}
QFrame#contextPopup QWidget#authorValueWidget {
    border-left: 3px solid transparent;
    padding-left: 6px;
}
QFrame#contextPopup QWidget#authorValueWidget[kalamActiveField="true"] {
    background-color: %8;
    color: %9;
    border-left: 3px solid %10;
}
QFrame#contextPopup QLabel#kalamSurfaceMode {
    background-color: %10;
    color: %9;
    padding: 4px 8px;
}
QFrame#contextPopup QLabel#kalamContextHint {
    color: %4;
    padding: 4px;
}
QFrame#contextPopup QLabel#kalamProjectedCursor {
    background-color: %6;
    color: %7;
    border: none;
    padding: 0px 3px;
}
QFrame#contextPopup QLabel#kalamContextValidation {
    background-color: %3;
    color: %4;
    border-left: 3px solid %5;
    padding: 4px 8px;
}
QWidget#engineViewport {
    background-color: %15;
    border: 1px solid %5;
}
QWidget#engineViewport:focus {
    border: 2px solid %10;
}
QToolTip,
QMenu {
    background-color: %3;
    color: %4;
    border: 1px solid %5;
}
QMenu::item:selected {
    background-color: %10;
    color: %9;
}
)");
    const QStringList replacements{
        css_colour(theme.inactive_header_background),
        css_colour(theme.active_header_background),
        css_colour(theme.float_background),
        css_colour(theme.float_foreground),
        css_colour(theme.border),
        css_colour(theme.normal_background),
        css_colour(theme.normal_foreground),
        css_colour(theme.selection_background),
        css_colour(theme.selection_foreground),
        css_colour(theme.active_header_background),
        css_colour(theme.inactive_header_background),
        css_colour(theme.inactive_header_foreground),
        css_colour(theme.hover_background),
        css_colour(theme.hover_foreground),
        css_colour(theme.viewport_background),
    };
    for (qsizetype index = replacements.size(); index != 0; --index)
    {
        result.replace(
            QStringLiteral("%") + QString::number(index),
            replacements.at(index - 1));
    }
    return result;
}

void Neovim_Theme_Bridge::apply_to_application(
    QApplication* application,
    const Neovim_Theme& theme)
{
    if (application == nullptr)
    {
        return;
    }

    if (application->style() == nullptr ||
        application->style()->objectName() != QStringLiteral("fusion"))
    {
        application->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }

    QPalette palette = application->palette();
    palette.setColor(QPalette::Active, QPalette::Window, theme.normal_background);
    palette.setColor(QPalette::Active, QPalette::WindowText, theme.normal_foreground);
    palette.setColor(QPalette::Active, QPalette::Base, theme.normal_background);
    palette.setColor(QPalette::Active, QPalette::AlternateBase, theme.hover_background);
    palette.setColor(QPalette::Active, QPalette::Text, theme.normal_foreground);
    palette.setColor(QPalette::Active, QPalette::Button, theme.inactive_header_background);
    palette.setColor(QPalette::Active, QPalette::ButtonText, theme.inactive_header_foreground);
    palette.setColor(QPalette::Active, QPalette::Highlight, theme.selection_background);
    palette.setColor(QPalette::Active, QPalette::HighlightedText, theme.selection_foreground);
    palette.setColor(QPalette::Active, QPalette::ToolTipBase, theme.float_background);
    palette.setColor(QPalette::Active, QPalette::ToolTipText, theme.float_foreground);
    palette.setColor(QPalette::Inactive, QPalette::Window, theme.normal_background);
    palette.setColor(QPalette::Inactive, QPalette::WindowText, theme.normal_foreground);
    palette.setColor(QPalette::Inactive, QPalette::Base, theme.normal_background);
    palette.setColor(QPalette::Inactive, QPalette::Text, theme.normal_foreground);
    palette.setColor(QPalette::Inactive, QPalette::Button, theme.inactive_header_background);
    palette.setColor(QPalette::Inactive, QPalette::ButtonText, theme.inactive_header_foreground);
    palette.setColor(QPalette::Inactive, QPalette::Highlight, theme.hover_background);
    palette.setColor(QPalette::Inactive, QPalette::HighlightedText, theme.hover_foreground);
    application->setPalette(palette);
    application->setFont(theme.font);
    application->setStyleSheet(style_sheet(theme));

    application->setProperty(
        "kalam.theme.viewportBackground",
        theme.viewport_background);
    application->setProperty("kalam.theme.viewportFocus", theme.viewport_focus);
    application->setProperty("kalam.theme.border", theme.border);
    application->setProperty("kalam.theme.error", theme.error);
    application->setProperty("kalam.theme.warning", theme.warning);
    application->setProperty("kalam.theme.information", theme.information);
    application->setProperty("kalam.theme.hint", theme.hint);
}

void Neovim_Theme_Bridge::handle_ready()
{
    if (connector_ == nullptr || api_ != nullptr)
    {
        return;
    }

    api_ = connector_->api0();
    if (api_ == nullptr)
    {
        return;
    }
    connect(
        api_,
        &NeovimQt::NeovimApi0::neovimNotification,
        this,
        [this](const QByteArray& name, const QVariantList& arguments)
        {
            handle_notification(name, arguments);
        });
    install_live_refresh_hooks();
    request_refresh();
}

void Neovim_Theme_Bridge::handle_notification(
    const QByteArray& name,
    const QVariantList& arguments)
{
    Q_UNUSED(arguments);
    if (name == QByteArrayLiteral("kalam.theme_changed"))
    {
        request_refresh();
    }
}

void Neovim_Theme_Bridge::install_live_refresh_hooks()
{
    if (api_ == nullptr || connector_ == nullptr)
    {
        return;
    }
    api_->vim_call_function(
        QByteArrayLiteral("luaeval"),
        QVariantList{
            kInstall_Live_Refresh_Hooks_Expression,
            QVariant::fromValue<qulonglong>(connector_->channel())});
}

void Neovim_Theme_Bridge::request_refresh()
{
    if (request_in_flight_)
    {
        refresh_pending_ = true;
        return;
    }
    QTimer::singleShot(0, this, [this]() { request_theme_snapshot(); });
}

void Neovim_Theme_Bridge::request_theme_snapshot()
{
    if (request_in_flight_ || api_ == nullptr || connector_ == nullptr ||
        !connector_->isReady())
    {
        return;
    }

    NeovimQt::MsgpackRequest* request = api_->vim_call_function(
        QByteArrayLiteral("luaeval"),
        QVariantList{kTheme_Snapshot_Expression, QVariant()});
    if (request == nullptr)
    {
        return;
    }
    request_in_flight_ = true;
    connect(
        request,
        &NeovimQt::MsgpackRequest::finished,
        this,
        [this](quint32, quint64, const QVariant& response)
        {
            request_in_flight_ = false;
            const QVariantMap payload = response.toMap();
            if (payload.contains(QStringLiteral("highlights")))
            {
                QApplication* application = qobject_cast<QApplication*>(
                    QCoreApplication::instance());
                if (application != nullptr)
                {
                    const Neovim_Theme theme = theme_from_payload(
                        payload,
                        fallback_palette_,
                        fallback_font_);
                    const QString fingerprint = theme_fingerprint(theme);
                    if (fingerprint != last_fingerprint_)
                    {
                        last_fingerprint_ = fingerprint;
                        apply_to_application(application, theme);
                    }
                }
            }
            if (refresh_pending_)
            {
                refresh_pending_ = false;
                request_refresh();
            }
        });
    connect(
        request,
        &NeovimQt::MsgpackRequest::error,
        this,
        [this](quint32, quint64, const QVariant&)
        {
            request_in_flight_ = false;
            if (refresh_pending_)
            {
                refresh_pending_ = false;
                request_refresh();
            }
        });
}

} // namespace Kalam::Editor
