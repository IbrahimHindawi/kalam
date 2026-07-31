#ifndef KALAM_EDITOR_QT_GENERATED_FORM_H
#define KALAM_EDITOR_QT_GENERATED_FORM_H

#include "editor_types.h"

#include <QDialog>
#include <QString>
#include <QVector>

class QEvent;
class QKeyEvent;
class QLabel;
class QScrollArea;

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
        const QVector<Author_Value_Owned>& values,
        Author_Field_Id committed_field = 0);
    void focus_modal();
    QByteArray mode() const;
    qsizetype active_field() const;
    int active_component() const;
    bool is_dirty() const;
    bool has_modal_selection() const;
    bool has_validation_error() const;
    void set_validation_error(const QString& error);
    void set_busy(bool busy);
    bool next_dirty_edit(
        Field_Descriptor* field,
        Author_Value_Owned* original,
        Author_Value_Owned* value) const;
    void replay_previews();
    void close_committed();

signals:
    void fieldPreviewRequested(
        Kalam::Editor::Document_Handle document,
        Kalam::Editor::Field_Descriptor field,
        Kalam::Editor::Author_Value_Owned original,
        Kalam::Editor::Author_Value_Owned value);
    void writeRequested(bool close_after_write);
    void cancelAllRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    Document_Handle document_;
    Schema_Descriptor schema_;
    QVector<Author_Value_Owned> original_values_;
    QVector<Author_Value_Widget*> editors_;
    QVector<bool> dirty_fields_;
    QScrollArea* scroll_area_ = nullptr;
    QLabel* mode_label_ = nullptr;
    QLabel* validation_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    qsizetype active_field_ = -1;
    int active_component_ = 0;
    QByteArray mode_ = QByteArrayLiteral("n");
    QString command_text_;
    bool busy_ = false;
    bool closing_ = false;

    int field_index(Author_Field_Id field_id) const;
    bool handle_key(QKeyEvent* event, bool from_control);
    void enter_mode(const QByteArray& mode);
    void execute_command();
    void move_field(int direction);
    void move_component(int direction);
    void set_active_field(qsizetype field_index);
    void refresh_modal_selection();
    void refresh_statusline();
    bool locate_control(
        const QObject* object,
        qsizetype* field_index,
        int* component) const;
};

} // namespace Kalam::Editor

#endif
