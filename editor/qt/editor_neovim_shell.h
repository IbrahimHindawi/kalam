#ifndef KALAM_EDITOR_QT_EDITOR_NEOVIM_SHELL_H
#define KALAM_EDITOR_QT_EDITOR_NEOVIM_SHELL_H

#include "editor_types.h"

#include "neovim_grid_widget.h"

#include <QFont>

namespace Kalam::Editor
{

class Neovim_Rpc_Client;
class Neovim_Theme_Bridge;

/*
 * The editor's Neovim surface: the Kalam-owned grid widget plus the editor
 * contracts the main window relies on — cursor geometry publication, the
 * attach flag, and the confirm-quit close handshake (spec_2.md section 2.1).
 */
class Editor_Neovim_Shell final : public Neovim_Grid_Widget
{
    Q_OBJECT

public:
    explicit Editor_Neovim_Shell(
        Neovim_Rpc_Client* client,
        QWidget* parent = nullptr);

    Cursor_Geometry cursor_geometry() const;
    bool isNeovimAttached() const;

    /*
     * Asks Neovim to quit with confirmation and reports false; the actual
     * close is authorized later by neovimGuiCloseRequest when the process
     * exits. Shadows QWidget::close on purpose, matching the previous
     * shell's contract.
     */
    bool close();

signals:
    void cursorGeometryChanged(Kalam::Editor::Cursor_Geometry geometry);
    void neovimGuiCloseRequest(int status);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    Neovim_Rpc_Client* client_ = nullptr;
    Neovim_Theme_Bridge* theme_bridge_ = nullptr;

    void publish_cursor_geometry();
    void publish_editor_font();
    void attach_when_ready();

    /* Parses Neovim's 'guifont' ("Family:h12"); unparsable keeps fallback. */
    static QFont font_from_guifont(
        const QString& guifont,
        const QFont& fallback);
};

} // namespace Kalam::Editor

#endif
