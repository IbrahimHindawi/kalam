#include "neovim_grid_widget.h"

#include "neovim_input_translation.h"
#include "neovim_rpc_client.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QtDebug>

namespace Kalam::Editor
{

namespace
{

const int Minimum_Grid_Columns = 12;
const int Minimum_Grid_Rows = 3;

QFont default_grid_font()
{
    /*
     * Kalam is the GUI, so it owns the grid font until Neovim states one
     * with 'guifont'. Start from the system's preferred fixed-pitch face
     * rather than a hardcoded family, so the default tracks the machine
     * instead of one developer's taste.
     */
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (font.family().isEmpty())
    {
        font.setFamily(QStringLiteral("Consolas"));
    }
    font.setPointSize(11);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setKerning(false);
    return font;
}

} // namespace

Neovim_Grid_Widget::Neovim_Grid_Widget(
    Neovim_Rpc_Client* client,
    QWidget* parent)
    : QWidget(parent)
    , client_(client)
    , font_(default_grid_font())
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setAttribute(Qt::WA_OpaquePaintEvent);
    compute_cell_metrics();

    if (client_ != nullptr)
    {
        connect(
            client_,
            &Neovim_Rpc_Client::redrawReceived,
            this,
            &Neovim_Grid_Widget::handle_redraw);
    }
}

const Neovim_Grid_Model& Neovim_Grid_Widget::model() const
{
    return model_;
}

bool Neovim_Grid_Widget::is_attached() const
{
    return attached_;
}

QFont Neovim_Grid_Widget::grid_font() const
{
    return font_;
}

QSize Neovim_Grid_Widget::cell_size() const
{
    return QSize(cell_width_, cell_height_);
}

void Neovim_Grid_Widget::set_grid_font(const QFont& font)
{
    font_ = font;
    font_.setStyleHint(QFont::Monospace);
    font_.setFixedPitch(true);
    font_.setKerning(false);
    metrics_warning_reported_ = false;
    compute_cell_metrics();
    if (attached_)
    {
        const int columns =
            qMax(Minimum_Grid_Columns, width() / cell_width_);
        const int rows = qMax(Minimum_Grid_Rows, height() / cell_height_);
        request_grid_size(columns, rows);
    }
    update();
}

void Neovim_Grid_Widget::compute_cell_metrics()
{
    const QFontMetrics metrics(font_);
    const int wide_advance =
        metrics.horizontalAdvance(QLatin1Char('W'));
    const int narrow_advance =
        metrics.horizontalAdvance(QLatin1Char('i'));
    if (wide_advance != narrow_advance && !metrics_warning_reported_)
    {
        metrics_warning_reported_ = true;
        qWarning().noquote()
            << QStringLiteral("the grid font is not fixed pitch "
                              "(W=%1, i=%2); using the maximum")
                .arg(wide_advance)
                .arg(narrow_advance);
    }
    cell_width_ = qMax(1, qMax(wide_advance, narrow_advance));
    ascent_ = metrics.ascent();
    cell_height_ =
        qMax(1, ascent_ + metrics.descent() + qMax(0, metrics.leading()));
}

void Neovim_Grid_Widget::attach_ui()
{
    if (attached_ || client_ == nullptr || !client_->is_ready())
    {
        return;
    }
    const int columns = qMax(
        Minimum_Grid_Columns,
        qMax(1, width()) / cell_width_);
    const int rows = qMax(
        Minimum_Grid_Rows,
        qMax(1, height()) / cell_height_);

    QVariantMap options;
    options.insert(QStringLiteral("rgb"), true);
    options.insert(QStringLiteral("ext_linegrid"), true);
    options.insert(QStringLiteral("ext_multigrid"), true);
    client_->call(
        QByteArrayLiteral("nvim_ui_attach"),
        QVariantList() << columns << rows << options);
    attached_ = true;
    last_sent_columns_ = columns;
    last_sent_rows_ = rows;
    emit attached();
}

void Neovim_Grid_Widget::request_grid_size(int columns, int rows)
{
    if (client_ == nullptr || !attached_)
    {
        return;
    }
    if (columns == last_sent_columns_ && rows == last_sent_rows_)
    {
        return;
    }
    last_sent_columns_ = columns;
    last_sent_rows_ = rows;
    client_->call(
        QByteArrayLiteral("nvim_ui_try_resize"),
        QVariantList() << columns << rows);
}

