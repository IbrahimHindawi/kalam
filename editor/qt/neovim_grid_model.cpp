#include "neovim_grid_model.h"

#include "neovim_msgpack.h"

#include <QtMath>

namespace Kalam::Editor
{

namespace
{

const Grid_Cell Default_Cell;
const qint64 Layout_Grid = 1;

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

qint64 window_handle_from_argument(const QVariant& argument)
{
    if (argument.userType() == qMetaTypeId<Msgpack_Ext>())
    {
        bool ok = false;
        const qint64 handle =
            msgpack_ext_handle_value(argument.value<Msgpack_Ext>(), &ok);
        return ok ? handle : -1;
    }
    if (argument.userType() == QMetaType::LongLong ||
        argument.userType() == QMetaType::ULongLong)
    {
        return argument.toLongLong();
    }
    return -1;
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

/* ------------------------------------------------------ grid storage */

Neovim_Grid_Model::Grid_Data* Neovim_Grid_Model::grid_data(qint64 grid)
{
    const auto found = grids_.find(grid);
    return found == grids_.end() ? nullptr : &found.value();
}

const Neovim_Grid_Model::Grid_Data*
Neovim_Grid_Model::grid_data(qint64 grid) const
{
    const auto found = grids_.constFind(grid);
    return found == grids_.constEnd() ? nullptr : &found.value();
}

int Neovim_Grid_Model::columns() const
{
    const Grid_Data* layout = grid_data(Layout_Grid);
    return layout == nullptr ? 0 : layout->columns;
}

int Neovim_Grid_Model::rows() const
{
    const Grid_Data* layout = grid_data(Layout_Grid);
    return layout == nullptr ? 0 : layout->rows;
}

QPoint Neovim_Grid_Model::grid_origin(qint64 grid) const
{
    if (grid == Layout_Grid)
    {
        return QPoint(0, 0);
    }
    const auto placement = placements_.constFind(grid);
    if (placement == placements_.constEnd() || !placement->placed)
    {
        return QPoint(0, 0);
    }
    return QPoint(placement->column, placement->row);
}

QRect Neovim_Grid_Model::grid_screen_rect(qint64 grid) const
{
    const Grid_Data* data = grid_data(grid);
    if (data == nullptr)
    {
        return QRect();
    }
    if (grid == Layout_Grid)
    {
        return QRect(0, 0, data->columns, data->rows);
    }
    const auto placement = placements_.constFind(grid);
    if (placement == placements_.constEnd() || !placement->placed)
    {
        return QRect();
    }
    return QRect(
        placement->column,
        placement->row,
        data->columns,
        data->rows);
}

QRect Neovim_Grid_Model::window_screen_rect(qint64 window_handle) const
{
    for (auto iterator = placements_.constBegin();
         iterator != placements_.constEnd();
         ++iterator)
    {
        if (iterator->window_handle == window_handle && iterator->placed)
        {
            return grid_screen_rect(iterator.key());
        }
    }
    return QRect();
}

bool Neovim_Grid_Model::window_is_floating(qint64 window_handle) const
{
    for (auto iterator = placements_.constBegin();
         iterator != placements_.constEnd();
         ++iterator)
    {
        if (iterator->window_handle == window_handle && iterator->placed)
        {
            return iterator->floating;
        }
    }
    return false;
}

void Neovim_Grid_Model::invalidate_composition()
{
    composition_dirty_ = true;
}

const QVector<qint64>& Neovim_Grid_Model::composition_top_down() const
{
    if (!composition_dirty_)
    {
        return composition_cache_;
    }
    composition_cache_.clear();

    /* Message grid on top, floats by descending z then recency, then
       ordinary windows by recency. Grid 1 is the implicit bottom. */
    QVector<qint64> windows;
    QVector<qint64> floats;
    QVector<qint64> messages;
    for (auto iterator = placements_.constBegin();
         iterator != placements_.constEnd();
         ++iterator)
    {
        if (!iterator->placed || !grids_.contains(iterator.key()))
        {
            continue;
        }
        if (iterator->is_message)
        {
            messages.append(iterator.key());
        }
        else if (iterator->floating)
        {
            floats.append(iterator.key());
        }
        else
        {
            windows.append(iterator.key());
        }
    }
    const auto by_recency = [this](qint64 a, qint64 b)
    {
        return placements_.value(a).order > placements_.value(b).order;
    };
    const auto by_z_then_recency = [this](qint64 a, qint64 b)
    {
        const Grid_Placement pa = placements_.value(a);
        const Grid_Placement pb = placements_.value(b);
        if (pa.z_index != pb.z_index)
        {
            return pa.z_index > pb.z_index;
        }
        return pa.order > pb.order;
    };
    {
        /* Insertion sorts; the lists are a handful of entries. */
        for (int index = 1; index < windows.size(); ++index)
        {
            for (int slot = index;
                 slot > 0 && by_recency(windows[slot], windows[slot - 1]);
                 --slot)
            {
                qSwap(windows[slot], windows[slot - 1]);
            }
        }
        for (int index = 1; index < floats.size(); ++index)
        {
            for (int slot = index;
                 slot > 0 && by_z_then_recency(floats[slot], floats[slot - 1]);
                 --slot)
            {
                qSwap(floats[slot], floats[slot - 1]);
            }
        }
    }

    composition_cache_ += messages;
    composition_cache_ += floats;
    composition_cache_ += windows;
    composition_dirty_ = false;
    return composition_cache_;
}

qint64 Neovim_Grid_Model::grid_at(
    int row,
    int column,
    int* local_row,
    int* local_column) const
{
    for (qint64 grid : composition_top_down())
    {
        const QRect rect = grid_screen_rect(grid);
        if (rect.isValid() && rect.contains(column, row))
        {
            if (local_row != nullptr)
            {
                *local_row = row - rect.top();
            }
            if (local_column != nullptr)
            {
                *local_column = column - rect.left();
            }
            return grid;
        }
    }
    if (local_row != nullptr)
    {
        *local_row = row;
    }
    if (local_column != nullptr)
    {
        *local_column = column;
    }
    return Layout_Grid;
}

const Grid_Cell& Neovim_Grid_Model::cell_at(int row, int column) const
{
    if (row < 0 || column < 0 || row >= rows() || column >= columns())
    {
        return Default_Cell;
    }
    int local_row = 0;
    int local_column = 0;
    const qint64 grid = grid_at(row, column, &local_row, &local_column);
    const Grid_Data* data = grid_data(grid);
    if (data == nullptr ||
        local_row >= data->rows ||
        local_column >= data->columns)
    {
        return Default_Cell;
    }
    return data->cells.at(local_row * data->columns + local_column);
}

QString Neovim_Grid_Model::row_text(int row) const
{
    QString text;
    if (row < 0 || row >= rows())
    {
        return text;
    }
    for (int column = 0; column < columns(); ++column)
    {
        text.append(cell_at(row, column).text);
    }
    return text;
}

/* ----------------------------------------------------------- cursor */

int Neovim_Grid_Model::cursor_row() const
{
    return grid_origin(cursor_grid_).y() + cursor_local_row_;
}

int Neovim_Grid_Model::cursor_column() const
{
    return grid_origin(cursor_grid_).x() + cursor_local_column_;
}

qint64 Neovim_Grid_Model::cursor_grid() const
{
    return cursor_grid_;
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

/* ------------------------------------------------------- highlights */

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

/* ----------------------------------------------------------- damage */

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

void Neovim_Grid_Model::damage_grid(
    qint64 grid,
    int row,
    int column,
    int width,
    int height)
{
    Grid_Data* data = grid_data(grid);
    if (data != nullptr)
    {
        data->pending_damage.append(QRect(column, row, width, height));
    }
}

void Neovim_Grid_Model::damage_global_rect(const QRect& cells)
{
    if (cells.isValid() && !cells.isEmpty())
    {
        global_pending_damage_.append(cells);
    }
}

void Neovim_Grid_Model::damage_cursor_cells()
{
    /* Two-wide damage keeps a cursor on a double-width cell repainting. */
    damage_global_rect(QRect(cursor_column(), cursor_row(), 2, 1));
}

void Neovim_Grid_Model::publish_flush(Grid_Update_Summary& summary)
{
    for (auto iterator = grids_.begin(); iterator != grids_.end(); ++iterator)
    {
        Grid_Data& data = iterator.value();
        if (data.pending_damage.isEmpty())
        {
            continue;
        }
        QRect screen_rect = grid_screen_rect(iterator.key());
        if (screen_rect.isValid())
        {
            for (const QRect& local : data.pending_damage)
            {
                const QRect translated = local
                    .translated(screen_rect.left(), screen_rect.top())
                    .intersected(screen_rect);
                if (!translated.isEmpty())
                {
                    published_damage_.append(translated);
                }
            }
        }
        data.pending_damage.clear();
    }
    published_damage_ += global_pending_damage_;
    global_pending_damage_.clear();
    summary.flushed = true;
}

/* ------------------------------------------------------------ apply */

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

        /* flush arrives with no argument tuples in some servers. */
        if (event.size() == 1)
        {
            if (name == QByteArrayLiteral("flush"))
            {
                publish_flush(summary);
            }
            else
            {
                ++ignored_event_count_;
            }
            continue;
        }

        for (int tuple_index = 1; tuple_index < event.size(); ++tuple_index)
        {
            const QVariantList arguments =
                event.at(tuple_index).toList();

            if (name == QByteArrayLiteral("grid_line"))
            {
                apply_grid_line(arguments);
            }
            else if (name == QByteArrayLiteral("grid_cursor_goto"))
            {
                apply_grid_cursor_goto(arguments, summary);
            }
            else if (name == QByteArrayLiteral("grid_scroll"))
            {
                apply_grid_scroll(arguments);
            }
            else if (name == QByteArrayLiteral("grid_resize"))
            {
                apply_grid_resize(arguments, summary);
            }
            else if (name == QByteArrayLiteral("grid_clear"))
            {
                apply_grid_clear(arguments);
            }
            else if (name == QByteArrayLiteral("grid_destroy"))
            {
                apply_grid_destroy(arguments, summary);
            }
            else if (name == QByteArrayLiteral("win_pos"))
            {
                apply_win_pos(arguments, summary);
            }
            else if (name == QByteArrayLiteral("win_float_pos"))
            {
                apply_win_float_pos(arguments, summary);
            }
            else if (name == QByteArrayLiteral("win_hide"))
            {
                apply_win_hide(arguments, summary);
            }
            else if (name == QByteArrayLiteral("win_close"))
            {
                apply_win_close(arguments, summary);
            }
            else if (name == QByteArrayLiteral("msg_set_pos"))
            {
                apply_msg_set_pos(arguments, summary);
            }
            else if (name == QByteArrayLiteral("win_viewport"))
            {
                /* Scroll telemetry; accepted and ignored. */
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
                damage_cursor_cells();
            }
            else if (name == QByteArrayLiteral("busy_stop"))
            {
                busy_ = false;
                summary.busy_changed = true;
                damage_cursor_cells();
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
                publish_flush(summary);
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
    const qint64 grid = arguments.at(0).toLongLong();
    const int new_columns = int(arguments.at(1).toLongLong());
    const int new_rows = int(arguments.at(2).toLongLong());
    if (grid <= 0 || new_columns <= 0 || new_rows <= 0)
    {
        ++ignored_event_count_;
        return;
    }

    Grid_Data& data = grids_[grid];
    QVector<Grid_Cell> new_cells(new_columns * new_rows);
    const int copy_rows = qMin(data.rows, new_rows);
    const int copy_columns = qMin(data.columns, new_columns);
    for (int row = 0; row < copy_rows; ++row)
    {
        for (int column = 0; column < copy_columns; ++column)
        {
            new_cells[row * new_columns + column] =
                data.cells.at(row * data.columns + column);
        }
    }
    data.cells = new_cells;
    data.columns = new_columns;
    data.rows = new_rows;
    data.pending_damage.clear();
    data.pending_damage.append(QRect(0, 0, new_columns, new_rows));

    if (grid == Layout_Grid)
    {
        summary.grid_resized = true;
        cursor_local_row_ = qMin(cursor_local_row_, new_rows - 1);
        cursor_local_column_ = qMin(cursor_local_column_, new_columns - 1);
    }
    invalidate_composition();
    summary.window_layout_changed = true;
}

void Neovim_Grid_Model::apply_grid_clear(const QVariantList& arguments)
{
    if (arguments.isEmpty())
    {
        ++ignored_event_count_;
        return;
    }
    Grid_Data* data = grid_data(arguments.at(0).toLongLong());
    if (data == nullptr)
    {
        ++ignored_event_count_;
        return;
    }
    data->cells.fill(Grid_Cell());
    data->pending_damage.append(QRect(0, 0, data->columns, data->rows));
}

void Neovim_Grid_Model::apply_grid_line(const QVariantList& arguments)
{
    if (arguments.size() < 4)
    {
        ++ignored_event_count_;
        return;
    }
    Grid_Data* data = grid_data(arguments.at(0).toLongLong());
    if (data == nullptr)
    {
        ++ignored_event_count_;
        return;
    }
    const int row = int(arguments.at(1).toLongLong());
    const int start_column = int(arguments.at(2).toLongLong());
    const QVariantList cells = arguments.at(3).toList();
    /* The optional trailing wrap flag is accepted and ignored. */

    if (row < 0 || row >= data->rows)
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
            if (column < 0 || column >= data->columns)
            {
                break;
            }
            Grid_Cell& target = data->cells[row * data->columns + column];
            target.text = text;
            target.highlight_id = current_highlight;
            ++column;
        }
    }

    if (column > start_column)
    {
        data->pending_damage.append(
            QRect(start_column, row, column - start_column, 1));
    }
}

void Neovim_Grid_Model::apply_grid_scroll(const QVariantList& arguments)
{
    if (arguments.size() < 6)
    {
        ++ignored_event_count_;
        return;
    }
    Grid_Data* data = grid_data(arguments.at(0).toLongLong());
    if (data == nullptr)
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

    const int clamped_top = qBound(0, top, data->rows);
    const int clamped_bottom = qBound(0, bottom, data->rows);
    const int clamped_left = qBound(0, left, data->columns);
    const int clamped_right = qBound(0, right, data->columns);

    const auto cell_reference = [data](int row, int column) -> Grid_Cell&
    {
        return data->cells[row * data->columns + column];
    };

    if (scroll_rows > 0)
    {
        for (int row = clamped_top + scroll_rows;
             row < clamped_bottom;
             ++row)
        {
            for (int column = clamped_left; column < clamped_right; ++column)
            {
                cell_reference(row - scroll_rows, column) =
                    cell_reference(row, column);
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
                    cell_reference(row, column);
            }
        }
    }

    /*
     * Revealed rows arrive as subsequent grid_line events and are not
     * synthesized here.
     */
    data->pending_damage.append(QRect(
        clamped_left,
        clamped_top,
        clamped_right - clamped_left,
        clamped_bottom - clamped_top));
}

void Neovim_Grid_Model::apply_grid_destroy(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.isEmpty())
    {
        ++ignored_event_count_;
        return;
    }
    const qint64 grid = arguments.at(0).toLongLong();
    if (grid == Layout_Grid || !grids_.contains(grid))
    {
        ++ignored_event_count_;
        return;
    }
    damage_global_rect(grid_screen_rect(grid));
    grids_.remove(grid);
    placements_.remove(grid);
    invalidate_composition();
    summary.window_layout_changed = true;
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
    damage_cursor_cells();
    cursor_grid_ = arguments.at(0).toLongLong();
    cursor_local_row_ = int(arguments.at(1).toLongLong());
    cursor_local_column_ = int(arguments.at(2).toLongLong());
    damage_cursor_cells();
    summary.cursor_moved = true;
}

void Neovim_Grid_Model::apply_win_pos(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.size() < 6)
    {
        ++ignored_event_count_;
        return;
    }
    const qint64 grid = arguments.at(0).toLongLong();
    if (grid <= Layout_Grid)
    {
        ++ignored_event_count_;
        return;
    }
    damage_global_rect(grid_screen_rect(grid));

