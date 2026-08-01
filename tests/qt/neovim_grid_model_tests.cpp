#include "neovim_grid_model.h"
#include "neovim_msgpack.h"
#include "neovim_rpc_client.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QVariantList>
#include <QVariantMap>

using Kalam::Editor::Grid_Cursor_Shape;
using Kalam::Editor::Grid_Update_Summary;
using Kalam::Editor::Neovim_Grid_Model;
using Kalam::Editor::Neovim_Rpc_Client;
using Kalam::Editor::Neovim_Rpc_Request;

namespace
{

int failures = 0;

void check(bool condition, const QString& message)
{
    if (!condition)
    {
        qCritical().noquote() << QStringLiteral("check failed: %1").arg(message);
        ++failures;
    }
}

template <typename Predicate>
bool spin_until(Predicate predicate, int timeout_milliseconds = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate())
    {
        if (timer.elapsed() > timeout_milliseconds)
        {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

/* --------------------------------------------------- event builders */

QVariant event(const QByteArray& name, const QVariantList& tuple)
{
    QVariantList entry;
    entry << name << QVariant(tuple);
    return QVariant(entry);
}

QVariant cell(const QByteArray& text)
{
    return QVariant(QVariantList() << text);
}

QVariant cell(const QByteArray& text, qlonglong highlight_id)
{
    return QVariant(QVariantList() << text << highlight_id);
}

QVariant cell(const QByteArray& text, qlonglong highlight_id, qlonglong repeat)
{
    return QVariant(QVariantList() << text << highlight_id << repeat);
}

QVariantList resize_events(int columns, int rows)
{
    QVariantList events;
    events << event(
        QByteArrayLiteral("grid_resize"),
        QVariantList() << 1 << columns << rows);
    return events;
}

/* ------------------------------------------------------------ tests */

void test_grid_line_inheritance_and_repeat()
{
    Neovim_Grid_Model model;
    model.apply_redraw(resize_events(20, 4));

    QVariantList cells;
    cells << cell(QByteArrayLiteral("a"), 5)
          << cell(QByteArrayLiteral("b"))
          << cell(QByteArrayLiteral("c"), 7, 3);
    QVariantList events;
    events << event(
        QByteArrayLiteral("grid_line"),
        QVariantList() << 1 << 0 << 2 << QVariant(cells));
    model.apply_redraw(events);

    check(
        model.cell_at(0, 2).text == QStringLiteral("a") &&
            model.cell_at(0, 2).highlight_id == 5,
        QStringLiteral("the first cell carries its own highlight"));
    check(
        model.cell_at(0, 3).text == QStringLiteral("b") &&
            model.cell_at(0, 3).highlight_id == 5,
        QStringLiteral("a missing hl_id inherits the previous cell's"));
    check(
        model.cell_at(0, 4).text == QStringLiteral("c") &&
            model.cell_at(0, 6).text == QStringLiteral("c") &&
            model.cell_at(0, 6).highlight_id == 7,
        QStringLiteral("a repeat count expands the run"));
    check(
        model.cell_at(0, 7).text == QStringLiteral(" "),
        QStringLiteral("cells past the run stay untouched"));
}

void test_double_width_cells()
{
    Neovim_Grid_Model model;
    model.apply_redraw(resize_events(10, 2));

    QVariantList cells;
    cells << cell(QByteArrayLiteral("\xe6\xbc\xa2"), 3)
          << cell(QByteArrayLiteral(""));
    QVariantList events;
    events << event(
        QByteArrayLiteral("grid_line"),
        QVariantList() << 1 << 1 << 0 << QVariant(cells));
    model.apply_redraw(events);

    check(
        model.cell_at(1, 0).text == QString::fromUtf8("\xe6\xbc\xa2"),
        QStringLiteral("the left half holds the wide grapheme"));
    check(
        model.cell_at(1, 1).text.isEmpty(),
        QStringLiteral("the right half of a double-width cell is empty"));
}

void test_multiple_tuples_in_one_event()
{
    Neovim_Grid_Model model;
    model.apply_redraw(resize_events(10, 4));

    QVariantList entry;
    entry << QByteArrayLiteral("grid_line")
          << QVariant(QVariantList()
                << 1 << 0 << 0
                << QVariant(QVariantList() << cell(QByteArrayLiteral("x"), 1)))
          << QVariant(QVariantList()
                << 1 << 1 << 0
                << QVariant(QVariantList() << cell(QByteArrayLiteral("y"), 1)));
    QVariantList events;
    events << QVariant(entry);
    model.apply_redraw(events);

    check(
        model.cell_at(0, 0).text == QStringLiteral("x") &&
            model.cell_at(1, 0).text == QStringLiteral("y"),
        QStringLiteral("one event entry can carry several tuples"));
}

void test_grid_scroll()
{
    Neovim_Grid_Model model;
    model.apply_redraw(resize_events(4, 5));

    /* Rows 0..4 hold markers a..e in column 0. */
    QVariantList fill_events;
    const char markers[] = { 'a', 'b', 'c', 'd', 'e' };
    for (int row = 0; row < 5; ++row)
    {
        fill_events << event(
            QByteArrayLiteral("grid_line"),
            QVariantList()
                << 1 << row << 0
                << QVariant(QVariantList()
                    << cell(QByteArray(1, markers[row]), 1)));
    }
    model.apply_redraw(fill_events);

    /* Scroll rows [1, 4) up by one: row 2 lands on row 1, and so on. */
    QVariantList scroll_events;
    scroll_events << event(
        QByteArrayLiteral("grid_scroll"),
        QVariantList() << 1 << 1 << 4 << 0 << 4 << 1 << 0);
    model.apply_redraw(scroll_events);

    check(
        model.cell_at(0, 0).text == QStringLiteral("a"),
        QStringLiteral("rows above the region stay put on scroll up"));
    check(
        model.cell_at(1, 0).text == QStringLiteral("c") &&
            model.cell_at(2, 0).text == QStringLiteral("d"),
        QStringLiteral("the region content moves up"));
    check(
        model.cell_at(3, 0).text == QStringLiteral("d"),
        QStringLiteral("revealed rows keep stale content until grid_line"));
    check(
        model.cell_at(4, 0).text == QStringLiteral("e"),
        QStringLiteral("rows below the region stay put on scroll up"));

    /* Scroll the same region down by one: content moves back down. */
    QVariantList scroll_down;
    scroll_down << event(
        QByteArrayLiteral("grid_scroll"),
        QVariantList() << 1 << 1 << 4 << 0 << 4 << -1 << 0);
    model.apply_redraw(scroll_down);

    check(
        model.cell_at(2, 0).text == QStringLiteral("c") &&
            model.cell_at(3, 0).text == QStringLiteral("d"),
        QStringLiteral("the region content moves down on a negative count"));
    check(
        model.cell_at(1, 0).text == QStringLiteral("c"),
        QStringLiteral("rows revealed by a downward scroll keep stale "
                       "content"));
}

void test_flush_batching()
{
    Neovim_Grid_Model model;

    Grid_Update_Summary resize_summary =
        model.apply_redraw(resize_events(8, 2));
    check(
        !resize_summary.flushed && !model.has_published_damage(),
        QStringLiteral("nothing publishes before flush"));

    QVariantList line_events;
    line_events << event(
        QByteArrayLiteral("grid_line"),
        QVariantList()
            << 1 << 0 << 0
            << QVariant(QVariantList() << cell(QByteArrayLiteral("z"), 1)));
    model.apply_redraw(line_events);
    check(
        !model.has_published_damage(),
        QStringLiteral("grid_line damage stays pending before flush"));

    QVariantList flush_events;
    flush_events << event(QByteArrayLiteral("flush"), QVariantList());
    const Grid_Update_Summary flush_summary =
        model.apply_redraw(flush_events);
    check(
        flush_summary.flushed && model.has_published_damage(),
        QStringLiteral("flush publishes the accumulated damage"));

    const QVector<QRect> damage = model.take_published_damage();
    check(
        !damage.isEmpty(),
        QStringLiteral("published damage is non-empty"));
    bool covers_cell = false;
    for (const QRect& rectangle : damage)
    {
        if (rectangle.contains(0, 0))
        {
            covers_cell = true;
        }
    }
    check(
        covers_cell,
        QStringLiteral("published damage covers the written cell"));
    check(
        !model.has_published_damage(),
        QStringLiteral("taking the damage clears it"));
}

void test_highlights_and_default_colors()
{
    Neovim_Grid_Model model;

    QVariantMap attributes;
    attributes.insert(QStringLiteral("foreground"), qlonglong(0xff0000));
    attributes.insert(QStringLiteral("background"), qlonglong(0x00ff00));
    attributes.insert(QStringLiteral("bold"), true);
    attributes.insert(QStringLiteral("underdouble"), true);

    QVariantList events;
    events << event(
        QByteArrayLiteral("default_colors_set"),
        QVariantList()
            << qlonglong(0xffffff) << qlonglong(0x101010) << qlonglong(-1));
    events << event(
        QByteArrayLiteral("hl_attr_define"),
        QVariantList()
            << qlonglong(9) << attributes << QVariantMap() << QVariantList());
    events << event(
        QByteArrayLiteral("hl_group_set"),
        QVariantList() << QByteArrayLiteral("Normal") << qlonglong(9));
    const Grid_Update_Summary summary = model.apply_redraw(events);

    check(
        summary.default_colors_changed && summary.highlights_changed,
        QStringLiteral("the summary reports color and highlight changes"));
    check(
        model.highlight(9).foreground == QColor(0xff, 0x00, 0x00) &&
            model.highlight(9).bold &&
            model.highlight(9).underdouble,
        QStringLiteral("hl_attr_define attributes resolve"));
    check(
        !model.highlight(9).special.isValid(),
        QStringLiteral("an absent color stays invalid"));
    check(
        model.highlight(0).foreground == QColor(0xff, 0xff, 0xff) &&
            model.highlight(0).background == QColor(0x10, 0x10, 0x10),
        QStringLiteral("id 0 resolves to the default colors"));
    check(
        model.highlight(12345).background == QColor(0x10, 0x10, 0x10),
        QStringLiteral("unknown ids resolve to the default colors"));
    check(
        model.highlight_id_for_group(QStringLiteral("Normal")) == 9,
        QStringLiteral("hl_group_set records the group id"));
}

void test_modes_cursor_and_busy()
{
    Neovim_Grid_Model model;
    model.apply_redraw(resize_events(10, 4));

    QVariantMap normal_mode;
    normal_mode.insert(QStringLiteral("name"), QByteArrayLiteral("normal"));
    normal_mode.insert(
        QStringLiteral("cursor_shape"), QByteArrayLiteral("block"));
    QVariantMap insert_mode;
    insert_mode.insert(QStringLiteral("name"), QByteArrayLiteral("insert"));
    insert_mode.insert(
        QStringLiteral("cursor_shape"), QByteArrayLiteral("vertical"));
    insert_mode.insert(QStringLiteral("cell_percentage"), qlonglong(25));

    QVariantList events;
    events << event(
        QByteArrayLiteral("mode_info_set"),
        QVariantList()
            << true
            << QVariant(QVariantList() << normal_mode << insert_mode));
    events << event(
        QByteArrayLiteral("mode_change"),
        QVariantList() << QByteArrayLiteral("insert") << 1);
    events << event(
        QByteArrayLiteral("grid_cursor_goto"),
        QVariantList() << 1 << 2 << 7);
    model.apply_redraw(events);

    check(
        model.current_mode().shape == Grid_Cursor_Shape::Vertical &&
            model.current_mode().cell_percentage == 25,
        QStringLiteral("mode_change selects the mode's cursor shape"));
    check(
        model.cursor_row() == 2 && model.cursor_column() == 7,
        QStringLiteral("grid_cursor_goto moves the cursor"));
    check(
        model.cursor_visible(),
        QStringLiteral("the cursor is visible while not busy"));

    QVariantList busy_events;
    busy_events << event(QByteArrayLiteral("busy_start"), QVariantList());
    model.apply_redraw(busy_events);
    check(
        !model.cursor_visible(),
        QStringLiteral("busy_start hides the cursor"));
}

void test_unknown_events_and_other_grids()
{
    Neovim_Grid_Model model;
    model.apply_redraw(resize_events(10, 4));
    const qint64 ignored_before = model.ignored_event_count();

    QVariantList events;
    events << event(
        QByteArrayLiteral("some_future_event"),
        QVariantList() << 1 << 2 << 3);
    events << event(
        QByteArrayLiteral("grid_line"),
        QVariantList()
            << 4 << 0 << 0
            << QVariant(QVariantList() << cell(QByteArrayLiteral("w"), 1)));
    model.apply_redraw(events);

    check(
        model.ignored_event_count() == ignored_before + 2,
        QStringLiteral("unknown events and other grids are counted"));
    check(
        model.cell_at(0, 0).text == QStringLiteral(" "),
        QStringLiteral("another grid's line never lands on grid 1"));
    check(
        model.columns() == 10 && model.rows() == 4,
        QStringLiteral("the grid survives unknown traffic"));
}

/* ------------------------------------------------- multigrid tests */

QVariant window_handle_ext(qint64 handle)
{
    QByteArray payload;
    const Kalam::Editor::Msgpack_Status status =
        Kalam::Editor::msgpack_encode(QVariant(qlonglong(handle)), payload);
    check(
        status == Kalam::Editor::Msgpack_Status::Ok,
        QStringLiteral("the test encodes a window handle payload"));
    Kalam::Editor::Msgpack_Ext ext;
    ext.type = 1;
    ext.payload = payload;
    return QVariant::fromValue(ext);
}

void test_multigrid_placement_and_composition()
{
    Neovim_Grid_Model model;
    model.apply_redraw(resize_events(20, 10));

    QVariantList fill;
    fill << event(
        QByteArrayLiteral("grid_line"),
        QVariantList()
            << 1 << 0 << 0
            << QVariant(QVariantList() << cell(QByteArrayLiteral("g"), 1)));
    model.apply_redraw(fill);

    QVariantList placement_events;
    placement_events << event(
        QByteArrayLiteral("grid_resize"),
        QVariantList() << 2 << 10 << 3);
    placement_events << event(
        QByteArrayLiteral("win_pos"),
        QVariantList()
            << 2 << window_handle_ext(1000) << 2 << 5 << 10 << 3);
    placement_events << event(
        QByteArrayLiteral("grid_line"),
        QVariantList()
            << 2 << 0 << 0
            << QVariant(QVariantList() << cell(QByteArrayLiteral("w"), 1)));
    const Grid_Update_Summary summary =
        model.apply_redraw(placement_events);

    check(
        summary.window_layout_changed,
        QStringLiteral("win_pos reports a layout change"));
    check(
        model.cell_at(2, 5).text == QStringLiteral("w"),
        QStringLiteral("a placed grid's cell composes at its offset"));
    check(
        model.cell_at(0, 0).text == QStringLiteral("g"),
        QStringLiteral("grid 1 shows through outside the window"));
    check(
        model.window_screen_rect(1000) == QRect(5, 2, 10, 3),
        QStringLiteral("the window handle maps to its screen rectangle"));

    int local_row = -1;
    int local_column = -1;
    check(
        model.grid_at(2, 5, &local_row, &local_column) == 2 &&
            local_row == 0 && local_column == 0,
        QStringLiteral("hit-testing returns the window grid and local cell"));
    check(
        model.grid_at(0, 0) == 1,
        QStringLiteral("hit-testing falls back to grid 1"));

    QVariantList cursor_events;
    cursor_events << event(
        QByteArrayLiteral("grid_cursor_goto"),
        QVariantList() << 2 << 1 << 1);
    cursor_events << event(QByteArrayLiteral("flush"), QVariantList());
    model.apply_redraw(cursor_events);
    check(
        model.cursor_grid() == 2 &&
            model.cursor_row() == 3 &&
            model.cursor_column() == 6,
        QStringLiteral("the cursor publishes grid-offset global cells"));

    bool covers_window_cell = false;
    for (const QRect& damage : model.take_published_damage())
    {
        if (damage.contains(5, 2))
        {
            covers_window_cell = true;
        }
    }
    check(
        covers_window_cell,
        QStringLiteral("flush translates window damage to global cells"));
}

void test_float_anchoring_and_message_grid()
{
    Neovim_Grid_Model model;
    model.apply_redraw(resize_events(20, 10));

    QVariantList events;
    events << event(
        QByteArrayLiteral("grid_resize"),
        QVariantList() << 3 << 4 << 2);
    events << event(
        QByteArrayLiteral("win_float_pos"),
        QVariantList()
            << 3 << window_handle_ext(1001) << QByteArrayLiteral("NW")
            << 1 << 1.0 << 2.0 << true << 60);
    model.apply_redraw(events);

    check(
        model.window_screen_rect(1001) == QRect(2, 1, 4, 2),
        QStringLiteral("a NW float anchors at its offset"));
    check(
        model.window_is_floating(1001),
        QStringLiteral("the float is reported floating"));
    check(
        model.grid_at(1, 2) == 3,
        QStringLiteral("the float sits above grid 1"));

    QVariantList southeast;
    southeast << event(
        QByteArrayLiteral("win_float_pos"),
        QVariantList()
            << 3 << window_handle_ext(1001) << QByteArrayLiteral("SE")
            << 1 << 6.0 << 6.0 << true << 60);
    model.apply_redraw(southeast);
    check(
        model.window_screen_rect(1001) == QRect(2, 4, 4, 2),
        QStringLiteral("a SE float subtracts its own size"));

    /* The message grid outranks everything at its rows. */
    QVariantList message;
    message << event(
        QByteArrayLiteral("grid_resize"),
        QVariantList() << 4 << 20 << 2);
    message << event(
        QByteArrayLiteral("msg_set_pos"),
        QVariantList() << 4 << 8 << false << QByteArrayLiteral("-"));
    model.apply_redraw(message);
    check(
        model.grid_at(8, 0) == 4 && model.grid_at(9, 19) == 4,
        QStringLiteral("the message grid composes on top"));
    check(
        model.grid_at(0, 0) == 1,
        QStringLiteral("rows above the message grid are untouched"));
}

void test_hide_close_and_destroy()
{
    Neovim_Grid_Model model;
    model.apply_redraw(resize_events(20, 10));

    QVariantList events;
    events << event(
        QByteArrayLiteral("grid_resize"),
        QVariantList() << 2 << 10 << 3);
    events << event(
        QByteArrayLiteral("win_pos"),
        QVariantList()
            << 2 << window_handle_ext(1000) << 0 << 0 << 10 << 3);
    model.apply_redraw(events);
    check(
        model.grid_at(0, 0) == 2,
        QStringLiteral("the window grid composes before hiding"));

    QVariantList hide;
    hide << event(QByteArrayLiteral("win_hide"), QVariantList() << 2);
    const Grid_Update_Summary hide_summary = model.apply_redraw(hide);
    check(
        hide_summary.window_layout_changed &&
            model.grid_at(0, 0) == 1 &&
            !model.window_screen_rect(1000).isValid(),
        QStringLiteral("win_hide removes the grid from composition"));

    QVariantList replace;
    replace << event(
        QByteArrayLiteral("win_pos"),
        QVariantList()
            << 2 << window_handle_ext(1000) << 4 << 4 << 10 << 3);
    model.apply_redraw(replace);
    check(
        model.window_screen_rect(1000) == QRect(4, 4, 10, 3),
        QStringLiteral("a hidden grid re-places"));

    QVariantList destroy;
    destroy << event(QByteArrayLiteral("grid_destroy"), QVariantList() << 2);
    model.apply_redraw(destroy);
    check(
        model.grid_at(4, 4) == 1 &&
            !model.window_screen_rect(1000).isValid(),
        QStringLiteral("grid_destroy removes grid and placement"));
}

/* --------------------------------------------- live Neovim golden run */

void run_real_neovim_golden(const QString& neovim_program)
{
    QObject arena;
    auto* client = new Neovim_Rpc_Client(&arena);
    Neovim_Grid_Model model;

    int flush_count = 0;
    QObject::connect(
        client,
        &Neovim_Rpc_Client::redrawReceived,
        &arena,
        [&](const QVariantList& events)
        {
            const Grid_Update_Summary summary = model.apply_redraw(events);
            if (summary.flushed)
            {
                ++flush_count;
            }
        });

    int ready_count = 0;
    QObject::connect(
        client,
        &Neovim_Rpc_Client::ready,
        &arena,
        [&]()
        {
            ++ready_count;
        });
    int exit_count = 0;
    QObject::connect(
        client,
        &Neovim_Rpc_Client::processExited,
        &arena,
        [&](int)
        {
            ++exit_count;
        });

    check(
        client->spawn(
            neovim_program,
            QStringList() << QStringLiteral("--clean")),
        QStringLiteral("nvim --embed spawns for the golden run"));
    check(
        spin_until([&]() { return ready_count == 1; }, 15000),
        QStringLiteral("the golden run handshake completes"));

    QVariantMap ui_options;
    ui_options.insert(QStringLiteral("rgb"), true);
    ui_options.insert(QStringLiteral("ext_linegrid"), true);
    ui_options.insert(QStringLiteral("ext_multigrid"), true);

    /*
     * A user's init.lua sets 'guifont' before any UI attaches, so the
     * value must arrive with the attach-time option_set burst rather than
     * only on later changes.
     */
    client->call(
        QByteArrayLiteral("nvim_command"),
        QVariantList()
            << QByteArrayLiteral("set guifont=IosevkaTerm\\ NF:h10"));

    client->call(
        QByteArrayLiteral("nvim_ui_attach"),
        QVariantList() << 60 << 12 << ui_options);

    check(
        spin_until([&]()
        {
            return model.columns() == 60 && model.rows() == 12 &&
                flush_count > 0;
        }),
        QStringLiteral("attaching resizes the grid and flushes"));

    check(
        model.guifont().contains(QStringLiteral("Iosevka")),
        QStringLiteral("a guifont set before attach arrives with the "
                       "attach-time options (model reports '%1')")
            .arg(model.guifont()));

    client->call(
        QByteArrayLiteral("nvim_input"),
        QVariantList() << QByteArrayLiteral("ihello world<Esc>"));

    check(
        spin_until([&]()
        {
            return model.row_text(0).startsWith(
                QStringLiteral("hello world"));
        }),
        QStringLiteral("typed text appears in the grid model (row 0 is "
                       "'%1')").arg(model.row_text(0).trimmed()));
    check(
        spin_until([&]()
        {
            return model.cursor_row() == 0 && model.cursor_column() == 10;
        }),
        QStringLiteral("the cursor lands on the last typed character"));
    check(
        model.current_mode().name.startsWith(QStringLiteral("normal")),
        QStringLiteral("the mode returns to normal after <Esc> (mode is "
                       "'%1')").arg(model.current_mode().name));

    /* Force a scroll region move and confirm the model follows. */
    client->call(
        QByteArrayLiteral("nvim_command"),
        QVariantList()
            << QByteArrayLiteral("call setline(1, range(1, 200))"));
    client->call(
        QByteArrayLiteral("nvim_input"),
        QVariantList() << QByteArrayLiteral("G"));
    check(
        spin_until([&]()
        {
            for (int row = 0; row < model.rows(); ++row)
            {
                if (model.row_text(row).startsWith(QStringLiteral("200")))
                {
                    return true;
                }
            }
            return false;
        }),
        QStringLiteral("jumping to the end scrolls 200 into view"));

    /* A vertical split yields two placed windows tiling the screen. */
    client->call(
        QByteArrayLiteral("nvim_command"),
        QVariantList() << QByteArrayLiteral("vsplit"));

    QVariantList window_handles;
    Neovim_Rpc_Request* list_request = client->call(
        QByteArrayLiteral("nvim_list_wins"),
        QVariantList());
    QObject::connect(
        list_request,
        &Neovim_Rpc_Request::finished,
        &arena,
        [&](quint32, const QVariant& result)
        {
            window_handles = result.toList();
        });
    check(
        spin_until([&]() { return window_handles.size() == 2; }),
        QStringLiteral("the split yields two windows"));

    QVector<qint64> handles;
    for (const QVariant& entry : window_handles)
    {
        bool ok = false;
        const qint64 handle = Kalam::Editor::msgpack_ext_handle_value(
            entry.value<Kalam::Editor::Msgpack_Ext>(),
            &ok);
        check(
            ok,
            QStringLiteral("real Window ext payloads decode to handles"));
        handles.append(handle);
    }

    check(
        spin_until([&]()
        {
            if (handles.size() != 2)
            {
                return false;
            }
            const QRect first = model.window_screen_rect(handles.at(0));
            const QRect second = model.window_screen_rect(handles.at(1));
            return first.isValid() && second.isValid() &&
                !first.intersects(second) &&
                first.height() == second.height() &&
                first.width() > 10 && second.width() > 10;
        }),
        QStringLiteral("win_pos places both split windows disjointly"));

    /*
     * 'guifont' must reach the UI: kalam is the GUI, so this option is the
     * only way Neovim can state which font the grid should use.
     */
    client->call(
        QByteArrayLiteral("nvim_command"),
        QVariantList()
            << QByteArrayLiteral("set guifont=IosevkaTerm\\ NF:h10"));
    check(
        spin_until([&]()
        {
            return model.guifont().contains(QStringLiteral("Iosevka"));
        }),
        QStringLiteral("setting guifont reaches the UI (model reports '%1')")
            .arg(model.guifont()));

    client->call(
        QByteArrayLiteral("nvim_command"),
        QVariantList() << QByteArrayLiteral("qall!"));
    check(
        spin_until([&]() { return exit_count == 1; }),
        QStringLiteral("the golden run shuts down cleanly"));
}

} // namespace

int main(int argument_count, char** arguments)
{
    QCoreApplication application(argument_count, arguments);

    QString neovim_program;
    const QStringList application_arguments =
        QCoreApplication::arguments();
    const int neovim_flag =
        application_arguments.indexOf(QStringLiteral("--nvim"));
    if (neovim_flag >= 0 &&
        neovim_flag + 1 < application_arguments.size())
    {
        neovim_program = application_arguments.at(neovim_flag + 1);
    }

    if (neovim_program.isEmpty())
    {
        test_grid_line_inheritance_and_repeat();
        test_double_width_cells();
        test_multiple_tuples_in_one_event();
        test_grid_scroll();
        test_flush_batching();
        test_highlights_and_default_colors();
        test_modes_cursor_and_busy();
        test_unknown_events_and_other_grids();
        test_multigrid_placement_and_composition();
        test_float_anchoring_and_message_grid();
        test_hide_close_and_destroy();
    }
    else
    {
        run_real_neovim_golden(neovim_program);
    }

    if (failures == 0)
    {
        qInfo().noquote() << QStringLiteral("all grid model tests passed");
    }
    return failures == 0 ? 0 : 1;
}
