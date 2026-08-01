#ifndef KALAM_EDITOR_QT_NEOVIM_GRID_MODEL_H
#define KALAM_EDITOR_QT_NEOVIM_GRID_MODEL_H

#include <QColor>
#include <QHash>
#include <QRect>
#include <QString>
#include <QVariantList>
#include <QVector>

namespace Kalam::Editor
{

/*
 * ext_linegrid / ext_multigrid state machine (spec_2.md section 5,
 * docs/buffer_hosted_viewport.md section 2): redraw events in, composed
 * cell state and global damage rectangles out. Holds no widget and paints
 * nothing, so every protocol rule is testable headlessly.
 *
 * Grid 1 is the global layout grid and defines the screen size. Without
 * ext_multigrid it is the only grid and every published coordinate is
 * trivially global. With ext_multigrid, window grids are placed on grid 1
 * by win_pos / win_float_pos / msg_set_pos, and the composed accessors
 * (cell_at, row_text, cursor_row/column, take_published_damage) always
 * speak global screen cells.
 */

struct Grid_Cell
{
    /*
     * One grapheme as delivered by Neovim. An empty string marks the right
     * half of a double-width cell; the protocol sends it that way, so no
     * width tables are consulted anywhere.
     */
    QString text = QStringLiteral(" ");
    qint64 highlight_id = 0;
};

struct Grid_Highlight
{
    QColor foreground;
    QColor background;
    QColor special;
    bool reverse = false;
    bool bold = false;
    bool italic = false;
    bool strikethrough = false;
    bool underline = false;
    bool undercurl = false;
    bool underdouble = false;
    bool underdotted = false;
    bool underdashed = false;
    int blend = 0;
};

enum class Grid_Cursor_Shape
{
    Block,
    Horizontal,
    Vertical
};

struct Grid_Mode_Info
{
    QString name;
    Grid_Cursor_Shape shape = Grid_Cursor_Shape::Block;
    int cell_percentage = 100;
    qint64 attribute_id = 0;
};

/* What one apply_redraw() batch changed, for the widget to react to. */
struct Grid_Update_Summary
{
    bool flushed = false;
    bool grid_resized = false;
    bool cursor_moved = false;
    bool mode_changed = false;
    bool default_colors_changed = false;
    bool highlights_changed = false;
    bool title_changed = false;
    bool guifont_changed = false;
    bool busy_changed = false;
    bool window_layout_changed = false;
    int bell_count = 0;
    int visual_bell_count = 0;
};

class Neovim_Grid_Model final
{
public:
    Neovim_Grid_Model();

    /*
     * Applies one redraw notification payload: a list of
     * [event_name, argument_tuple...] entries, each possibly carrying
     * several argument tuples.
     */
    Grid_Update_Summary apply_redraw(const QVariantList& events);

    /* Screen size: the dimensions of grid 1. */
    int columns() const;
    int rows() const;

    /* Composed lookups in global screen cells. */
    const Grid_Cell& cell_at(int row, int column) const;
    QString row_text(int row) const;

    /*
     * The grid occupying a global cell, topmost first (message grid, then
     * floats, then windows, then grid 1), with the grid-local cell for
     * mouse targeting.
     */
    qint64 grid_at(
        int row,
        int column,
        int* local_row = nullptr,
        int* local_column = nullptr) const;

    /* A placed grid's rectangle in global cells; invalid when unplaced. */
    QRect grid_screen_rect(qint64 grid) const;

    /*
     * The rectangle of the grid owned by a window handle (as reported by
     * win_pos and matched through msgpack_ext_handle_value). Invalid when
     * the window is unknown, hidden, or unplaced.
     */
    QRect window_screen_rect(qint64 window_handle) const;
    bool window_is_floating(qint64 window_handle) const;

    /* Cursor in global screen cells, plus its owning grid. */
    int cursor_row() const;
    int cursor_column() const;
    qint64 cursor_grid() const;
    bool cursor_visible() const;
    Grid_Mode_Info current_mode() const;

