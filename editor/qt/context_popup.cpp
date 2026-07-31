#include "context_popup.h"

#include "qt_value_widgets.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
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
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);

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
        editor->set_keyboard_passive(true);
        editors_.push_back(editor);
        form->addRow(QString::fromUtf8(fields.at(index).name), editor);
        connect(
            editor,
            &Author_Value_Widget::previewRequested,
            this,
            &Context_Popup::fieldPreviewRequested);
    }
    root_layout->addLayout(form);
    root_layout->addStretch(1);

    auto* hint = new QLabel(
        QStringLiteral("j/k fields  |  h/l components  |  i edit  |  Esc normal"),
        this);
    hint->setObjectName(QStringLiteral("kalamContextHint"));
    hint->setAlignment(Qt::AlignCenter);
    root_layout->addWidget(hint);

    auto* buttons = new QHBoxLayout();
    buttons->setContentsMargins(9, 5, 9, 9);
    buttons->addStretch();
    auto* discard_button = new QPushButton(QStringLiteral("Discard (:q!)"), this);
    auto* apply_button = new QPushButton(QStringLiteral("Apply (:w)"), this);
    auto* apply_close_button = new QPushButton(
        QStringLiteral("Apply && Close (:wq)"),
        this);
    for (QPushButton* button : {discard_button, apply_button, apply_close_button})
    {
        button->setAutoDefault(false);
        button->setDefault(false);
        button->setFocusPolicy(Qt::NoFocus);
        buttons->addWidget(button);
    }
    root_layout->addLayout(buttons);

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
}

void Context_Popup::project_to(
    QWidget* shell,
    const Context_Session_State& state,
    const QSize& cell_size)
{
    if (shell == nullptr || cell_size.width() <= 0 || cell_size.height() <= 0 ||
        state.window_width <= 0 || state.window_height <= 0)
    {
        hide();
        return;
    }
    if (parentWidget() != shell)
    {
        setParent(shell);
    }

    const QRect requested(
        static_cast<int>(state.window_column) * cell_size.width(),
        static_cast<int>(state.window_row) * cell_size.height(),
        static_cast<int>(state.window_width) * cell_size.width(),
        static_cast<int>(state.window_height) * cell_size.height());
    const QRect visible = requested.intersected(shell->rect());
    if (!visible.isValid() || visible.isEmpty())
    {
        hide();
        return;
    }
    setGeometry(visible);
    show();
    raise();
}

void Context_Popup::set_values(const QVector<Author_Value_Owned>& values)
{
    const qsizetype count = qMin(editors_.size(), values.size());
    for (qsizetype index = 0; index < count; ++index)
    {
        editors_.at(index)->set_value(values.at(index));
    }
}

void Context_Popup::set_active_field(qsizetype field_index)
{
    if (editors_.isEmpty())
    {
        active_field_ = -1;
        return;
    }
    active_field_ = qBound<qsizetype>(0, field_index, editors_.size() - 1);
    for (qsizetype index = 0; index < editors_.size(); ++index)
    {
        Author_Value_Widget* editor = editors_.at(index);
        editor->setProperty("kalamActiveField", index == active_field_);
        editor->style()->unpolish(editor);
        editor->style()->polish(editor);
        editor->update();
    }
}

void Context_Popup::set_mode(const QByteArray& mode)
{
    mode_ = mode;
    QString label = QStringLiteral("NORMAL");
    if (mode.startsWith('i'))
        label = QStringLiteral("INSERT");
    else if (mode.startsWith('R'))
        label = QStringLiteral("REPLACE");
    else if (mode.startsWith('v') || mode.startsWith('V') ||
             (!mode.isEmpty() && mode.at(0) == char(0x16)))
        label = QStringLiteral("VISUAL");
    else if (mode.startsWith('c'))
        label = QStringLiteral("COMMAND");
    mode_label_->setText(label);
    refresh_projected_cursor();
}

void Context_Popup::set_buffer_cursor(
    const QByteArray& text_utf8,
    qint64 row,
    qint64 byte_column)
{
    buffer_text_ = text_utf8;
    buffer_cursor_row_ = row;
    buffer_cursor_column_ = byte_column;
    refresh_projected_cursor();
}

bool Context_Popup::has_projected_cursor() const
{
    for (const Author_Value_Widget* editor : editors_)
    {
        if (editor->has_projected_cursor())
            return true;
    }
    return false;
}

void Context_Popup::refresh_projected_cursor()
{
    for (Author_Value_Widget* editor : editors_)
        editor->set_projected_cursor(false, {}, 0, mode_);
    if (editors_.isEmpty() || buffer_text_.isEmpty())
        return;

    const QList<QByteArray> lines = buffer_text_.split('\n');
    if (lines.isEmpty())
        return;
    const qsizetype field_index = qBound<qsizetype>(
        0,
        static_cast<qsizetype>(qMax<qint64>(0, buffer_cursor_row_ - 1)),
        editors_.size() - 1);
    const qsizetype line_index = qBound<qsizetype>(
        0,
        field_index + 1,
        lines.size() - 1);
    const QByteArray line = lines.at(line_index);
    qsizetype value_start = line.indexOf(':');
    if (value_start >= 0)
    {
        ++value_start;
        while (value_start < line.size() &&
               (line.at(value_start) == ' ' || line.at(value_start) == '\t'))
        {
            ++value_start;
        }
    }
    else
    {
        value_start = 0;
    }

    qsizetype value_end = line.size();
    while (value_end > value_start &&
           (line.at(value_end - 1) == ' ' || line.at(value_end - 1) == '\t'))
    {
        --value_end;
    }
    if (value_end > value_start && line.at(value_end - 1) == ',')
    {
        --value_end;
        while (value_end > value_start &&
               (line.at(value_end - 1) == ' ' || line.at(value_end - 1) == '\t'))
        {
            --value_end;
        }
    }

    active_field_ = field_index;
    for (qsizetype index = 0; index < editors_.size(); ++index)
    {
        Author_Value_Widget* editor = editors_.at(index);
        editor->setProperty("kalamActiveField", index == active_field_);
        editor->style()->unpolish(editor);
        editor->style()->polish(editor);
        editor->update();
    }
    editors_.at(field_index)->set_projected_cursor(
        true,
        line.mid(value_start, value_end - value_start),
        buffer_cursor_column_ - value_start,
        mode_);
}

void Context_Popup::set_validation_error(const QString& error)
{
    validation_label_->setText(error);
    validation_label_->setVisible(!error.isEmpty());
}

} // namespace Kalam::Editor