    Grid_Placement& placement = placements_[grid];
    placement.window_handle = window_handle_from_argument(arguments.at(1));
    placement.row = int(arguments.at(2).toLongLong());
    placement.column = int(arguments.at(3).toLongLong());
    placement.placed = true;
    placement.floating = false;
    placement.is_message = false;
    placement.z_index = 0;
    placement.order = ++placement_counter_;
    /* width/height (arguments 4 and 5) follow via grid_resize. */

    invalidate_composition();
    damage_global_rect(grid_screen_rect(grid));
    summary.window_layout_changed = true;
}

void Neovim_Grid_Model::apply_win_float_pos(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.size() < 6)
    {
        ++ignored_event_count_;
        return;
    }
    const qint64 grid = arguments.at(0).toLongLong();
    const Grid_Data* data = grid_data(grid);
    if (grid <= Layout_Grid || data == nullptr)
    {
        ++ignored_event_count_;
        return;
    }
    damage_global_rect(grid_screen_rect(grid));

    const QByteArray anchor = arguments.at(2).toByteArray();
    const qint64 anchor_grid = arguments.at(3).toLongLong();
    const double anchor_row = arguments.at(4).toDouble();
    const double anchor_column = arguments.at(5).toDouble();
    const qint64 z_index = arguments.size() > 7
        ? arguments.at(7).toLongLong()
        : 50;

