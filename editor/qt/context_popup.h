#ifndef KALAM_EDITOR_QT_CONTEXT_POPUP_H
#define KALAM_EDITOR_QT_CONTEXT_POPUP_H

#include "editor_types.h"

#include <QFrame>
#include <QVector>

class QLabel;

namespace Kalam::Editor
{

class Author_Value_Widget;

class Context_Popup final : public QFrame
{
    Q_OBJECT

public:
    explicit Context_Popup(
        const QVector<Field_Descriptor>& fields,
        const QVector<Author_Value_Owned>& values,
        QWidget* parent = nullptr);

    void project_to(
        QWidget* shell,
        const Context_Session_State& state,
        const QSize& cell_size);
    void set_values(const QVector<Author_Value_Owned>& values);
    void set_active_field(qsizetype field_index);
    void set_mode(const QByteArray& mode);
    void set_buffer_cursor(
        const QByteArray& text_utf8,
        qint64 row,
        qint64 byte_column);
    bool has_projected_cursor() const;
    void set_validation_error(const QString& error);

signals:
    void fieldPreviewRequested(
        Author_Field_Id field_id,
        Kalam::Editor::Author_Value_Owned value);
    void applyRequested(bool close_after_write);
    void cancelRequested();

private:
    QVector<Author_Value_Widget*> editors_;
    QLabel* mode_label_ = nullptr;
    QLabel* validation_label_ = nullptr;
    qsizetype active_field_ = -1;
    QByteArray mode_;
    QByteArray buffer_text_;
    qint64 buffer_cursor_row_ = 0;
    qint64 buffer_cursor_column_ = 0;

    void refresh_projected_cursor();
};

} // namespace Kalam::Editor

#endif
