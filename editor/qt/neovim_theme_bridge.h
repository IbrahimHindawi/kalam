#ifndef KALAM_EDITOR_QT_NEOVIM_THEME_BRIDGE_H
#define KALAM_EDITOR_QT_NEOVIM_THEME_BRIDGE_H

#include <QColor>
#include <QFont>
#include <QPalette>
#include <QPointer>
#include <QVariantMap>

#include <QObject>

class QApplication;

namespace Kalam::Editor
{

class Neovim_Rpc_Client;

/*
 * The application-facing subset of Neovim's highlight table.  The bridge
 * receives already-resolved highlight definitions from Neovim, then applies
 * the fallback order from todo.md.  A missing or fully transparent colour is
 * deliberately treated as unset:
 *
 * - Normal falls back to Qt's pre-theme Window and WindowText colours.
 * - NormalFloat falls back to Normal.
 * - FloatBorder falls back to WinSeparator, then a derived Normal contrast.
 * - PmenuSel falls back to Visual; Pmenu falls back to CursorLine.
 * - StatusLine and StatusLineNC fall back to the active and inactive row
 *   colours respectively.
 * - Diagnostic colours fall back to legible conventional diagnostic colours.
 *
 * This keeps a partially-defined colourscheme usable and makes the rules
 * available to future graph, gizmo, and viewport-overlay renderers.
 */
struct Neovim_Theme
{
    QColor normal_background;
    QColor normal_foreground;
    QColor float_background;
    QColor float_foreground;
    QColor border;
    QColor selection_background;
    QColor selection_foreground;
    QColor hover_background;
    QColor hover_foreground;
    QColor active_header_background;
    QColor active_header_foreground;
    QColor inactive_header_background;
    QColor inactive_header_foreground;
    QColor error;
    QColor warning;
    QColor information;
    QColor hint;
    QColor viewport_background;
    QColor viewport_focus;
    QFont font;
    bool has_neovim_font = false;
};

class Neovim_Theme_Bridge final : public QObject
{
public:
    explicit Neovim_Theme_Bridge(
        Neovim_Rpc_Client* client,
        QObject* parent = nullptr);

    static Neovim_Theme theme_from_payload(
        const QVariantMap& payload,
        const QPalette& fallback_palette,
        const QFont& fallback_font);
    static QString style_sheet(const Neovim_Theme& theme);
    static void apply_to_application(
        QApplication* application,
        const Neovim_Theme& theme);

private:
    QPointer<Neovim_Rpc_Client> client_;
    QPalette fallback_palette_;
    QFont fallback_font_;
    QString last_fingerprint_;
    bool request_in_flight_ = false;
    bool refresh_pending_ = false;

    void handle_ready();
    void handle_notification(
        const QByteArray& name,
        const QVariantList& arguments);
    void install_live_refresh_hooks();
    void request_refresh();
    void request_theme_snapshot();
};

} // namespace Kalam::Editor

#endif
