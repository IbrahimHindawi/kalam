#include "neovim_grid_model.h"

namespace Kalam::Editor
{

namespace
{

const Grid_Cell Default_Cell;

QColor color_from_rgb(qlonglong value)
{
    if (value < 0)
    {
        return QColor();
    }
    return QColor(
        int((value >> 16) & 0xff),
        int((value >> 8) & 0xff),
        int(value & 0xff));
}

bool tuple_targets_other_grid(const QVariantList& arguments)
{
    return !arguments.isEmpty() && arguments.at(0).toLongLong() != 1;
}

Grid_Highlight highlight_from_attributes(const QVariantMap& attributes)
{
    Grid_Highlight highlight;
    highlight.foreground = color_from_rgb(
        attributes.value(QStringLiteral("foreground"), -1).toLongLong());
    highlight.background = color_from_rgb(
        attributes.value(QStringLiteral("background"), -1).toLongLong());
    highlight.special = color_from_rgb(
        attributes.value(QStringLiteral("special"), -1).toLongLong());
    highlight.reverse =
        attributes.value(QStringLiteral("reverse")).toBool();
    highlight.bold = attributes.value(QStringLiteral("bold")).toBool();
    highlight.italic = attributes.value(QStringLiteral("italic")).toBool();
    highlight.strikethrough =
        attributes.value(QStringLiteral("strikethrough")).toBool();
    highlight.underline =
        attributes.value(QStringLiteral("underline")).toBool();
    highlight.undercurl =
        attributes.value(QStringLiteral("undercurl")).toBool();
    highlight.underdouble =
        attributes.value(QStringLiteral("underdouble")).toBool();
    highlight.underdotted =
        attributes.value(QStringLiteral("underdotted")).toBool();
    highlight.underdashed =
        attributes.value(QStringLiteral("underdashed")).toBool();
    highlight.blend =
        int(attributes.value(QStringLiteral("blend"), 0).toLongLong());
    return highlight;
}

} // namespace

Neovim_Grid_Model::Neovim_Grid_Model() = default;

int Neovim_Grid_Model::columns() const
{
    return columns_;
}

int Neovim_Grid_Model::rows() const
{
    return rows_;
}

bool Neovim_Grid_Model::cell_in_grid(int row, int column) const
{
    return row >= 0 && row < rows_ && column >= 0 && column < columns_;
}

const Grid_Cell& Neovim_Grid_Model::cell_at(int row, int column) const
{
    if (!cell_in_grid(row, column))
    {
        return Default_Cell;
    }
    return cells_.at(row * columns_ + column);
}

Grid_Cell& Neovim_Grid_Model::cell_reference(int row, int column)
{
    return cells_[row * columns_ + column];
}

QString Neovim_Grid_Model::row_text(int row) const
{
    QString text;
    if (row < 0 || row >= rows_)
    {
        return text;
    }
    for (int column = 0; column < columns_; ++column)
    {
        text.append(cell_at(row, column).text);
    }
    return text;
}

int Neovim_Grid_Model::cursor_row() const
{
    return cursor_row_;
}

int Neovim_Grid_Model::cursor_column() const
{
    return cursor_column_;
}

bool Neovim_Grid_Model::cursor_visible() const
{
    return !busy_;
}

Grid_Mode_Info Neovim_Grid_Model::current_mode() const
{
    if (mode_index_ >= 0 && mode_index_ < modes_.size())
    {
        return modes_.at(mode_index_);
    }
    return Grid_Mode_Info();
}

Grid_Highlight Neovim_Grid_Model::highlight(qint64 highlight_id) const
{
    /* Id 0 and unknown ids resolve to the default colors. */
    return highlights_.value(highlight_id, default_colors_);
}

bool Neovim_Grid_Model::has_highlight(qint64 highlight_id) const
{
    return highlights_.contains(highlight_id);
}

qint64 Neovim_Grid_Model::highlight_id_for_group(
    const QString& group_name) const
{
    return highlight_groups_.value(group_name, -1);
}

Grid_Highlight Neovim_Grid_Model::default_colors() const
{
    return default_colors_;
}

QString Neovim_Grid_Model::title() const
{
    return title_;
}

QString Neovim_Grid_Model::guifont() const
{
    return guifont_;
}

bool Neovim_Grid_Model::mouse_enabled() const
{
    return mouse_enabled_;
}

bool Neovim_Grid_Model::has_published_damage() const
{
    return !published_damage_.isEmpty();
}

QVector<QRect> Neovim_Grid_Model::take_published_damage()
{
    QVector<QRect> damage = published_damage_;
    published_damage_.clear();
    return damage;
}

qint64 Neovim_Grid_Model::ignored_event_count() const
{
    return ignored_event_count_;
}

void Neovim_Grid_Model::damage(int row, int column, int width, int height)
{
    pending_damage_.append(QRect(column, row, width, height));
}

void Neovim_Grid_Model::damage_all()
{
    pending_damage_.clear();
    pending_damage_.append(QRect(0, 0, columns_, rows_));
}

Grid_Update_Summary Neovim_Grid_Model::apply_redraw(const QVariantList& events)
{
    Grid_Update_Summary summary;

    for (const QVariant& event_entry : events)
    {
        const QVariantList event = event_entry.toList();
        if (event.isEmpty())
        {
            ++ignored_event_count_;
            continue;
        }
        const QByteArray name = event.at(0).toByteArray();

        for (int tuple_index = 1; tuple_index < event.size(); ++tuple_index)
        {
            const QVariantList arguments =
                event.at(tuple_index).toList();

            if (name == QByteArrayLiteral("grid_line"))
            {
                if (tuple_targets_other_grid(arguments))
                {
                    ++ignored_event_count_;
                    continue;
                }
                apply_grid_line(arguments);
            }
            else if (name == QByteArrayLiteral("grid_cursor_goto"))
            {
                if (tuple_targets_other_grid(arguments))
                {
                    ++ignored_event_count_;
                    continue;
                }
                apply_grid_cursor_goto(arguments, summary);
            }
            else if (name == QByteArrayLiteral("grid_scroll"))
            {
                if (tuple_targets_other_grid(arguments))
                {
                    ++ignored_event_count_;
                    continue;
                }
                apply_grid_scroll(arguments);
            }
            else if (name == QByteArrayLiteral("grid_resize"))
            {
                if (tuple_targets_other_grid(arguments))
                {
                    ++ignored_event_count_;
                    continue;
                }
                apply_grid_resize(arguments, summary);
            }
            else if (name == QByteArrayLiteral("grid_clear"))
            {
                if (tuple_targets_other_grid(arguments))
                {
                    ++ignored_event_count_;
                    continue;
                }
                apply_grid_clear(arguments);
            }
            else if (name == QByteArrayLiteral("grid_destroy"))
            {
                /* Grid 1 is never destroyed in revision 1. */
                ++ignored_event_count_;
            }
            else if (name == QByteArrayLiteral("default_colors_set"))
            {
                apply_default_colors_set(arguments, summary);
            }
            else if (name == QByteArrayLiteral("hl_attr_define"))
            {
                apply_hl_attr_define(arguments, summary);
            }
            else if (name == QByteArrayLiteral("hl_group_set"))
            {
                apply_hl_group_set(arguments, summary);
            }
            else if (name == QByteArrayLiteral("mode_info_set"))
            {
                apply_mode_info_set(arguments, summary);
            }
            else if (name == QByteArrayLiteral("mode_change"))
            {
                apply_mode_change(arguments, summary);
            }
            else if (name == QByteArrayLiteral("option_set"))
            {
                apply_option_set(arguments, summary);
            }
            else if (name == QByteArrayLiteral("set_title"))
            {
                apply_set_title(arguments, summary);
            }
            else if (name == QByteArrayLiteral("set_icon"))
            {
                /* Accepted and ignored. */
            }
            else if (name == QByteArrayLiteral("busy_start"))
            {
                busy_ = true;
                summary.busy_changed = true;
                damage(cursor_row_, cursor_column_, 2, 1);
            }
            else if (name == QByteArrayLiteral("busy_stop"))
            {
                busy_ = false;
                summary.busy_changed = true;
                damage(cursor_row_, cursor_column_, 2, 1);
            }
            else if (name == QByteArrayLiteral("mouse_on"))
            {
                mouse_enabled_ = true;
            }
            else if (name == QByteArrayLiteral("mouse_off"))
            {
                mouse_enabled_ = false;
            }
            else if (name == QByteArrayLiteral("bell"))
            {
                ++summary.bell_count;
            }
            else if (name == QByteArrayLiteral("visual_bell"))
            {
                ++summary.visual_bell_count;
            }
            else if (name == QByteArrayLiteral("flush"))
            {
                published_damage_ += pending_damage_;
                pending_damage_.clear();
                summary.flushed = true;
            }
            else
            {
                /*
                 * The protocol adds events at existing api levels; unknown
                 * names are never fatal.
                 */
                ++ignored_event_count_;
            }
        }

        /* flush arrives with no argument tuples. */
        if (event.size() == 1 && name == QByteArrayLiteral("flush"))
        {
            published_damage_ += pending_damage_;
            pending_damage_.clear();
            summary.flushed = true;
        }
    }

    return summary;
}

void Neovim_Grid_Model::apply_grid_resize(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.size() < 3)
    {
        ++ignored_event_count_;
        return;
    }
    const int new_columns = int(arguments.at(1).toLongLong());
    const int new_rows = int(arguments.at(2).toLongLong());
    if (new_columns <= 0 || new_rows <= 0)
    {
        ++ignored_event_count_;
        return;
    }

