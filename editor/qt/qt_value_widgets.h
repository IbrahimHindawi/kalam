#ifndef KALAM_EDITOR_QT_VALUE_WIDGETS_H
#define KALAM_EDITOR_QT_VALUE_WIDGETS_H

#include "editor_types.h"

#include <QVector>
#include <QWidget>

class QCheckBox;
class QColorDialog;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace Kalam::Editor
{

class Author_Value_Widget final : public QWidget
{
    Q_OBJECT

public:
    explicit Author_Value_Widget(
        const Field_Descriptor& field,
        const Author_Value_Owned& value,
        QWidget* parent = nullptr);

    const Field_Descriptor& field() const;
    Author_Value_Owned value() const;
    void set_value(const Author_Value_Owned& value);
    void set_keyboard_passive(bool passive);
    QVector<QWidget*> editable_controls() const;
    int editable_component_count() const;
    int component_for_descendant(const QObject* object) const;
    void set_modal_selection(bool active, int component, bool insert_mode);
    void focus_component(int component);
    bool has_modal_selection() const;
    void focus_first_control();

signals:
    void previewRequested(
        Author_Field_Id field_id,
        Kalam::Editor::Author_Value_Owned value);
    void commitRequested(
        Author_Field_Id field_id,
        Kalam::Editor::Author_Value_Owned value);

private:
    Field_Descriptor field_;
    QVector<QDoubleSpinBox*> numeric_spins_;
    QSpinBox* integer_spin_ = nullptr;
    QCheckBox* checkbox_ = nullptr;
    QLineEdit* line_edit_ = nullptr;
    QPushButton* color_button_ = nullptr;
    QColorDialog* color_dialog_ = nullptr;
    QWidget* modal_selection_control_ = nullptr;
    Author_Value_Owned color_dialog_original_;
    bool updating_ = false;

    void build_numeric_editor();
    void choose_color();
    void clear_modal_selection();
    void publish_preview();
    void publish_commit();
    int spin_component_count() const;
};

} // namespace Kalam::Editor

#endif
