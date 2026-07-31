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
 * ext_linegrid state machine (spec_2.md section 5): redraw events in, cell
 * state and damage rectangles out. Holds no widget and paints nothing, so
 * every protocol rule is testable headlessly.
 *
 * Only grid 1 is tracked in revision 1; events for other grids are counted
 * as ignored.
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

    int columns() const;
    int rows() const;
    const Grid_Cell& cell_at(int row, int column) const;
    QString row_text(int row) const;

    int cursor_row() const;
    int cursor_column() const;
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
     * Damage accumulates in cell coordinates while events apply and is
     * published only by flush; nothing should repaint before then.
     */
    bool has_published_damage() const;
    QVector<QRect> take_published_damage();

    qint64 ignored_event_count() const;

private:
    int columns_ = 0;
    int rows_ = 0;
    QVector<Grid_Cell> cells_;
    QHash<qint64, Grid_Highlight> highlights_;
    QHash<QString, qint64> highlight_groups_;
    Grid_Highlight default_colors_;
    QVector<Grid_Mode_Info> modes_;
    int mode_index_ = 0;
    int cursor_row_ = 0;
    int cursor_column_ = 0;
    bool busy_ = false;
    bool mouse_enabled_ = true;
    QString title_;
    QString guifont_;
    QVector<QRect> pending_damage_;
    QVector<QRect> published_damage_;
    qint64 ignored_event_count_ = 0;

    Grid_Cell& cell_reference(int row, int column);
    void damage(int row, int column, int width, int height);
    void damage_all();
    bool cell_in_grid(int row, int column) const;

    void apply_grid_resize(const QVariantList& arguments,
                           Grid_Update_Summary& summary);
    void apply_grid_clear(const QVariantList& arguments);
    void apply_grid_line(const QVariantList& arguments);
    void apply_grid_scroll(const QVariantList& arguments);
    void apply_grid_cursor_goto(const QVariantList& arguments,
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