    QVector<Grid_Cell> new_cells(new_columns * new_rows);
    const int copy_rows = qMin(rows_, new_rows);
    const int copy_columns = qMin(columns_, new_columns);
    for (int row = 0; row < copy_rows; ++row)
    {
        for (int column = 0; column < copy_columns; ++column)
        {
            new_cells[row * new_columns + column] =
                cells_.at(row * columns_ + column);
        }
    }
    cells_ = new_cells;
    columns_ = new_columns;
    rows_ = new_rows;
    cursor_row_ = qMin(cursor_row_, rows_ - 1);
    cursor_column_ = qMin(cursor_column_, columns_ - 1);
    summary.grid_resized = true;
    damage_all();
}

void Neovim_Grid_Model::apply_grid_clear(const QVariantList& arguments)
{
    Q_UNUSED(arguments);
    cells_.fill(Grid_Cell());
    damage_all();
}

void Neovim_Grid_Model::apply_grid_line(const QVariantList& arguments)
{
    if (arguments.size() < 4)
    {
        ++ignored_event_count_;
        return;
    }
    const int row = int(arguments.at(1).toLongLong());
    const int start_column = int(arguments.at(2).toLongLong());
    const QVariantList cells = arguments.at(3).toList();
    /* The optional trailing wrap flag is accepted and ignored. */

    if (row < 0 || row >= rows_)
    {
        ++ignored_event_count_;
        return;
    }

    /*
     * A missing hl_id inherits the previous cell's within this event
     * tuple; the first cell always carries one.
     */
    qint64 current_highlight = 0;
    int column = start_column;
    for (const QVariant& cell_entry : cells)
    {
        const QVariantList cell = cell_entry.toList();
        if (cell.isEmpty())
        {
            continue;
        }
        const QString text =
            QString::fromUtf8(cell.at(0).toByteArray());
        if (cell.size() > 1)
        {
            current_highlight = cell.at(1).toLongLong();
        }
        qint64 repeat = 1;
        if (cell.size() > 2)
        {
            repeat = cell.at(2).toLongLong();
        }
        for (qint64 index = 0; index < repeat; ++index)
        {
            if (!cell_in_grid(row, column))
            {
                break;
            }
            Grid_Cell& target = cell_reference(row, column);
            target.text = text;
            target.highlight_id = current_highlight;
            ++column;
        }
    }

    if (column > start_column)
    {
        damage(row, start_column, column - start_column, 1);
    }
}