void Neovim_Grid_Widget::handle_redraw(const QVariantList& events)
{
    const Grid_Update_Summary summary = model_.apply_redraw(events);

    if (summary.cursor_moved || summary.mode_changed ||
        summary.busy_changed || summary.grid_resized)
    {
        cursor_dirty_ = true;
    }
    /*
     * Layout changes and the flush that publishes them need not arrive in
     * one batch, so the dirty flag latches across batches exactly as the
     * cursor's does.
     */
    if (summary.window_layout_changed)
    {
        layout_dirty_ = true;
    }
    if (summary.title_changed)
    {
        emit titleChanged(model_.title());
    }
    if (summary.guifont_changed)
    {
        emit guifontChanged(model_.guifont());
    }
    if (summary.grid_resized)
    {
        updateGeometry();
    }

    if (!summary.flushed)
    {
        return;
    }

    /* The flush is the frame boundary; nothing repaints before it. */
    if (summary.grid_resized || summary.default_colors_changed ||
        layout_dirty_)
    {
        model_.take_published_damage();
        update();
    }
    else if (model_.has_published_damage())
    {
        update(damage_to_pixels(model_.take_published_damage()));
    }
    if (layout_dirty_)
    {
        layout_dirty_ = false;
        emit gridLayoutChanged();
    }
    if (cursor_dirty_)
    {
        cursor_dirty_ = false;
        const QRect rectangle = cursor_cell_rectangle();
        emit cursorRectangleChanged(rectangle);
        if (QGuiApplication::inputMethod() != nullptr && hasFocus())
        {
            QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
        }
    }
}

QRegion Neovim_Grid_Widget::damage_to_pixels(
    const QVector<QRect>& damage) const
{
    QRegion region;
    for (const QRect& cells : damage)
    {
        region += QRect(
            cells.left() * cell_width_,
            cells.top() * cell_height_,
            cells.width() * cell_width_,
            cells.height() * cell_height_);
    }
    return region;
}

QRect Neovim_Grid_Widget::cell_rectangle(
    int row,
    int column,
    int width_cells) const
{
    return QRect(
        column * cell_width_,
        row * cell_height_,
        width_cells * cell_width_,
        cell_height_);
}

bool Neovim_Grid_Widget::is_double_width_at(int row, int column) const
{
    return !model_.cell_at(row, column).text.isEmpty() &&
        column + 1 < model_.columns() &&
        model_.cell_at(row, column + 1).text.isEmpty();
}

QRect Neovim_Grid_Widget::cursor_cell_rectangle() const
{
    const int row = model_.cursor_row();
    const int column = model_.cursor_column();
    return cell_rectangle(
        row,
        column,
        is_double_width_at(row, column) ? 2 : 1);
}

QColor Neovim_Grid_Widget::resolved_foreground(
    const Grid_Highlight& highlight) const
{
    if (highlight.foreground.isValid())
    {
        return highlight.foreground;
    }
    const QColor default_foreground = model_.default_colors().foreground;
    return default_foreground.isValid()
        ? default_foreground
        : QColor(Qt::white);
}

QColor Neovim_Grid_Widget::resolved_background(
    const Grid_Highlight& highlight) const
{
    if (highlight.background.isValid())
    {
        return highlight.background;
    }
    const QColor default_background = model_.default_colors().background;
    return default_background.isValid()
        ? default_background
        : QColor(Qt::black);
}

void Neovim_Grid_Widget::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);

    for (const QRect& rectangle : event->region())
    {
        paint_cells(painter, rectangle);
    }

    /* Margins beyond the grid keep the default background. */
    const QRect grid_area(
        0,
        0,
        model_.columns() * cell_width_,
        model_.rows() * cell_height_);
    const QRegion margins =
        QRegion(rect()).subtracted(grid_area).intersected(event->region());
    const QColor margin_color =
        resolved_background(model_.default_colors());
    for (const QRect& margin : margins)
    {
        painter.fillRect(margin, margin_color);
    }

    paint_cursor(painter);
}

