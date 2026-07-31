#ifndef KALAM_EDITOR_QT_NEOVIM_GRID_WIDGET_H
#define KALAM_EDITOR_QT_NEOVIM_GRID_WIDGET_H

#include "neovim_grid_model.h"

#include <QFont>
#include <QPointer>
#include <QWidget>

namespace Kalam::Editor
{

class Neovim_Rpc_Client;

/*
 * Paints the grid model and owns all user input while the editor surface
 * has focus (spec_2.md sections 6 and 7). The widget never interprets
 * protocol semantics itself; it renders what the model holds and forwards
 * translated input to the RPC client.
 *
 * The client may be null in tests; redraw payloads are then delivered
 * directly through handle_redraw().
 */
class Neovim_Grid_Widget : public QWidget
{
    Q_OBJECT

public:
    explicit Neovim_Grid_Widget(
        Neovim_Rpc_Client* client,
        QWidget* parent = nullptr);

    /* Sends nvim_ui_attach for the current size. Requires a ready client. */
    void attach_ui();
    bool is_attached() const;

    const Neovim_Grid_Model& model() const;

    void set_grid_font(const QFont& font);
    QFont grid_font() const;
    QSize cell_size() const;

    /* The cursor cell in widget coordinates; two cells wide on a
       double-width cell. */
    QRect cursor_cell_rectangle() const;

    QSize sizeHint() const override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

public slots:
    void handle_redraw(const QVariantList& events);

signals:
    void cursorRectangleChanged(QRect widget_rectangle);
    void titleChanged(QString title);
    void guifontChanged(QString guifont);
    void attached();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    bool event(QEvent* event) override;

private:
    QPointer<Neovim_Rpc_Client> client_;
    Neovim_Grid_Model model_;
    QFont font_;
    int cell_width_ = 1;
    int cell_height_ = 1;
    int ascent_ = 1;
    bool attached_ = false;
    bool cursor_dirty_ = false;
    bool metrics_warning_reported_ = false;
    int last_sent_columns_ = 0;
    int last_sent_rows_ = 0;
    int last_drag_row_ = -1;
    int last_drag_column_ = -1;

    void compute_cell_metrics();
    void request_grid_size(int columns, int rows);
    void send_text_input(const QString& notation);
    void send_mouse_input(
        const QByteArray& button,
        const QByteArray& action,
        const QByteArray& modifiers,
        int row,
        int column);
    QPoint cell_from_position(const QPointF& position) const;
    QRect cell_rectangle(int row, int column, int width_cells) const;
    QRegion damage_to_pixels(const QVector<QRect>& damage) const;
    void paint_cells(QPainter& painter, const QRect& pixel_rectangle);
    void paint_cursor(QPainter& painter);
    bool is_double_width_at(int row, int column) const;
    QColor resolved_foreground(const Grid_Highlight& highlight) const;
    QColor resolved_background(const Grid_Highlight& highlight) const;
};

} // namespace Kalam::Editor

#endif
