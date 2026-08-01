#include "context_popup.h"

#include "qt_value_widgets.h"

#include <QEvent>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace Kalam::Editor
{

Context_Popup::Context_Popup(
    const QVector<Field_Descriptor>& fields,
    const QVector<Author_Value_Owned>& values,
    QWidget* parent)
    : QFrame(parent)
{
    Q_ASSERT(fields.size() == values.size());
    setObjectName(QStringLiteral("contextPopup"));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_StyledBackground, true);
    /* Needed so the corner shows the resize cursor without a button held. */
    setMouseTracking(true);

    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(1, 1, 1, 1);
    root_layout->setSpacing(0);

    auto* title_bar = new QHBoxLayout();
    title_bar->setContentsMargins(0, 0, 0, 0);
    title_bar->setSpacing(0);
    auto* title_label = new QLabel(QStringLiteral("Kalam context"), this);
    title_label->setObjectName(QStringLiteral("kalamSurfaceTitle"));
    title_bar->addWidget(title_label, 1);
    mode_label_ = new QLabel(QStringLiteral("NORMAL"), this);
    mode_label_->setObjectName(QStringLiteral("kalamSurfaceMode"));
    mode_label_->setMinimumWidth(72);
    title_bar->addWidget(mode_label_);
    auto* close_button = new QPushButton(QStringLiteral("x"), this);
    close_button->setObjectName(QStringLiteral("kalamSurfaceClose"));
    close_button->setAutoDefault(false);
    close_button->setFocusPolicy(Qt::NoFocus);
    close_button->setFixedWidth(28);
    title_bar->addWidget(close_button);
    root_layout->addLayout(title_bar);

    validation_label_ = new QLabel(this);
    validation_label_->setObjectName(QStringLiteral("kalamContextValidation"));
    validation_label_->setWordWrap(true);
    validation_label_->hide();
    root_layout->addWidget(validation_label_);

    auto* form = new QFormLayout();
    form->setContentsMargins(14, 14, 14, 8);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);
    for (qsizetype index = 0; index < fields.size(); ++index)
    {
        auto* editor = new Author_Value_Widget(
            fields.at(index),
            values.at(index),
            this);
        editor->set_keyboard_passive(false);
        editors_.push_back(editor);
        form->addRow(QString::fromUtf8(fields.at(index).name), editor);
        connect(
            editor,
            &Author_Value_Widget::previewRequested,
            this,
            [this](
                Author_Field_Id field_id,
                const Author_Value_Owned& value)
            {
                if (busy_)
                    return;
                dirty_ = true;
                set_validation_error(QString());
                refresh_statusline();
                emit fieldPreviewRequested(field_id, value);
            });
        editor->installEventFilter(this);
        const QList<QWidget*> controls = editor->findChildren<QWidget*>();
        for (QWidget* control : controls)
            control->installEventFilter(this);
    }
    root_layout->addLayout(form, 1);

    auto* buttons = new QHBoxLayout();
    buttons->setContentsMargins(9, 5, 9, 5);
    buttons->addStretch();
    auto* discard_button = new QPushButton(QStringLiteral("Discard (:q!)"), this);
    auto* apply_button = new QPushButton(QStringLiteral("Write (:w)"), this);
    auto* apply_close_button = new QPushButton(
        QStringLiteral("Write && Close (:wq)"),
        this);
    for (QPushButton* button : {discard_button, apply_button, apply_close_button})
    {
        button->setAutoDefault(false);
        button->setDefault(false);
        button->setFocusPolicy(Qt::NoFocus);
        buttons->addWidget(button);
    }
    root_layout->addLayout(buttons);

    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("kalamContextStatusline"));
    status_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    status_label_->setTextInteractionFlags(Qt::NoTextInteraction);
    status_label_->setMinimumHeight(24);
    root_layout->addWidget(status_label_);

    connect(close_button, &QPushButton::clicked, this, &Context_Popup::cancelRequested);
    connect(discard_button, &QPushButton::clicked, this, &Context_Popup::cancelRequested);
    connect(
        apply_button,
        &QPushButton::clicked,
        this,
        [this]() { emit applyRequested(false); });
    connect(
        apply_close_button,
        &QPushButton::clicked,
        this,
        [this]() { emit applyRequested(true); });

    set_active_field(editors_.isEmpty() ? -1 : 0);
    refresh_statusline();
}