void Neovim_Grid_Widget::paint_cells(
    QPainter& painter,
    const QRect& pixel_rectangle)
{
    if (model_.columns() <= 0 || model_.rows() <= 0)
    {
        painter.fillRect(
            pixel_rectangle,
            resolved_background(model_.default_colors()));
        return;
    }

    const int first_row =
        qBound(0, pixel_rectangle.top() / cell_height_, model_.rows() - 1);
    const int last_row = qBound(
        0,
        (pixel_rectangle.bottom()) / cell_height_,
        model_.rows() - 1);
    int first_column = qBound(
        0,
        pixel_rectangle.left() / cell_width_,
        model_.columns() - 1);
    const int last_column = qBound(
        0,
        (pixel_rectangle.right()) / cell_width_,
        model_.columns() - 1);

    for (int row = first_row; row <= last_row; ++row)
    {
        int column = first_column;

        /* A damage rect starting on a right half repaints its left half. */
        if (column > 0 &&
            model_.cell_at(row, column).text.isEmpty() &&
            !model_.cell_at(row, column - 1).text.isEmpty())
        {
            --column;
        }

        while (column <= last_column)
        {
            const Grid_Cell& cell = model_.cell_at(row, column);
            if (cell.text.isEmpty())
            {
                /* The right half; its left neighbor painted it. */
                ++column;
                continue;
            }

            const bool double_width = is_double_width_at(row, column);
            const int cell_span = double_width ? 2 : 1;
            const QRect target = cell_rectangle(row, column, cell_span);
            const Grid_Highlight highlight =
                model_.highlight(cell.highlight_id);

            QColor foreground = resolved_foreground(highlight);
            QColor background = resolved_background(highlight);
            if (highlight.reverse)
            {
                const QColor swap = foreground;
                foreground = background;
                background = swap;
            }

            painter.fillRect(target, background);

            if (cell.text != QStringLiteral(" "))
            {
                QFont cell_font = font_;
                cell_font.setBold(highlight.bold);
                cell_font.setItalic(highlight.italic);
                cell_font.setStrikeOut(highlight.strikethrough);
                painter.setFont(cell_font);
                painter.setPen(foreground);
                painter.drawText(
                    QPoint(target.left(), target.top() + ascent_),
                    cell.text);
            }
            else if (highlight.strikethrough)
            {
                painter.setPen(foreground);
                painter.drawLine(
                    target.left(),
                    target.top() + cell_height_ / 2,
                    target.right(),
                    target.top() + cell_height_ / 2);
            }

            const QColor special = highlight.special.isValid()
                ? highlight.special
                : foreground;
            const int underline_y = target.top() + ascent_ + 1;
            QPen underline_pen(special);
            underline_pen.setWidthF(qMax(1.0, cell_height_ / 16.0));

            if (highlight.underline || highlight.underdouble)
            {
                painter.setPen(underline_pen);
                painter.drawLine(
                    target.left(), underline_y, target.right(), underline_y);
                if (highlight.underdouble)
                {
                    painter.drawLine(
                        target.left(),
                        underline_y + 2,
                        target.right(),
                        underline_y + 2);
                }
            }
            else if (highlight.underdotted || highlight.underdashed)
            {
                underline_pen.setStyle(
                    highlight.underdotted ? Qt::DotLine : Qt::DashLine);
                painter.setPen(underline_pen);
                painter.drawLine(
                    target.left(), underline_y, target.right(), underline_y);
            }
            else if (highlight.undercurl)
            {
                painter.setPen(underline_pen);
                QPainterPath wave;
                const qreal amplitude =
                    qMax(1.0, cell_height_ / 8.0) / 2.0;
                const qreal middle = underline_y;
                wave.moveTo(target.left(), middle);
                const qreal quarter = target.width() / 4.0;
                wave.cubicTo(
                    target.left() + quarter,
                    middle - amplitude * 2,
                    target.left() + quarter,
                    middle + amplitude * 2,
                    target.left() + quarter * 2,
                    middle);
                wave.cubicTo(
                    target.left() + quarter * 3,
                    middle - amplitude * 2,
                    target.left() + quarter * 3,
                    middle + amplitude * 2,
                    target.left() + quarter * 4,
                    middle);
                painter.drawPath(wave);
            }

            column += cell_span;
        }
    }
}

