#include "editor_neovim_shell.h"

#include "neovim_theme_bridge.h"

#include <QMoveEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>

#include "neovimconnector.h"

namespace Kalam::Editor
{

Editor_Neovim_Shell::Editor_Neovim_Shell(
    NeovimQt::NeovimConnector* connector,
    QWidget* parent)
    : NeovimQt::Shell(connector, parent)
{
    theme_bridge_ = new Neovim_Theme_Bridge(connector, this);

    connect(
        connector,
        &NeovimQt::NeovimConnector::ready,
        this,
        [this]()
        {
            init();
        },
        Qt::QueuedConnection);
    if (connector->isReady())
    {
        QTimer::singleShot(0, this, [this]()
        {
            init();
        });
    }
    connect(
        this,
        &ShellWidget::cellSizeChanged,
        this,
        [this](QSize)
        {
            publish_cursor_geometry();
        });
}

Cursor_Geometry Editor_Neovim_Shell::cursor_geometry() const
{
    Cursor_Geometry geometry;
    geometry.grid = active_grid_;
    geometry.row = m_cursor_pos.y();
    geometry.column = m_cursor_pos.x();
    geometry.cell_size = cellSize();
    geometry.grid_origin = QPoint(0, 0);
    geometry.cursor_rect = neovimCursorRect();
    return geometry;
}

void Editor_Neovim_Shell::handleGridCursorGoto(
    const QVariantList& arguments)
{
    if (!arguments.isEmpty())
    {
        bool converted = false;
        const qint64 grid = arguments.at(0).toLongLong(&converted);
        if (converted)
        {
            active_grid_ = grid;
        }
    }

    NeovimQt::Shell::handleGridCursorGoto(arguments);
    publish_cursor_geometry();
}

void Editor_Neovim_Shell::resizeEvent(QResizeEvent* event)
{
    NeovimQt::Shell::resizeEvent(event);
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
    NeovimQt::Shell::moveEvent(event);
    publish_cursor_geometry();
}

void Editor_Neovim_Shell::showEvent(QShowEvent* event)
{
    NeovimQt::Shell::showEvent(event);
    publish_cursor_geometry();
}

void Editor_Neovim_Shell::publish_cursor_geometry()
{
    emit cursorGeometryChanged(cursor_geometry());
}

} // namespace Kalam::Editor
