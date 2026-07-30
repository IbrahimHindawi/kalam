#include "generated_form.h"

#include "qt_value_widgets.h"

#include <QCloseEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

namespace Kalam::Editor
{

Generated_Form::Generated_Form(
    const Document_Handle& document,
    const Schema_Descriptor& schema,
    const QVector<Author_Value_Owned>& values,
    QWidget* parent)
    : QDialog(parent, Qt::Tool),
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
    resize(460, 520);

    auto* root_layout = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto* contents = new QWidget(scroll);
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
        editors_.push_back(editor);
        form->addRow(QString::fromUtf8(field.name), editor);
        connect(
            editor,
            &Author_Value_Widget::previewRequested,
            this,
            [this](Author_Field_Id field_id, Author_Value_Owned value)
            {
                const int index = field_index(field_id);
                if (index >= 0)
                {
                    emit fieldPreviewRequested(
                        document_,
                        schema_.fields.at(index),
                        original_values_.at(index),
                        value);
                }
            });
        connect(
            editor,
            &Author_Value_Widget::commitRequested,
            this,
            [this](Author_Field_Id field_id, Author_Value_Owned value)
            {
                const int index = field_index(field_id);
                if (index >= 0)
                {
                    emit fieldCommitRequested(
                        document_,
                        schema_.fields.at(index),
                        original_values_.at(index),
                        value);
                }
            });
    }
    scroll->setWidget(contents);
    root_layout->addWidget(scroll);
}

const Document_Handle& Generated_Form::document() const
{
    return document_;
}

void Generated_Form::refresh(
    const Document_Handle& document,
    const QVector<Author_Value_Owned>& values)
{
    if (values.size() != editors_.size())
    {
        close();
        return;
    }
    document_ = document;
    original_values_ = values;
    for (qsizetype index = 0; index < editors_.size(); ++index)
    {
        editors_.at(index)->set_value(values.at(index));
    }
}

void Generated_Form::closeEvent(QCloseEvent* event)
{
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