    const QPoint base = grid_origin(anchor_grid);
    int row = base.y() + int(qFloor(anchor_row));
    int column = base.x() + int(qFloor(anchor_column));
    if (anchor.startsWith('S'))
    {
        row -= data->rows;
    }
    if (anchor.endsWith('E'))
    {
        column -= data->columns;
    }

    Grid_Placement& placement = placements_[grid];
    placement.window_handle = window_handle_from_argument(arguments.at(1));
    placement.row = row;
    placement.column = column;
    placement.placed = true;
    placement.floating = true;
    placement.is_message = false;
    placement.z_index = z_index;
    placement.order = ++placement_counter_;

    invalidate_composition();
    damage_global_rect(grid_screen_rect(grid));
    summary.window_layout_changed = true;
}

void Neovim_Grid_Model::apply_win_hide(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.isEmpty())
    {
        ++ignored_event_count_;
        return;
    }
    const qint64 grid = arguments.at(0).toLongLong();
    const auto placement = placements_.find(grid);
    if (placement == placements_.end())
    {
        ++ignored_event_count_;
        return;
    }
    damage_global_rect(grid_screen_rect(grid));
    placement->placed = false;
    invalidate_composition();
    summary.window_layout_changed = true;
}

void Neovim_Grid_Model::apply_win_close(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.isEmpty())
    {
        ++ignored_event_count_;
        return;
    }
    const qint64 grid = arguments.at(0).toLongLong();
    if (!placements_.contains(grid))
    {
        ++ignored_event_count_;
        return;
    }
    damage_global_rect(grid_screen_rect(grid));
    placements_.remove(grid);
    invalidate_composition();
    summary.window_layout_changed = true;
}

void Neovim_Grid_Model::apply_msg_set_pos(
    const QVariantList& arguments,
    Grid_Update_Summary& summary)
{
    if (arguments.size() < 2)
    {
        ++ignored_event_count_;
        return;
    }
    const qint64 grid = arguments.at(0).toLongLong();
    if (grid <= Layout_Grid || !grids_.contains(grid))
    {
        ++ignored_event_count_;
        return;
    }
    damage_global_rect(grid_screen_rect(grid));

    Grid_Placement& placement = placements_[grid];
    placement.row = int(arguments.at(1).toLongLong());
    placement.column = 0;
    placement.placed = true;
    placement.floating = false;
    placement.is_message = true;
    placement.z_index = 200;
    placement.order = ++placement_counter_;

    invalidate_composition();
    damage_global_rect(grid_screen_rect(grid));
    summary.window_layout_changed = true;
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
    damage_global_rect(QRect(0, 0, columns(), rows()));
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
    damage_cursor_cells();
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
