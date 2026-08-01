#include "editor_neovim_shell.h"

#include "neovim_font_names.h"
#include "neovim_rpc_client.h"
#include "neovim_theme_bridge.h"

#include <QFontDatabase>
#include <QFontInfo>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QtDebug>

#include <stdio.h>

namespace Kalam::Editor
{

Editor_Neovim_Shell::Editor_Neovim_Shell(
    Neovim_Rpc_Client* client,
    QWidget* parent)
    : Neovim_Grid_Widget(client, parent)
    , client_(client)
{
    Q_ASSERT(client != nullptr);
    theme_bridge_ = new Neovim_Theme_Bridge(client, this);

    connect(
        client,
        &Neovim_Rpc_Client::ready,
        this,
        &Editor_Neovim_Shell::attach_when_ready,
        Qt::QueuedConnection);
    if (client->is_ready())
    {
        QTimer::singleShot(
            0,
            this,
            &Editor_Neovim_Shell::attach_when_ready);
    }
    connect(
        client,
        &Neovim_Rpc_Client::processExited,
        this,
        [this](int exit_code)
        {
            emit neovimGuiCloseRequest(exit_code);
        });
    connect(
        this,
        &Neovim_Grid_Widget::cursorRectangleChanged,
        this,
        [this](QRect)
        {
            publish_cursor_geometry();
        });

    /*
     * 'guifont' retunes the grid, and Qt surfaces follow the grid rather
     * than the Qt default so the context popup always matches the editor
     * text. Setting the grid font does not re-emit guifontChanged, so
     * this cannot loop.
     */
    connect(
        this,
        &Neovim_Grid_Widget::guifontChanged,
        this,
        [this](const QString& guifont)
        {
            const QFont parsed = font_from_guifont(guifont, grid_font());
            if (parsed != grid_font())
            {
                set_grid_font(parsed);
            }
            publish_editor_font();
        });
    publish_editor_font();
}

Cursor_Geometry Editor_Neovim_Shell::cursor_geometry() const
{
    Cursor_Geometry geometry;
    geometry.grid = 1;
    geometry.row = model().cursor_row();
    geometry.column = model().cursor_column();
    geometry.cell_size = cell_size();
    geometry.grid_origin = QPoint(0, 0);
    geometry.cursor_rect = cursor_cell_rectangle();
    return geometry;
}

bool Editor_Neovim_Shell::isNeovimAttached() const
{
    return is_attached() && client_ != nullptr && client_->is_connected();
}

bool Editor_Neovim_Shell::close()
{
    if (client_ == nullptr || !client_->is_ready())
    {
        return QWidget::close();
    }
    client_->call(
        QByteArrayLiteral("nvim_command"),
        QVariantList() << QByteArrayLiteral("confirm qall"));
    return false;
}

void Editor_Neovim_Shell::attach_when_ready()
{
    attach_ui();
}

void Editor_Neovim_Shell::resizeEvent(QResizeEvent* event)
{
    Neovim_Grid_Widget::resizeEvent(event);
    QTimer::singleShot(
        0,
        this,
        [this]()
        {
            publish_cursor_geometry();
        });
}

void Editor_Neovim_Shell::moveEvent(QMoveEvent* event)
{
    Neovim_Grid_Widget::moveEvent(event);
    publish_cursor_geometry();
}

void Editor_Neovim_Shell::showEvent(QShowEvent* event)
{
    Neovim_Grid_Widget::showEvent(event);
    attach_when_ready();
    publish_cursor_geometry();
}

void Editor_Neovim_Shell::publish_cursor_geometry()
{
    emit cursorGeometryChanged(cursor_geometry());
}

void Editor_Neovim_Shell::publish_editor_font()
{
    if (theme_bridge_ != nullptr)
    {
        theme_bridge_->set_editor_font(grid_font());
    }
}

QFont Editor_Neovim_Shell::font_from_guifont(
    const QString& guifont,
    const QFont& fallback)
{
    /*
     * Neovim's 'guifont' is "Family:hN" with escaped spaces, and may list
     * several comma-separated candidates. Revision 1 takes the first
     * family and its height; an unparsable value keeps the current font.
     */
    const QString first = guifont.split(QLatin1Char(',')).value(0).trimmed();
    if (first.isEmpty())
    {
        return fallback;
    }

    const QStringList parts = first.split(QLatin1Char(':'));
    QString family = parts.value(0);
    family.replace(QStringLiteral("\\ "), QStringLiteral(" "));
    family.replace(QStringLiteral("\\,"), QStringLiteral(","));
    if (family.isEmpty())
    {
        return fallback;
    }

    QFont result = fallback;
    result.setFamily(neovim_resolve_font_family(family));
    for (qsizetype index = 1; index < parts.size(); ++index)
    {
        const QString attribute = parts.at(index);
        if (attribute.startsWith(QLatin1Char('h')) && attribute.size() > 1)
        {
            bool valid = false;
            const qreal point_size = attribute.mid(1).toDouble(&valid);
            if (valid && point_size > 0.0)
            {
                result.setPointSizeF(point_size);
            }
        }
        else if (attribute == QStringLiteral("b"))
        {
            result.setBold(true);
        }
        else if (attribute == QStringLiteral("i"))
        {
            result.setItalic(true);
        }
    }
    return result;
}

} // namespace Kalam::Editor