void Neovim_Grid_Widget::paint_cursor(QPainter& painter)
{
    if (!model_.cursor_visible() || !hasFocus())
    {
        return;
    }
    const int row = model_.cursor_row();
    const int column = model_.cursor_column();
    if (row < 0 || row >= model_.rows() ||
        column < 0 || column >= model_.columns())
    {
        return;
    }

    const Grid_Mode_Info mode = model_.current_mode();
    const Grid_Cell& cell = model_.cell_at(row, column);
    QRect target = cursor_cell_rectangle();

    QColor cursor_background;
    QColor cursor_foreground;
    if (mode.attribute_id != 0 && model_.has_highlight(mode.attribute_id))
    {
        const Grid_Highlight cursor_highlight =
            model_.highlight(mode.attribute_id);
        cursor_background = resolved_background(cursor_highlight);
        cursor_foreground = resolved_foreground(cursor_highlight);
        if (cursor_highlight.reverse)
        {
            const QColor swap = cursor_background;
            cursor_background = cursor_foreground;
            cursor_foreground = swap;
        }
    }
    else
    {
        /* attr_id 0 means reverse video of the cell under the cursor. */
        const Grid_Highlight cell_highlight =
            model_.highlight(cell.highlight_id);
        QColor cell_foreground = resolved_foreground(cell_highlight);
        QColor cell_background = resolved_background(cell_highlight);
        if (cell_highlight.reverse)
        {
            const QColor swap = cell_foreground;
            cell_foreground = cell_background;
            cell_background = swap;
        }
        cursor_background = cell_foreground;
        cursor_foreground = cell_background;
    }

    switch (mode.shape)
    {
    case Grid_Cursor_Shape::Vertical:
        target.setWidth(qMax(
            1,
            cell_width_ * qBound(1, mode.cell_percentage, 100) / 100));
        painter.fillRect(target, cursor_background);
        return;
    case Grid_Cursor_Shape::Horizontal:
    {
        const int bar_height = qMax(
            1,
            cell_height_ * qBound(1, mode.cell_percentage, 100) / 100);
        target.setTop(target.bottom() + 1 - bar_height);
        painter.fillRect(target, cursor_background);
        return;
    }
    case Grid_Cursor_Shape::Block:
        break;
    }

    painter.fillRect(target, cursor_background);
    if (!cell.text.isEmpty() && cell.text != QStringLiteral(" "))
    {
        const Grid_Highlight cell_highlight =
            model_.highlight(cell.highlight_id);
        QFont cell_font = font_;
        cell_font.setBold(cell_highlight.bold);
        cell_font.setItalic(cell_highlight.italic);
        painter.setFont(cell_font);
        painter.setPen(cursor_foreground);
        painter.drawText(
            QPoint(target.left(), target.top() + ascent_),
            cell.text);
    }
}

QSize Neovim_Grid_Widget::sizeHint() const
{
    const int columns = model_.columns() > 0 ? model_.columns() : 80;
    const int rows = model_.rows() > 0 ? model_.rows() : 24;
    return QSize(columns * cell_width_, rows * cell_height_);
}

void Neovim_Grid_Widget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    const int columns =
        qMax(Minimum_Grid_Columns, width() / cell_width_);
    const int rows = qMax(Minimum_Grid_Rows, height() / cell_height_);
    request_grid_size(columns, rows);
}

bool Neovim_Grid_Widget::event(QEvent* event)
{
    if (event->type() == QEvent::DevicePixelRatioChange ||
        event->type() == QEvent::FontChange)
    {
        compute_cell_metrics();
        const int columns =
            qMax(Minimum_Grid_Columns, width() / cell_width_);
        const int rows = qMax(Minimum_Grid_Rows, height() / cell_height_);
        request_grid_size(columns, rows);
        update();
    }
    return QWidget::event(event);
}

void Neovim_Grid_Widget::send_text_input(const QString& notation)
{
    if (client_ == nullptr || notation.isEmpty())
    {
        return;
    }
    client_->call(
        QByteArrayLiteral("nvim_input"),
        QVariantList() << notation.toUtf8());
}

bool Neovim_Grid_Widget::focusNextPrevChild(bool next)
{
    Q_UNUSED(next);
    return false;
}

