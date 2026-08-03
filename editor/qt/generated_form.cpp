#include "generated_form.h"

#include "qt_value_widgets.h"

#include <QCloseEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QVBoxLayout>

namespace Kalam::Editor
{

Generated_Form::Generated_Form(
    const Document_Handle& document,
    const Schema_Descriptor& schema,
    const QVector<Author_Value_Owned>& values,
    QWidget* parent)
    : QDialog(parent, Qt::Tool | Qt::FramelessWindowHint),
      document_(document),
      schema_(schema),
      original_values_(values)
{
    Q_ASSERT(schema.fields.size() == values.size());
    setObjectName(QStringLiteral("generatedAuthorForm"));
    setWindowTitle(
        QStringLiteral("%1 - generated form")
            .arg(QFileInfo(document.path).fileName()));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);
    setFocusPolicy(Qt::StrongFocus);

    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(1, 1, 1, 1);
    root_layout->setSpacing(0);
    auto* title_bar = new QHBoxLayout();
    title_bar->setContentsMargins(0, 0, 0, 0);
    title_bar->setSpacing(0);
    auto* title_label = new QLabel(windowTitle(), this);
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

    scroll_area_ = new QScrollArea(this);
    scroll_area_->setWidgetResizable(true);
    /*
     * A field must never be cut off horizontally: the form is sized to its
     * content instead (see fit_to_contents), so a horizontal bar would only
     * ever mean the sizing failed.
     */
    scroll_area_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* contents = new QWidget(scroll_area_);
    auto* form = new QFormLayout(contents);

    QByteArray previous_group;
    for (qsizetype index = 0; index < schema.fields.size(); ++index)
    {
        const Field_Descriptor& field = schema.fields.at(index);
        if (!field.group.isEmpty() && field.group != previous_group)
        {
            auto* group_label = new QLabel(
                QString::fromUtf8(field.group),
                contents);
            QFont font = group_label->font();
            font.setBold(true);
            group_label->setFont(font);
            form->addRow(group_label);
            previous_group = field.group;
        }

        auto* editor = new Author_Value_Widget(
            field,
            values.at(index),
            contents);
        editor->set_keyboard_passive(false);
        editors_.push_back(editor);
        dirty_fields_.push_back(false);
        form->addRow(QString::fromUtf8(field.name), editor);
        connect(
            editor,
            &Author_Value_Widget::previewRequested,
            this,
            [this](Author_Field_Id field_id, Author_Value_Owned value)
            {
                const int index = field_index(field_id);
                if (index >= 0 && !busy_)
                {
                    dirty_fields_[index] = true;
                    set_validation_error(QString());
                    refresh_statusline();
                    emit fieldPreviewRequested(
                        document_,
                        schema_.fields.at(index),
                        original_values_.at(index),
                        value);
                }
            });
        editor->installEventFilter(this);
        const QList<QWidget*> controls = editor->findChildren<QWidget*>();
        for (QWidget* control : controls)
            control->installEventFilter(this);
    }
    scroll_area_->setWidget(contents);
    root_layout->addWidget(scroll_area_, 1);

    auto* buttons = new QHBoxLayout();
    buttons->setContentsMargins(9, 5, 9, 5);
    buttons->addStretch();
    auto* discard_button = new QPushButton(QStringLiteral("Discard (:q!)"), this);
    auto* write_button = new QPushButton(QStringLiteral("Write (:w)"), this);
    auto* write_close_button = new QPushButton(
        QStringLiteral("Write && Close (:wq)"),
        this);
    for (QPushButton* button : {discard_button, write_button, write_close_button})
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

    connect(close_button, &QPushButton::clicked, this, &Generated_Form::close);
    connect(discard_button, &QPushButton::clicked, this, &Generated_Form::close);
    connect(write_button, &QPushButton::clicked, this, [this]()
    {
        emit writeRequested(false);
    });
    connect(
        write_close_button,
        &QPushButton::clicked,
        this,
        [this]() { emit writeRequested(true); });

    set_active_field(editors_.isEmpty() ? -1 : 0);
    refresh_statusline();
    fit_to_contents(contents);
}

const Document_Handle& Generated_Form::document() const
{
    return document_;
}

void Generated_Form::refresh(
    const Document_Handle& document,
    const QVector<Author_Value_Owned>& values,
    Author_Field_Id committed_field)
{
    if (values.size() != editors_.size())
    {
        close();
        return;
    }

    QVector<Author_Value_Owned> staged;
    staged.reserve(editors_.size());
    for (const Author_Value_Widget* editor : editors_)
        staged.push_back(editor->value());

    document_ = document;
    original_values_ = values;
    for (qsizetype index = 0; index < editors_.size(); ++index)
    {
        const bool committed =
            committed_field != 0 &&
            schema_.fields.at(index).id == committed_field;
        if (committed)
            dirty_fields_[index] = false;

        editors_.at(index)->set_value(
            dirty_fields_.at(index) ? staged.at(index) : values.at(index));
    }
    refresh_modal_selection();
    refresh_statusline();
}

void Generated_Form::focus_modal()
{
    enter_mode(QByteArrayLiteral("n"));
}

QByteArray Generated_Form::mode() const
{
    return mode_;
}

qsizetype Generated_Form::active_field() const
{
    return active_field_;
}

int Generated_Form::active_component() const
{
    return active_component_;
}

bool Generated_Form::is_dirty() const
{
    for (bool dirty : dirty_fields_)
    {
        if (dirty)
            return true;
    }
    return false;
}

bool Generated_Form::has_modal_selection() const
{
    for (const Author_Value_Widget* editor : editors_)
    {
        if (editor->has_modal_selection())
            return true;
    }
    return false;
}

bool Generated_Form::has_validation_error() const
{
    return validation_label_ != nullptr && !validation_label_->text().isEmpty();
}