void Neovim_Grid_Model::apply_grid_scroll(const QVariantList& arguments)
{
    if (arguments.size() < 6)
    {
        ++ignored_event_count_;
        return;
    }

    /* top and left are inclusive; bot and right are exclusive. */
    const int top = int(arguments.at(1).toLongLong());
    const int bottom = int(arguments.at(2).toLongLong());
    const int left = int(arguments.at(3).toLongLong());
    const int right = int(arguments.at(4).toLongLong());
    const int scroll_rows = int(arguments.at(5).toLongLong());

    const int clamped_top = qBound(0, top, rows_);
    const int clamped_bottom = qBound(0, bottom, rows_);
    const int clamped_left = qBound(0, left, columns_);
    const int clamped_right = qBound(0, right, columns_);

    if (scroll_rows > 0)
    {
        for (int row = clamped_top + scroll_rows;
             row < clamped_bottom;
             ++row)
        {
            for (int column = clamped_left; column < clamped_right; ++column)
            {
                cell_reference(row - scroll_rows, column) =
                    cell_at(row, column);
            }
        }
    }
    else if (scroll_rows < 0)
    {
        for (int row = clamped_bottom + scroll_rows - 1;
             row >= clamped_top;
             --row)
        {
            for (int column = clamped_left; column < clamped_right; ++column)
            {
                cell_reference(row - scroll_rows, column) =
                    cell_at(row, column);
            }
        }
    }

    /*
     * Revealed rows arrive as subsequent grid_line events and are not
     * synthesized here.
     */
    damage(
        clamped_top,
        clamped_left,
        clamped_right - clamped_left,
        clamped_bottom - clamped_top);
}