void Neovim_Grid_Widget::keyPressEvent(QKeyEvent* event)
{
    const QString notation =
        input_notation(event->key(), event->modifiers(), event->text());
    if (notation.isEmpty())
    {
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
    send_text_input(notation);
}

QPoint Neovim_Grid_Widget::cell_from_position(
    const QPointF& position) const
{
    const int column = qBound(
        0,
        int(position.x()) / cell_width_,
        qMax(0, model_.columns() - 1));
    const int row = qBound(
        0,
        int(position.y()) / cell_height_,
        qMax(0, model_.rows() - 1));
    return QPoint(column, row);
}

void Neovim_Grid_Widget::send_mouse_input(
    const QByteArray& button,
    const QByteArray& action,
    const QByteArray& modifiers,
    const QPoint& global_cell)
{
    if (client_ == nullptr)
    {
        return;
    }
    /* Multigrid mouse input is grid-targeted with grid-local cells. */
    int local_row = 0;
    int local_column = 0;
    const qint64 grid = model_.grid_at(
        global_cell.y(),
        global_cell.x(),
        &local_row,
        &local_column);
    client_->call(
        QByteArrayLiteral("nvim_input_mouse"),
        QVariantList()
            << button
            << action
            << modifiers
            << grid
            << local_row
            << local_column);
}

void Neovim_Grid_Widget::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);
    if (!model_.mouse_enabled())
    {
        return;
    }
    const Mouse_Translation translation = mouse_translation(
        event->button(), event->type(), event->modifiers());
    if (!translation.valid)
    {
        return;
    }
    const QPoint cell = cell_from_position(event->position());
    last_drag_row_ = cell.y();
    last_drag_column_ = cell.x();
    send_mouse_input(
        translation.button,
        translation.action,
        translation.modifiers,
        cell);
    event->accept();
}

void Neovim_Grid_Widget::mouseDoubleClickEvent(QMouseEvent* event)
{
    mousePressEvent(event);
}

void Neovim_Grid_Widget::mouseReleaseEvent(QMouseEvent* event)
{
    if (!model_.mouse_enabled())
    {
        return;
    }
    const Mouse_Translation translation = mouse_translation(
        event->button(), event->type(), event->modifiers());
    if (!translation.valid)
    {
        return;
    }
    const QPoint cell = cell_from_position(event->position());
    send_mouse_input(
        translation.button,
        translation.action,
        translation.modifiers,
        cell);
    event->accept();
}

void Neovim_Grid_Widget::mouseMoveEvent(QMouseEvent* event)
{
    if (!model_.mouse_enabled())
    {
        return;
    }
    Qt::MouseButton held = Qt::NoButton;
    if (event->buttons() & Qt::LeftButton)
    {
        held = Qt::LeftButton;
    }
    else if (event->buttons() & Qt::RightButton)
    {
        held = Qt::RightButton;
    }
    else if (event->buttons() & Qt::MiddleButton)
    {
        held = Qt::MiddleButton;
    }
    if (held == Qt::NoButton)
    {
        return;
    }
    const QPoint cell = cell_from_position(event->position());
    if (cell.y() == last_drag_row_ && cell.x() == last_drag_column_)
    {
        return;
    }
    last_drag_row_ = cell.y();
    last_drag_column_ = cell.x();
    const Mouse_Translation translation =
        mouse_translation(held, QEvent::MouseMove, event->modifiers());
    if (!translation.valid)
    {
        return;
    }
    send_mouse_input(
        translation.button,
        translation.action,
        translation.modifiers,
        cell);
    event->accept();
}

void Neovim_Grid_Widget::wheelEvent(QWheelEvent* event)
{
    if (!model_.mouse_enabled())
    {
        return;
    }
    const Mouse_Translation translation =
        wheel_translation(event->angleDelta().y(), event->modifiers());
    if (!translation.valid)
    {
        return;
    }
    const QPoint cell = cell_from_position(event->position());
    send_mouse_input(
        translation.button,
        translation.action,
        translation.modifiers,
        cell);
    event->accept();
}

void Neovim_Grid_Widget::inputMethodEvent(QInputMethodEvent* event)
{
    /*
     * Commit strings are sent literally; preedit overlay rendering is
     * deferred (spec_2.md section 12).
     */
    const QString committed = event->commitString();
    if (!committed.isEmpty())
    {
        send_text_input(literal_input_escaped(committed));
    }
    event->accept();
}

QVariant Neovim_Grid_Widget::inputMethodQuery(
    Qt::InputMethodQuery query) const
{
    switch (query)
    {
    case Qt::ImEnabled:
        return true;
    case Qt::ImCursorRectangle:
        return cursor_cell_rectangle();
    default:
        return QWidget::inputMethodQuery(query);
    }
}

} // namespace Kalam::Editor