namespace
{
const int Resize_Grip_Size = 16;
}

bool Context_Popup::in_resize_grip(const QPoint& position) const
{
    return position.x() >= width() - Resize_Grip_Size &&
        position.y() >= height() - Resize_Grip_Size;
}

void Context_Popup::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton &&
        in_resize_grip(event->position().toPoint()))
    {
        resizing_ = true;
        resize_origin_ = event->globalPosition().toPoint();
        resize_start_size_ = size();
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void Context_Popup::mouseMoveEvent(QMouseEvent* event)
{
    setCursor(
        in_resize_grip(event->position().toPoint()) || resizing_
            ? Qt::SizeFDiagCursor
            : Qt::ArrowCursor);

    if (!resizing_)
    {
        QFrame::mouseMoveEvent(event);
        return;
    }

    const QPoint delta = event->globalPosition().toPoint() - resize_origin_;
    int new_width = resize_start_size_.width() + delta.x();
    int new_height = resize_start_size_.height() + delta.y();
    const QWidget* host = parentWidget();
    if (host != nullptr)
    {
        new_width = qMin(new_width, host->width() - x());
        new_height = qMin(new_height, host->height() - y());
    }
    resize(
        qMax(minimumSizeHint().width(), new_width),
        qMax(minimumSizeHint().height(), new_height));
    user_sized_ = true;
    event->accept();
}

void Context_Popup::mouseReleaseEvent(QMouseEvent* event)
{
    if (resizing_ && event->button() == Qt::LeftButton)
    {
        resizing_ = false;
        event->accept();
        return;
    }
    QFrame::mouseReleaseEvent(event);
}

void Context_Popup::paintEvent(QPaintEvent* event)
{
    QFrame::paintEvent(event);

    /* Three diagonal ticks marking the drag corner. */
    QPainter painter(this);
    QColor tick = palette().color(QPalette::WindowText);
    tick.setAlpha(140);
    painter.setPen(QPen(tick, 1));
    for (int offset = 3; offset <= 11; offset += 4)
    {
        painter.drawLine(
            width() - offset,
            height() - 2,
            width() - 2,
            height() - offset);
    }
}

QSize Context_Popup::compact_size_limit() const
{
    /*
     * The popup shows all of its content by default: it grows to the
     * layout's hint so nothing is clipped or hidden behind a scroll bar,
     * whatever font Neovim is using. The only bound is the editor surface
     * itself, and the user can drag the bottom-right corner from there.
     * The acceptance smoke and the theme tests assert against this same
     * limit so the three can never drift.
     */
    const QSize hint = sizeHint();
    const QWidget* host = parentWidget();
    if (host == nullptr)
    {
        return hint;
    }
    return QSize(
        qMin(hint.width(), host->width()),
        qMin(hint.height(), host->height()));
}

void Context_Popup::project_to(
    QWidget* shell,
    const QRect& anchor_rect,
    const QSize& cell_size)
{
    if (shell == nullptr || cell_size.width() <= 0 || cell_size.height() <= 0)
    {
        hide();
        return;
    }
    if (parentWidget() != shell)
        setParent(shell);

    const int margin = qMax(4, cell_size.width() / 2);
    const int editor_bottom_reserve = (2 * cell_size.height()) + margin;
    QRect available = shell->rect().adjusted(
        margin,
        margin,
        -margin,
        -editor_bottom_reserve);
    if (!available.isValid() || available.isEmpty())
    {
        hide();
        return;
    }

    ensurePolished();
    if (layout() != nullptr)
        layout()->activate();
    const QSize hint = sizeHint();
    /*
     * Show everything by default. The popup may be wider than the space
     * beside the cursor -- a wide monospace face makes that common -- and
     * that is preferred over clipping a field or hiding it behind a
     * scroll bar. A user-dragged size wins until the popup closes.
     */
    const int maximum_width = qMin(hint.width(), available.width());
    const int maximum_height = qMin(hint.height(), available.height());
    const int minimum_width = qMin(360, maximum_width);
    const int minimum_height = qMin(180, maximum_height);
    QSize popup_size(
        qBound(minimum_width, hint.width(), maximum_width),
        qBound(minimum_height, hint.height(), maximum_height));
    if (user_sized_)
    {
        popup_size = QSize(
            qBound(minimum_width, width(), available.width()),
            qBound(minimum_height, height(), available.height()));
    }

    QRect anchor = anchor_rect;
    if (!anchor.isValid() || anchor.isEmpty())
    {
        anchor = QRect(
            shell->rect().center(),
            QSize(cell_size.width(), cell_size.height()));
    }

    const int gap = qMax(4, cell_size.width());
    int x = anchor.right() + 1 + gap;
    if (x + popup_size.width() - 1 > available.right())
        x = anchor.left() - gap - popup_size.width();
    x = qBound(
        available.left(),
        x,
        available.right() - popup_size.width() + 1);

    int y = anchor.top();
    if (y + popup_size.height() - 1 > available.bottom())
        y = anchor.top() - gap - popup_size.height();
    y = qBound(
        available.top(),
        y,
        available.bottom() - popup_size.height() + 1);

    setGeometry(QRect(QPoint(x, y), popup_size));
    show();
    raise();
}

QVector<Author_Value_Owned> Context_Popup::staged_values() const
{
    QVector<Author_Value_Owned> result;
    result.reserve(editors_.size());
    for (const Author_Value_Widget* editor : editors_)
        result.push_back(editor->value());
    return result;
}

void Context_Popup::set_values(
    const QVector<Author_Value_Owned>& values,
    bool mark_clean)
{
    const qsizetype count = qMin(editors_.size(), values.size());
    for (qsizetype index = 0; index < count; ++index)
        editors_.at(index)->set_value(values.at(index));
    dirty_ = !mark_clean;
    set_validation_error(QString());
    refresh_modal_selection();
    refresh_statusline();
}

void Context_Popup::set_active_field(qsizetype field_index)
{
    if (editors_.isEmpty())
    {
        active_field_ = -1;
        active_component_ = 0;
        refresh_modal_selection();
        return;
    }
    active_field_ = qBound<qsizetype>(0, field_index, editors_.size() - 1);
    const int count = editors_.at(active_field_)->editable_component_count();
    active_component_ = count > 0
        ? qBound(0, active_component_, count - 1)
        : 0;
    refresh_modal_selection();
}

void Context_Popup::focus_modal()
{
    enter_mode(QByteArrayLiteral("n"));
}

QByteArray Context_Popup::mode() const
{
    return mode_;
}

qsizetype Context_Popup::active_field() const
{
    return active_field_;
}

int Context_Popup::active_component() const
{
    return active_component_;
}

bool Context_Popup::is_dirty() const
{
    return dirty_;
}

bool Context_Popup::has_modal_selection() const
{
    for (const Author_Value_Widget* editor : editors_)
    {
        if (editor->has_modal_selection())
            return true;
    }
    return false;
}

bool Context_Popup::has_validation_error() const
{
    return validation_label_ != nullptr && !validation_label_->text().isEmpty();
}

void Context_Popup::set_validation_error(const QString& error)
{
    validation_label_->setText(error);
    validation_label_->setVisible(!error.isEmpty());
}

void Context_Popup::set_busy(bool busy)
{
    busy_ = busy;
    for (Author_Value_Widget* editor : editors_)
        editor->setEnabled(!busy);
    const QList<QPushButton*> buttons = findChildren<QPushButton*>();
    for (QPushButton* button : buttons)
        button->setEnabled(!busy);
    if (busy_)
        setFocus(Qt::OtherFocusReason);
    refresh_modal_selection();
    refresh_statusline();
}

bool Context_Popup::eventFilter(QObject* watched, QEvent* event)
{
    qsizetype field_index = -1;
    int component = -1;
    const bool from_control =
        locate_control(watched, &field_index, &component);

    if (from_control &&
        (event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::FocusIn))
    {
        active_field_ = field_index;
        active_component_ = component;
        if (event->type() == QEvent::MouseButtonPress &&
            mode_ != QByteArrayLiteral("i"))
        {
            enter_mode(QByteArrayLiteral("i"));
        }
        else
        {
            refresh_modal_selection();
        }
    }

    if (event->type() == QEvent::KeyPress)
    {
        auto* key_event = static_cast<QKeyEvent*>(event);
        if (handle_key(key_event, from_control))
            return true;
    }
    return QFrame::eventFilter(watched, event);
}

void Context_Popup::keyPressEvent(QKeyEvent* event)
{
    if (!handle_key(event, false))
        QFrame::keyPressEvent(event);
}

bool Context_Popup::handle_key(QKeyEvent* event, bool from_control)
{
    if (event == nullptr)
        return false;

    const int key = event->key();
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    const bool control_left_bracket =
        key == Qt::Key_BracketLeft &&
        modifiers.testFlag(Qt::ControlModifier);

    if (busy_)
    {
        event->accept();
        return true;
    }

    if (mode_ == QByteArrayLiteral("i"))
    {
        if (key == Qt::Key_Escape || control_left_bracket)
        {
            enter_mode(QByteArrayLiteral("n"));
            event->accept();
            return true;
        }
        return !from_control;
    }

    if (mode_ == QByteArrayLiteral("c"))
    {
        if (key == Qt::Key_Escape || control_left_bracket)
        {
            enter_mode(QByteArrayLiteral("n"));
        }
        else if (key == Qt::Key_Backspace)
        {
            if (command_text_.isEmpty())
                enter_mode(QByteArrayLiteral("n"));
            else
                command_text_.chop(1);
        }
        else if (key == Qt::Key_Return || key == Qt::Key_Enter)
        {
            execute_command();
        }
        else if (!event->text().isEmpty() &&
                 !modifiers.testFlag(Qt::ControlModifier) &&
                 !modifiers.testFlag(Qt::AltModifier) &&
                 !modifiers.testFlag(Qt::MetaModifier))
        {
            command_text_.append(event->text());
        }
        refresh_statusline();
        event->accept();
        return true;
    }

    if (key == Qt::Key_I)
    {
        enter_mode(QByteArrayLiteral("i"));
    }
    else if (key == Qt::Key_J || key == Qt::Key_Down ||
             key == Qt::Key_Tab)
    {
        move_field(1);
    }
    else if (key == Qt::Key_K || key == Qt::Key_Up ||
             key == Qt::Key_Backtab)
    {
        move_field(-1);
    }
    else if (key == Qt::Key_H || key == Qt::Key_Left)
    {
        move_component(-1);
    }
    else if (key == Qt::Key_L || key == Qt::Key_Right)
    {
        move_component(1);
    }
    else if (key == Qt::Key_Return || key == Qt::Key_Enter)
    {
        enter_mode(QByteArrayLiteral("i"));
    }
    else if (key == Qt::Key_Colon ||
             event->text() == QStringLiteral(":"))
    {
        enter_mode(QByteArrayLiteral("c"));
    }
    event->accept();
    return true;
}

void Context_Popup::enter_mode(const QByteArray& mode)
{
    mode_ = mode;
    if (mode_ != QByteArrayLiteral("c"))
        command_text_.clear();

    if (mode_ == QByteArrayLiteral("i") &&
        active_field_ >= 0 &&
        active_field_ < editors_.size())
    {
        editors_.at(active_field_)->focus_component(active_component_);
    }
    else
    {
        setFocus(Qt::ShortcutFocusReason);
    }
    refresh_modal_selection();
    refresh_statusline();
}

void Context_Popup::execute_command()
{
    const QString command = command_text_.trimmed();
    command_text_.clear();

    if (command == QStringLiteral("w"))
    {
        enter_mode(QByteArrayLiteral("n"));
        emit applyRequested(false);
        return;
    }
    if (command == QStringLiteral("wq") ||
        command == QStringLiteral("x"))
    {
        enter_mode(QByteArrayLiteral("n"));
        emit applyRequested(true);
        return;
    }
    if (command == QStringLiteral("q!"))
    {
        enter_mode(QByteArrayLiteral("n"));
        emit cancelRequested();
        return;
    }
    if (command == QStringLiteral("q"))
    {
        if (dirty_)
        {
            set_validation_error(QStringLiteral(
                "No write since last change (use :q! to discard)"));
            enter_mode(QByteArrayLiteral("n"));
            return;
        }
        enter_mode(QByteArrayLiteral("n"));
        emit cancelRequested();
        return;
    }

    set_validation_error(
        command.isEmpty()
            ? QStringLiteral("Not an editor command")
            : QStringLiteral("Not an editor command: %1").arg(command));
    enter_mode(QByteArrayLiteral("n"));
}

void Context_Popup::move_field(int direction)
{
    if (editors_.isEmpty())
        return;
    set_active_field(qBound<qsizetype>(
        0,
        active_field_ + direction,
        editors_.size() - 1));
}

void Context_Popup::move_component(int direction)
{
    if (active_field_ < 0 || active_field_ >= editors_.size())
        return;
    const int count = editors_.at(active_field_)->editable_component_count();
    if (count <= 0)
        return;
    active_component_ = qBound(0, active_component_ + direction, count - 1);
    refresh_modal_selection();
}

void Context_Popup::refresh_modal_selection()
{
    for (qsizetype index = 0; index < editors_.size(); ++index)
    {
        Author_Value_Widget* editor = editors_.at(index);
        const bool active = index == active_field_;
        editor->setProperty("kalamActiveField", active);
        editor->set_modal_selection(
            active,
            active_component_,
            mode_ == QByteArrayLiteral("i"));
        editor->style()->unpolish(editor);
        editor->style()->polish(editor);
        editor->update();
    }

    QString label = QStringLiteral("NORMAL");
    if (mode_ == QByteArrayLiteral("i"))
        label = QStringLiteral("INSERT");
    else if (mode_ == QByteArrayLiteral("c"))
        label = QStringLiteral("COMMAND");
    if (mode_label_ != nullptr)
        mode_label_->setText(label);
}

void Context_Popup::refresh_statusline()
{
    if (status_label_ == nullptr)
        return;

    if (busy_)
    {
        status_label_->setText(QStringLiteral(" WRITING..."));
        return;
    }
    if (mode_ == QByteArrayLiteral("c"))
    {
        status_label_->setText(QStringLiteral(" :%1").arg(command_text_));
        return;
    }
    if (mode_ == QByteArrayLiteral("i"))
    {
        status_label_->setText(QStringLiteral(
            " INSERT  Esc normal | Qt staging only until :w"));
        return;
    }

    status_label_->setText(QStringLiteral(
        " NORMAL%1  j/k fields | h/l components | i edit | :w | :wq | :q!")
        .arg(dirty_ ? QStringLiteral(" [+]") : QString()));
}

bool Context_Popup::locate_control(
    const QObject* object,
    qsizetype* field_index,
    int* component) const
{
    for (qsizetype index = 0; index < editors_.size(); ++index)
    {
        const int found = editors_.at(index)->component_for_descendant(object);
        if (found < 0)
            continue;
        if (field_index != nullptr)
            *field_index = index;
        if (component != nullptr)
            *component = found;
        return true;
    }
    return false;
}

} // namespace Kalam::Editor
