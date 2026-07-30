#ifndef KALAM_EDITOR_QT_CONTEXT_POPUP_H
#define KALAM_EDITOR_QT_CONTEXT_POPUP_H

#include "editor_types.h"

#include <QDialog>
#include <QVector>

namespace Kalam::Editor
{

class Author_Value_Widget;

class Context_Popup final : public QDialog
{
    Q_OBJECT

public:
    explicit Context_Popup(
        const QVector<Field_Descriptor>& fields,
        const QVector<Author_Value_Owned>& values,
        QWidget* parent = nullptr);

    void anchor_to(QWidget* shell, const QRect& cursor_rect_in_shell);

signals:
    void fieldPreviewRequested(
        Author_Field_Id field_id,
        Kalam::Editor::Author_Value_Owned value);
    void acceptRequested();
    void cancelRequested();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    QVector<Author_Value_Widget*> editors_;
    bool completed_ = false;
};

} // namespace Kalam::Editor

#endif
