#ifndef KALAM_EDITOR_QT_EDITOR_NEOVIM_SHELL_H
#define KALAM_EDITOR_QT_EDITOR_NEOVIM_SHELL_H

#include "editor_types.h"

#include "shell.h"

namespace Kalam::Editor
{

/*
 * A deliberately tiny Neovim-Qt adaptation. The upstream Shell remains a
 * reusable QWidget; this subclass exposes cursor geometry already known by
 * ShellWidget and emits it at the point where grid-cursor updates arrive.
 */
class Editor_Neovim_Shell final : public NeovimQt::Shell
{
    Q_OBJECT

public:
    explicit Editor_Neovim_Shell(
        NeovimQt::NeovimConnector* connector,
        QWidget* parent = nullptr);

    Cursor_Geometry cursor_geometry() const;

signals:
    void cursorGeometryChanged(Kalam::Editor::Cursor_Geometry geometry);

protected:
    void handleGridCursorGoto(const QVariantList& arguments) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    qint64 active_grid_ = 1;

    void publish_cursor_geometry();
};

} // namespace Kalam::Editor

#endif
