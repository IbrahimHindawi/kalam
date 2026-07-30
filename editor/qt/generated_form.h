#ifndef KALAM_EDITOR_QT_GENERATED_FORM_H
#define KALAM_EDITOR_QT_GENERATED_FORM_H

#include "editor_types.h"

#include <QDialog>
#include <QVector>

namespace Kalam::Editor
{

class Author_Value_Widget;

class Generated_Form final : public QDialog
{
    Q_OBJECT

public:
    explicit Generated_Form(
        const Document_Handle& document,
        const Schema_Descriptor& schema,
        const QVector<Author_Value_Owned>& values,
        QWidget* parent = nullptr);

    const Document_Handle& document() const;
    void refresh(
        const Document_Handle& document,
        const QVector<Author_Value_Owned>& values);

signals:
    void fieldPreviewRequested(
        Kalam::Editor::Document_Handle document,
        Kalam::Editor::Field_Descriptor field,
        Kalam::Editor::Author_Value_Owned original,
        Kalam::Editor::Author_Value_Owned value);
    void fieldCommitRequested(
        Kalam::Editor::Document_Handle document,
        Kalam::Editor::Field_Descriptor field,
        Kalam::Editor::Author_Value_Owned original,
        Kalam::Editor::Author_Value_Owned value);
    void cancelAllRequested();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    Document_Handle document_;
    Schema_Descriptor schema_;
    QVector<Author_Value_Owned> original_values_;
    QVector<Author_Value_Widget*> editors_;
    bool closing_ = false;

    int field_index(Author_Field_Id field_id) const;
};

} // namespace Kalam::Editor

#endif
