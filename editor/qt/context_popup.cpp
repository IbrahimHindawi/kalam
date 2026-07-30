#include "context_popup.h"

#include "qt_value_widgets.h"

#include <QCloseEvent>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QVBoxLayout>

namespace Kalam::Editor
{

Context_Popup::Context_Popup(
    const QVector<Field_Descriptor>& fields,
    const QVector<Author_Value_Owned>& values,
    QWidget* parent)
    : QDialog(parent, Qt::Tool)
{
    Q_ASSERT(fields.size() == values.size());
    setObjectName(QStringLiteral("contextPopup"));
    setWindowTitle(QStringLiteral("Edit authored value"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);

    auto* root_layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    for (qsizetype index = 0; index < fields.size(); ++index)
    {
        auto* editor = new Author_Value_Widget(
            fields.at(index),
            values.at(index),
            this);
        editors_.push_back(editor);
        form->addRow(QString::fromUtf8(fields.at(index).name), editor);
        connect(
            editor,
            &Author_Value_Widget::previewRequested,
            this,
            &Context_Popup::fieldPreviewRequested);
    }
    root_layout->addLayout(form);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    auto* cancel_button = new QPushButton(QStringLiteral("Cancel"), this);
    auto* accept_button = new QPushButton(QStringLiteral("Accept"), this);
    accept_button->setDefault(true);
    buttons->addWidget(cancel_button);
    buttons->addWidget(accept_button);
    root_layout->addLayout(buttons);

    connect(
        cancel_button,
        &QPushButton::clicked,
        this,
        [this]()
        {
            completed_ = true;
            emit cancelRequested();
        });
    connect(
        accept_button,
        &QPushButton::clicked,
        this,
        [this]()
        {
            completed_ = true;
            emit acceptRequested();
        });

    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, cancel_button, &QPushButton::click);
    auto* enter = new QShortcut(QKeySequence(Qt::Key_Return), this);
    connect(enter, &QShortcut::activated, accept_button, &QPushButton::click);
}

void Context_Popup::anchor_to(
    QWidget* shell,
    const QRect& cursor_rect_in_shell)
{
    if (shell == nullptr || !cursor_rect_in_shell.isValid())
    {
        close();
        return;
    }

    adjustSize();
    const QPoint cursor_bottom_left = shell->mapToGlobal(
        cursor_rect_in_shell.bottomLeft() + QPoint(0, 3));
    QScreen* screen = QGuiApplication::screenAt(cursor_bottom_left);
    if (screen == nullptr)
    {
        screen = shell->screen();
    }
    QRect usable = screen != nullptr
        ? screen->availableGeometry()
        : QRect(cursor_bottom_left, size());

    QPoint target = cursor_bottom_left;
    if (target.x() + width() > usable.right())
    {
        target.setX(usable.right() - width() + 1);
    }
    if (target.y() + height() > usable.bottom())
    {
        target.setY(
            shell->mapToGlobal(cursor_rect_in_shell.topLeft()).y() -
            height() - 3);
    }
    target.setX(qMax(usable.left(), target.x()));
    target.setY(qMax(usable.top(), target.y()));
    move(target);
}

void Context_Popup::closeEvent(QCloseEvent* event)
{
    if (!completed_)
    {
        completed_ = true;
        emit cancelRequested();
    }
    QDialog::closeEvent(event);
}

} // namespace Kalam::Editor