void Generated_Form::set_validation_error(const QString& error)
{
    validation_label_->setText(error);
    validation_label_->setVisible(!error.isEmpty());
}

void Generated_Form::set_busy(bool busy)
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

bool Generated_Form::next_dirty_edit(
    Field_Descriptor* field,
    Author_Value_Owned* original,
    Author_Value_Owned* value) const
{
    for (qsizetype index = 0; index < dirty_fields_.size(); ++index)
    {
        if (!dirty_fields_.at(index))
            continue;
        if (field != nullptr)
            *field = schema_.fields.at(index);
        if (original != nullptr)
            *original = original_values_.at(index);
        if (value != nullptr)
            *value = editors_.at(index)->value();
        return true;
    }
    return false;
}

void Generated_Form::replay_previews()
{
    for (qsizetype index = 0; index < dirty_fields_.size(); ++index)
    {
        if (!dirty_fields_.at(index))
            continue;
        emit fieldPreviewRequested(
            document_,
            schema_.fields.at(index),
            original_values_.at(index),
            editors_.at(index)->value());
    }
}

void Generated_Form::close_committed()
{
    closing_ = true;
    close();
}

bool Generated_Form::eventFilter(QObject* watched, QEvent* event)
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
    return QDialog::eventFilter(watched, event);
}

void Generated_Form::keyPressEvent(QKeyEvent* event)
{
    if (!handle_key(event, false))
        QDialog::keyPressEvent(event);
}

bool Generated_Form::handle_key(QKeyEvent* event, bool from_control)
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

void Generated_Form::enter_mode(const QByteArray& mode)
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

void Generated_Form::execute_command()
{
    const QString command = command_text_.trimmed();
    command_text_.clear();

    if (command == QStringLiteral("w"))
    {
        enter_mode(QByteArrayLiteral("n"));
        emit writeRequested(false);
        return;
    }
    if (command == QStringLiteral("wq") ||
        command == QStringLiteral("x"))
    {
        enter_mode(QByteArrayLiteral("n"));
        emit writeRequested(true);
        return;
    }
    if (command == QStringLiteral("q!"))
    {
        enter_mode(QByteArrayLiteral("n"));
        close();
        return;
    }
    if (command == QStringLiteral("q"))
    {
        if (is_dirty())
        {
            set_validation_error(QStringLiteral(
                "No write since last change (use :q! to discard)"));
            enter_mode(QByteArrayLiteral("n"));
            return;
        }
        enter_mode(QByteArrayLiteral("n"));
        close();
        return;
    }

    set_validation_error(
        command.isEmpty()
            ? QStringLiteral("Not an editor command")
            : QStringLiteral("Not an editor command: %1").arg(command));
    enter_mode(QByteArrayLiteral("n"));
}

void Generated_Form::move_field(int direction)
{
    if (editors_.isEmpty())
        return;
    set_active_field(qBound<qsizetype>(
        0,
        active_field_ + direction,
        editors_.size() - 1));
}

void Generated_Form::move_component(int direction)
{
    if (active_field_ < 0 || active_field_ >= editors_.size())
        return;
    const int count = editors_.at(active_field_)->editable_component_count();
    if (count <= 0)
        return;
    active_component_ = qBound(0, active_component_ + direction, count - 1);
    refresh_modal_selection();
}

void Generated_Form::fit_to_contents(QWidget* contents)
{
    if (contents == nullptr || scroll_area_ == nullptr)
    {
        return;
    }

    /*
     * Size the window to the fields rather than to a fixed guess: the
     * width of a generated form depends on the schema and on whatever
     * font Neovim asked for, so a hardcoded width clipped content as soon
     * as either changed. Width always fits; height fits when the screen
     * allows and otherwise scrolls, which is the one bar worth keeping.
     */
    contents->adjustSize();
    const QSize content_hint = contents->sizeHint();
    const int frame = 2 * scroll_area_->frameWidth();
    const int bar_width = scroll_area_->verticalScrollBar() != nullptr
        ? scroll_area_->verticalScrollBar()->sizeHint().width()
        : 0;

    scroll_area_->setMinimumWidth(content_hint.width() + frame + bar_width);
    scroll_area_->setMinimumHeight(
        qMin(content_hint.height() + frame, 900));

    adjustSize();

    const QScreen* screen = this->screen();
    if (screen != nullptr)
    {
        const QRect available = screen->availableGeometry();
        resize(
            qMin(width(), available.width() - 80),
            qMin(height(), available.height() - 80));
    }

    /*
     * The minimum is only needed to drive the initial size; keeping it
     * would stop the user shrinking the window afterwards.
     */
    scroll_area_->setMinimumWidth(0);
    scroll_area_->setMinimumHeight(0);
}

void Generated_Form::set_active_field(qsizetype field_index)
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
    if (scroll_area_ != nullptr)
        scroll_area_->ensureWidgetVisible(editors_.at(active_field_));
}

void Generated_Form::refresh_modal_selection()
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

void Generated_Form::refresh_statusline()
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
        .arg(is_dirty() ? QStringLiteral(" [+]") : QString()));
}

bool Generated_Form::locate_control(
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

void Generated_Form::closeEvent(QCloseEvent* event)
{
    if (busy_)
    {
        event->ignore();
        return;
    }
    if (!closing_)
    {
        closing_ = true;
        emit cancelAllRequested();
    }
    QDialog::closeEvent(event);
}

int Generated_Form::field_index(Author_Field_Id field_id) const
{
    for (qsizetype index = 0; index < schema_.fields.size(); ++index)
    {
        if (schema_.fields.at(index).id == field_id)
        {
            return static_cast<int>(index);
        }
    }
    return -1;
}

} // namespace Kalam::Editor