    Grid_Highlight highlight(qint64 highlight_id) const;
    bool has_highlight(qint64 highlight_id) const;
    qint64 highlight_id_for_group(const QString& group_name) const;
    Grid_Highlight default_colors() const;

    QString title() const;
    QString guifont() const;
    bool mouse_enabled() const;

    /*
     * Damage accumulates in global cells while events apply and is
     * published only by flush; nothing should repaint before then.
     */
    bool has_published_damage() const;
    QVector<QRect> take_published_damage();

    qint64 ignored_event_count() const;

private:
    struct Grid_Data
    {
        int columns = 0;
        int rows = 0;
        QVector<Grid_Cell> cells;
        QVector<QRect> pending_damage;
    };

    struct Grid_Placement
    {
        qint64 window_handle = -1;
        int row = 0;
        int column = 0;
        bool placed = false;
        bool floating = false;
        bool is_message = false;
        qint64 z_index = 0;
        qint64 order = 0;
    };

    QHash<qint64, Grid_Data> grids_;
    QHash<qint64, Grid_Placement> placements_;
    qint64 placement_counter_ = 0;
    mutable QVector<qint64> composition_cache_;
    mutable bool composition_dirty_ = true;

    QHash<qint64, Grid_Highlight> highlights_;
    QHash<QString, qint64> highlight_groups_;
    Grid_Highlight default_colors_;
    QVector<Grid_Mode_Info> modes_;
    int mode_index_ = 0;
    qint64 cursor_grid_ = 1;
    int cursor_local_row_ = 0;
    int cursor_local_column_ = 0;
    bool busy_ = false;
    bool mouse_enabled_ = true;
    QString title_;
    QString guifont_;
    QVector<QRect> global_pending_damage_;
    QVector<QRect> published_damage_;
    qint64 ignored_event_count_ = 0;

    Grid_Data* grid_data(qint64 grid);
    const Grid_Data* grid_data(qint64 grid) const;
    QPoint grid_origin(qint64 grid) const;
    const QVector<qint64>& composition_top_down() const;
    void invalidate_composition();
    void damage_grid(qint64 grid, int row, int column, int width, int height);
    void damage_global_rect(const QRect& cells);
    void damage_cursor_cells();
    void publish_flush(Grid_Update_Summary& summary);

    void apply_grid_resize(const QVariantList& arguments,
                           Grid_Update_Summary& summary);
    void apply_grid_clear(const QVariantList& arguments);
    void apply_grid_line(const QVariantList& arguments);
    void apply_grid_scroll(const QVariantList& arguments);
    void apply_grid_destroy(const QVariantList& arguments,
                            Grid_Update_Summary& summary);
    void apply_grid_cursor_goto(const QVariantList& arguments,
                                Grid_Update_Summary& summary);
    void apply_win_pos(const QVariantList& arguments,
                       Grid_Update_Summary& summary);
    void apply_win_float_pos(const QVariantList& arguments,
                             Grid_Update_Summary& summary);
    void apply_win_hide(const QVariantList& arguments,
                        Grid_Update_Summary& summary);
    void apply_win_close(const QVariantList& arguments,
                         Grid_Update_Summary& summary);
    void apply_msg_set_pos(const QVariantList& arguments,
                           Grid_Update_Summary& summary);
    void apply_default_colors_set(const QVariantList& arguments,
                                  Grid_Update_Summary& summary);
    void apply_hl_attr_define(const QVariantList& arguments,
                              Grid_Update_Summary& summary);
    void apply_hl_group_set(const QVariantList& arguments,
                            Grid_Update_Summary& summary);
    void apply_mode_info_set(const QVariantList& arguments,
                             Grid_Update_Summary& summary);
    void apply_mode_change(const QVariantList& arguments,
                           Grid_Update_Summary& summary);
    void apply_option_set(const QVariantList& arguments,
                          Grid_Update_Summary& summary);
    void apply_set_title(const QVariantList& arguments,
                         Grid_Update_Summary& summary);
};

} // namespace Kalam::Editor

#endif