void Neovim_Grid_Model::apply_grid_cursor_goto(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.size() < 3)
    {
        ++ignored_event_count_;
        return;
    }
    /* Two-wide damage keeps a cursor on a double-width cell repainting. */
    damage(cursor_row_, cursor_column_, 2, 1);
    cursor_row_ = int(arguments.at(1).toLongLong());
    cursor_column_ = int(arguments.at(2).toLongLong());
    damage(cursor_row_, cursor_column_, 2, 1);
    summary.cursor_moved = true;
}

void Neovim_Grid_Model::apply_default_colors_set(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.size() < 3)
    {
        ++ignored_event_count_;
        return;
    }
    default_colors_.foreground =
        color_from_rgb(arguments.at(0).toLongLong());
    default_colors_.background =
        color_from_rgb(arguments.at(1).toLongLong());
    default_colors_.special = color_from_rgb(arguments.at(2).toLongLong());
    summary.default_colors_changed = true;
    damage_all();
}

void Neovim_Grid_Model::apply_hl_attr_define(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.size() < 2)
    {
        ++ignored_event_count_;
        return;
    }
    const qint64 highlight_id = arguments.at(0).toLongLong();
    const QVariantMap rgb_attributes = arguments.at(1).toMap();
    highlights_.insert(
        highlight_id,
        highlight_from_attributes(rgb_attributes));
    summary.highlights_changed = true;
}

void Neovim_Grid_Model::apply_hl_group_set(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.size() < 2)
    {
        ++ignored_event_count_;
        return;
    }
    highlight_groups_.insert(
        QString::fromUtf8(arguments.at(0).toByteArray()),
        arguments.at(1).toLongLong());
    summary.highlights_changed = true;
}

void Neovim_Grid_Model::apply_mode_info_set(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.size() < 2)
    {
        ++ignored_event_count_;
        return;
    }
    modes_.clear();
    const QVariantList mode_entries = arguments.at(1).toList();
    modes_.reserve(mode_entries.size());
    for (const QVariant& entry : mode_entries)
    {
        const QVariantMap properties = entry.toMap();
        Grid_Mode_Info mode;
        mode.name = QString::fromUtf8(
            properties.value(QStringLiteral("name")).toByteArray());
        const QByteArray shape =
            properties.value(QStringLiteral("cursor_shape")).toByteArray();
        if (shape == QByteArrayLiteral("horizontal"))
        {
            mode.shape = Grid_Cursor_Shape::Horizontal;
        }
        else if (shape == QByteArrayLiteral("vertical"))
        {
            mode.shape = Grid_Cursor_Shape::Vertical;
        }
        else
        {
            mode.shape = Grid_Cursor_Shape::Block;
        }
        mode.cell_percentage = int(
            properties.value(QStringLiteral("cell_percentage"), 100)
                .toLongLong());
        mode.attribute_id =
            properties.value(QStringLiteral("attr_id"), 0).toLongLong();
        modes_.append(mode);
    }
    summary.mode_changed = true;
}

void Neovim_Grid_Model::apply_mode_change(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.size() < 2)
    {
        ++ignored_event_count_;
        return;
    }
    mode_index_ = int(arguments.at(1).toLongLong());
    summary.mode_changed = true;
    damage(cursor_row_, cursor_column_, 2, 1);
}

void Neovim_Grid_Model::apply_option_set(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.size() < 2)
    {
        ++ignored_event_count_;
        return;
    }
    const QByteArray option_name = arguments.at(0).toByteArray();
    if (option_name == QByteArrayLiteral("guifont"))
    {
        const QString value =
            QString::fromUtf8(arguments.at(1).toByteArray());
        if (value != guifont_)
        {
            guifont_ = value;
            summary.guifont_changed = true;
        }
    }
}

void Neovim_Grid_Model::apply_set_title(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.isEmpty())
    {
        ++ignored_event_count_;
        return;
    }
    title_ = QString::fromUtf8(arguments.at(0).toByteArray());
    summary.title_changed = true;
}

} // namespace Kalam::Editor
