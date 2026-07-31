#ifndef KALAM_EDITOR_QT_CONTEXT_POPUP_H
#define KALAM_EDITOR_QT_CONTEXT_POPUP_H

#include "editor_types.h"

#include <QFrame>
#include <QString>
#include <QVector>

class QEvent;
class QKeyEvent;
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
        const QRect& anchor_rect,
        const QSize& cell_size);
    QVector<Author_Value_Owned> staged_values() const;
    void set_values(
        const QVector<Author_Value_Owned>& values,
        bool mark_clean = true);
    void set_active_field(qsizetype field_index);
    void focus_modal();
    QByteArray mode() const;
    qsizetype active_field() const;
    int active_component() const;
    bool is_dirty() const;
    bool has_modal_selection() const;
    bool has_validation_error() const;
    void set_validation_error(const QString& error);
    void set_busy(bool busy);

signals:
    void fieldPreviewRequested(
        Author_Field_Id field_id,
        Kalam::Editor::Author_Value_Owned value);
    void applyRequested(bool close_after_write);
    void cancelRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    QVector<Author_Value_Widget*> editors_;
    QLabel* mode_label_ = nullptr;
    QLabel* validation_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    qsizetype active_field_ = -1;
    int active_component_ = 0;
    QByteArray mode_ = QByteArrayLiteral("n");
    QString command_text_;
    bool dirty_ = false;
    bool busy_ = false;

    bool handle_key(QKeyEvent* event, bool from_control);
    void enter_mode(const QByteArray& mode);
    void execute_command();
    void move_field(int direction);
    void move_component(int direction);
    void refresh_modal_selection();
    void refresh_statusline();
    bool locate_control(
        const QObject* object,
        qsizetype* field_index,
        int* component) const;
};

} // namespace Kalam::Editor

#endif
