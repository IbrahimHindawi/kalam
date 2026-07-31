#include "editor_neovim_shell.h"

#include "neovim_rpc_client.h"
#include "neovim_theme_bridge.h"

#include <QMoveEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>

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

} // namespace Kalam::Editor
